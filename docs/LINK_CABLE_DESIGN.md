# Gen 2 link-cable multiplayer — design

> *Two `rom.exe` instances, one ENet socket, full Pokémon Cable Club. Shared between [pokemon-gold](https://github.com/sp00nznet/pokemon-gold), [pokemon-silver](https://github.com/sp00nznet/pokemon-silver), and [pokemon-crystal](https://github.com/sp00nznet/pokemon-crystal); implemented in [sp00nznet/gb-recompiled](https://github.com/sp00nznet/gb-recompiled).*

The infrastructure lives in the fork (one set of code serves all three games); the per-port repos just bump the submodule pointer once the work lands.

---

## 1 · What the hardware actually does

Two memory-mapped registers control the serial port:

| Reg | Addr | Purpose |
|---|---|---|
| `SB` | `$FF01` | Serial transfer data — the byte shifted in/out |
| `SC` | `$FF02` | Serial control bits |

`SC` bits:

| Bit | Meaning |
|---:|---|
| 7 | Transfer start (write 1 to begin, hardware clears when complete) |
| 1 | Clock source: 1 = internal (this side is master), 0 = external (slave, waiting for master to drive the clock) |
| 0 | Clock speed (CGB only): 0 = 8192 Hz, 1 = 262144 Hz |

When master starts a transfer, both ends shift 8 bits across the wire (master's old SB → slave's new SB; slave's old SB → master's new SB). On completion, **both** sides get a serial interrupt (`IF` bit 3).

DMG byte rate: ~1024 bytes/sec. CGB-fast byte rate: ~32 KB/sec. Bandwidth is negligible.

Crucially: **the game on master spins waiting for the transfer to complete; the game on slave spins waiting for the master to initiate**. Both sides synchronously block. That's the same model the runtime must offer.

---

## 2 · Where the runtime is today

`gbrecomp/runtime/src/gbrt.c` line ~898:

```c
if (addr == 0xFF02 && (value & 0x80)) {
    printf("%c", ctx->io[0x01]); fflush(stdout);
    ctx->io[0x0F] |= 0x08;   // raise IF bit 3 (serial interrupt)
}
```

That's it. It prints the outgoing byte to stdout (for Tetris-style debug logging) and self-fires the serial interrupt. There's no partner. Anything that actually reads SB after the "transfer" sees the byte it just sent — which is fine for solo play (most Gen 2 code paths never touch SC), but useless the moment a player walks into the Cable Club.

The `GBPlatformCallbacks.on_serial_byte(ctx, byte)` hook exists in the header but is never invoked.

---

## 3 · The proposed serial subsystem

### 3.1 Callback shape

Add to `GBPlatformCallbacks`:

```c
/* Called when the game writes SC with bit 7 set. The platform layer
 * (a) inspects `mode` to determine whether this side is the master, and
 * (b) exchanges `out_byte` for the partner's byte, blocking up to
 *     `timeout_ms` if the partner isn't ready.
 * Return 0xFF on timeout (open-bus default — same as a disconnected cable).
 */
typedef enum {
    GB_SERIAL_MASTER = 1,
    GB_SERIAL_SLAVE  = 0,
} GBSerialMode;

uint8_t (*serial_exchange)(GBContext* ctx, uint8_t out_byte,
                           GBSerialMode mode, uint32_t timeout_ms);
```

Default (when callback is NULL or unset): return `0xFF`, mirroring an unplugged cable. Preserves current behaviour for solo play.

### 3.2 What changes in `gbrt.c`

Replace the printf hack with:

```c
if (addr == 0xFF02 && (value & 0x80)) {
    ctx->io[addr - 0xFF00] = value;   // store SC

    GBSerialMode mode = (value & 0x02) ? GB_SERIAL_MASTER : GB_SERIAL_SLAVE;
    uint8_t out = ctx->io[0x01];      // current SB
    uint8_t in  = 0xFF;
    if (ctx->callbacks.serial_exchange) {
        in = ctx->callbacks.serial_exchange(ctx, out, mode, /*timeout_ms*/ 2000);
    }
    ctx->io[0x01] = in;                // overwrite SB with partner's byte
    ctx->io[0x02] &= ~0x80;            // clear start bit (transfer complete)
    ctx->io[0x0F] |= 0x08;             // raise IF bit 3
    return;
}
```

Solo play unchanged (no callback ⇒ returns 0xFF, same as before). Linked play: bytes really exchange.

### 3.3 Timing model

Real hardware takes ~1 ms per byte. The recompiled runtime treats `serial_exchange` as synchronous-on-the-game-thread, blocking up to `timeout_ms` (2 s default) for the partner. While blocked, audio still plays from the ring buffer (audio thread is independent) — actually a useful property since the Cable Club music keeps going.

Once both rom.exe instances are linked and inside the Cable Club, both threads spend most of their time in `serial_exchange` waiting on each other; that's exactly what real cables do.

If a partner disconnects mid-trade: 2 s timeout fires → `serial_exchange` returns `0xFF` → game sees the "cable was unplugged" path and bails out.

---

## 4 · The mp_gen2 module

New file: `gbrecomp/runtime/src/multiplayer/mp_gen2_serial.cpp`.

Owns:

- ENet host/client management (reuses la-mp's session connection)
- A single ENet reliable channel for serial bytes (low bandwidth, must arrive in order)
- A pair of mutex-protected queues:
  - `inbox` — bytes arrived from partner
  - `outbox` — bytes we want to send (mostly drained immediately)
- The implementation of `serial_exchange`

Wire packet:

```c
struct MPSerialByte {
    uint8_t header;      // = MP_PKT_SERIAL
    uint8_t out_byte;    // the byte we shifted out
    uint8_t mode;        // GB_SERIAL_MASTER or GB_SERIAL_SLAVE
    uint32_t seq;        // monotonically increasing transfer id
};
```

When *we* are master: push `MPSerialByte{out_byte, MASTER, seq++}`, then block on `inbox.pop(timeout)` for a `MPSerialByte{partner_out_byte, SLAVE, seq}` reply.

When *we* are slave: block on `inbox.pop(timeout)` for `MPSerialByte{partner_out_byte, MASTER, seq}`, then reply with `MPSerialByte{our_out_byte, SLAVE, seq}`.

Either side that times out sends a disconnect notice and returns `0xFF`.

---

## 5 · Save-directory isolation

Two rom.exe windows on the same machine currently both write to `<exe-dir>/POKEMON_GLDAAUE.sav` — they'd clobber each other. Add a CLI flag:

```
rom.exe --save-dir saves/p1
rom.exe --save-dir saves/p2
```

When set, `sdl_get_sidecar_path()` writes `<save-dir>/<title>.{sav,rtc}` instead of `<exe-dir>/<title>.{sav,rtc}`. One-liner in `platform_sdl.cpp` plumbing; goes upstream.

---

## 6 · UI

Multiplayer menu adds a "Cable Club" submenu with:

- Host (listen on configurable port, default 21385 — la-mp uses 21384)
- Join (host:port)
- Status indicator: ⚪ idle / 🟡 waiting / 🟢 linked / 🔴 dropped

The indicator is a small overlay (top-right corner) implemented in the la-mp `mp_indicators.cpp` style. ImGui modal for connect UI.

The link-cable session is independent of the la-mp world-sync overlay — both can be enabled but they don't talk to each other. la-mp's `mp_session.cpp` already handles ENet host bookkeeping; mp_gen2 piggybacks on its connection.

---

## 7 · What the games already do (no code needed from us)

Once the runtime correctly relays serial bytes, the game ROM does all the protocol work itself:

- **Sync byte exchange**: at Cable Club door, both games handshake by trading sync bytes until both agree they're talking.
- **Trade-block exchange**: 415 bytes per side (party data, OT, nicknames), plus Pokédex deltas.
- **Battle setup**: ~similar block exchange, then turn-by-turn move selections.
- **Time Capsule** (Gold/Silver only): downgraded Gen 1 ↔ Gen 2 protocol; same byte-level mechanics.

Documented in pret/pokegold's `engine/link/` directory. We don't need to understand the protocols at all — just relay bytes.

---

## 8 · Implementation phases

1. **`--save-dir` flag** — small, contained, useful immediately even for solo testing.
2. **Serial subsystem in gbrt** — replace the printf, add callback. Verify solo play is unchanged.
3. **mp_gen2 module** — ENet bridge, host/join CLI flags.
4. **UI overlay** — Cable Club submenu, status indicator.
5. **Bump submodule in all three port repos.**
6. **Test**: Gold ↔ Crystal trade, on localhost.
7. **Test**: battle, same pair.
8. **Iterate**: timeouts, disconnect handling, edge cases.

Steps 1-3 are mechanical. Step 6 is the moment-of-truth: either the games handshake and trade, or we're debugging at the byte level.

---

## 9 · Risks / unknowns

- **Timing drift** — DMG serial expects ~1 ms per byte. If our `serial_exchange` runs in <0.1 ms in solo (it does — just returns 0xFF), some Gen 2 code paths *might* trip on the lack of "transfer time" cycles. We may need to insert dummy cycles into `gb_tick` during a serial transfer. Unlikely to bite trades; more plausible during battle move-prediction loops.
- **CGB high-speed serial** — the protocol uses `SC` bit 0 = 1 for 32× faster transfer. Should work identically with our model (just same byte exchange, different timing). Worth verifying both Gold (slow) and Crystal (might use fast) handshakes.
- **Mid-trade disconnect** — what does Gold do if SB stays 0xFF for several seconds? Probably reverts to overworld. Need to verify it doesn't corrupt save data.
- **Open Cable Club without a partner** — Gold will keep retrying handshake bytes. Our timeout (2 s default per byte) means the game appears to "lag" then eventually times out. Fine.

---

## 10 · Out of scope (for now)

- NAT punch-through / matchmaking — assume LAN/VPN/manual port forward for MVP.
- Mystery Gift (Crystal-only IR feature).
- Stadium 2 transfer (uses different protocol via Transfer Pak).
- Mobile adapter (Japan-only Crystal feature).
- Cross-generation trades (Time Capsule) — should work for free via byte relay but not the priority verification target.
