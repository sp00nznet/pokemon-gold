/*
 * Compat shim for building gb-recompiled-generated C code under MSVC.
 *
 * The recompiler currently emits `__attribute__((noinline))` on its
 * per-chunk dispatch helpers (a GCC/Clang extension). MinGW understands
 * it natively, MSVC doesn't — and mis-parses the function header, which
 * cascades into thousands of "undeclared identifier 'ctx'" errors.
 *
 * Force-include this from CMake (/FI flag) for the `rom` target.
 *
 * TODO: upstream this as a `#ifdef _MSC_VER / #define __attribute__(x) /
 * #endif` block emitted by c_emitter at the top of rom.c. PR target:
 * sp00nznet/gb-recompiled (NOT arcanite24).
 */
#ifndef GBRECOMP_MSVC_COMPAT_H
#define GBRECOMP_MSVC_COMPAT_H

#ifdef _MSC_VER
#  define __attribute__(x)
#endif

#endif
