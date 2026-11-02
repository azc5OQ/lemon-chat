#ifndef GUI_CLIB_STDLIB_H
#define GUI_CLIB_STDLIB_H

// Configuration (define consistently for stdlib.c and all its callers):
// STDLIB_ENABLE_ASSEMBLY_OPTIMIZATIONS=0 (default): no inline assembly.
//   Set to 1 to allow supported compiler/architecture-specific assembly paths.
//   This numeric switch uses #if: defining it to 0 keeps assembly disabled.
//   On the Windows/MSVC ABI, 1 also supplies local stack probes for Clang
//   i386/AMD64/ARM64 and MSVC i386. ARM64EC is excluded (different helper ABI).
//   Other targets (or 0) use the host's stack probes
//   when the compiler emits them. No separate assembly source is required.
// STDLIB_USE_HARDWARE_SQRT=1 (default): use SSE2/AArch64 sqrt instructions
//   when supported; assembly paths additionally require the assembly switch.
//   Set to 0 to force portable C math.
// STDLIB_USE_SIMD=1 (default): use available SSE2/NEON for bounded memory
//   operations; 0 forces the portable implementation. No runtime CPU probing.
// STDLIB_DEFINE_FLOAT_MARKER=1 (default): provide the Windows/MSVC-ABI floating
//   point link marker in stdlib.c. Set to 0 when the host supplies it. Ignored
//   on other ABIs; the marker does not initialize the floating-point environment.
// STDLIB_DEFINE_MEMORY_HELPERS=0 (default): leave compiler-generated memory
//   entry points to the host. Set to 1 for a standalone link: stdlib.c supplies
//   memcpy/memmove/memset using the same local implementations as stdlib__*.
//   These entry points perform no allocation or operating-system calls.
// CPU/compiler detection: ../architecture.h (ARCHITECTURE_* presence macros).
// Intrinsics have their own SIMD/sqrt switches; the assembly switch controls
// handwritten assembly only. Compiler code generation still follows its target
// flags. Do not manually define CPU capability macros.
// Private implementation macros are not supported build options.
// Use C11, IEEE-754 binary32/binary64 and normal floating-point semantics.
// Do not enable fast-math (/fp:fast): it invalidates NaN/signed-zero handling.
// Local math does not use errno or call the OS. Integer parsing saturates
// on overflow without errno. Decimal parsing and formatting have the explicit
// contracts documented below; this is not a complete locale-aware libc.

#ifndef STDLIB_ENABLE_ASSEMBLY_OPTIMIZATIONS
#define STDLIB_ENABLE_ASSEMBLY_OPTIMIZATIONS 0
#endif
#ifndef STDLIB_USE_HARDWARE_SQRT
#define STDLIB_USE_HARDWARE_SQRT 1
#endif
#ifndef STDLIB_USE_SIMD
#define STDLIB_USE_SIMD 1
#endif
#ifndef STDLIB_DEFINE_FLOAT_MARKER
#define STDLIB_DEFINE_FLOAT_MARKER 1
#endif
#ifndef STDLIB_DEFINE_MEMORY_HELPERS
#define STDLIB_DEFINE_MEMORY_HELPERS 0
#endif
#if (STDLIB_ENABLE_ASSEMBLY_OPTIMIZATIONS != 0 && STDLIB_ENABLE_ASSEMBLY_OPTIMIZATIONS != 1) || (STDLIB_USE_HARDWARE_SQRT != 0 && STDLIB_USE_HARDWARE_SQRT != 1) || (STDLIB_USE_SIMD != 0 && STDLIB_USE_SIMD != 1) || (STDLIB_DEFINE_FLOAT_MARKER != 0 && STDLIB_DEFINE_FLOAT_MARKER != 1) || (STDLIB_DEFINE_MEMORY_HELPERS != 0 && STDLIB_DEFINE_MEMORY_HELPERS != 1)
#error "stdlib feature switches must be 0 or 1"
#endif

#include "../types.h"
#include <stdarg.h>

// Unsigned magnitude also represents abs(INT_MIN), without signed overflow.
static inline uint stdlib__abs(int value)
{
    return value < 0 ? 0u - (uint)value : (uint)value;
}

// Reverse byte order, independent of the host's endianness.
static inline uint stdlib__bswap32(uint value)
{
    return (value >> 24) | ((value >> 8) & 0x0000ff00u) |
           ((value << 8) & 0x00ff0000u) | (value << 24);
}

static inline uint64 stdlib__bswap64(uint64 value)
{
    return ((uint64)stdlib__bswap32((uint)value) << 32) |
           stdlib__bswap32((uint)(value >> 32));
}

// ===== integer division ===================================================
// Same operand restrictions as C / and %: nonzero denominator, and a signed
// quotient representable in int64. Signed quotients truncate toward zero;
// signed remainders have the numerator's sign. A NULL remainder is allowed.
// 32-bit targets use the local C algorithm instead of a compiler helper.
uint64 stdlib__udivmod64(uint64 numerator, uint64 denominator, uint64* remainder);
int64 stdlib__divmod64(int64 numerator, int64 denominator, int64* remainder);

static inline uint64 stdlib__udiv64(uint64 numerator, uint64 denominator)
{
#if defined(ARCHITECTURE_NATIVE_DIV64)
    return numerator / denominator;
#else
    return stdlib__udivmod64(numerator, denominator, NULL_POINTER);
#endif
}
static inline uint64 stdlib__urem64(uint64 numerator, uint64 denominator)
{
#if defined(ARCHITECTURE_NATIVE_DIV64)
    return numerator % denominator;
#else
    uint64 remainder;
    stdlib__udivmod64(numerator, denominator, &remainder);
    return remainder;
#endif
}
static inline int64 stdlib__div64(int64 numerator, int64 denominator)
{
#if defined(ARCHITECTURE_NATIVE_DIV64)
    return numerator / denominator;
#else
    return stdlib__divmod64(numerator, denominator, NULL_POINTER);
#endif
}
static inline int64 stdlib__rem64(int64 numerator, int64 denominator)
{
#if defined(ARCHITECTURE_NATIVE_DIV64)
    return numerator % denominator;
#else
    int64 remainder;
    stdlib__divmod64(numerator, denominator, &remainder);
    return remainder;
#endif
}

// Project-local subset of libc. Lengths use int64; actual buffers must fit
// native address space. Allocation, file I/O and threading belong to the host.
// ===== memory =============================================================

void* stdlib__memcpy(void* dst, const void* src, int64 count);

void* stdlib__memset(void* dst, int value, int64 count);

// stdlib__null_memory zeroes `count` bytes: the call every function makes
// on its buffers and structs right after declaring them (the lemon-chat
// idiom, clib__null_memory there), so a reader sees what the function
// uses before it is used.
void  stdlib__null_memory(void* dst, int64 count);

void* stdlib__memmove(void* dst, const void* src, int64 count);

int   stdlib__memcmp(const void* a, const void* b, int64 count);

// ===== strings ============================================================

int64       stdlib__strlen(const char* s);

int         stdlib__strcmp(const char* a, const char* b);

int         stdlib__strncmp(const char* a, const char* b, int64 n);

const char* stdlib__strstr(const char* haystack, const char* needle);

const char* stdlib__strchr(const char* s, int ch);

// The last occurrence instead of the first, the way strrchr works.
const char* stdlib__strrchr(const char* s, int ch);

// The first `ch` within `count` bytes, or NULL_POINTER.
const void* stdlib__memchr(const void* block, int ch, int64 count);

// In-place, allocation-free heapsort; O(n log n), constant stack usage.
void stdlib__qsort(void* base, nuint count, nuint size, int (*compare)(const void*, const void*));

// strncpy without its two surprises: the result is always terminated and
// the remainder of the destination is not padded. Returns the length
// written, which is at most dst_cap - 1.
int64       stdlib__strncpy_safe(char* dst, int64 dst_cap, const char* src, int64 count);

int64       stdlib__strcpy_safe(char* dst, int64 dst_cap, const char* src);

int64       stdlib__strcat_safe(char* dst, int64 dst_cap, const char* src);

int64       stdlib__i64toa(char* dst, int64 dst_cap, int64 value);
// Lower-case hexadecimal, no prefix and no padding. Returns the length
// written, or 0 when the buffer is too small for the digits plus a
// terminator.
int64       stdlib__u64toa_hex(char* dst, int64 dst_cap, uint64 value);

// Standalone, locale-independent formatting: %s/%c/%d/%i/%u/%o/%x/%X/%p,
// %f/%F and %%, flags, widths, precisions, h/hh/l/ll modifiers. Project-size
// modifiers: j consumes int64/uint64, z/t consume nint/nuint. Width/precision
// are capped at INT_MAX. Unsupported conversions print literally; %e/%g/%a,
// wide strings, locale and positional arguments are not implemented.
// Fixed-point float conversion rounds the exact binary64 value to nearest-even
// without allocation, including subnormals, large finite values and signed zero.
// Positive capacity NUL-terminates; return the required length excluding NUL.
// NULL dst/nonpositive capacity counts only. Output lengths must fit int64.
int64       stdlib__vformat(char* dst, int64 dst_cap, const char* fmt, va_list args);
int64       stdlib__format(char* dst, int64 dst_cap, const char* fmt, ...);
// Standalone code has no process environment; always returns NULL_POINTER.
const char* stdlib__getenv(const char* name);

// ===== char classifiers ===================================================
// ASCII-only. The gui parses CSS-shaped style files and XML-shaped
// UI files; both specs restrict structure bytes to ASCII, so full
// locale-aware ctype.h is overkill.

int stdlib__isspace(int c);

int stdlib__isdigit(int c);

int stdlib__isxdigit(int c);

int stdlib__isalpha(int c);

int stdlib__isalnum(int c);

int stdlib__tolower(int c);

int stdlib__toupper(int c);

// ===== number parsing =====================================================

// ASCII integer parsing saturates at the result type limits; no errno.
// Base is 0 (auto) or 2..36. No conversion leaves endptr at the input.
int  stdlib__atoi(const char* s);

long stdlib__strtol(const char* s, char** endptr, int base);

int64 stdlib__strtoll(const char* s, char** endptr, int base);

// Decimal-only: whitespace, sign, fraction, e/E exponent. No hex/NaN/Inf
// spelling or locale. Linear scan, bounded exponent work, approximate binary64
// conversion (not guaranteed correctly rounded); strtof casts that result.
double stdlib__strtod(const char* s, char** endptr);

float  stdlib__strtof(const char* s, char** endptr);

double stdlib__atof(const char* s);

// ===== math ===============================================================
// Local math includes range reduction, tables and IEEE special cases.
// Optional hardware sqrt is selected at compile time in stdlib_math.h.
float  stdlib__sqrtf (float x);
float  stdlib__fabsf (float x);
float  stdlib__floorf(float x);
float  stdlib__ceilf (float x);
float  stdlib__fmodf (float x, float y);
double stdlib__sqrt (double x);
double stdlib__fabs (double x);
double stdlib__floor(double x);
double stdlib__ceil (double x);
double stdlib__fmod (double x, double y);


// The numeric operand wins over a NaN. Signed-zero ties preserve -0 for min
// and +0 for max, independent of operand order.
static inline float stdlib__fminf(float x, float y)
{
    if (x != x) return y;
    if (y != y) return x;
    if (x == 0.0f && y == 0.0f) {
        union { float f; uint u; } a = {x}, b = {y};
        a.u |= b.u; return a.f;
    }
    return x < y ? x : y;
}
static inline float stdlib__fmaxf(float x, float y)
{
    if (x != x) return y;
    if (y != y) return x;
    if (x == 0.0f && y == 0.0f) {
        union { float f; uint u; } a = {x}, b = {y};
        a.u &= b.u; return a.f;
    }
    return x > y ? x : y;
}

// Local transcendental functions.
float  stdlib__sinf (float x);
float  stdlib__cosf (float x);
float  stdlib__tanf (float x);
float stdlib__atanf(float x);
float stdlib__atan2f(float y, float x);
float  stdlib__powf (float base, float exponent);
float  stdlib__expf (float x);
float  stdlib__logf (float x);
float  stdlib__acosf(float x);

// Binary64 routines for numerical applications. Implemented in stdlib_double.c.
// No OS, allocator, errno, or system libm. These are approximate, not guaranteed
// correctly rounded; keep normal IEEE semantics (no fast-math).
// exp: finite real arguments, with IEEE overflow/underflow and NaN/Inf handling.
// log: positive real arguments; 0 -> -Inf, negative -> NaN, +Inf -> +Inf.
// pow_positive: positive finite base, finite exponent; other domains -> NaN.
// cos_bounded: finite argument in [-2*pi, 2*pi]; outside -> NaN. This deliberately
// does not claim a full-range trigonometric argument-reduction implementation.
double stdlib__exp(double x);
double stdlib__log(double x);
double stdlib__pow_positive(double x, double exponent);
double stdlib__cos_bounded(double x);
double stdlib__hypot(double x, double y);
double stdlib__copysign(double magnitude, double sign);
int    stdlib__isfinite(double x);
double stdlib__nan(void);

#endif
