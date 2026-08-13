#include <stdint.h>

/* Freestanding 64-bit integer divide/modulo helpers.
 *
 * Plain -m32 x86 has no hardware 64-bit divide, so the compiler emits
 * calls to a handful of well-known libgcc runtime symbols wherever C
 * source does 64-bit / or % (aml.c's AML Divide/Mod opcodes need real
 * 64-bit math, since ACPI integers are 64-bit). Normally libgcc.a
 * supplies these, but plenty of freestanding/OS-dev setups don't have
 * a 32-bit libgcc handy (a 64-bit-only host toolchain, a stripped
 * cross toolchain, etc). Defining them here removes that dependency
 * entirely - the linker resolves calls to these names from this file
 * instead of ever reaching for -lgcc.
 *
 * These use the exact names/signatures/calling convention libgcc uses
 * on i386, so they're a transparent drop-in either way: if a real
 * libgcc is present too, the linker just uses these first since this
 * object comes before it. Implementation is plain bit-by-bit binary
 * long division - not fast, but this only ever runs on ACPI table/
 * battery-sized numbers, so that's fine. */

static uint64_t udivmod64(uint64_t num, uint64_t den, uint64_t* rem_out) {
    if (den == 0) {
        if (rem_out) *rem_out = 0;
        return 0;
    }
    uint64_t quotient = 0;
    uint64_t remainder = 0;
    for (int i = 63; i >= 0; i--) {
        remainder = (remainder << 1) | ((num >> i) & 1u);
        if (remainder >= den) {
            remainder -= den;
            quotient |= ((uint64_t)1 << i);
        }
    }
    if (rem_out) *rem_out = remainder;
    return quotient;
}

uint64_t __udivmoddi4(uint64_t num, uint64_t den, uint64_t* rem_out) {
    return udivmod64(num, den, rem_out);
}

uint64_t __udivdi3(uint64_t num, uint64_t den) {
    return udivmod64(num, den, 0);
}

uint64_t __umoddi3(uint64_t num, uint64_t den) {
    uint64_t rem;
    udivmod64(num, den, &rem);
    return rem;
}

static uint64_t abs64(int64_t v) {
    /* Casting a negative signed value to unsigned is well-defined (it
     * wraps mod 2^64 to the two's-complement bit pattern); negating
     * that as unsigned then correctly yields the magnitude without
     * ever forming -INT64_MIN as a signed value, which would overflow. */
    uint64_t u = (uint64_t)v;
    return (v < 0) ? (uint64_t)0 - u : u;
}

int64_t __divdi3(int64_t a, int64_t b) {
    int neg = (a < 0) != (b < 0);
    uint64_t q = udivmod64(abs64(a), abs64(b), 0);
    return neg ? -(int64_t)q : (int64_t)q;
}

int64_t __moddi3(int64_t a, int64_t b) {
    uint64_t rem;
    udivmod64(abs64(a), abs64(b), &rem);
    return (a < 0) ? -(int64_t)rem : (int64_t)rem;
}
