/* Adopted from meek-ui/src/clib. Numerical kernels retain their original notices.
 * Only clib__ symbols are exported; no libc interposition or imported memory manager.
 * See clib-math.LICENSE for the numerical kernel licenses. */
#include "clib.h"
#include <limits.h>
static int64 clib__vformat(char *destination, int64 capacity, const char *format, va_list arguments);
static int64 clib__strncpy_safe(char *destination, int64 capacity, const char *source, int64 count);

#define CLIB_ENABLE_ASSEMBLY_OPTIMIZATIONS 0
#define CLIB_USE_HARDWARE_SQRT 1
#define CLIB_USE_SIMD 1

#ifndef MEEK_ARCHITECTURE_H
#define MEEK_ARCHITECTURE_H

// Target detection lives here. These are detected presence macros, not user
// options: select the CPU/ISA through the compiler's target flags. Exactly one
// architecture below is selected; unknown CPUs retain the portable C path.
// Pointer width is independent of ISA (x32, MIPS n32, etc.); types.h uses
// the compiler ABI rather than inferring pointer width from the CPU name.
#if defined(__aarch64__) || defined(_M_ARM64) || defined(_M_ARM64EC)
#ifndef ARCHITECTURE_ARM64
#define ARCHITECTURE_ARM64
#endif
#elif defined(__arm__) || defined(_M_ARM)
#ifndef ARCHITECTURE_ARM32
#define ARCHITECTURE_ARM32
#endif
#elif defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
#ifndef ARCHITECTURE_AMD64
#define ARCHITECTURE_AMD64
#endif
#elif defined(__i386__) || defined(_M_IX86)
#ifndef ARCHITECTURE_I386
#define ARCHITECTURE_I386
#endif
#elif defined(__mips64) || defined(__mips64__)
#ifndef ARCHITECTURE_MIPS64
#define ARCHITECTURE_MIPS64
#endif
#elif defined(__mips__) || defined(__mips) || defined(_M_MRX000)
#ifndef ARCHITECTURE_MIPS32
#define ARCHITECTURE_MIPS32
#endif
#elif defined(__sparc__) && (defined(__sparcv9) || defined(__sparc_v9__) || defined(__arch64__))
#ifndef ARCHITECTURE_SPARC64
#define ARCHITECTURE_SPARC64
#endif
#elif defined(__sparc__)
#ifndef ARCHITECTURE_SPARC32
#define ARCHITECTURE_SPARC32
#endif
#elif defined(__riscv) && __riscv_xlen == 64
#ifndef ARCHITECTURE_RISCV64
#define ARCHITECTURE_RISCV64
#endif
#elif defined(__riscv) && __riscv_xlen == 32
#ifndef ARCHITECTURE_RISCV32
#define ARCHITECTURE_RISCV32
#endif
#elif defined(__powerpc64__) || defined(__ppc64__)
#ifndef ARCHITECTURE_POWERPC64
#define ARCHITECTURE_POWERPC64
#endif
#elif defined(__powerpc__) || defined(__ppc__) || defined(_M_PPC)
#ifndef ARCHITECTURE_POWERPC32
#define ARCHITECTURE_POWERPC32
#endif
#else
#ifndef ARCHITECTURE_GENERIC
#define ARCHITECTURE_GENERIC
#endif
#endif

// Native address width comes from the target ABI, including 16-bit targets
// and ILP32 on otherwise 64-bit CPUs. Unsupported widths fail explicitly.
#if defined(__SIZEOF_POINTER__)
#define ARCHITECTURE_POINTER_BYTES __SIZEOF_POINTER__
#elif defined(_WIN64)
#define ARCHITECTURE_POINTER_BYTES 8
#elif defined(_WIN32)
#define ARCHITECTURE_POINTER_BYTES 4
#else
#error "Compiler must expose pointer width in architecture.h"
#endif
#if ARCHITECTURE_POINTER_BYTES != 2 && ARCHITECTURE_POINTER_BYTES != 4 && ARCHITECTURE_POINTER_BYTES != 8
#error "Supported native pointer widths are 2, 4 and 8 bytes"
#endif

// Compiler identity and ABI are separate: clang-cl uses the MSVC ABI.
#if defined(__clang__)
#define COMPILER_CLANG
#elif defined(_MSC_VER)
#define COMPILER_MSVC
#elif defined(__GNUC__)
#define COMPILER_GCC
#endif
#if defined(_MSC_VER)
#define COMPILER_MSVC_ABI
#endif
// ARM64EC shares ARM64 instructions but uses different stack-helper symbols.
#if defined(__arm64ec__) || defined(_M_ARM64EC)
#define COMPILER_ARM64EC_ABI
#endif

// Optional ISA capabilities. ARM32 does not imply NEON; AMD64 implies SSE2
// with the MSVC ABI, while GCC/Clang explicitly advertise enabled features.
#if (defined(ARCHITECTURE_AMD64) || defined(ARCHITECTURE_I386)) && (defined(__SSE2__) || (defined(COMPILER_MSVC) && (defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))))
#ifndef ARCHITECTURE_SSE2
#define ARCHITECTURE_SSE2
#endif
#endif
#if (defined(ARCHITECTURE_ARM32) || defined(ARCHITECTURE_ARM64)) && (defined(__ARM_NEON) || defined(__ARM_NEON__)) && (defined(COMPILER_GCC) || defined(COMPILER_CLANG))
#ifndef ARCHITECTURE_NEON
#define ARCHITECTURE_NEON
#endif
#endif
#if defined(ARCHITECTURE_ARM64) && defined(__ARM_FP) && (defined(COMPILER_GCC) || defined(COMPILER_CLANG))
#ifndef ARCHITECTURE_ARM64_SQRT_ASM
#define ARCHITECTURE_ARM64_SQRT_ASM
#endif
#endif

// Enables bounded binary64 arithmetic shortcuts; avoid them on soft-float CPUs.
#if defined(ARCHITECTURE_SSE2) || (defined(__ARM_FP) && (__ARM_FP & 8)) || (defined(__riscv_flen) && __riscv_flen >= 64)
#define ARCHITECTURE_FAST_DOUBLE
#endif

// Integer division capabilities. RISC-V can omit the divide extension and
// ARM32 can omit hardware division, so do not infer those from pointer width.
#if defined(ARCHITECTURE_AMD64) || defined(ARCHITECTURE_ARM64) || defined(ARCHITECTURE_MIPS64) || defined(ARCHITECTURE_SPARC64) || defined(ARCHITECTURE_POWERPC64) || (defined(ARCHITECTURE_RISCV64) && defined(__riscv_div))
#define ARCHITECTURE_NATIVE_DIV64
#endif
#if defined(ARCHITECTURE_NATIVE_DIV64) || defined(ARCHITECTURE_I386) || defined(ARCHITECTURE_MIPS32) || defined(ARCHITECTURE_POWERPC32) || (defined(ARCHITECTURE_ARM32) && defined(__ARM_FEATURE_IDIV)) || (defined(ARCHITECTURE_RISCV32) && defined(__riscv_div))
#define ARCHITECTURE_NATIVE_DIV32
#endif

#endif

/* Local byte/ASCII routines. Raw pointers must refer to the stated extent or a terminated string.
 * Allocation, files, networking, environment and threads remain host responsibilities.
 * Formatting supports integers, strings, characters, pointers and fixed-point %f; not %g/%e/%a.
 */
uint clib__abs(int value)
{
    return value < 0 ? 0u - (uint)value : (uint)value;
}
uint clib__bswap32(uint value)
{
    return (value >> 24) | ((value >> 8) & 0x0000ff00u) | ((value << 8) & 0x00ff0000u) | (value << 24);
}
uint64 clib__bswap64(uint64 value)
{
    return ((uint64)clib__bswap32((uint)value) << 32) | clib__bswap32((uint)(value >> 32));
}
uint64 clib__udiv64(uint64 numerator, uint64 denominator)
{
#if defined(ARCHITECTURE_NATIVE_DIV64)
    return numerator / denominator;
#else
    return clib__udivmod64(numerator, denominator, NULL_POINTER);
#endif
}
uint64 clib__urem64(uint64 numerator, uint64 denominator)
{
#if defined(ARCHITECTURE_NATIVE_DIV64)
    return numerator % denominator;
#else
    uint64 remainder;
    clib__udivmod64(numerator, denominator, &remainder);
    return remainder;
#endif
}
int64 clib__div64(int64 numerator, int64 denominator)
{
#if defined(ARCHITECTURE_NATIVE_DIV64)
    return numerator / denominator;
#else
    return clib__divmod64(numerator, denominator, NULL_POINTER);
#endif
}
int64 clib__rem64(int64 numerator, int64 denominator)
{
#if defined(ARCHITECTURE_NATIVE_DIV64)
    return numerator % denominator;
#else
    int64 remainder;
    clib__divmod64(numerator, denominator, &remainder);
    return remainder;
#endif
}
float clib__fminf(float x, float y)
{
    if (x != x)
    {
        return y;
    }
    if (y != y)
    {
        return x;
    }
    if (x == 0.0f && y == 0.0f)
    {
        union
        {
            float f;
            uint u;
        } a = { x }, b = { y };
        a.u |= b.u;
        return a.f;
    }
    return x < y ? x : y;
}
float clib__fmaxf(float x, float y)
{
    if (x != x)
    {
        return y;
    }
    if (y != y)
    {
        return x;
    }
    if (x == 0.0f && y == 0.0f)
    {
        union
        {
            float f;
            uint u;
        } a = { x }, b = { y };
        a.u &= b.u;
        return a.f;
    }
    return x > y ? x : y;
}
// Private implementation, included by stdlib.c. Never read outside the caller's
// byte count, and keep optimized loops local instead of becoming external calls.
#if defined(COMPILER_CLANG)
#define CLIB_NO_LIBCALL __attribute__((no_builtin))
#elif defined(COMPILER_GCC)
#define CLIB_NO_LIBCALL __attribute__((optimize("no-tree-loop-distribute-patterns")))
#else
#define CLIB_NO_LIBCALL
#endif

#if CLIB_USE_SIMD && defined(ARCHITECTURE_SSE2)
#include <emmintrin.h>
typedef __m128i _clib_internal_vector;
#define CLIB_VECTOR_LOAD(p) _mm_loadu_si128((const __m128i *)(p))
#define CLIB_VECTOR_STORE(p, v) _mm_storeu_si128((__m128i *)(p), (v))
#define CLIB_VECTOR_FILL(v) _mm_set1_epi8((char)(v))
#define CLIB_HAS_VECTOR 1
#elif CLIB_USE_SIMD && defined(ARCHITECTURE_NEON)
#include <arm_neon.h>
typedef uint8x16_t _clib_internal_vector;
#define CLIB_VECTOR_LOAD(p) vld1q_u8((const ubyte *)(p))
#define CLIB_VECTOR_STORE(p, v) vst1q_u8((ubyte *)(p), (v))
#define CLIB_VECTOR_FILL(v) vdupq_n_u8((ubyte)(v))
#define CLIB_HAS_VECTOR 1
#else
#define CLIB_HAS_VECTOR 0
#endif

static CLIB_NO_LIBCALL void *clib__copy_bytes(void *dst, const void *src, nuint n)
{
    ubyte *d = (ubyte *)dst;
    const ubyte *s = (const ubyte *)src;
    if (!d || !s || !n)
    {
        return dst;
    }
#if CLIB_HAS_VECTOR
    while (n >= 64)
    {
        _clib_internal_vector a = CLIB_VECTOR_LOAD(s);
        _clib_internal_vector b = CLIB_VECTOR_LOAD(s + 16);
        _clib_internal_vector c = CLIB_VECTOR_LOAD(s + 32);
        _clib_internal_vector e = CLIB_VECTOR_LOAD(s + 48);
        CLIB_VECTOR_STORE(d, a);
        CLIB_VECTOR_STORE(d + 16, b);
        CLIB_VECTOR_STORE(d + 32, c);
        CLIB_VECTOR_STORE(d + 48, e);
        s += 64;
        d += 64;
        n -= 64;
    }
    while (n >= 16)
    {
        CLIB_VECTOR_STORE(d, CLIB_VECTOR_LOAD(s));
        s += 16;
        d += 16;
        n -= 16;
    }
#endif
    // MSVC has no per-function no_builtin equivalent. Volatile byte accesses
    // keep its scalar fallback from being rewritten to an external call.
#if defined(COMPILER_MSVC)
    volatile ubyte *tail = d;
    while (n--)
    {
        *tail++ = *s++;
    }
#else
    while (n--)
    {
        *d++ = *s++;
    }
#endif
    return dst;
}

static CLIB_NO_LIBCALL void *clib__fill_bytes(void *dst, int value, nuint n)
{
    ubyte *d = (ubyte *)dst;
    if (!d || !n)
    {
        return dst;
    }
#if CLIB_HAS_VECTOR
    _clib_internal_vector v = CLIB_VECTOR_FILL(value);
    while (n >= 64)
    {
        CLIB_VECTOR_STORE(d, v);
        CLIB_VECTOR_STORE(d + 16, v);
        CLIB_VECTOR_STORE(d + 32, v);
        CLIB_VECTOR_STORE(d + 48, v);
        d += 64;
        n -= 64;
    }
    while (n >= 16)
    {
        CLIB_VECTOR_STORE(d, v);
        d += 16;
        n -= 16;
    }
#endif
#if defined(COMPILER_MSVC)
    volatile ubyte *tail = d;
    while (n--)
    {
        *tail++ = (ubyte)value;
    }
#else
    while (n--)
    {
        *d++ = (ubyte)value;
    }
#endif
    return dst;
}

static CLIB_NO_LIBCALL void *clib__move_bytes(void *dst, const void *src, nuint n)
{
    ubyte *d = (ubyte *)dst;
    const ubyte *s = (const ubyte *)src;
    if (!d || !s || d == s || !n)
    {
        return dst;
    }
    // Integer addresses avoid relational comparisons of unrelated pointers.
    if ((nuint)d < (nuint)s || (nuint)d - (nuint)s >= n)
    {
        return clib__copy_bytes(dst, src, n);
    }
    d += n;
    s += n;
#if CLIB_HAS_VECTOR
    while (n >= 16)
    {
        d -= 16;
        s -= 16;
        CLIB_VECTOR_STORE(d, CLIB_VECTOR_LOAD(s));
        n -= 16;
    }
#endif
    while (n--)
    {
        *--d = *--s;
    }
    return dst;
}

CLIB_NO_LIBCALL void *clib__memcpy(void *dst, const void *src, int64 count)
{
    return count <= 0 ? dst : clib__copy_bytes(dst, src, (nuint)count);
}

CLIB_NO_LIBCALL void *clib__memset(void *dst, int value, int64 count)
{
    return count <= 0 ? dst : clib__fill_bytes(dst, value, (nuint)count);
}

CLIB_NO_LIBCALL void *clib__memmove(void *dst, const void *src, int64 count)
{
    return count <= 0 ? dst : clib__move_bytes(dst, src, (nuint)count);
}

uint64 clib__udivmod64(uint64 numerator, uint64 denominator, uint64 *remainder)
{
#if defined(ARCHITECTURE_NATIVE_DIV64)
    uint64 quotient = numerator / denominator;
    if (remainder)
    {
        *remainder = numerator % denominator;
    }
    return quotient;
#else
    uint64 quotient = 0;
    uint64 bit = 1;
    uint64 divisor = denominator;
    // Zero is outside the API contract; keep the normalization loop bounded
    // even for that invalid input. No platform-specific trap is required.
    if (!denominator || numerator < denominator)
    {
        if (remainder)
        {
            *remainder = numerator;
        }
        return 0;
    }
    if ((denominator & (denominator - 1)) == 0)
    {
        // Common row widths and binary units need only shifts and a mask.
        // All shift counts are constants, including on narrow targets.
        quotient = numerator;
        if (divisor >> 32)
        {
            divisor >>= 32;
            quotient >>= 32;
        }
        if (divisor >> 16)
        {
            divisor >>= 16;
            quotient >>= 16;
        }
        if (divisor >> 8)
        {
            divisor >>= 8;
            quotient >>= 8;
        }
        if (divisor >> 4)
        {
            divisor >>= 4;
            quotient >>= 4;
        }
        if (divisor >> 2)
        {
            divisor >>= 2;
            quotient >>= 2;
        }
        if (divisor >> 1)
        {
            quotient >>= 1;
        }
        if (remainder)
        {
            *remainder = numerator & (denominator - 1);
        }
        return quotient;
    }
#if defined(ARCHITECTURE_NATIVE_DIV32)
    if ((numerator >> 32) == 0)
    {
        uint n = (uint)numerator, d = (uint)denominator;
        quotient = n / d;
        if (remainder)
        {
            *remainder = n % d;
        }
        return quotient;
    }
#endif
    // Align only as far as needed. divisor <= numerator/2 prevents overflow;
    // subtracting descending multiples gives quotient and remainder together.
    while (divisor <= (numerator >> 1))
    {
        divisor <<= 1;
        bit <<= 1;
    }
    while (bit)
    {
        if (numerator >= divisor)
        {
            numerator -= divisor;
            quotient |= bit;
        }
        divisor >>= 1;
        bit >>= 1;
    }
    if (remainder)
    {
        *remainder = numerator;
    }
    return quotient;
#endif
}

int64 clib__divmod64(int64 numerator, int64 denominator, int64 *remainder)
{
#if defined(ARCHITECTURE_NATIVE_DIV64)
    int64 quotient = numerator / denominator;
    if (remainder)
    {
        *remainder = numerator % denominator;
    }
    return quotient;
#else
    // Unsigned magnitudes also represent INT64_MIN without signed overflow.
    uint64 n = numerator < 0 ? 0ULL - (uint64)numerator : (uint64)numerator;
    uint64 d = denominator < 0 ? 0ULL - (uint64)denominator : (uint64)denominator;
    uint64 r;
    uint64 q = clib__udivmod64(n, d, remainder ? &r : NULL_POINTER);
    if (remainder)
    {
        *remainder = numerator < 0 && r ? -(int64)(r - 1) - 1 : (int64)r;
    }
    return (numerator < 0) != (denominator < 0) && q ? -(int64)(q - 1) - 1 : (int64)q;
#endif
}

// ===== memory =============================================================

/**
 * @brief zero `count` bytes of `dst`. The call a function makes on its buffers and structs right after declaring them.
 *
 * @param void* dst -> destination buffer; NULL is a no-op.
 * @param int64 count -> bytes to clear.
 *
 * @return void
 */
void clib__null_memory(void *dst, int64 count)
{
    if (dst)
    {
        clib__memset(dst, 0, count);
    }
}

/**
 * @brief byte-wise compare `count` bytes. Returns 0 on equality, negative if first differing byte in `a` is less than the one in `b`,
 * positive otherwise. Matches memcmp.
 *
 * @param const void* a
 * @param const void* b
 * @param int64 count
 *
 * @return int -> void
 */
int clib__memcmp(const void *a, const void *b, int64 count)
{
    if (count <= 0)
    {
        return 0;
    }
    if (a == b)
    {
        return 0;
    }
    if (a == NULL_POINTER)
    {
        return -1;
    }
    if (b == NULL_POINTER)
    {
        return 1;
    }
    const ubyte *pa = (const ubyte *)a;
    const ubyte *pb = (const ubyte *)b;
    while (count-- > 0)
    {
        int diff = (int)*pa - (int)*pb;
        if (diff != 0)
        {
            return diff;
        }
        pa++;
        pb++;
    }
    return 0;
}

// ===== strings ============================================================

static void _clib_internal_sort_swap(ubyte *a, ubyte *b, nuint size)
{
    while (size--)
    {
        ubyte v = *a;
        *a++ = *b;
        *b++ = v;
    }
}

static void _clib_internal_sort_sift(ubyte *base, nuint parent, nuint count, nuint size, int (*compare)(const void *, const void *))
{
    // parent < count/2 also keeps parent*2+1 from overflowing.
    while (parent < count / 2)
    {
        nuint child = parent * 2 + 1;
        if (child + 1 < count && compare(base + child * size, base + (child + 1) * size) < 0)
        {
            child++;
        }
        if (compare(base + parent * size, base + child * size) >= 0)
        {
            break;
        }
        _clib_internal_sort_swap(base + parent * size, base + child * size, size);
        parent = child;
    }
}

void clib__qsort(void *base, nuint count, nuint size, int (*compare)(const void *, const void *))
{
    if (!base || !compare || !size || count < 2 || count > (nuint)-1 / size)
    {
        return;
    }
    ubyte *bytes = (ubyte *)base;
    for (nuint i = count / 2; i > 0; i--)
    {
        _clib_internal_sort_sift(bytes, i - 1, count, size, compare);
    }
    while (--count)
    {
        _clib_internal_sort_swap(bytes, bytes + count * size, size);
        _clib_internal_sort_sift(bytes, 0, count, size, compare);
    }
}

/**
 * @brief length of a null-terminated C string, NOT counting the terminator. No arbitrary cap (the existing purelib__strlen capped at 1000,
 * which is a silent-truncation hazard this implementation avoids).
 *
 * @param const char* s -> null-terminated string.
 *
 * @return int64 -> byte count before the terminator.
 */
CLIB_NO_LIBCALL int64 clib__strlen(const char *s)
{
    if (s == NULL_POINTER)
    {
        return 0;
    }
    const char *p = s;
    while (*p != 0)
    {
        p++;
    }
    return (int64)(p - s);
}

/**
 * @brief lexicographic compare of two null-terminated strings. Returns 0 on equality, negative if `a < b`, positive if `a > b`. Matches
 * strcmp semantics.
 *
 * @param const char* a
 * @param const char* b
 *
 * @return int -> void
 */
int clib__strcmp(const char *a, const char *b)
{
    if (a == b)
    {
        return 0;
    }
    if (a == NULL_POINTER)
    {
        return -1;
    }
    if (b == NULL_POINTER)
    {
        return 1;
    }
    // Cast to ubyte for the subtract so byte values >= 128
    // don't get sign-extended into a negative when comparing. libc's
    // strcmp does the same; without this, "abc" vs "\x80bc" would
    // wrongly report "abc > ...".
    while (*a != 0 && *a == *b)
    {
        a++;
        b++;
    }
    return (int)(ubyte)*a - (int)(ubyte)*b;
}

/**
 * @brief like strcmp but compares at most `n` bytes. Early-returns 0 if both strings match for their first n bytes (even if longer).
 *
 * @param const char* a
 * @param const char* b
 * @param int64 n
 *
 * @return int -> void
 */
int clib__strncmp(const char *a, const char *b, int64 n)
{
    if (n <= 0)
    {
        return 0;
    }
    if (a == b)
    {
        return 0;
    }
    if (a == NULL_POINTER)
    {
        return -1;
    }
    if (b == NULL_POINTER)
    {
        return 1;
    }
    while (n > 0 && *a != 0 && *a == *b)
    {
        a++;
        b++;
        n--;
    }
    if (n == 0)
    {
        return 0;
    }
    return (int)(ubyte)*a - (int)(ubyte)*b;
}

/**
 * @brief find the first occurrence of `needle` inside `haystack`. Returns a pointer to the start of the match, or NULL. Matches strstr.
 * Both arguments must be null-terminated.
 *
 * @param const char* haystack
 * @param const char* needle
 *
 * @return const char* -> void
 */
const char *clib__strstr(const char *haystack, const char *needle)
{
    if (haystack == NULL_POINTER || needle == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    // Empty needle conventionally matches at position 0 (libc
    // strstr behaves this way -- returns `haystack`).
    if (*needle == 0)
    {
        return haystack;
    }
    // O(n*m) naive scan. For the parser use case (looking up short
    // keywords) this is fine; replace with Boyer-Moore or similar if
    // a caller ever hits perf problems.
    for (const char *p = haystack; *p != 0; p++)
    {
        const char *h = p;
        const char *n = needle;
        while (*n != 0 && *h == *n)
        {
            h++;
            n++;
        }
        if (*n == 0)
        {
            return p;
        }
    }
    return NULL_POINTER;
}

/**
 * @brief find the first occurrence of `ch` in the null-terminated string `s`. Returns a pointer to it or NULL. Matches strchr. The
 * terminator IS matched if you pass `ch = 0`.
 *
 * @param const char* s
 * @param int ch
 *
 * @return const char* -> void
 */
const char *clib__strchr(const char *s, int ch)
{
    if (s == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    char target = (char)ch;
    while (*s != 0)
    {
        if (*s == target)
        {
            return s;
        }
        s++;
    }
    // libc strchr matches the terminator when ch == 0, returning a
    // pointer to the final byte. Match that behaviour.
    if (target == 0)
    {
        return s;
    }
    return NULL_POINTER;
}

/**
 * @brief copy at most `dst_cap` bytes from `src` into `dst`. Always null-terminates `dst` (provided dst_cap >= 1). Truncates if `src`
 * doesn't fit. Returns the number of source bytes that WOULD have been copied (not including the terminator), so callers can detect
 * truncation: `if (written >= dst_cap) { ... }`. This is the strlcpy contract -- safer than strncpy (which doesn't always terminate) and
 * safer than strcpy (no cap).
 *
 * @param char* dst
 * @param int64 dst_cap -> total bytes in dst (including terminator room).
 * @param const char* src
 *
 * @return int64 -> strlen(src) at time of call.
 */
static int64 clib__strcpy_safe(char *dst, int64 dst_cap, const char *src)
{
    // Always returns strlen(src) (so the caller can detect truncation
    // via the result). Even if dst_cap is 0 we still walk src to get
    // that length; fast paths are available if the caller doesn't
    // care about detecting truncation.
    int64 src_len = clib__strlen(src);
    if (dst == NULL_POINTER || dst_cap <= 0)
    {
        return src_len;
    }
    int64 copy_len = src_len;
    if (copy_len > dst_cap - 1)
    {
        copy_len = dst_cap - 1;
    }
    for (int64 i = 0; i < copy_len; i++)
    {
        dst[i] = src[i];
    }
    dst[copy_len] = 0;
    return src_len;
}

// ===== char classifiers ===================================================

/**
 * @brief aSCII whitespace: space, tab, newline, carriage return, form feed, vertical tab. Matches isspace over the 7-bit subset.
 *
 * @param int c -> byte value.
 *
 * @return int -> non-zero if whitespace.
 */
int clib__isspace(int c)
{
    return (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') ? 1 : 0;
}

/**
 * @brief aSCII decimal digit. @function clib__isdigit
 *
 * @return int -> void
 */
int clib__isdigit(int c)
{
    return (c >= '0' && c <= '9') ? 1 : 0;
}

/**
 * @brief aSCII hexadecimal digit (0-9, a-f, A-F). @function clib__isxdigit
 *
 * @return int -> void
 */
int clib__isxdigit(int c)
{
    if (c >= '0' && c <= '9')
    {
        return 1;
    }
    if (c >= 'a' && c <= 'f')
    {
        return 1;
    }
    if (c >= 'A' && c <= 'F')
    {
        return 1;
    }
    return 0;
}

/**
 * @brief aSCII alphabetic letter. @function clib__isalpha
 *
 * @return int -> void
 */
int clib__isalpha(int c)
{
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) ? 1 : 0;
}

/**
 * @brief aSCII alphanumeric. @function clib__isalnum
 *
 * @return int -> void
 */
int clib__isalnum(int c)
{
    return (clib__isalpha(c) || clib__isdigit(c)) ? 1 : 0;
}

/**
 * @brief aSCII lowercase conversion; non-letters pass through. @function clib__tolower
 *
 * @return int -> void
 */
int clib__tolower(int c)
{
    if (c >= 'A' && c <= 'Z')
    {
        return c + ('a' - 'A');
    }
    return c;
}

/**
 * @brief aSCII uppercase conversion; non-letters pass through. @function clib__toupper
 *
 * @return int -> void
 */
int clib__toupper(int c)
{
    if (c >= 'a' && c <= 'z')
    {
        return c - ('a' - 'A');
    }
    return c;
}

// ===== number parsing =====================================================

/**
 * @brief parse a base-10 integer from `s`, consuming leading whitespace + optional sign + contiguous digits. Matches atoi semantics
 * (including "returns 0 on garbage input" — so prefer `clib__strtol` if you need to distinguish "parsed zero" from "couldn't parse").
 *
 * @param const char* s
 *
 * @return int -> void
 */
// Parse using an unsigned magnitude so LONG_MIN/INT64_MIN never require
// overflowing signed arithmetic. Keep consuming digits after saturation.
static int64 _clib_internal__parse_integer(const char *s, char **endptr, int base, uint64 maximum)
{
    const char *p = s;
    uint64 value = 0, limit, cutoff, cutlim;
    int negative = 0, any = 0, overflow = 0;
    if (endptr)
    {
        *endptr = (char *)s;
    }
    if (!s || (base != 0 && (base < 2 || base > 36)))
    {
        return 0;
    }
    while (clib__isspace((ubyte)*p))
    {
        p++;
    }
    if (*p == '-' || *p == '+')
    {
        negative = *p++ == '-';
    }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && clib__isxdigit((ubyte)p[2]))
    {
        p += 2;
        base = 16;
    }
    if (base == 0)
    {
        base = *p == '0' ? 8 : 10;
    }
    limit = maximum + (uint64)negative;
    cutoff = clib__udivmod64(limit, (uint)base, &cutlim);
    for (;; p++)
    {
        int c = (ubyte)*p;
        int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 36;
        if (digit >= base)
        {
            break;
        }
        any = 1;
        if (value > cutoff || (value == cutoff && (uint)digit > cutlim))
        {
            overflow = 1;
        }
        else if (!overflow)
        {
            value = value * (uint)base + (uint)digit;
        }
    }
    if (!any)
    {
        return 0;
    }
    if (endptr)
    {
        *endptr = (char *)p;
    }
    if (overflow)
    {
        value = limit;
    }
    // -(magnitude - 1) - 1 also handles the most negative signed value.
    return negative && value ? -(int64)(value - 1) - 1 : (int64)value;
}

int clib__atoi(const char *s)
{
    return (int)_clib_internal__parse_integer(s, NULL_POINTER, 10, INT_MAX);
}
long clib__strtol(const char *s, char **endptr, int base)
{
    return (long)_clib_internal__parse_integer(s, endptr, base, LONG_MAX);
}
int64 clib__strtoll(const char *s, char **endptr, int base)
{
    return _clib_internal__parse_integer(s, endptr, base, 0x7fffffffffffffffULL);
}

// ===== local math =========================================================
// Local math implementation included only by stdlib.c.
// Adapted from selected musl 1.2.5 numerical kernels; no external library.
// Copyright/license notices are retained below and in clib-math.LICENSE.
// Configuration is documented at the top of stdlib.h. CPU checks use only
// architecture.h's normalized feature macros; all integer types are local.

// Private amalgamation: preserves existing stdlib.c-only integration.
// Names are remapped before inclusion; no host libm is linked.
// Private numeric helpers. See clib-math.LICENSE for source licenses.
#ifndef MEEK_CLIB_MATH_HELPERS_H
#define MEEK_CLIB_MATH_HELPERS_H
#include <float.h>
#if FLT_RADIX != 2 || FLT_MANT_DIG != 24 || DBL_MANT_DIG != 53 || DBL_MAX_EXP != 1024
#error "Local math requires IEEE binary32 float and binary64 double"
#endif
_Static_assert(sizeof(float) == 4 && sizeof(double) == 8, "Unsupported floating point layout");
#define hidden
#define M_PI_2 1.57079632679489661923132169163975144
#if FLT_EVAL_METHOD == 2
typedef long double float_t;
typedef long double double_t;
#elif FLT_EVAL_METHOD == 1
typedef double float_t;
typedef double double_t;
#else
typedef float float_t;
typedef double double_t;
#endif
#define isnan(x) ((x) != (x))
#undef INFINITY
#define INFINITY asfloat(0x7f800000u)
static double _clib_internal_math_scalbn(double, int);
/* Support non-nearest rounding mode.  */
#define WANT_ROUNDING 1
/* Support signaling NaNs.  */
#define WANT_SNAN 0

#if WANT_SNAN
#error SNaN is unsupported
#else
#define issignalingf_inline(x) 0
#define issignaling_inline(x) 0
#endif

#ifndef _CLIB_INTERNAL_TOINT_INTRINSICS
#define _CLIB_INTERNAL_TOINT_INTRINSICS 0
#endif

#if _CLIB_INTERNAL_TOINT_INTRINSICS
/* Round x to nearest int in all rounding modes, ties have to be rounded
   consistently with converttoint so the results match.  If the result
   would be outside of [-2^31, 2^31-1] then the semantics is unspecified.  */
static double_t roundtoint(double_t);

/* Convert x to nearest int in all rounding modes, ties have to be rounded
   consistently with roundtoint.  If the result is not representible in an
   int then the semantics is unspecified.  */
static int converttoint(double_t);
#endif

/* Helps static branch prediction so hot path can be better optimized.  */
#if defined(COMPILER_GCC) || defined(COMPILER_CLANG)
#define predict_true(x) __builtin_expect(!!(x), 1)
#define predict_false(x) __builtin_expect(x, 0)
#else
#define predict_true(x) (x)
#define predict_false(x) (x)
#endif

/* Evaluate an expression as the specified type. With standard excess
   precision handling a type cast or assignment is enough (with
   -ffloat-store an assignment is required, in old compilers argument
   passing and return statement may not drop excess precision).  */

static inline float eval_as_float(float x)
{
    float y = x;
    return y;
}

static inline double eval_as_double(double x)
{
    double y = x;
    return y;
}

/* fp_barrier returns its input, but limits code transformations
   as if it had a side-effect (e.g. observable io) and returned
   an arbitrary value.  */

#ifndef fp_barrierf
#define fp_barrierf fp_barrierf
static inline float fp_barrierf(float x)
{
    volatile float y = x;
    return y;
}
#endif

#ifndef fp_barrier
#define fp_barrier fp_barrier
static inline double fp_barrier(double x)
{
    volatile double y = x;
    return y;
}
#endif

#ifndef fp_barrierl
#define fp_barrierl fp_barrierl
static inline long double fp_barrierl(long double x)
{
    volatile long double y = x;
    return y;
}
#endif

/* fp_force_eval ensures that the input value is computed when that's
   otherwise unused.  To prevent the constant folding of the input
   expression, an additional fp_barrier may be needed or a compilation
   mode that does so (e.g. -frounding-math in gcc). Then it can be
   used to evaluate an expression for its fenv side-effects only.   */

#ifndef fp_force_evalf
#define fp_force_evalf fp_force_evalf
static inline void fp_force_evalf(float x)
{
    volatile float y;
    y = x;
}
#endif

#ifndef fp_force_eval
#define fp_force_eval fp_force_eval
static inline void fp_force_eval(double x)
{
    volatile double y;
    y = x;
}
#endif

#ifndef fp_force_evall
#define fp_force_evall fp_force_evall
static inline void fp_force_evall(long double x)
{
    volatile long double y;
    y = x;
}
#endif

#define FORCE_EVAL(x)                         \
    do                                        \
    {                                         \
        if (sizeof(x) == sizeof(float))       \
        {                                     \
            fp_force_evalf(x);                \
        }                                     \
        else if (sizeof(x) == sizeof(double)) \
        {                                     \
            fp_force_eval(x);                 \
        }                                     \
        else                                  \
        {                                     \
            fp_force_evall(x);                \
        }                                     \
    } while (0)

#define asuint(f) \
    ((union {     \
        float _f; \
        uint _i;  \
    }){ f })      \
        ._i
#define asfloat(i) \
    ((union {      \
        uint _i;   \
        float _f;  \
    }){ i })       \
        ._f
#define asuint64(f) \
    ((union {       \
        double _f;  \
        uint64 _i;  \
    }){ f })        \
        ._i
#define asdouble(i) \
    ((union {       \
        uint64 _i;  \
        double _f;  \
    }){ i })        \
        ._f

#define EXTRACT_WORDS(hi, lo, d)  \
    do                            \
    {                             \
        uint64 __u = asuint64(d); \
        (hi) = __u >> 32;         \
        (lo) = (uint)__u;         \
    } while (0)

#define GET_HIGH_WORD(hi, d)      \
    do                            \
    {                             \
        (hi) = asuint64(d) >> 32; \
    } while (0)

#define GET_LOW_WORD(lo, d)       \
    do                            \
    {                             \
        (lo) = (uint)asuint64(d); \
    } while (0)

#define INSERT_WORDS(d, hi, lo)                            \
    do                                                     \
    {                                                      \
        (d) = asdouble(((uint64)(hi) << 32) | (uint)(lo)); \
    } while (0)

#define SET_HIGH_WORD(d, hi) INSERT_WORDS(d, hi, (uint)asuint64(d))

#define SET_LOW_WORD(d, lo) INSERT_WORDS(d, asuint64(d) >> 32, lo)

#define GET_FLOAT_WORD(w, d) \
    do                       \
    {                        \
        (w) = asuint(d);     \
    } while (0)

#define SET_FLOAT_WORD(d, w) \
    do                       \
    {                        \
        (d) = asfloat(w);    \
    } while (0)

static int _clib_internal_math__rem_pio2_large(double *, double *, int, int, int);

static int _clib_internal_math__rem_pio2f(float, double *);
static float _clib_internal_math__sindf(double);
static float _clib_internal_math__cosdf(double);
static float _clib_internal_math__tandf(double, int);

/* error handling functions */
static float _clib_internal_math__math_xflowf(uint, float);
static float _clib_internal_math__math_uflowf(uint);
static float _clib_internal_math__math_oflowf(uint);
static float _clib_internal_math__math_divzerof(uint);
static float _clib_internal_math__math_invalidf(float);
static double _clib_internal_math__math_invalid(double);
#if LDBL_MANT_DIG != DBL_MANT_DIG
#endif

#endif

/*
 * Shared data between clib__expf, exp2f and clib__powf.
 *
 * Copyright (c) 2017-2018, Arm Limited.
 * SPDX-License-Identifier: MIT
 */

/*
 * Copyright (c) 2017-2018, Arm Limited.
 * SPDX-License-Identifier: MIT
 */
#ifndef _EXP2F_DATA_H
#define _EXP2F_DATA_H

/* Shared between clib__expf, exp2f and clib__powf.  */
#define EXP2F_TABLE_BITS 5
#define EXP2F_POLY_ORDER 3
static const struct exp2f_data
{
    uint64 tab[1 << EXP2F_TABLE_BITS];
    double shift_scaled;
    double poly[EXP2F_POLY_ORDER];
    double shift;
    double invln2_scaled;
    double poly_scaled[EXP2F_POLY_ORDER];
} _clib_internal_math__exp2f_data;

#endif

#define N (1 << EXP2F_TABLE_BITS)

static const struct exp2f_data _clib_internal_math__exp2f_data = {
    /* tab[i] = uint(2^(i/N)) - (i << 52-BITS)
       used for computing 2^(k/N) for an int |k| < 150 N as
       double(tab[k%N] + (k << 52-BITS)) */
    .tab =
        {
            0x3ff0000000000000, 0x3fefd9b0d3158574, 0x3fefb5586cf9890f, 0x3fef9301d0125b51, 0x3fef72b83c7d517b, 0x3fef54873168b9aa,
            0x3fef387a6e756238, 0x3fef1e9df51fdee1, 0x3fef06fe0a31b715, 0x3feef1a7373aa9cb, 0x3feedea64c123422, 0x3feece086061892d,
            0x3feebfdad5362a27, 0x3feeb42b569d4f82, 0x3feeab07dd485429, 0x3feea47eb03a5585, 0x3feea09e667f3bcd, 0x3fee9f75e8ec5f74,
            0x3feea11473eb0187, 0x3feea589994cce13, 0x3feeace5422aa0db, 0x3feeb737b0cdc5e5, 0x3feec49182a3f090, 0x3feed503b23e255d,
            0x3feee89f995ad3ad, 0x3feeff76f2fb5e47, 0x3fef199bdd85529c, 0x3fef3720dcef9069, 0x3fef5818dcfba487, 0x3fef7c97337b9b5f,
            0x3fefa4afa2a490da, 0x3fefd0765b6e4540,
        },
    .shift_scaled = 0x1.8p+52 / N,
    .poly =
        {
            0x1.c6af84b912394p-5,
            0x1.ebfce50fac4f3p-3,
            0x1.62e42ff0c52d6p-1,
        },
    .shift = 0x1.8p+52,
    .invln2_scaled = 0x1.71547652b82fep+0 * N,
    .poly_scaled =
        {
            0x1.c6af84b912394p-5 / N / N / N,
            0x1.ebfce50fac4f3p-3 / N / N,
            0x1.62e42ff0c52d6p-1 / N,
        },
};

#undef N
/*
 * Data definition for clib__powf.
 *
 * Copyright (c) 2017-2018, Arm Limited.
 * SPDX-License-Identifier: MIT
 */

/*
 * Copyright (c) 2017-2018, Arm Limited.
 * SPDX-License-Identifier: MIT
 */
#ifndef _POWF_DATA_H
#define _POWF_DATA_H

#define POWF_LOG2_TABLE_BITS 4
#define POWF_LOG2_POLY_ORDER 5
#if _CLIB_INTERNAL_TOINT_INTRINSICS
#define POWF_SCALE_BITS EXP2F_TABLE_BITS
#else
#define POWF_SCALE_BITS 0
#endif
#define POWF_SCALE ((double)(1 << POWF_SCALE_BITS))
static const struct powf_log2_data
{
    struct
    {
        double invc, logc;
    } tab[1 << POWF_LOG2_TABLE_BITS];
    double poly[POWF_LOG2_POLY_ORDER];
} _clib_internal_math__powf_log2_data;

#endif

static const struct powf_log2_data _clib_internal_math__powf_log2_data = {
    .tab =
        {
            {0x1.661ec79f8f3bep+0, -0x1.efec65b963019p-2 * POWF_SCALE},
            {0x1.571ed4aaf883dp+0, -0x1.b0b6832d4fca4p-2 * POWF_SCALE},
            {0x1.49539f0f010bp+0, -0x1.7418b0a1fb77bp-2 * POWF_SCALE},
            {0x1.3c995b0b80385p+0, -0x1.39de91a6dcf7bp-2 * POWF_SCALE},
            {0x1.30d190c8864a5p+0, -0x1.01d9bf3f2b631p-2 * POWF_SCALE},
            {0x1.25e227b0b8eap+0, -0x1.97c1d1b3b7afp-3 * POWF_SCALE},
            {0x1.1bb4a4a1a343fp+0, -0x1.2f9e393af3c9fp-3 * POWF_SCALE},
            {0x1.12358f08ae5bap+0, -0x1.960cbbf788d5cp-4 * POWF_SCALE},
            {0x1.0953f419900a7p+0, -0x1.a6f9db6475fcep-5 * POWF_SCALE},
            {0x1p+0, 0x0p+0 * POWF_SCALE},
            {0x1.e608cfd9a47acp-1, 0x1.338ca9f24f53dp-4 * POWF_SCALE},
            {0x1.ca4b31f026aap-1, 0x1.476a9543891bap-3 * POWF_SCALE},
            {0x1.b2036576afce6p-1, 0x1.e840b4ac4e4d2p-3 * POWF_SCALE},
            {0x1.9c2d163a1aa2dp-1, 0x1.40645f0c6651cp-2 * POWF_SCALE},
            {0x1.886e6037841edp-1, 0x1.88e9c2c1b9ff8p-2 * POWF_SCALE},
            {0x1.767dcf5534862p-1, 0x1.ce0a44eb17bccp-2 * POWF_SCALE},
        },
    .poly = {
        0x1.27616c9496e0bp-2 * POWF_SCALE,
        -0x1.71969a075c67ap-2 * POWF_SCALE,
        0x1.ec70a6ca7baddp-2 * POWF_SCALE,
        -0x1.7154748bef6c8p-1 * POWF_SCALE,
        0x1.71547652ab82bp0 * POWF_SCALE,
    }};

/*
 * Data definition for clib__logf.
 *
 * Copyright (c) 2017-2018, Arm Limited.
 * SPDX-License-Identifier: MIT
 */

/*
 * Copyright (c) 2017-2018, Arm Limited.
 * SPDX-License-Identifier: MIT
 */
#ifndef _LOGF_DATA_H
#define _LOGF_DATA_H

#define LOGF_TABLE_BITS 4
#define LOGF_POLY_ORDER 4
static const struct logf_data
{
    struct
    {
        double invc, logc;
    } tab[1 << LOGF_TABLE_BITS];
    double ln2;
    double poly[LOGF_POLY_ORDER - 1]; /* First order coefficient is 1.  */
} _clib_internal_math__logf_data;

#endif

static const struct logf_data _clib_internal_math__logf_data = {.tab =
                                                                    {
                                                                        {0x1.661ec79f8f3bep+0, -0x1.57bf7808caadep-2},
                                                                        {0x1.571ed4aaf883dp+0, -0x1.2bef0a7c06ddbp-2},
                                                                        {0x1.49539f0f010bp+0, -0x1.01eae7f513a67p-2},
                                                                        {0x1.3c995b0b80385p+0, -0x1.b31d8a68224e9p-3},
                                                                        {0x1.30d190c8864a5p+0, -0x1.6574f0ac07758p-3},
                                                                        {0x1.25e227b0b8eap+0, -0x1.1aa2bc79c81p-3},
                                                                        {0x1.1bb4a4a1a343fp+0, -0x1.a4e76ce8c0e5ep-4},
                                                                        {0x1.12358f08ae5bap+0, -0x1.1973c5a611cccp-4},
                                                                        {0x1.0953f419900a7p+0, -0x1.252f438e10c1ep-5},
                                                                        {0x1p+0, 0x0p+0},
                                                                        {0x1.e608cfd9a47acp-1, 0x1.aa5aa5df25984p-5},
                                                                        {0x1.ca4b31f026aap-1, 0x1.c5e53aa362eb4p-4},
                                                                        {0x1.b2036576afce6p-1, 0x1.526e57720db08p-3},
                                                                        {0x1.9c2d163a1aa2dp-1, 0x1.bc2860d22477p-3},
                                                                        {0x1.886e6037841edp-1, 0x1.1058bc8a07ee1p-2},
                                                                        {0x1.767dcf5534862p-1, 0x1.4043057b6ee09p-2},
                                                                    },
                                                                .ln2 = 0x1.62e42fefa39efp-1,
                                                                .poly = {
                                                                    -0x1.00ea348b88334p-2,
                                                                    0x1.5575b0be00b6ap-2,
                                                                    -0x1.ffffef20a4123p-2,
                                                                }};

#ifndef _SQRT_DATA_H
#define _SQRT_DATA_H

/* if x in [1,2): i = (int)(64*x);
   if x in [2,4): i = (int)(32*x-64);
   _clib_internal_math__rsqrt_tab[i]*2^-16 is estimating 1/clib__sqrt(x) with small relative error:
   |_clib_internal_math__rsqrt_tab[i]*0x1p-16*clib__sqrt(x) - 1| < -0x1.fdp-9 < 2^-8 */
static const ushort _clib_internal_math__rsqrt_tab[128];

#endif

static const ushort _clib_internal_math__rsqrt_tab[128] = {
    0xb451, 0xb2f0, 0xb196, 0xb044, 0xaef9, 0xadb6, 0xac79, 0xab43, 0xaa14, 0xa8eb, 0xa7c8, 0xa6aa, 0xa592, 0xa480, 0xa373, 0xa26b, 0xa168, 0xa06a, 0x9f70, 0x9e7b, 0x9d8a, 0x9c9d, 0x9bb5, 0x9ad1, 0x99f0, 0x9913, 0x983a, 0x9765, 0x9693, 0x95c4, 0x94f8, 0x9430, 0x936b, 0x92a9, 0x91ea, 0x912e, 0x9075, 0x8fbe, 0x8f0a, 0x8e59, 0x8daa, 0x8cfe, 0x8c54, 0x8bac, 0x8b07, 0x8a64, 0x89c4, 0x8925, 0x8889, 0x87ee, 0x8756, 0x86c0, 0x862b, 0x8599, 0x8508, 0x8479, 0x83ec, 0x8361, 0x82d8, 0x8250, 0x81c9, 0x8145, 0x80c2, 0x8040, 0xff02, 0xfd0e, 0xfb25, 0xf947, 0xf773, 0xf5aa, 0xf3ea, 0xf234, 0xf087, 0xeee3, 0xed47, 0xebb3, 0xea27, 0xe8a3, 0xe727, 0xe5b2, 0xe443, 0xe2dc, 0xe17a, 0xe020, 0xdecb, 0xdd7d, 0xdc34, 0xdaf1, 0xd9b3, 0xd87b, 0xd748, 0xd61a, 0xd4f1, 0xd3cd, 0xd2ad, 0xd192, 0xd07b, 0xcf69, 0xce5b, 0xcd51, 0xcc4a, 0xcb48, 0xca4a, 0xc94f, 0xc858, 0xc764, 0xc674, 0xc587, 0xc49d, 0xc3b7, 0xc2d4, 0xc1f4, 0xc116, 0xc03c, 0xbf65, 0xbe90, 0xbdbe, 0xbcef, 0xbc23, 0xbb59, 0xba91, 0xb9cc, 0xb90a, 0xb84a, 0xb78c, 0xb6d0, 0xb617, 0xb560,
};

static float _clib_internal_math__math_xflowf(uint sign, float y)
{
    return eval_as_float(fp_barrierf(sign ? -y : y) * y);
}

static float _clib_internal_math__math_oflowf(uint sign)
{
    return _clib_internal_math__math_xflowf(sign, 0x1p97f);
}

static float _clib_internal_math__math_uflowf(uint sign)
{
    return _clib_internal_math__math_xflowf(sign, 0x1p-95f);
}

static float _clib_internal_math__math_divzerof(uint sign)
{
    return fp_barrierf(sign ? -1.0f : 1.0f) / 0.0f;
}

static float _clib_internal_math__math_invalidf(float x)
{
    return (x - x) / (x - x);
}

static double _clib_internal_math__math_invalid(double x)
{
    return (x - x) / (x - x);
}

static double _clib_internal_math_scalbn(double x, int n)
{
    union
    {
        double f;
        uint64 i;
    } u;
    double_t y = x;

    if (n > 1023)
    {
        y *= 0x1p1023;
        n -= 1023;
        if (n > 1023)
        {
            y *= 0x1p1023;
            n -= 1023;
            if (n > 1023)
            {
                n = 1023;
            }
        }
    }
    else if (n < -1022)
    {
        /* make sure final n < -53 to avoid double
           rounding in the subnormal range */
        y *= 0x1p-1022 * 0x1p53;
        n += 1022 - 53;
        if (n < -1022)
        {
            y *= 0x1p-1022 * 0x1p53;
            n += 1022 - 53;
            if (n < -1022)
            {
                n = -1022;
            }
        }
    }
    u.i = (uint64)(0x3ff + n) << 52;
    x = y * u.f;
    return x;
}

float clib__fabsf(float x)
{
    union
    {
        float f;
        uint i;
    } u = { x };
    u.i &= 0x7fffffff;
    return u.f;
}

double clib__fabs(double x)
{
    union
    {
        double f;
        uint64 i;
    } u = { x };
    u.i &= -1ULL / 2;
    return u.f;
}

float clib__floorf(float x)
{
    union
    {
        float f;
        uint i;
    } u = { x };
    int e = (int)(u.i >> 23 & 0xff) - 0x7f;
    uint m;

    if (e >= 23)
    {
        return x;
    }
    if (e >= 0)
    {
        m = 0x007fffff >> e;
        if ((u.i & m) == 0)
        {
            return x;
        }
        FORCE_EVAL(x + 0x1p120f);
        if (u.i >> 31)
        {
            u.i += m;
        }
        u.i &= ~m;
    }
    else
    {
        FORCE_EVAL(x + 0x1p120f);
        if (u.i >> 31 == 0)
        {
            u.i = 0;
        }
        else if (u.i << 1)
        {
            u.f = -1.0;
        }
    }
    return u.f;
}

#if FLT_EVAL_METHOD == 0 || FLT_EVAL_METHOD == 1
#define EPS DBL_EPSILON
#elif FLT_EVAL_METHOD == 2
#define EPS LDBL_EPSILON
#endif
static const double_t _clib_internal_math_floor_toint = 1 / EPS;

double clib__floor(double x)
{
    union
    {
        double f;
        uint64 i;
    } u = { x };
    int e = u.i >> 52 & 0x7ff;
    double_t y;

    if (e >= 0x3ff + 52 || x == 0)
    {
        return x;
    }
    /* y = int(x) - x, where int(x) is an integer neighbor of x */
    if (u.i >> 63)
    {
        y = x - _clib_internal_math_floor_toint + _clib_internal_math_floor_toint - x;
    }
    else
    {
        y = x + _clib_internal_math_floor_toint - _clib_internal_math_floor_toint - x;
    }
    /* special case because of non-nearest rounding modes */
    if (e <= 0x3ff - 1)
    {
        FORCE_EVAL(y);
        return u.i >> 63 ? -1 : 0;
    }
    if (y > 0)
    {
        return x + y - 1;
    }
    return x + y;
}

#undef EPS

float clib__ceilf(float x)
{
    union
    {
        float f;
        uint i;
    } u = { x };
    int e = (int)(u.i >> 23 & 0xff) - 0x7f;
    uint m;

    if (e >= 23)
    {
        return x;
    }
    if (e >= 0)
    {
        m = 0x007fffff >> e;
        if ((u.i & m) == 0)
        {
            return x;
        }
        FORCE_EVAL(x + 0x1p120f);
        if (u.i >> 31 == 0)
        {
            u.i += m;
        }
        u.i &= ~m;
    }
    else
    {
        FORCE_EVAL(x + 0x1p120f);
        if (u.i >> 31)
        {
            u.f = -0.0;
        }
        else if (u.i << 1)
        {
            u.f = 1.0;
        }
    }
    return u.f;
}

#if FLT_EVAL_METHOD == 0 || FLT_EVAL_METHOD == 1
#define EPS DBL_EPSILON
#elif FLT_EVAL_METHOD == 2
#define EPS LDBL_EPSILON
#endif
static const double_t _clib_internal_math_ceil_toint = 1 / EPS;

double clib__ceil(double x)
{
    union
    {
        double f;
        uint64 i;
    } u = { x };
    int e = u.i >> 52 & 0x7ff;
    double_t y;

    if (e >= 0x3ff + 52 || x == 0)
    {
        return x;
    }
    /* y = int(x) - x, where int(x) is an integer neighbor of x */
    if (u.i >> 63)
    {
        y = x - _clib_internal_math_ceil_toint + _clib_internal_math_ceil_toint - x;
    }
    else
    {
        y = x + _clib_internal_math_ceil_toint - _clib_internal_math_ceil_toint - x;
    }
    /* special case because of non-nearest rounding modes */
    if (e <= 0x3ff - 1)
    {
        FORCE_EVAL(y);
        return u.i >> 63 ? -0.0 : 1;
    }
    if (y < 0)
    {
        return x + y + 1;
    }
    return x + y;
}

#undef EPS

float clib__fmodf(float x, float y)
{
    union
    {
        float f;
        uint i;
    } ux = { x }, uy = { y };
    int ex = ux.i >> 23 & 0xff;
    int ey = uy.i >> 23 & 0xff;
    uint sx = ux.i & 0x80000000;
    uint i;
    uint uxi = ux.i;

    if (uy.i << 1 == 0 || isnan(y) || ex == 0xff)
    {
        return (x * y) / (x * y);
    }
    if (uxi << 1 <= uy.i << 1)
    {
        if (uxi << 1 == uy.i << 1)
        {
            return 0 * x;
        }
        return x;
    }

#if defined(ARCHITECTURE_FAST_DOUBLE)
    /* meek-ui: for normal operands and quotient < 2^24, a 24-bit
     * significand times the integer quotient fits exactly in binary64.
     * The subtraction is exact, avoiding the bit-at-a-time reduction. */
    if (ex && ey && ex - ey < 24)
    {
        ux.i &= 0x7fffffff;
        uy.i &= 0x7fffffff;
        double ax = ux.f, ay = uy.f;
        uint quotient = (uint)(ax / ay);
        ux.f = (float)(ax - (double)quotient * ay);
        ux.i |= sx;
        return ux.f;
    }
#endif

    /* normalize x and y */
    if (!ex)
    {
        for (i = uxi << 9; i >> 31 == 0; ex--, i <<= 1)
            ;
        uxi <<= -ex + 1;
    }
    else
    {
        uxi &= -1U >> 9;
        uxi |= 1U << 23;
    }
    if (!ey)
    {
        for (i = uy.i << 9; i >> 31 == 0; ey--, i <<= 1)
            ;
        uy.i <<= -ey + 1;
    }
    else
    {
        uy.i &= -1U >> 9;
        uy.i |= 1U << 23;
    }

    /* x mod y */
    for (; ex > ey; ex--)
    {
        i = uxi - uy.i;
        if (i >> 31 == 0)
        {
            if (i == 0)
            {
                return 0 * x;
            }
            uxi = i;
        }
        uxi <<= 1;
    }
    i = uxi - uy.i;
    if (i >> 31 == 0)
    {
        if (i == 0)
        {
            return 0 * x;
        }
        uxi = i;
    }
    for (; uxi >> 23 == 0; uxi <<= 1, ex--)
        ;

    /* scale result up */
    if (ex > 0)
    {
        uxi -= 1U << 23;
        uxi |= (uint)ex << 23;
    }
    else
    {
        uxi >>= -ex + 1;
    }
    uxi |= sx;
    ux.i = uxi;
    return ux.f;
}

double clib__fmod(double x, double y)
{
    union
    {
        double f;
        uint64 i;
    } ux = { x }, uy = { y };
    int ex = ux.i >> 52 & 0x7ff;
    int ey = uy.i >> 52 & 0x7ff;
    int sx = ux.i >> 63;
    uint64 i;

    /* in the followings uxi should be ux.i, but then gcc wrongly adds */
    /* float load/store to inner loops ruining performance and code size */
    uint64 uxi = ux.i;

    if (uy.i << 1 == 0 || isnan(y) || ex == 0x7ff)
    {
        return (x * y) / (x * y);
    }
    if (uxi << 1 <= uy.i << 1)
    {
        if (uxi << 1 == uy.i << 1)
        {
            return 0 * x;
        }
        return x;
    }

    /* normalize x and y */
    if (!ex)
    {
        for (i = uxi << 12; i >> 63 == 0; ex--, i <<= 1)
            ;
        uxi <<= -ex + 1;
    }
    else
    {
        uxi &= -1ULL >> 12;
        uxi |= 1ULL << 52;
    }
    if (!ey)
    {
        for (i = uy.i << 12; i >> 63 == 0; ey--, i <<= 1)
            ;
        uy.i <<= -ey + 1;
    }
    else
    {
        uy.i &= -1ULL >> 12;
        uy.i |= 1ULL << 52;
    }

    /* x mod y */
    for (; ex > ey; ex--)
    {
        i = uxi - uy.i;
        if (i >> 63 == 0)
        {
            if (i == 0)
            {
                return 0 * x;
            }
            uxi = i;
        }
        uxi <<= 1;
    }
    i = uxi - uy.i;
    if (i >> 63 == 0)
    {
        if (i == 0)
        {
            return 0 * x;
        }
        uxi = i;
    }
    for (; uxi >> 52 == 0; uxi <<= 1, ex--)
        ;

    /* scale result */
    if (ex > 0)
    {
        uxi -= 1ULL << 52;
        uxi |= (uint64)ex << 52;
    }
    else
    {
        uxi >>= -ex + 1;
    }
    uxi |= (uint64)sx << 63;
    ux.i = uxi;
    return ux.f;
}

/* origin: FreeBSD /usr/src/lib/msun/src/k_sinf.c */
/*
 * Conversion to float by Ian Lance Taylor, Cygnus Support, ian@cygnus.com.
 * Optimized by Bruce D. Evans.
 */
/*
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 */

/* |sin(x)/x - s(x)| < 2**-37.5 (~[-4.89e-12, 4.824e-12]). */
static const double S1 = -0x15555554cbac77.0p-55, /* -0.166666666416265235595 */
    S2 = 0x111110896efbb2.0p-59, /*  0.0083333293858894631756 */
    S3 = -0x1a00f9e2cae774.0p-65, /* -0.000198393348360966317347 */
    S4 = 0x16cd878c3b46a7.0p-71; /*  0.0000027183114939898219064 */

static float _clib_internal_math__sindf(double x)
{
    double_t r, s, w, z;

    /* Try to optimize for parallel evaluation as in _clib_internal_math__tandf.c. */
    z = x * x;
    w = z * z;
    r = S3 + z * S4;
    s = z * x;
    return (x + s * (S1 + z * S2)) + s * w * r;
}

/* origin: FreeBSD /usr/src/lib/msun/src/k_cosf.c */
/*
 * Conversion to float by Ian Lance Taylor, Cygnus Support, ian@cygnus.com.
 * Debugged and optimized by Bruce D. Evans.
 */
/*
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 */

/* |cos(x) - c(x)| < 2**-34.1 (~[-5.37e-11, 5.295e-11]). */
static const double C0 = -0x1ffffffd0c5e81.0p-54, /* -0.499999997251031003120 */
    C1 = 0x155553e1053a42.0p-57, /*  0.0416666233237390631894 */
    C2 = -0x16c087e80f1e27.0p-62, /* -0.00138867637746099294692 */
    C3 = 0x199342e0ee5069.0p-68; /*  0.0000243904487962774090654 */

static float _clib_internal_math__cosdf(double x)
{
    double_t r, w, z;

    /* Try to optimize for parallel evaluation as in _clib_internal_math__tandf.c. */
    z = x * x;
    w = z * z;
    r = C2 + z * C3;
    return ((1.0 + z * C0) + w * C1) + (w * z) * r;
}

/* origin: FreeBSD /usr/src/lib/msun/src/k_tanf.c */
/*
 * Conversion to float by Ian Lance Taylor, Cygnus Support, ian@cygnus.com.
 * Optimized by Bruce D. Evans.
 */
/*
 * ====================================================
 * Copyright 2004 Sun Microsystems, Inc.  All Rights Reserved.
 *
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 */

/* |tan(x)/x - t(x)| < 2**-25.5 (~[-2e-08, 2e-08]). */
static const double T[] = {
    0x15554d3418c99f.0p-54, /* 0.333331395030791399758 */
    0x1112fd38999f72.0p-55, /* 0.133392002712976742718 */
    0x1b54c91d865afe.0p-57, /* 0.0533812378445670393523 */
    0x191df3908c33ce.0p-58, /* 0.0245283181166547278873 */
    0x185dadfcecf44e.0p-61, /* 0.00297435743359967304927 */
    0x1362b9bf971bcd.0p-59, /* 0.00946564784943673166728 */
};

static float _clib_internal_math__tandf(double x, int odd)
{
    double_t z, r, w, s, t, u;

    z = x * x;
    /*
     * Split up the polynomial into small independent terms to give
     * opportunities for parallel evaluation.  The chosen splitting is
     * micro-optimized for Athlons (XP, X64).  It costs 2 multiplications
     * relative to Horner's method on sequential machines.
     *
     * We add the small terms from lowest degree up for efficiency on
     * non-sequential machines (the lowest degree terms tend to be ready
     * earlier).  Apart from this, we don't care about order of
     * operations, and don't need to to care since we have precision to
     * spare.  However, the chosen splitting is good for accuracy too,
     * and would give results as accurate as Horner's method if the
     * small terms were added from highest degree down.
     */
    r = T[4] + z * T[5];
    t = T[2] + z * T[3];
    w = z * z;
    s = z * x;
    u = T[0] + z * T[1];
    r = (x + s * u) + (s * w) * (t + w * r);
    return odd ? -1.0 / r : r;
}

/* origin: FreeBSD /usr/src/lib/msun/src/k_rem_pio2.c */
/*
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunSoft, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 */
/*
 * _clib_internal_math__rem_pio2_large(x,y,e0,nx,prec)
 * double x[],y[]; int e0,nx,prec;
 *
 * _clib_internal_math__rem_pio2_large return the last three digits of N with
 *              y = x - N*pi/2
 * so that |y| < pi/2.
 *
 * The method is to compute the integer (mod 8) and fraction parts of
 * (2/pi)*x without doing the full multiplication. In general we
 * skip the part of the product that are known to be a huge integer (
 * more accurately, = 0 mod 8 ). Thus the number of operations are
 * independent of the exponent of the input.
 *
 * (2/pi) is represented by an array of 24-bit integers in ipio2[].
 *
 * Input parameters:
 *      x[]     The input value (must be positive) is broken into nx
 *              pieces of 24-bit integers in double precision format.
 *              x[i] will be the i-th 24 bit of x. The scaled exponent
 *              of x[0] is given in input parameter e0 (i.e., x[0]*2^e0
 *              match x's up to 24 bits.
 *
 *              Example of breaking a double positive z into x[0]+x[1]+x[2]:
 *                      e0 = ilogb(z)-23
 *                      z  = _clib_internal_math_scalbn(z,-e0)
 *              for i = 0,1,2
 *                      x[i] = clib__floor(z)
 *                      z    = (z-x[i])*2**24
 *
 *
 *      y[]     ouput result in an array of double precision numbers.
 *              The dimension of y[] is:
 *                      24-bit  precision       1
 *                      53-bit  precision       2
 *                      64-bit  precision       2
 *                      113-bit precision       3
 *              The actual value is the sum of them. Thus for 113-bit
 *              precison, one may have to do something like:
 *
 *              long double t,w,r_head, r_tail;
 *              t = (long double)y[2] + (long double)y[1];
 *              w = (long double)y[0];
 *              r_head = t+w;
 *              r_tail = w - (r_head - t);
 *
 *      e0      The exponent of x[0]. Must be <= 16360 or you need to
 *              expand the ipio2 table.
 *
 *      nx      dimension of x[]
 *
 *      prec    an integer indicating the precision:
 *                      0       24  bits (single)
 *                      1       53  bits (double)
 *                      2       64  bits (extended)
 *                      3       113 bits (quad)
 *
 * External function:
 *      double _clib_internal_math_scalbn(), clib__floor();
 *
 *
 * Here is the description of some local variables:
 *
 *      jk      jk+1 is the initial number of terms of ipio2[] needed
 *              in the computation. The minimum and recommended value
 *              for jk is 3,4,4,6 for single, double, extended, and quad.
 *              jk+1 must be 2 larger than you might expect so that our
 *              recomputation test works. (Up to 24 bits in the integer
 *              part (the 24 bits of it that we compute) and 23 bits in
 *              the fraction part may be lost to cancelation before we
 *              recompute.)
 *
 *      jz      local integer variable indicating the number of
 *              terms of ipio2[] used.
 *
 *      jx      nx - 1
 *
 *      jv      index for pointing to the suitable ipio2[] for the
 *              computation. In general, we want
 *                      ( 2^e0*x[0] * ipio2[jv-1]*2^(-24jv) )/8
 *              is an integer. Thus
 *                      e0-3-24*jv >= 0 or (e0-3)/24 >= jv
 *              Hence jv = max(0,(e0-3)/24).
 *
 *      jp      jp+1 is the number of terms in PIo2[] needed, jp = jk.
 *
 *      q[]     double array with integral value, representing the
 *              24-bits chunk of the product of x and 2/pi.
 *
 *      q0      the corresponding exponent of q[0]. Note that the
 *              exponent for q[i] would be q0-24*i.
 *
 *      PIo2[]  double precision array, obtained by cutting pi/2
 *              into 24 bits chunks.
 *
 *      f[]     ipio2[] in floating point
 *
 *      iq[]    integer array by breaking up q[] in 24-bits chunk.
 *
 *      fq[]    final product of x*(2/pi) in fq[0],..,fq[jk]
 *
 *      ih      integer. If >0 it indicates q[] is >= 0.5, hence
 *              it also indicates the *sign* of the result.
 *
 */
/*
 * Constants:
 * The hexadecimal values are the intended ones for the following
 * constants. The decimal values may be used, provided that the
 * compiler will convert from decimal to binary accurately enough
 * to produce the hexadecimal values shown.
 */

static const int init_jk[] = { 3, 4, 4, 6 }; /* initial value for jk */

/*
 * Table of constants for 2/pi, 396 Hex digits (476 decimal) of 2/pi
 *
 *              integer array, contains the (24*i)-th to (24*i+23)-th
 *              bit of 2/pi after binary point. The corresponding
 *              floating value is
 *
 *                      ipio2[i] * 2^(-24(i+1)).
 *
 * NB: This table must have at least (e0-3)/24 + jk terms.
 *     For quad precision (e0 <= 16360, jk = 6), this is 686.
 */
static const int ipio2[] = {
    0xA2F983, 0x6E4E44, 0x1529FC, 0x2757D1, 0xF534DD, 0xC0DB62, 0x95993C, 0x439041, 0xFE5163, 0xABDEBB, 0xC561B7, 0x246E3A, 0x424DD2, 0xE00649, 0x2EEA09, 0xD1921C, 0xFE1DEB, 0x1CB129, 0xA73EE8, 0x8235F5, 0x2EBB44, 0x84E99C, 0x7026B4, 0x5F7E41, 0x3991D6, 0x398353, 0x39F49C, 0x845F8B, 0xBDF928, 0x3B1FF8, 0x97FFDE, 0x05980F, 0xEF2F11, 0x8B5A0A, 0x6D1F6D, 0x367ECF, 0x27CB09, 0xB74F46, 0x3F669E, 0x5FEA2D, 0x7527BA, 0xC7EBE5, 0xF17B3D, 0x0739F7, 0x8A5292, 0xEA6BFB, 0x5FB11F, 0x8D5D08, 0x560330, 0x46FC7B, 0x6BABF0, 0xCFBC20, 0x9AF436, 0x1DA9E3, 0x91615E, 0xE61B08, 0x659985, 0x5F14A0, 0x68408D, 0xFFD880, 0x4D7327, 0x310606, 0x1556CA, 0x73A8C9, 0x60E27B, 0xC08C6B,

#if LDBL_MAX_EXP > 1024
    0x47C419, 0xC367CD, 0xDCE809, 0x2A8359, 0xC4768B, 0x961CA6, 0xDDAF44, 0xD15719, 0x053EA5, 0xFF0705, 0x3F7E33, 0xE832C2, 0xDE4F98, 0x327DBB, 0xC33D26, 0xEF6B1E, 0x5EF89F, 0x3A1F35, 0xCAF27F, 0x1D87F1, 0x21907C, 0x7C246A, 0xFA6ED5, 0x772D30, 0x433B15, 0xC614B5, 0x9D19C3, 0xC2C4AD, 0x414D2C, 0x5D000C, 0x467D86, 0x2D71E3, 0x9AC69B, 0x006233, 0x7CD2B4, 0x97A7B4, 0xD55537, 0xF63ED7, 0x1810A3, 0xFC764D, 0x2A9D64, 0xABD770, 0xF87C63, 0x57B07A, 0xE71517, 0x5649C0, 0xD9D63B, 0x3884A7, 0xCB2324, 0x778AD6, 0x23545A, 0xB91F00, 0x1B0AF1, 0xDFCE19, 0xFF319F, 0x6A1E66, 0x615799, 0x47FBAC, 0xD87F7E, 0xB76522, 0x89E832, 0x60BFE6, 0xCDC4EF, 0x09366C, 0xD43F5D, 0xD7DE16, 0xDE3B58, 0x929BDE, 0x2822D2, 0xE88628, 0x4D58E2, 0x32CAC6, 0x16E308, 0xCB7DE0, 0x50C017, 0xA71DF3, 0x5BE018, 0x34132E, 0x621283, 0x014883, 0x5B8EF5, 0x7FB0AD, 0xF2E91E, 0x434A48, 0xD36710, 0xD8DDAA, 0x425FAE, 0xCE616A, 0xA4280A, 0xB499D3, 0xF2A606, 0x7F775C, 0x83C2A3, 0x883C61, 0x78738A, 0x5A8CAF, 0xBDD76F, 0x63A62D, 0xCBBFF4, 0xEF818D, 0x67C126, 0x45CA55, 0x36D9CA, 0xD2A828, 0x8D61C2, 0x77C912, 0x142604, 0x9B4612, 0xC459C4, 0x44C5C8, 0x91B24D, 0xF31700, 0xAD43D4, 0xE54929, 0x10D5FD, 0xFCBE00, 0xCC941E, 0xEECE70, 0xF53E13, 0x80F1EC, 0xC3E7B3, 0x28F8C7, 0x940593, 0x3E71C1, 0xB3092E, 0xF3450B, 0x9C1288, 0x7B20AB, 0x9FB52E, 0xC29247, 0x2F327B, 0x6D550C, 0x90A772, 0x1FE76B, 0x96CB31, 0x4A1679, 0xE27941, 0x89DFF4, 0x9794E8, 0x84E6E2, 0x973199, 0x6BED88, 0x365F5F, 0x0EFDBB, 0xB49A48, 0x6CA467, 0x427271, 0x325D8D, 0xB8159F, 0x09E5BC, 0x25318D, 0x3974F7, 0x1C0530, 0x010C0D, 0x68084B, 0x58EE2C,
    0x90AA47, 0x02E774, 0x24D6BD, 0xA67DF7, 0x72486E, 0xEF169F, 0xA6948E, 0xF691B4, 0x5153D1, 0xF20ACF, 0x339820, 0x7E4BF5, 0x6863B2, 0x5F3EDD, 0x035D40, 0x7F8985, 0x295255, 0xC06437, 0x10D86D, 0x324832, 0x754C5B, 0xD4714E, 0x6E5445, 0xC1090B, 0x69F52A, 0xD56614, 0x9D0727, 0x50045D, 0xDB3BB4, 0xC576EA, 0x17F987, 0x7D6B49, 0xBA271D, 0x296996, 0xACCCC6, 0x5414AD, 0x6AE290, 0x89D988, 0x50722C, 0xBEA404, 0x940777, 0x7030F3, 0x27FC00, 0xA871EA, 0x49C266, 0x3DE064, 0x83DD97, 0x973FA3, 0xFD9443, 0x8C860D, 0xDE4131, 0x9D3992, 0x8C70DD, 0xE7B717, 0x3BDF08, 0x2B3715, 0xA0805C, 0x93805A, 0x921110, 0xD8E80F, 0xAF806C, 0x4BFFDB, 0x0F9038, 0x761859, 0x15A562, 0xBBCB61, 0xB989C7, 0xBD4010, 0x04F2D2, 0x277549, 0xF6B6EB, 0xBB22DB, 0xAA140A, 0x2F2689, 0x768364, 0x333B09, 0x1A940E, 0xAA3A51, 0xC2A31D, 0xAEEDAF, 0x12265C, 0x4DC26D, 0x9C7A2D, 0x9756C0, 0x833F03, 0xF6F009, 0x8C402B, 0x99316D, 0x07B439, 0x15200C, 0x5BC3D8, 0xC492F5, 0x4BADC6, 0xA5CA4E, 0xCD37A7, 0x36A9E6, 0x9492AB, 0x6842DD, 0xDE6319, 0xEF8C76, 0x528B68, 0x37DBFC, 0xABA1AE, 0x3115DF, 0xA1AE00, 0xDAFB0C, 0x664D64, 0xB705ED, 0x306529, 0xBF5657, 0x3AFF47, 0xB9F96A, 0xF3BE75, 0xDF9328, 0x3080AB, 0xF68C66, 0x15CB04, 0x0622FA, 0x1DE4D9, 0xA4B33D, 0x8F1B57, 0x09CD36, 0xE9424E, 0xA4BE13, 0xB52333, 0x1AAAF0, 0xA8654F, 0xA5C1D2, 0x0F3F0B, 0xCD785B, 0x76F923, 0x048B7B, 0x721789, 0x53A6C6, 0xE26E6F, 0x00EBEF, 0x584A9B, 0xB7DAC4, 0xBA66AA, 0xCFCF76, 0x1D02D1, 0x2DF1B1, 0xC1998C, 0x77ADC3, 0xDA4886, 0xA05DF7, 0xF480C6, 0x2FF0AC, 0x9AECDD, 0xBC5C3F, 0x6DDED0, 0x1FC790, 0xB6DB2A, 0x3A25A3, 0x9AAF00, 0x9353AD,
    0x0457B6, 0xB42D29, 0x7E804B, 0xA707DA, 0x0EAA76, 0xA1597B, 0x2A1216, 0x2DB7DC, 0xFDE5FA, 0xFEDB89, 0xFDBE89, 0x6C76E4, 0xFCA906, 0x70803E, 0x156E85, 0xFF87FD, 0x073E28, 0x336761, 0x86182A, 0xEABD4D, 0xAFE7B3, 0x6E6D8F, 0x396795, 0x5BBF31, 0x48D784, 0x16DF30, 0x432DC7, 0x356125, 0xCE70C9, 0xB8CB30, 0xFD6CBF, 0xA200A4, 0xE46C05, 0xA0DD5A, 0x476F21, 0xD21262, 0x845CB9, 0x496170, 0xE0566B, 0x015299, 0x375550, 0xB7D51E, 0xC4F133, 0x5F6E13, 0xE4305D, 0xA92E85, 0xC3B21D, 0x3632A1, 0xA4B708, 0xD4B1EA, 0x21F716, 0xE4698F, 0x77FF27, 0x80030C, 0x2D408D, 0xA0CD4F, 0x99A520, 0xD3A2B3, 0x0A5D2F, 0x42F9B4, 0xCBDA11, 0xD0BE7D, 0xC1DB9B, 0xBD17AB, 0x81A2CA, 0x5C6A08, 0x17552E, 0x550027, 0xF0147F, 0x8607E1, 0x640B14, 0x8D4196, 0xDEBE87, 0x2AFDDA, 0xB6256B, 0x34897B, 0xFEF305, 0x9EBFB9, 0x4F6A68, 0xA82A4A, 0x5AC44F, 0xBCF82D, 0x985AD7, 0x95C7F4, 0x8D4D0D, 0xA63A20, 0x5F57A4, 0xB13F14, 0x953880, 0x0120CC, 0x86DD71, 0xB6DEC9, 0xF560BF, 0x11654D, 0x6B0701, 0xACB08C, 0xD0C0B2, 0x485551, 0x0EFB1E, 0xC37295, 0x3B06A3, 0x3540C0, 0x7BDC06, 0xCC45E0, 0xFA294E, 0xC8CAD6, 0x41F3E8, 0xDE647C, 0xD8649B, 0x31BED9, 0xC397A4, 0xD45877, 0xC5E369, 0x13DAF0, 0x3C3ABA, 0x461846, 0x5F7555, 0xF5BDD2, 0xC6926E, 0x5D2EAC, 0xED440E, 0x423E1C, 0x87C461, 0xE9FD29, 0xF3D6E7, 0xCA7C22, 0x35916F, 0xC5E008, 0x8DD7FF, 0xE26A6E, 0xC6FDB0, 0xC10893, 0x745D7C, 0xB2AD6B, 0x9D6ECD, 0x7B723E, 0x6A11C6, 0xA9CFF7, 0xDF7329, 0xBAC9B5, 0x5100B7, 0x0DB2E2, 0x24BA74, 0x607DE5, 0x8AD874, 0x2C150D, 0x0C1881, 0x94667E, 0x162901, 0x767A9F, 0xBEFDFD, 0xEF4556, 0x367ED9, 0x13D9EC, 0xB9BA8B, 0xFC97C4,
    0x27A831, 0xC36EF1, 0x36C594, 0x56A8D8, 0xB5A8B4, 0x0ECCCF, 0x2D8912, 0x34576F, 0x89562C, 0xE3CE99, 0xB920D6, 0xAA5E6B, 0x9C2A3E, 0xCC5F11, 0x4A0BFD, 0xFBF4E1, 0x6D3B8E, 0x2C86E2, 0x84D4E9, 0xA9B4FC, 0xD1EEEF, 0xC9352E, 0x61392F, 0x442138, 0xC8D91B, 0x0AFC81, 0x6A4AFB, 0xD81C2F, 0x84B453, 0x8C994E, 0xCC2254, 0xDC552A, 0xD6C6C0, 0x96190B, 0xB8701A, 0x649569, 0x605A26, 0xEE523F, 0x0F117F, 0x11B5F4, 0xF5CBFC, 0x2DBC34, 0xEEBC34, 0xCC5DE8, 0x605EDD, 0x9B8E67, 0xEF3392, 0xB817C9, 0x9B5861, 0xBC57E1, 0xC68351, 0x103ED8, 0x4871DD, 0xDD1C2D, 0xA118AF, 0x462C21, 0xD7F359, 0x987AD9, 0xC0549E, 0xFA864F, 0xFC0656, 0xAE79E5, 0x362289, 0x22AD38, 0xDC9367, 0xAAE855, 0x382682, 0x9BE7CA, 0xA40D51, 0xB13399, 0x0ED7A9, 0x480569, 0xF0B265, 0xA7887F, 0x974C88, 0x36D1F9, 0xB39221, 0x4A827B, 0x21CF98, 0xDC9F40, 0x5547DC, 0x3A74E1, 0x42EB67, 0xDF9DFE, 0x5FD45E, 0xA4677B, 0x7AACBA, 0xA2F655, 0x23882B, 0x55BA41, 0x086E59, 0x862A21, 0x834739, 0xE6E389, 0xD49EE5, 0x40FB49, 0xE956FF, 0xCA0F1C, 0x8A59C5, 0x2BFA94, 0xC5C1D3, 0xCFC50F, 0xAE5ADB, 0x86C547, 0x624385, 0x3B8621, 0x94792C, 0x876110, 0x7B4C2A, 0x1A2C80, 0x12BF43, 0x902688, 0x893C78, 0xE4C4A8, 0x7BDBE5, 0xC23AC4, 0xEAF426, 0x8A67F7, 0xBF920D, 0x2BA365, 0xB1933D, 0x0B7CBD, 0xDC51A4, 0x63DD27, 0xDDE169, 0x19949A, 0x9529A8, 0x28CE68, 0xB4ED09, 0x209F44, 0xCA984E, 0x638270, 0x237C7E, 0x32B90F, 0x8EF5A7, 0xE75614, 0x08F121, 0x2A9DB5, 0x4D7E6F, 0x5119A5, 0xABF9B5, 0xD6DF82, 0x61DD96, 0x023616, 0x9F3AC4, 0xA1A283, 0x6DED72, 0x7A8D39, 0xA9B882, 0x5C326B, 0x5B2746, 0xED3400, 0x7700D2, 0x55F4FC, 0x4D5901, 0x8071E0,
#endif
};

static const double PIo2[] = {
    1.57079625129699707031e+00, /* 0x3FF921FB, 0x40000000 */
    7.54978941586159635335e-08, /* 0x3E74442D, 0x00000000 */
    5.39030252995776476554e-15, /* 0x3CF84698, 0x80000000 */
    3.28200341580791294123e-22, /* 0x3B78CC51, 0x60000000 */
    1.27065575308067607349e-29, /* 0x39F01B83, 0x80000000 */
    1.22933308981111328932e-36, /* 0x387A2520, 0x40000000 */
    2.73370053816464559624e-44, /* 0x36E38222, 0x80000000 */
    2.16741683877804819444e-51, /* 0x3569F31D, 0x00000000 */
};

static int _clib_internal_math__rem_pio2_large(double *x, double *y, int e0, int nx, int prec)
{
    int jz, jx, jv, jp, jk, carry, n, iq[20], i, j, k, m, q0, ih;
    double z, fw, f[20], fq[20], q[20];

    /* initialize jk*/
    jk = init_jk[prec];
    jp = jk;

    /* determine jx,jv,q0, note that 3>q0 */
    jx = nx - 1;
    jv = (e0 - 3) / 24;
    if (jv < 0)
    {
        jv = 0;
    }
    q0 = e0 - 24 * (jv + 1);

    /* set up f[0] to f[jx+jk] where f[jx+jk] = ipio2[jv+jk] */
    j = jv - jx;
    m = jx + jk;
    for (i = 0; i <= m; i++, j++)
    {
        f[i] = j < 0 ? 0.0 : (double)ipio2[j];
    }

    /* compute q[0],q[1],...q[jk] */
    for (i = 0; i <= jk; i++)
    {
        for (j = 0, fw = 0.0; j <= jx; j++)
        {
            fw += x[j] * f[jx + i - j];
        }
        q[i] = fw;
    }

    jz = jk;
recompute:
    /* distill q[] into iq[] reversingly */
    for (i = 0, j = jz, z = q[jz]; j > 0; i++, j--)
    {
        fw = (double)(int)(0x1p-24 * z);
        iq[i] = (int)(z - 0x1p24 * fw);
        z = q[j - 1] + fw;
    }

    /* compute n */
    z = _clib_internal_math_scalbn(z, q0); /* actual value of z */
    z -= 8.0 * clib__floor(z * 0.125); /* trim off integer >= 8 */
    n = (int)z;
    z -= (double)n;
    ih = 0;
    if (q0 > 0)
    { /* need iq[jz-1] to determine n */
        i = iq[jz - 1] >> (24 - q0);
        n += i;
        iq[jz - 1] -= i << (24 - q0);
        ih = iq[jz - 1] >> (23 - q0);
    }
    else if (q0 == 0)
    {
        ih = iq[jz - 1] >> 23;
    }
    else if (z >= 0.5)
    {
        ih = 2;
    }

    if (ih > 0)
    { /* q > 0.5 */
        n += 1;
        carry = 0;
        for (i = 0; i < jz; i++)
        { /* compute 1-q */
            j = iq[i];
            if (carry == 0)
            {
                if (j != 0)
                {
                    carry = 1;
                    iq[i] = 0x1000000 - j;
                }
            }
            else
            {
                iq[i] = 0xffffff - j;
            }
        }
        if (q0 > 0)
        { /* rare case: chance is 1 in 12 */
            switch (q0)
            {
            case 1:
                iq[jz - 1] &= 0x7fffff;
                break;
            case 2:
                iq[jz - 1] &= 0x3fffff;
                break;
            }
        }
        if (ih == 2)
        {
            z = 1.0 - z;
            if (carry != 0)
            {
                z -= _clib_internal_math_scalbn(1.0, q0);
            }
        }
    }

    /* check if recomputation is needed */
    if (z == 0.0)
    {
        j = 0;
        for (i = jz - 1; i >= jk; i--)
        {
            j |= iq[i];
        }
        if (j == 0)
        { /* need recomputation */
            for (k = 1; iq[jk - k] == 0; k++)
                ; /* k = no. of terms needed */

            for (i = jz + 1; i <= jz + k; i++)
            { /* add q[jz+1] to q[jz+k] */
                f[jx + i] = (double)ipio2[jv + i];
                for (j = 0, fw = 0.0; j <= jx; j++)
                {
                    fw += x[j] * f[jx + i - j];
                }
                q[i] = fw;
            }
            jz += k;
            goto recompute;
        }
    }

    /* chop off zero terms */
    if (z == 0.0)
    {
        jz -= 1;
        q0 -= 24;
        while (iq[jz] == 0)
        {
            jz--;
            q0 -= 24;
        }
    }
    else
    { /* break z into 24-bit if necessary */
        z = _clib_internal_math_scalbn(z, -q0);
        if (z >= 0x1p24)
        {
            fw = (double)(int)(0x1p-24 * z);
            iq[jz] = (int)(z - 0x1p24 * fw);
            jz += 1;
            q0 += 24;
            iq[jz] = (int)fw;
        }
        else
        {
            iq[jz] = (int)z;
        }
    }

    /* convert integer "bit" chunk to floating-point value */
    fw = _clib_internal_math_scalbn(1.0, q0);
    for (i = jz; i >= 0; i--)
    {
        q[i] = fw * (double)iq[i];
        fw *= 0x1p-24;
    }

    /* compute PIo2[0,...,jp]*q[jz,...,0] */
    for (i = jz; i >= 0; i--)
    {
        for (fw = 0.0, k = 0; k <= jp && k <= jz - i; k++)
        {
            fw += PIo2[k] * q[i + k];
        }
        fq[jz - i] = fw;
    }

    /* compress fq[] into y[] */
    switch (prec)
    {
    case 0:
        fw = 0.0;
        for (i = jz; i >= 0; i--)
        {
            fw += fq[i];
        }
        y[0] = ih == 0 ? fw : -fw;
        break;
    case 1:
    case 2:
        fw = 0.0;
        for (i = jz; i >= 0; i--)
        {
            fw += fq[i];
        }
        // TODO: drop excess precision here once double_t is used
        fw = (double)fw;
        y[0] = ih == 0 ? fw : -fw;
        fw = fq[0] - fw;
        for (i = 1; i <= jz; i++)
        {
            fw += fq[i];
        }
        y[1] = ih == 0 ? fw : -fw;
        break;
    case 3: /* painful */
        for (i = jz; i > 0; i--)
        {
            fw = fq[i - 1] + fq[i];
            fq[i] += fq[i - 1] - fw;
            fq[i - 1] = fw;
        }
        for (i = jz; i > 1; i--)
        {
            fw = fq[i - 1] + fq[i];
            fq[i] += fq[i - 1] - fw;
            fq[i - 1] = fw;
        }
        for (fw = 0.0, i = jz; i >= 2; i--)
        {
            fw += fq[i];
        }
        if (ih == 0)
        {
            y[0] = fq[0];
            y[1] = fq[1];
            y[2] = fw;
        }
        else
        {
            y[0] = -fq[0];
            y[1] = -fq[1];
            y[2] = -fw;
        }
    }
    return n & 7;
}

/* origin: FreeBSD /usr/src/lib/msun/src/e_rem_pio2f.c */
/*
 * Conversion to float by Ian Lance Taylor, Cygnus Support, ian@cygnus.com.
 * Debugged and optimized by Bruce D. Evans.
 */
/*
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 */
/* _clib_internal_math__rem_pio2f(x,y)
 *
 * return the remainder of x rem pi/2 in *y
 * use double precision for everything except passing x
 * use _clib_internal_math__rem_pio2_large() for large x
 */

#if FLT_EVAL_METHOD == 0 || FLT_EVAL_METHOD == 1
#define EPS DBL_EPSILON
#elif FLT_EVAL_METHOD == 2
#define EPS LDBL_EPSILON
#endif

/*
 * invpio2:  53 bits of 2/pi
 * pio2_1:   first 25 bits of pi/2
 * pio2_1t:  pi/2 - pio2_1
 */
static const double _clib_internal_math___rem_pio2f_toint = 1.5 / EPS, pio4 = 0x1.921fb6p-1, invpio2 = 6.36619772367581382433e-01, /* 0x3FE45F30, 0x6DC9C883 */
    pio2_1 = 1.57079631090164184570e+00, /* 0x3FF921FB, 0x50000000 */
    pio2_1t = 1.58932547735281966916e-08; /* 0x3E5110b4, 0x611A6263 */

static int _clib_internal_math__rem_pio2f(float x, double *y)
{
    union
    {
        float f;
        uint i;
    } u = { x };
    double tx[1], ty[1];
    double_t fn;
    uint ix;
    int n, sign, e0;

    ix = u.i & 0x7fffffff;
    /* 25+53 bit pi is good enough for medium size */
    if (ix < 0x4dc90fdb)
    { /* |x| ~< 2^28*(pi/2), medium size */
        /* Use a specialized rint() to get fn. */
        fn = (double_t)x * invpio2 + _clib_internal_math___rem_pio2f_toint - _clib_internal_math___rem_pio2f_toint;
        n = (int)fn;
        *y = x - fn * pio2_1 - fn * pio2_1t;
        /* Matters with directed rounding. */
        if (predict_false(*y < -pio4))
        {
            n--;
            fn--;
            *y = x - fn * pio2_1 - fn * pio2_1t;
        }
        else if (predict_false(*y > pio4))
        {
            n++;
            fn++;
            *y = x - fn * pio2_1 - fn * pio2_1t;
        }
        return n;
    }
    if (ix >= 0x7f800000)
    { /* x is inf or NaN */
        *y = x - x;
        return 0;
    }
    /* scale x into [2^23, 2^24-1] */
    sign = u.i >> 31;
    e0 = (ix >> 23) - (0x7f + 23); /* e0 = ilogb(|x|)-23, positive */
    u.i = ix - (e0 << 23);
    tx[0] = u.f;
    n = _clib_internal_math__rem_pio2_large(tx, ty, e0, 1, 0);
    if (sign)
    {
        *y = -ty[0];
        return -n;
    }
    *y = ty[0];
    return n;
}

#undef EPS
/* origin: FreeBSD /usr/src/lib/msun/src/s_sinf.c */
/*
 * Conversion to float by Ian Lance Taylor, Cygnus Support, ian@cygnus.com.
 * Optimized by Bruce D. Evans.
 */
/*
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 */

/* Small multiples of pi/2 rounded to double precision. */
static const double s1pio2 = 1 * M_PI_2, /* 0x3FF921FB, 0x54442D18 */
    s2pio2 = 2 * M_PI_2, /* 0x400921FB, 0x54442D18 */
    s3pio2 = 3 * M_PI_2, /* 0x4012D97C, 0x7F3321D2 */
    s4pio2 = 4 * M_PI_2; /* 0x401921FB, 0x54442D18 */

float clib__sinf(float x)
{
    double y;
    uint ix;
    int n, sign;

    GET_FLOAT_WORD(ix, x);
    sign = ix >> 31;
    ix &= 0x7fffffff;

    if (ix <= 0x3f490fda)
    { /* |x| ~<= pi/4 */
        if (ix < 0x39800000)
        { /* |x| < 2**-12 */
            /* raise inexact if x!=0 and underflow if subnormal */
            FORCE_EVAL(ix < 0x00800000 ? x / 0x1p120f : x + 0x1p120f);
            return x;
        }
        return _clib_internal_math__sindf(x);
    }
    if (ix <= 0x407b53d1)
    { /* |x| ~<= 5*pi/4 */
        if (ix <= 0x4016cbe3)
        { /* |x| ~<= 3pi/4 */
            if (sign)
            {
                return -_clib_internal_math__cosdf(x + s1pio2);
            }
            else
            {
                return _clib_internal_math__cosdf(x - s1pio2);
            }
        }
        return _clib_internal_math__sindf(sign ? -(x + s2pio2) : -(x - s2pio2));
    }
    if (ix <= 0x40e231d5)
    { /* |x| ~<= 9*pi/4 */
        if (ix <= 0x40afeddf)
        { /* |x| ~<= 7*pi/4 */
            if (sign)
            {
                return _clib_internal_math__cosdf(x + s3pio2);
            }
            else
            {
                return -_clib_internal_math__cosdf(x - s3pio2);
            }
        }
        return _clib_internal_math__sindf(sign ? x + s4pio2 : x - s4pio2);
    }

    /* sin(Inf or NaN) is NaN */
    if (ix >= 0x7f800000)
    {
        return x - x;
    }

    /* general argument reduction needed */
    n = _clib_internal_math__rem_pio2f(x, &y);
    switch (n & 3)
    {
    case 0:
        return _clib_internal_math__sindf(y);
    case 1:
        return _clib_internal_math__cosdf(y);
    case 2:
        return _clib_internal_math__sindf(-y);
    default:
        return -_clib_internal_math__cosdf(y);
    }
}

/* origin: FreeBSD /usr/src/lib/msun/src/s_cosf.c */
/*
 * Conversion to float by Ian Lance Taylor, Cygnus Support, ian@cygnus.com.
 * Optimized by Bruce D. Evans.
 */
/*
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 */

/* Small multiples of pi/2 rounded to double precision. */
static const double c1pio2 = 1 * M_PI_2, /* 0x3FF921FB, 0x54442D18 */
    c2pio2 = 2 * M_PI_2, /* 0x400921FB, 0x54442D18 */
    c3pio2 = 3 * M_PI_2, /* 0x4012D97C, 0x7F3321D2 */
    c4pio2 = 4 * M_PI_2; /* 0x401921FB, 0x54442D18 */

float clib__cosf(float x)
{
    double y;
    uint ix;
    unsigned n, sign;

    GET_FLOAT_WORD(ix, x);
    sign = ix >> 31;
    ix &= 0x7fffffff;

    if (ix <= 0x3f490fda)
    { /* |x| ~<= pi/4 */
        if (ix < 0x39800000)
        { /* |x| < 2**-12 */
            /* raise inexact if x != 0 */
            FORCE_EVAL(x + 0x1p120f);
            return 1.0f;
        }
        return _clib_internal_math__cosdf(x);
    }
    if (ix <= 0x407b53d1)
    { /* |x| ~<= 5*pi/4 */
        if (ix > 0x4016cbe3) /* |x|  ~> 3*pi/4 */
        {
            return -_clib_internal_math__cosdf(sign ? x + c2pio2 : x - c2pio2);
        }
        else
        {
            if (sign)
            {
                return _clib_internal_math__sindf(x + c1pio2);
            }
            else
            {
                return _clib_internal_math__sindf(c1pio2 - x);
            }
        }
    }
    if (ix <= 0x40e231d5)
    { /* |x| ~<= 9*pi/4 */
        if (ix > 0x40afeddf) /* |x| ~> 7*pi/4 */
        {
            return _clib_internal_math__cosdf(sign ? x + c4pio2 : x - c4pio2);
        }
        else
        {
            if (sign)
            {
                return _clib_internal_math__sindf(-x - c3pio2);
            }
            else
            {
                return _clib_internal_math__sindf(x - c3pio2);
            }
        }
    }

    /* cos(Inf or NaN) is NaN */
    if (ix >= 0x7f800000)
    {
        return x - x;
    }

    /* general argument reduction needed */
    n = _clib_internal_math__rem_pio2f(x, &y);
    switch (n & 3)
    {
    case 0:
        return _clib_internal_math__cosdf(y);
    case 1:
        return _clib_internal_math__sindf(-y);
    case 2:
        return -_clib_internal_math__cosdf(y);
    default:
        return _clib_internal_math__sindf(y);
    }
}

/* origin: FreeBSD /usr/src/lib/msun/src/s_tanf.c */
/*
 * Conversion to float by Ian Lance Taylor, Cygnus Support, ian@cygnus.com.
 * Optimized by Bruce D. Evans.
 */
/*
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 */

/* Small multiples of pi/2 rounded to double precision. */
static const double t1pio2 = 1 * M_PI_2, /* 0x3FF921FB, 0x54442D18 */
    t2pio2 = 2 * M_PI_2, /* 0x400921FB, 0x54442D18 */
    t3pio2 = 3 * M_PI_2, /* 0x4012D97C, 0x7F3321D2 */
    t4pio2 = 4 * M_PI_2; /* 0x401921FB, 0x54442D18 */

float clib__tanf(float x)
{
    double y;
    uint ix;
    unsigned n, sign;

    GET_FLOAT_WORD(ix, x);
    sign = ix >> 31;
    ix &= 0x7fffffff;

    if (ix <= 0x3f490fda)
    { /* |x| ~<= pi/4 */
        if (ix < 0x39800000)
        { /* |x| < 2**-12 */
            /* raise inexact if x!=0 and underflow if subnormal */
            FORCE_EVAL(ix < 0x00800000 ? x / 0x1p120f : x + 0x1p120f);
            return x;
        }
        return _clib_internal_math__tandf(x, 0);
    }
    if (ix <= 0x407b53d1)
    { /* |x| ~<= 5*pi/4 */
        if (ix <= 0x4016cbe3) /* |x| ~<= 3pi/4 */
        {
            return _clib_internal_math__tandf((sign ? x + t1pio2 : x - t1pio2), 1);
        }
        else
        {
            return _clib_internal_math__tandf((sign ? x + t2pio2 : x - t2pio2), 0);
        }
    }
    if (ix <= 0x40e231d5)
    { /* |x| ~<= 9*pi/4 */
        if (ix <= 0x40afeddf) /* |x| ~<= 7*pi/4 */
        {
            return _clib_internal_math__tandf((sign ? x + t3pio2 : x - t3pio2), 1);
        }
        else
        {
            return _clib_internal_math__tandf((sign ? x + t4pio2 : x - t4pio2), 0);
        }
    }

    /* tan(Inf or NaN) is NaN */
    if (ix >= 0x7f800000)
    {
        return x - x;
    }

    /* argument reduction */
    n = _clib_internal_math__rem_pio2f(x, &y);
    return _clib_internal_math__tandf(y, n & 1);
}

/* origin: FreeBSD /usr/src/lib/msun/src/s_atanf.c */
/*
 * Conversion to float by Ian Lance Taylor, Cygnus Support, ian@cygnus.com.
 */
/*
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 */

static const float atanhi[] = {
    4.6364760399e-01, /* atan(0.5)hi 0x3eed6338 */
    7.8539812565e-01, /* atan(1.0)hi 0x3f490fda */
    9.8279368877e-01, /* atan(1.5)hi 0x3f7b985e */
    1.5707962513e+00, /* atan(inf)hi 0x3fc90fda */
};

static const float atanlo[] = {
    5.0121582440e-09, /* atan(0.5)lo 0x31ac3769 */
    3.7748947079e-08, /* atan(1.0)lo 0x33222168 */
    3.4473217170e-08, /* atan(1.5)lo 0x33140fb4 */
    7.5497894159e-08, /* atan(inf)lo 0x33a22168 */
};

static const float aT[] = {
    3.3333328366e-01, -1.9999158382e-01, 1.4253635705e-01, -1.0648017377e-01, 6.1687607318e-02,
};

float clib__atanf(float x)
{
    float_t w, s1, s2, z;
    uint ix, sign;
    int id;

    GET_FLOAT_WORD(ix, x);
    sign = ix >> 31;
    ix &= 0x7fffffff;
    if (ix >= 0x4c800000)
    { /* if |x| >= 2**26 */
        if (isnan(x))
        {
            return x;
        }
        z = atanhi[3] + 0x1p-120f;
        return sign ? -z : z;
    }
    if (ix < 0x3ee00000)
    { /* |x| < 0.4375 */
        if (ix < 0x39800000)
        { /* |x| < 2**-12 */
            if (ix < 0x00800000)
            {
                /* raise underflow for subnormal x */
                FORCE_EVAL(x * x);
            }
            return x;
        }
        id = -1;
    }
    else
    {
        x = clib__fabsf(x);
        if (ix < 0x3f980000)
        { /* |x| < 1.1875 */
            if (ix < 0x3f300000)
            { /*  7/16 <= |x| < 11/16 */
                id = 0;
                x = (2.0f * x - 1.0f) / (2.0f + x);
            }
            else
            { /* 11/16 <= |x| < 19/16 */
                id = 1;
                x = (x - 1.0f) / (x + 1.0f);
            }
        }
        else
        {
            if (ix < 0x401c0000)
            { /* |x| < 2.4375 */
                id = 2;
                x = (x - 1.5f) / (1.0f + 1.5f * x);
            }
            else
            { /* 2.4375 <= |x| < 2**26 */
                id = 3;
                x = -1.0f / x;
            }
        }
    }
    /* end of argument reduction */
    z = x * x;
    w = z * z;
    /* break sum from i=0 to 10 aT[i]z**(i+1) into odd and even poly */
    s1 = z * (aT[0] + w * (aT[2] + w * aT[4]));
    s2 = w * (aT[1] + w * aT[3]);
    if (id < 0)
    {
        return x - x * (s1 + s2);
    }
    z = atanhi[id] - ((x * (s1 + s2) - atanlo[id]) - x);
    return sign ? -z : z;
}

/* origin: FreeBSD /usr/src/lib/msun/src/e_atan2f.c */
/*
 * Conversion to float by Ian Lance Taylor, Cygnus Support, ian@cygnus.com.
 */
/*
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 */

static const float pi = 3.1415927410e+00, /* 0x40490fdb */
    pi_lo = -8.7422776573e-08; /* 0xb3bbbd2e */

float clib__atan2f(float y, float x)
{
    float z;
    uint m, ix, iy;

    if (isnan(x) || isnan(y))
    {
        return x + y;
    }
    GET_FLOAT_WORD(ix, x);
    GET_FLOAT_WORD(iy, y);
    if (ix == 0x3f800000) /* x=1.0 */
    {
        return clib__atanf(y);
    }
    m = ((iy >> 31) & 1) | ((ix >> 30) & 2); /* 2*sign(x)+sign(y) */
    ix &= 0x7fffffff;
    iy &= 0x7fffffff;

    /* when y = 0 */
    if (iy == 0)
    {
        switch (m)
        {
        case 0:
        case 1:
            return y; /* atan(+-0,+anything)=+-0 */
        case 2:
            return pi; /* atan(+0,-anything) = pi */
        case 3:
            return -pi; /* atan(-0,-anything) =-pi */
        }
    }
    /* when x = 0 */
    if (ix == 0)
    {
        return m & 1 ? -pi / 2 : pi / 2;
    }
    /* when x is INF */
    if (ix == 0x7f800000)
    {
        if (iy == 0x7f800000)
        {
            switch (m)
            {
            case 0:
                return pi / 4; /* atan(+INF,+INF) */
            case 1:
                return -pi / 4; /* atan(-INF,+INF) */
            case 2:
                return 3 * pi / 4; /*atan(+INF,-INF)*/
            case 3:
                return -3 * pi / 4; /*atan(-INF,-INF)*/
            }
        }
        else
        {
            switch (m)
            {
            case 0:
                return 0.0f; /* atan(+...,+INF) */
            case 1:
                return -0.0f; /* atan(-...,+INF) */
            case 2:
                return pi; /* atan(+...,-INF) */
            case 3:
                return -pi; /* atan(-...,-INF) */
            }
        }
    }
    /* |y/x| > 0x1p26 */
    if (ix + (26 << 23) < iy || iy == 0x7f800000)
    {
        return m & 1 ? -pi / 2 : pi / 2;
    }

    /* z = atan(|y/x|) with correct underflow */
    if ((m & 2) && iy + (26 << 23) < ix) /*|y/x| < 0x1p-26, x < 0 */
    {
        z = 0.0;
    }
    else
    {
        z = clib__atanf(clib__fabsf(y / x));
    }
    switch (m)
    {
    case 0:
        return z; /* atan(+,+) */
    case 1:
        return -z; /* atan(-,+) */
    case 2:
        return pi - (z - pi_lo); /* atan(+,-) */
    default: /* case 3 */
        return (z - pi_lo) - pi; /* atan(-,-) */
    }
}

/* origin: FreeBSD /usr/src/lib/msun/src/e_acosf.c */
/*
 * Conversion to float by Ian Lance Taylor, Cygnus Support, ian@cygnus.com.
 */
/*
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 */

static const float pio2_hi = 1.5707962513e+00, /* 0x3fc90fda */
    pio2_lo = 7.5497894159e-08, /* 0x33a22168 */
    pS0 = 1.6666586697e-01, pS1 = -4.2743422091e-02, pS2 = -8.6563630030e-03, qS1 = -7.0662963390e-01;

static float R(float z)
{
    float_t p, q;
    p = z * (pS0 + z * (pS1 + z * pS2));
    q = 1.0f + z * qS1;
    return p / q;
}

float clib__acosf(float x)
{
    float z, w, s, c, df;
    uint hx, ix;

    GET_FLOAT_WORD(hx, x);
    ix = hx & 0x7fffffff;
    /* |x| >= 1 or nan */
    if (ix >= 0x3f800000)
    {
        if (ix == 0x3f800000)
        {
            if (hx >> 31)
            {
                return 2 * pio2_hi + 0x1p-120f;
            }
            return 0;
        }
        return 0 / (x - x);
    }
    /* |x| < 0.5 */
    if (ix < 0x3f000000)
    {
        if (ix <= 0x32800000) /* |x| < 2**-26 */
        {
            return pio2_hi + 0x1p-120f;
        }
        return pio2_hi - (x - (pio2_lo - x * R(x * x)));
    }
    /* x < -0.5 */
    if (hx >> 31)
    {
        z = (1 + x) * 0.5f;
        s = clib__sqrtf(z);
        w = R(z) * s - pio2_lo;
        return 2 * (pio2_hi - (s + w));
    }
    /* x > 0.5 */
    z = (1 - x) * 0.5f;
    s = clib__sqrtf(z);
    GET_FLOAT_WORD(hx, s);
    SET_FLOAT_WORD(df, hx & 0xfffff000);
    c = (z - df * df) / (s + df);
    w = R(z) * s + c;
    return 2 * (df + w);
}

/*
 * Single-precision e^x function.
 *
 * Copyright (c) 2017-2018, Arm Limited.
 * SPDX-License-Identifier: MIT
 */

/*
EXP2F_TABLE_BITS = 5
EXP2F_POLY_ORDER = 3

ULP error: 0.502 (nearest rounding.)
Relative error: 1.69 * 2^-34 in [-ln2/64, ln2/64] (before rounding.)
Wrong count: 170635 (all nearest rounding wrong results with fma.)
Non-nearest ULP error: 1 (rounded ULP error)
*/

#define N (1 << EXP2F_TABLE_BITS)
#define InvLn2N _clib_internal_math__exp2f_data.invln2_scaled
#define T _clib_internal_math__exp2f_data.tab
#define C _clib_internal_math__exp2f_data.poly_scaled

static inline uint top12(float x)
{
    return asuint(x) >> 20;
}

float clib__expf(float x)
{
    uint abstop;
    uint64 ki, t;
    double_t kd, xd, z, r, r2, y, s;

    xd = (double_t)x;
    abstop = top12(x) & 0x7ff;
    if (predict_false(abstop >= top12(88.0f)))
    {
        /* |x| >= 88 or x is nan.  */
        if (asuint(x) == asuint(-INFINITY))
        {
            return 0.0f;
        }
        if (abstop >= top12(INFINITY))
        {
            return x + x;
        }
        if (x > 0x1.62e42ep6f) /* x > log(0x1p128) ~= 88.72 */
        {
            return _clib_internal_math__math_oflowf(0);
        }
        if (x < -0x1.9fe368p6f) /* x < log(0x1p-150) ~= -103.97 */
        {
            return _clib_internal_math__math_uflowf(0);
        }
    }

    /* x*N/Ln2 = k + r with r in [-1/2, 1/2] and int k.  */
    z = InvLn2N * xd;

    /* Round and convert z to int, the result is in [-150*N, 128*N] and
       ideally ties-to-even rule is used, otherwise the magnitude of r
       can be bigger which gives larger approximation error.  */
#if _CLIB_INTERNAL_TOINT_INTRINSICS
    kd = roundtoint(z);
    ki = converttoint(z);
#else
#define SHIFT _clib_internal_math__exp2f_data.shift
    kd = eval_as_double(z + SHIFT);
    ki = asuint64(kd);
    kd -= SHIFT;
#endif
    r = z - kd;

    /* exp(x) = 2^(k/N) * 2^(r/N) ~= s * (C0*r^3 + C1*r^2 + C2*r + 1) */
    t = T[ki % N];
    t += ki << (52 - EXP2F_TABLE_BITS);
    s = asdouble(t);
    z = C[0] * r + C[1];
    r2 = r * r;
    y = C[2] * r + 1;
    y = z * r2 + y;
    y = y * s;
    return eval_as_float(y);
}

#undef N
#undef InvLn2N
#undef T
#undef C
#undef SHIFT
/*
 * Single-precision log function.
 *
 * Copyright (c) 2017-2018, Arm Limited.
 * SPDX-License-Identifier: MIT
 */

/*
LOGF_TABLE_BITS = 4
LOGF_POLY_ORDER = 4

ULP error: 0.818 (nearest rounding.)
Relative error: 1.957 * 2^-26 (before rounding.)
*/

#define T _clib_internal_math__logf_data.tab
#define A _clib_internal_math__logf_data.poly
#define Ln2 _clib_internal_math__logf_data.ln2
#define N (1 << LOGF_TABLE_BITS)
#define OFF 0x3f330000

float clib__logf(float x)
{
    double_t z, r, r2, y, y0, invc, logc;
    uint ix, iz, tmp;
    int k, i;

    ix = asuint(x);
    /* Fix sign of zero with downward rounding when x==1.  */
    if (WANT_ROUNDING && predict_false(ix == 0x3f800000))
    {
        return 0;
    }
    if (predict_false(ix - 0x00800000 >= 0x7f800000 - 0x00800000))
    {
        /* x < 0x1p-126 or inf or nan.  */
        if (ix * 2 == 0)
        {
            return _clib_internal_math__math_divzerof(1);
        }
        if (ix == 0x7f800000) /* log(inf) == inf.  */
        {
            return x;
        }
        if ((ix & 0x80000000) || ix * 2 >= 0xff000000)
        {
            return _clib_internal_math__math_invalidf(x);
        }
        /* x is subnormal, normalize it.  */
        ix = asuint(x * 0x1p23f);
        ix -= 23 << 23;
    }

    /* x = 2^k z; where z is in range [OFF,2*OFF] and exact.
       The range is split into N subintervals.
       The ith subinterval contains z and c is near its center.  */
    tmp = ix - OFF;
    i = (tmp >> (23 - LOGF_TABLE_BITS)) % N;
    k = (int)tmp >> 23; /* arithmetic shift */
    iz = ix - (tmp & 0xff800000);
    invc = T[i].invc;
    logc = T[i].logc;
    z = (double_t)asfloat(iz);

    /* log(x) = log1p(z/c-1) + log(c) + k*Ln2 */
    r = z * invc - 1;
    y0 = logc + (double_t)k * Ln2;

    /* Pipelined polynomial evaluation to approximate log1p(r).  */
    r2 = r * r;
    y = A[1] * r + A[2];
    y = A[0] * r2 + y;
    y = y * r2 + (y0 + r);
    return eval_as_float(y);
}

#undef T
#undef A
#undef Ln2
#undef N
#undef OFF
/*
 * Copyright (c) 2017-2018, Arm Limited.
 * SPDX-License-Identifier: MIT
 */

/*
POWF_LOG2_POLY_ORDER = 5
EXP2F_TABLE_BITS = 5

ULP error: 0.82 (~ 0.5 + relerr*2^24)
relerr: 1.27 * 2^-26 (Relative error ~= 128*Ln2*relerr_log2 + relerr_exp2)
relerr_log2: 1.83 * 2^-33 (Relative error of logx.)
relerr_exp2: 1.69 * 2^-34 (Relative error of exp2(ylogx).)
*/

#define N (1 << POWF_LOG2_TABLE_BITS)
#define T _clib_internal_math__powf_log2_data.tab
#define A _clib_internal_math__powf_log2_data.poly
#define OFF 0x3f330000

/* Subnormal input is normalized so ix has negative biased exponent.
   Output is multiplied by N (POWF_SCALE) if TOINT_INTRINICS is set.  */
static inline double_t log2_inline(uint ix)
{
    double_t z, r, r2, r4, p, q, y, y0, invc, logc;
    uint iz, top, tmp;
    int k, i;

    /* x = 2^k z; where z is in range [OFF,2*OFF] and exact.
       The range is split into N subintervals.
       The ith subinterval contains z and c is near its center.  */
    tmp = ix - OFF;
    i = (tmp >> (23 - POWF_LOG2_TABLE_BITS)) % N;
    top = tmp & 0xff800000;
    iz = ix - top;
    k = (int)top >> (23 - POWF_SCALE_BITS); /* arithmetic shift */
    invc = T[i].invc;
    logc = T[i].logc;
    z = (double_t)asfloat(iz);

    /* log2(x) = log1p(z/c-1)/ln2 + log2(c) + k */
    r = z * invc - 1;
    y0 = logc + (double_t)k;

    /* Pipelined polynomial evaluation to approximate log1p(r)/ln2.  */
    r2 = r * r;
    y = A[0] * r + A[1];
    p = A[2] * r + A[3];
    r4 = r2 * r2;
    q = A[4] * r + y0;
    q = p * r2 + q;
    y = y * r4 + q;
    return y;
}

#undef N
#undef T
#define N (1 << EXP2F_TABLE_BITS)
#define T _clib_internal_math__exp2f_data.tab
#define SIGN_BIAS (1 << (EXP2F_TABLE_BITS + 11))

/* The output of log2 and thus the input of exp2 is either scaled by N
   (in case of fast _clib_internal_math_powf_toint intrinsics) or not.  The unscaled xd must be
   in [-1021,1023], sign_bias sets the sign of the result.  */
static inline float exp2_inline(double_t xd, uint sign_bias)
{
    uint64 ki, ski, t;
    double_t kd, z, r, r2, y, s;

#if _CLIB_INTERNAL_TOINT_INTRINSICS
#define C _clib_internal_math__exp2f_data.poly_scaled
    /* N*x = k + r with r in [-1/2, 1/2] */
    kd = roundtoint(xd); /* k */
    ki = converttoint(xd);
#else
#define C _clib_internal_math__exp2f_data.poly
#define SHIFT _clib_internal_math__exp2f_data.shift_scaled
    /* x = k/N + r with r in [-1/(2N), 1/(2N)] */
    kd = eval_as_double(xd + SHIFT);
    ki = asuint64(kd);
    kd -= SHIFT; /* k/N */
#endif
    r = xd - kd;

    /* exp2(x) = 2^(k/N) * 2^r ~= s * (C0*r^3 + C1*r^2 + C2*r + 1) */
    t = T[ki % N];
    ski = ki + sign_bias;
    t += ski << (52 - EXP2F_TABLE_BITS);
    s = asdouble(t);
    z = C[0] * r + C[1];
    r2 = r * r;
    y = C[2] * r + 1;
    y = z * r2 + y;
    y = y * s;
    return eval_as_float(y);
}

/* Returns 0 if not int, 1 if odd int, 2 if even int.  The argument is
   the bit representation of a non-zero finite floating-point value.  */
static inline int checkint(uint iy)
{
    int e = iy >> 23 & 0xff;
    if (e < 0x7f)
    {
        return 0;
    }
    if (e > 0x7f + 23)
    {
        return 2;
    }
    if (iy & ((1 << (0x7f + 23 - e)) - 1))
    {
        return 0;
    }
    if (iy & (1 << (0x7f + 23 - e)))
    {
        return 1;
    }
    return 2;
}

static inline int zeroinfnan(uint ix)
{
    return 2 * ix - 1 >= 2u * 0x7f800000 - 1;
}

float clib__powf(float x, float y)
{
    uint sign_bias = 0;
    uint ix, iy;

    ix = asuint(x);
    iy = asuint(y);
    if (predict_false(ix - 0x00800000 >= 0x7f800000 - 0x00800000 || zeroinfnan(iy)))
    {
        /* Either (x < 0x1p-126 or inf or nan) or (y is 0 or inf or nan).  */
        if (predict_false(zeroinfnan(iy)))
        {
            if (2 * iy == 0)
            {
                return issignalingf_inline(x) ? x + y : 1.0f;
            }
            if (ix == 0x3f800000)
            {
                return issignalingf_inline(y) ? x + y : 1.0f;
            }
            if (2 * ix > 2u * 0x7f800000 || 2 * iy > 2u * 0x7f800000)
            {
                return x + y;
            }
            if (2 * ix == 2 * 0x3f800000)
            {
                return 1.0f;
            }
            if ((2 * ix < 2 * 0x3f800000) == !(iy & 0x80000000))
            {
                return 0.0f; /* |x|<1 && y==inf or |x|>1 && y==-inf.  */
            }
            return y * y;
        }
        if (predict_false(zeroinfnan(ix)))
        {
            float_t x2 = x * x;
            if (ix & 0x80000000 && checkint(iy) == 1)
            {
                x2 = -x2;
            }
            /* Without the barrier some versions of clang hoist the 1/x2 and
               thus division by zero exception can be signaled spuriously.  */
            return iy & 0x80000000 ? fp_barrierf(1 / x2) : x2;
        }
        /* x and y are non-zero finite.  */
        if (ix & 0x80000000)
        {
            /* Finite x < 0.  */
            int yint = checkint(iy);
            if (yint == 0)
            {
                return _clib_internal_math__math_invalidf(x);
            }
            if (yint == 1)
            {
                sign_bias = SIGN_BIAS;
            }
            ix &= 0x7fffffff;
        }
        if (ix < 0x00800000)
        {
            /* Normalize subnormal x so exponent becomes negative.  */
            ix = asuint(x * 0x1p23f);
            ix &= 0x7fffffff;
            ix -= 23 << 23;
        }
    }
    double_t logx = log2_inline(ix);
    double_t ylogx = y * logx; /* cannot overflow, y is single prec.  */
    if (predict_false((asuint64(ylogx) >> 47 & 0xffff) >= asuint64(126.0 * POWF_SCALE) >> 47))
    {
        /* |y*log(x)| >= 126.  */
        if (ylogx > 0x1.fffffffd1d571p+6 * POWF_SCALE)
        {
            return _clib_internal_math__math_oflowf(sign_bias);
        }
        if (ylogx <= -150.0 * POWF_SCALE)
        {
            return _clib_internal_math__math_uflowf(sign_bias);
        }
    }
    return exp2_inline(ylogx, sign_bias);
}

#undef N
#undef T
#undef A
#undef OFF
#undef N
#undef T
#undef SIGN_BIAS
#undef C
#undef SHIFT
#if CLIB_USE_HARDWARE_SQRT && defined(ARCHITECTURE_SSE2)
#include <emmintrin.h>
float clib__sqrtf(float x)
{
    return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x)));
}
double clib__sqrt(double x)
{
    return _mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(x)));
}
#elif CLIB_ENABLE_ASSEMBLY_OPTIMIZATIONS && CLIB_USE_HARDWARE_SQRT && defined(ARCHITECTURE_ARM64_SQRT_ASM)
float clib__sqrtf(float x)
{
    float y;
    __asm__("fsqrt %s0, %s1" : "=w"(y) : "w"(x));
    return y;
}
double clib__sqrt(double x)
{
    double y;
    __asm__("fsqrt %d0, %d1" : "=w"(y) : "w"(x));
    return y;
}
#else

#define FENV_SUPPORT 1

static inline uint _clib_internal_math_sqrtf_mul32(uint a, uint b)
{
    return (uint64)a * b >> 32;
}

/* see clib__sqrt.c for more detailed comments.  */

float clib__sqrtf(float x)
{
    uint ix, m, m1, m0, even, ey;

    ix = asuint(x);
    if (predict_false(ix - 0x00800000 >= 0x7f800000 - 0x00800000))
    {
        /* x < 0x1p-126 or inf or nan.  */
        if (ix * 2 == 0)
        {
            return x;
        }
        if (ix == 0x7f800000)
        {
            return x;
        }
        if (ix > 0x7f800000)
        {
            return _clib_internal_math__math_invalidf(x);
        }
        /* x is subnormal, normalize it.  */
        ix = asuint(x * 0x1p23f);
        ix -= 23 << 23;
    }

    /* x = 4^e m; with int e and m in [1, 4).  */
    even = ix & 0x00800000;
    m1 = (ix << 8) | 0x80000000;
    m0 = (ix << 7) & 0x7fffffff;
    m = even ? m0 : m1;

    /* 2^e is the exponent part of the return value.  */
    ey = ix >> 1;
    ey += 0x3f800000 >> 1;
    ey &= 0x7f800000;

    /* compute r ~ 1/clib__sqrt(m), s ~ clib__sqrt(m) with 2 goldschmidt iterations.  */
    static const uint three = 0xc0000000;
    uint r, s, d, u, i;
    i = (ix >> 17) % 128;
    r = (uint)_clib_internal_math__rsqrt_tab[i] << 16;
    /* |r*clib__sqrt(m) - 1| < 0x1p-8 */
    s = _clib_internal_math_sqrtf_mul32(m, r);
    /* |s/clib__sqrt(m) - 1| < 0x1p-8 */
    d = _clib_internal_math_sqrtf_mul32(s, r);
    u = three - d;
    r = _clib_internal_math_sqrtf_mul32(r, u) << 1;
    /* |r*clib__sqrt(m) - 1| < 0x1.7bp-16 */
    s = _clib_internal_math_sqrtf_mul32(s, u) << 1;
    /* |s/clib__sqrt(m) - 1| < 0x1.7bp-16 */
    d = _clib_internal_math_sqrtf_mul32(s, r);
    u = three - d;
    s = _clib_internal_math_sqrtf_mul32(s, u);
    /* -0x1.03p-28 < s/clib__sqrt(m) - 1 < 0x1.fp-31 */
    s = (s - 1) >> 6;
    /* s < clib__sqrt(m) < s + 0x1.08p-23 */

    /* compute nearest rounded result.  */
    uint d0, d1, d2;
    float y, t;
    d0 = (m << 16) - s * s;
    d1 = s - d0;
    d2 = d1 + s + 1;
    s += d1 >> 31;
    s &= 0x007fffff;
    s |= ey;
    y = asfloat(s);
    if (FENV_SUPPORT)
    {
        /* handle rounding and inexact exception. */
        uint tiny = predict_false(d2 == 0) ? 0 : 0x01000000;
        tiny |= (d1 ^ d2) & 0x80000000;
        t = asfloat(tiny);
        y = eval_as_float(y + t);
    }
    return y;
}

#undef FENV_SUPPORT

#define FENV_SUPPORT 1

/* returns a*b*2^-32 - e, with error 0 <= e < 1.  */
static inline uint mul32(uint a, uint b)
{
    return (uint64)a * b >> 32;
}

/* returns a*b*2^-64 - e, with error 0 <= e < 3.  */
static inline uint64 mul64(uint64 a, uint64 b)
{
    uint64 ahi = a >> 32;
    uint64 alo = a & 0xffffffff;
    uint64 bhi = b >> 32;
    uint64 blo = b & 0xffffffff;
    return ahi * bhi + (ahi * blo >> 32) + (alo * bhi >> 32);
}

double clib__sqrt(double x)
{
    uint64 ix, top, m;

    /* special case handling.  */
    ix = asuint64(x);
    top = ix >> 52;
    if (predict_false(top - 0x001 >= 0x7ff - 0x001))
    {
        /* x < 0x1p-1022 or inf or nan.  */
        if (ix * 2 == 0)
        {
            return x;
        }
        if (ix == 0x7ff0000000000000)
        {
            return x;
        }
        if (ix > 0x7ff0000000000000)
        {
            return _clib_internal_math__math_invalid(x);
        }
        /* x is subnormal, normalize it.  */
        ix = asuint64(x * 0x1p52);
        top = ix >> 52;
        top -= 52;
    }

    /* argument reduction:
       x = 4^e m; with integer e, and m in [1, 4)
       m: fixed point representation [2.62]
       2^e is the exponent part of the result.  */
    int even = top & 1;
    m = (ix << 11) | 0x8000000000000000;
    if (even)
    {
        m >>= 1;
    }
    top = (top + 0x3ff) >> 1;

    /* approximate r ~ 1/clib__sqrt(m) and s ~ clib__sqrt(m) when m in [1,4)

       initial estimate:
       7bit table lookup (1bit exponent and 6bit significand).

       iterative approximation:
       using 2 goldschmidt iterations with 32bit int arithmetics
       and a final iteration with 64bit int arithmetics.

       details:

       the relative error (e = r0 clib__sqrt(m)-1) of a linear estimate
       (r0 = a m + b) is |e| < 0.085955 ~ 0x1.6p-4 at best,
       a table lookup is faster and needs one less iteration
       6 bit lookup table (128b) gives |e| < 0x1.f9p-8
       7 bit lookup table (256b) gives |e| < 0x1.fdp-9
       for single and double prec 6bit is enough but for quad
       prec 7bit is needed (or modified iterations). to avoid
       one more iteration >=13bit table would be needed (16k).

       a newton-raphson iteration for r is
         w = r*r
         u = 3 - m*w
         r = r*u/2
       can use a goldschmidt iteration for s at the end or
         s = m*r

       first goldschmidt iteration is
         s = m*r
         u = 3 - s*r
         r = r*u/2
         s = s*u/2
       next goldschmidt iteration is
         u = 3 - s*r
         r = r*u/2
         s = s*u/2
       and at the end r is not computed only s.

       they use the same amount of operations and converge at the
       same quadratic rate, i.e. if
         r1 clib__sqrt(m) - 1 = e, then
         r2 clib__sqrt(m) - 1 = -3/2 e^2 - 1/2 e^3
       the advantage of goldschmidt is that the mul for s and r
       are independent (computed in parallel), however it is not
       "self synchronizing": it only uses the input m in the
       first iteration so rounding errors accumulate. at the end
       or when switching to larger precision arithmetics rounding
       errors dominate so the first iteration should be used.

       the fixed point representations are
         m: 2.30 r: 0.32, s: 2.30, d: 2.30, u: 2.30, three: 2.30
       and after switching to 64 bit
         m: 2.62 r: 0.64, s: 2.62, d: 2.62, u: 2.62, three: 2.62  */

    static const uint64 three = 0xc0000000;
    uint64 r, s, d, u, i;

    i = (ix >> 46) % 128;
    r = (uint)_clib_internal_math__rsqrt_tab[i] << 16;
    /* |r clib__sqrt(m) - 1| < 0x1.fdp-9 */
    s = mul32(m >> 32, r);
    /* |s/clib__sqrt(m) - 1| < 0x1.fdp-9 */
    d = mul32(s, r);
    u = three - d;
    r = mul32(r, u) << 1;
    /* |r clib__sqrt(m) - 1| < 0x1.7bp-16 */
    s = mul32(s, u) << 1;
    /* |s/clib__sqrt(m) - 1| < 0x1.7bp-16 */
    d = mul32(s, r);
    u = three - d;
    r = mul32(r, u) << 1;
    /* |r clib__sqrt(m) - 1| < 0x1.3704p-29 (measured worst-case) */
    r = r << 32;
    s = mul64(m, r);
    d = mul64(s, r);
    u = (three << 32) - d;
    s = mul64(s, u); /* repr: 3.61 */
    /* -0x1p-57 < s - clib__sqrt(m) < 0x1.8001p-61 */
    s = (s - 2) >> 9; /* repr: 12.52 */
    /* -0x1.09p-52 < s - clib__sqrt(m) < -0x1.fffcp-63 */

    /* s < clib__sqrt(m) < s + 0x1.09p-52,
       compute nearest rounded result:
       the nearest result to 52 bits is either s or s+0x1p-52,
       we can decide by comparing (2^52 s + 0.5)^2 to 2^104 m.  */
    uint64 d0, d1, d2;
    double y, t;
    d0 = (m << 42) - s * s;
    d1 = s - d0;
    d2 = d1 + s + 1;
    s += d1 >> 63;
    s &= 0x000fffffffffffff;
    s |= top << 52;
    y = asdouble(s);
    if (FENV_SUPPORT)
    {
        /* handle rounding modes and inexact exception:
           only (s+1)^2 == 2^42 m case is exact otherwise
           add a tiny value to cause the fenv effects.  */
        uint64 tiny = predict_false(d2 == 0) ? 0 : 0x0010000000000000;
        tiny |= (d1 ^ d2) & 0x8000000000000000;
        t = asdouble(tiny);
        y = eval_as_double(y + t);
    }
    return y;
}

#undef FENV_SUPPORT
#endif
#undef isnan
#undef INFINITY
#undef hidden
#undef M_PI_2

// ===== string building + float parsing ====================================
// Added so that path composition and integer labels no longer need
// snprintf, and the style / attribute parsers no longer need atof or
// strtof. Float parsing is decimal only (no hex floats, inf or nan) and
// accurate to float32, which is all the parsers need.

/**
 * @brief appends src to the string in dst, never writing past dst_cap bytes and always leaving dst terminated
 *
 * @param char* dst -> terminated destination string
 * @param int64 dst_cap -> capacity of dst in bytes, terminator included
 * @param const char* src -> string to append
 *
 * @return int64 -> the length the joined string would have without truncation; a value >= dst_cap means it was cut
 */
static int64 clib__strcat_safe(char *dst, int64 dst_cap, const char *src)
{
    int64 dst_len = 0;
    int64 src_len = clib__strlen(src);
    int64 room = 0;
    int64 copy_len = 0;
    int64 i = 0;

    if (dst == NULL_POINTER || dst_cap <= 0)
    {
        return src_len;
    }

    while (dst_len < dst_cap && dst[dst_len] != 0)
    {
        dst_len = dst_len + 1;
    }

    if (dst_len >= dst_cap)
    {
        // dst is not terminated within its capacity; terminate it and report the overflow
        dst[dst_cap - 1] = 0;
        return dst_cap + src_len;
    }

    room = dst_cap - 1 - dst_len;
    copy_len = (src_len < room) ? src_len : room;

    for (i = 0; i < copy_len; i++)
    {
        dst[dst_len + i] = src[i];
    }

    dst[dst_len + copy_len] = 0;

    return dst_len + src_len;
}

/**
 * @brief writes a signed 64-bit integer in decimal
 *
 * @param char* dst -> destination buffer, terminated on return when dst_cap > 0
 * @param int64 dst_cap -> capacity of dst in bytes, terminator included
 * @param int64 value -> the number
 *
 * @return int64 -> the number of characters the full text has; a value >= dst_cap means it was cut
 */
const char *clib__strrchr(const char *s, int ch)
{
    const char *found = NULL_POINTER;

    if (s == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    // walk to the terminator, remembering the last hit; the terminator
    // itself matches when ch is 0, as strrchr specifies
    while (*s != 0)
    {
        if (*s == (char)ch)
        {
            found = s;
        }
        s++;
    }
    if ((char)ch == 0)
    {
        return s;
    }
    return found;
}

const void *clib__memchr(const void *block, int ch, int64 count)
{
    const ubyte *bytes = (const ubyte *)block;
    int64 i = 0;

    if (block == NULL_POINTER || count <= 0)
    {
        return NULL_POINTER;
    }
    for (i = 0; i < count; i++)
    {
        if (bytes[i] == (ubyte)ch)
        {
            return (const void *)(bytes + i);
        }
    }
    return NULL_POINTER;
}

static int64 clib__strncpy_safe(char *dst, int64 dst_cap, const char *src, int64 count)
{
    int64 written = 0;

    if (dst == NULL_POINTER || dst_cap <= 0)
    {
        return 0;
    }
    if (src != NULL_POINTER)
    {
        while (written < count && written < dst_cap - 1 && src[written] != 0)
        {
            dst[written] = src[written];
            written++;
        }
    }
    dst[written] = 0;
    return written;
}

// Project-local formatter; no runtime or external formatter dependency.
// Project-local formatting. Included by stdlib.c, with no external library.
// Supported conversions and limits are documented in stdlib.h.
typedef struct _clib_internal_format_output
{
    char *dst;
    int64 cap;
    int64 at;
} _clib_internal_format_output;

static void _clib_internal_format_repeat(_clib_internal_format_output *out, char c, int64 n)
{
    if (n <= 0)
    {
        return;
    }
    int64 available = out->dst && out->cap > out->at + 1 ? out->cap - out->at - 1 : 0;
    int64 written = n < available ? n : available;
    if (written)
    {
        clib__memset(out->dst + out->at, c, written);
    }
    out->at += n;
}
static void _clib_internal_format_text(_clib_internal_format_output *out, const char *s, int64 n)
{
    if (n <= 0)
    {
        return;
    }
    int64 available = out->dst && out->cap > out->at + 1 ? out->cap - out->at - 1 : 0;
    int64 written = n < available ? n : available;
    if (written)
    {
        clib__memcpy(out->dst + out->at, s, written);
    }
    out->at += n;
}
static void _clib_internal_format_number(_clib_internal_format_output *out, uint64 value, int base, int upper, char sign, int alternate, int64 width, int64 precision, int left, int zero)
{
    char digits[64], prefix[3];
    int n = 0, prefix_n = 0;
    const char *alphabet = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    uint64 original = value;
    if (value || precision != 0)
    {
        do
        {
            uint64 digit;
            value = clib__udivmod64(value, (uint)base, &digit);
            digits[n++] = alphabet[digit];
        } while (value);
    }
    if (sign)
    {
        prefix[prefix_n++] = sign;
    }
    if (alternate && base == 16 && original)
    {
        prefix[prefix_n++] = '0';
        prefix[prefix_n++] = upper ? 'X' : 'x';
    }
    if (alternate && base == 8 && (n == 0 || digits[n - 1] != '0') && precision <= n)
    {
        precision = n + 1;
    }
    int64 leading = precision > n ? precision - n : 0;
    int64 padding = width - prefix_n - leading - n;
    if (padding < 0)
    {
        padding = 0;
    }
    if (!left && (!zero || precision >= 0))
    {
        _clib_internal_format_repeat(out, ' ', padding);
    }
    _clib_internal_format_text(out, prefix, prefix_n);
    if (!left && zero && precision < 0)
    {
        _clib_internal_format_repeat(out, '0', padding);
    }
    _clib_internal_format_repeat(out, '0', leading);
    while (n)
    {
        _clib_internal_format_text(out, &digits[--n], 1);
    }
    if (left)
    {
        _clib_internal_format_repeat(out, ' ', padding);
    }
}

// Convert the exact binary64 rational to decimal using base-1e9 limbs. A
// binary64 needs at most 767 significant decimal digits, including subnormals.
// Multiplication by 2 or 5 never overflows uint64. Typical UI numbers need only
// a few limbs; no heap and no float-to-integer conversion of an unchecked value.
static int _clib_internal_format_decimal(double value, char *digits, int *point)
{
    union
    {
        double d;
        uint64 u;
    } bits = { value };
    uint64 mantissa = bits.u & 0xfffffffffffffULL;
    int exponent = (int)((bits.u >> 52) & 2047);
    if (exponent)
    {
        mantissa |= 1ULL << 52;
        exponent -= 1075;
    }
    else
    {
        exponent = -1074;
    }
    if (!mantissa)
    {
        digits[0] = '0';
        *point = 1;
        return 1;
    }
    uint limbs[96];
    int count = 0;
    do
    {
        uint64 digit;
        mantissa = clib__udivmod64(mantissa, 1000000000u, &digit);
        limbs[count++] = (uint)digit;
    } while (mantissa);
    int scale = exponent < 0 ? -exponent : 0;
    uint factor = exponent < 0 ? 5 : 2;
    int steps = exponent < 0 ? -exponent : exponent;
    for (int i = 0; i < steps; i++)
    {
        uint64 carry = 0;
        for (int j = 0; j < count; j++)
        {
            uint64 next = (uint64)limbs[j] * factor + carry;
            uint64 digit;
            carry = clib__udivmod64(next, 1000000000u, &digit);
            limbs[j] = (uint)digit;
        }
        if (carry)
        {
            limbs[count++] = (uint)carry;
        }
    }
    int n = 0;
    for (int j = count - 1; j >= 0; j--)
    {
        uint divisor = 100000000;
        if (j == count - 1)
        {
            while (divisor > 1 && limbs[j] < divisor)
            {
                divisor /= 10;
            }
        }
        while (divisor)
        {
            digits[n++] = (char)('0' + limbs[j] / divisor % 10);
            divisor /= 10;
        }
    }
    *point = n - scale;
    return n;
}
static void _clib_internal_format_float(_clib_internal_format_output *out, double value, int upper, char positive_sign, int alternate, int64 width, int64 precision, int left, int zero)
{
    union
    {
        double d;
        uint64 u;
    } bits = { value };
    char sign = (bits.u >> 63) ? '-' : positive_sign;
    bits.u &= 0x7fffffffffffffffULL;
    if ((bits.u >> 52) == 2047)
    {
        const char *text = (bits.u & 0xfffffffffffffULL) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        int64 pad = width - 3 - (sign != 0);
        if (!left)
        {
            _clib_internal_format_repeat(out, ' ', pad);
        }
        if (sign)
        {
            _clib_internal_format_text(out, &sign, 1);
        }
        _clib_internal_format_text(out, text, 3);
        if (left)
        {
            _clib_internal_format_repeat(out, ' ', pad);
        }
        return;
    }
    if (precision < 0)
    {
        precision = 6;
    }
    char digits[800];
    int point;
    int n = _clib_internal_format_decimal(bits.d, digits, &point);
    int64 keep = (int64)point + precision;
    if (keep < n)
    {
        int up = 0;
        if (keep >= 0)
        {
            int next = digits[keep] - '0', sticky = 0;
            for (int i = (int)keep + 1; i < n; i++)
            {
                sticky |= digits[i] != '0';
            }
            up = next > 5 || (next == 5 && (sticky || (keep && ((digits[keep - 1] - '0') & 1))));
        }
        if (keep <= 0)
        {
            digits[0] = up ? '1' : '0';
            n = 1;
            point = up ? 1 - (int)precision : 1;
        }
        else
        {
            n = (int)keep;
            int i = n - 1;
            while (up && i >= 0 && digits[i] == '9')
            {
                digits[i--] = '0';
            }
            if (up && i >= 0)
            {
                digits[i]++;
            }
            else if (up)
            {
                digits[0] = '1';
                for (i = 1; i <= n; i++)
                {
                    digits[i] = '0';
                }
                n++;
                point++;
            }
        }
    }
    int64 integers = point > 0 ? point : 1;
    int dot = precision > 0 || alternate;
    int64 pad = width - integers - precision - dot - (sign != 0);
    if (!left && !zero)
    {
        _clib_internal_format_repeat(out, ' ', pad);
    }
    if (sign)
    {
        _clib_internal_format_text(out, &sign, 1);
    }
    if (!left && zero)
    {
        _clib_internal_format_repeat(out, '0', pad);
    }
    if (point <= 0)
    {
        _clib_internal_format_repeat(out, '0', 1);
    }
    else
    {
        int take = point < n ? point : n;
        _clib_internal_format_text(out, digits, take);
        _clib_internal_format_repeat(out, '0', point - take);
    }
    if (dot)
    {
        _clib_internal_format_text(out, ".", 1);
    }
    int64 remaining = precision;
    if (point < 0)
    {
        int64 zeros = -(int64)point < remaining ? -(int64)point : remaining;
        _clib_internal_format_repeat(out, '0', zeros);
        remaining -= zeros;
    }
    int start = point > 0 ? point : 0;
    int64 take = start < n ? n - start : 0;
    if (take > remaining)
    {
        take = remaining;
    }
    if (take)
    {
        _clib_internal_format_text(out, digits + start, take);
    }
    _clib_internal_format_repeat(out, '0', remaining - take);
    if (left)
    {
        _clib_internal_format_repeat(out, ' ', pad);
    }
}

static int64 clib__vformat(char *dst, int64 cap, const char *fmt, va_list args)
{
    _clib_internal_format_output out = { dst, cap > 0 ? cap : 0, 0 };
    if (!fmt)
    {
        fmt = "";
    }
    while (*fmt)
    {
        if (*fmt != '%')
        {
            _clib_internal_format_text(&out, fmt++, 1);
            continue;
        }
        fmt++;
        int left = 0, zero = 0, alternate = 0;
        char sign = 0;
        for (;;)
        {
            if (*fmt == '-')
            {
                left = 1;
            }
            else if (*fmt == '0')
            {
                zero = 1;
            }
            else if (*fmt == '+')
            {
                sign = '+';
            }
            else if (*fmt == ' ')
            {
                if (!sign)
                {
                    sign = ' ';
                }
            }
            else if (*fmt == '#')
            {
                alternate = 1;
            }
            else
            {
                break;
            }
            fmt++;
        }
        int64 width = 0, precision = -1;
        if (*fmt == '*')
        {
            width = va_arg(args, int);
            fmt++;
            if (width < 0)
            {
                left = 1;
                width = -width;
            }
        }
        else
        {
            while (*fmt >= '0' && *fmt <= '9')
            {
                if (width < INT_MAX)
                {
                    width = width * 10 + (*fmt - '0');
                }
                fmt++;
            }
        }
        if (width > INT_MAX)
        {
            width = INT_MAX;
        }
        if (*fmt == '.')
        {
            fmt++;
            precision = 0;
            if (*fmt == '*')
            {
                precision = va_arg(args, int);
                fmt++;
                if (precision < 0)
                {
                    precision = -1;
                }
            }
            else
            {
                while (*fmt >= '0' && *fmt <= '9')
                {
                    if (precision < INT_MAX)
                    {
                        precision = precision * 10 + (*fmt - '0');
                    }
                    fmt++;
                }
            }
            if (precision > INT_MAX)
            {
                precision = INT_MAX;
            }
        }
        char length = 0;
        if (*fmt == 'l' || *fmt == 'h' || *fmt == 'j' || *fmt == 'z' || *fmt == 't')
        {
            length = *fmt++;
            if (length == 'l' && *fmt == 'l')
            {
                length = 'q';
                fmt++;
            }
            if (length == 'h' && *fmt == 'h')
            {
                length = 'b';
                fmt++;
            }
        }
        char conversion = *fmt;
        if (!conversion)
        {
            break;
        }
        fmt++;
        if (conversion == 's' || conversion == 'c' || conversion == '%')
        {
            char c;
            const char *text;
            int64 n = 0;
            if (conversion == 's')
            {
                text = va_arg(args, const char *);
                if (!text)
                {
                    text = "(null)";
                }
                while ((precision < 0 || n < precision) && text[n])
                {
                    n++;
                }
            }
            else
            {
                c = conversion == '%' ? '%' : (char)va_arg(args, int);
                text = &c;
                n = 1;
            }
            if (!left)
            {
                _clib_internal_format_repeat(&out, ' ', width - n);
            }
            _clib_internal_format_text(&out, text, n);
            if (left)
            {
                _clib_internal_format_repeat(&out, ' ', width - n);
            }
        }
        else if (conversion == 'f' || conversion == 'F')
        {
            _clib_internal_format_float(&out, va_arg(args, double), conversion == 'F', sign, alternate, width, precision, left, zero);
        }
        else if (conversion == 'd' || conversion == 'i' || conversion == 'u' || conversion == 'x' || conversion == 'X' || conversion == 'o' || conversion == 'p')
        {
            uint64 value;
            int is_signed = conversion == 'd' || conversion == 'i';
            if (conversion == 'p')
            {
                value = (nuint)va_arg(args, void *);
                alternate = 1;
            }
            else if (is_signed)
            {
                int64 v;
                if (length == 'q' || length == 'j')
                {
                    v = va_arg(args, int64);
                }
                else if (length == 'l')
                {
                    v = va_arg(args, long);
                }
                else if (length == 'z' || length == 't')
                {
                    v = va_arg(args, nint);
                }
                else
                {
                    int i = va_arg(args, int);
                    v = length == 'h' ? (short)i : length == 'b' ? (signed char)i : i;
                }
                value = v < 0 ? 0ULL - (uint64)v : (uint64)v;
                if (v < 0)
                {
                    sign = '-';
                }
            }
            else
            {
                sign = 0;
                if (length == 'q' || length == 'j')
                {
                    value = va_arg(args, uint64);
                }
                else if (length == 'l')
                {
                    value = va_arg(args, unsigned long);
                }
                else if (length == 'z' || length == 't')
                {
                    value = va_arg(args, nuint);
                }
                else
                {
                    uint v = va_arg(args, uint);
                    value = length == 'h' ? (ushort)v : length == 'b' ? (ubyte)v : v;
                }
            }
            int base = conversion == 'o' ? 8 : (conversion == 'x' || conversion == 'X' || conversion == 'p') ? 16 : 10;
            _clib_internal_format_number(&out, value, base, conversion == 'X', sign, alternate, width, precision, left, zero);
        }
        else
        {
            _clib_internal_format_text(&out, "%", 1);
            _clib_internal_format_text(&out, &conversion, 1);
        }
    }
    if (dst && cap > 0)
    {
        dst[out.at < cap ? out.at : cap - 1] = 0;
    }
    return out.at;
}

static int64 clib__format(char *dst, int64 dst_cap, const char *fmt, ...)
{
    va_list args;
    int64 written = 0;

    va_start(args, fmt);
    written = clib__vformat(dst, dst_cap, fmt, args);
    va_end(args);
    return written;
}

static int64 clib__u64toa_hex(char *dst, int64 dst_cap, uint64 value)
{
    char digits[17];
    int64 length = 0;
    int64 i = 0;

    clib__null_memory(digits, sizeof(digits));
    if (dst == NULL_POINTER || dst_cap <= 0)
    {
        return 0;
    }
    dst[0] = 0;
    if (dst_cap == 1)
    {
        return 0;
    }
    // the digits come out backwards, so they are collected and reversed
    do
    {
        digits[length] = "0123456789abcdef"[(int)(value & 0xFull)];
        value >>= 4;
        length++;
    } while (value != 0 && length < 16);
    if (length + 1 > dst_cap)
    {
        return 0;
    }
    for (i = 0; i < length; i++)
    {
        dst[i] = digits[length - 1 - i];
    }
    dst[length] = 0;
    return length;
}

static const char *clib__getenv(const char *name)
{
    (void)name;
    return NULL_POINTER;
}

static int64 clib__i64toa(char *dst, int64 dst_cap, int64 value)
{
    char reversed[24];
    uint64 magnitude = 0;
    int64 n = 0;
    int64 copy_len = 0;
    int64 i = 0;
    boole negative = FALSE;

    if (value < 0)
    {
        negative = TRUE;
        // two steps so the most negative value does not overflow on negation
        magnitude = (uint64)(-(value + 1)) + 1ull;
    }
    else
    {
        magnitude = (uint64)value;
    }

    do
    {
        uint64 digit;
        magnitude = clib__udivmod64(magnitude, 10, &digit);
        reversed[n] = (char)('0' + (int)digit);
        n = n + 1;
    } while (magnitude != 0ull);

    if (negative == TRUE)
    {
        reversed[n] = '-';
        n = n + 1;
    }

    if (dst == NULL_POINTER || dst_cap <= 0)
    {
        return n;
    }

    copy_len = (n < dst_cap - 1) ? n : dst_cap - 1;

    for (i = 0; i < copy_len; i++)
    {
        dst[i] = reversed[n - 1 - i];
    }

    dst[copy_len] = 0;

    return n;
}

/**
 * @brief parses a decimal floating point number: leading whitespace, optional sign, digits, optional fraction, optional e/E exponent
 *
 * @param const char* s -> text to parse
 * @param char** endptr -> receives the first character not consumed, or s itself when nothing was parsed; may be NULL_POINTER
 *
 * @return double -> the value, 0.0 when nothing was parsed
 */
double clib__strtod(const char *s, char **endptr)
{
    const char *p = s;
    uint64 mantissa = 0;
    int64 exponent = 0, explicit_exponent = 0;
    int negative = 0, fractional = 0, any = 0, significant = 0;
    int first_dropped = -1, sticky = 0, exponent_negative = 0;
    static const double powers[] = { 1e1, 1e2, 1e4, 1e8, 1e16, 1e32, 1e64, 1e128, 1e256 };
    if (endptr)
    {
        *endptr = (char *)s;
    }
    if (!s)
    {
        return 0.0;
    }
    while (clib__isspace((ubyte)*p))
    {
        p++;
    }
    if (*p == '-' || *p == '+')
    {
        negative = *p++ == '-';
    }
    for (;; p++)
    {
        if (*p == '.' && !fractional)
        {
            fractional = 1;
            continue;
        }
        if (*p < '0' || *p > '9')
        {
            break;
        }
        int digit = *p - '0';
        any = 1;
        if (!significant && !digit)
        {
            if (fractional && exponent > -1000000000)
            {
                exponent--;
            }
        }
        else if (significant < 19)
        {
            mantissa = mantissa * 10 + (uint)digit;
            significant++;
            if (fractional)
            {
                exponent--;
            }
        }
        else
        {
            if (!fractional && exponent < 1000000000)
            {
                exponent++;
            }
            if (first_dropped < 0)
            {
                first_dropped = digit;
            }
            else
            {
                sticky |= digit;
            }
        }
    }
    if (!any)
    {
        return 0.0;
    }
    if (*p == 'e' || *p == 'E')
    {
        const char *mark = p++;
        if (*p == '-' || *p == '+')
        {
            exponent_negative = *p++ == '-';
        }
        const char *digits = p;
        while (*p >= '0' && *p <= '9')
        {
            if (explicit_exponent < 1000000000)
            {
                explicit_exponent = explicit_exponent * 10 + (*p - '0');
            }
            p++;
        }
        if (p == digits)
        {
            p = mark;
        }
        else
        {
            exponent += exponent_negative ? -explicit_exponent : explicit_exponent;
        }
    }
    if (endptr)
    {
        *endptr = (char *)p;
    }
    if (first_dropped > 5 || (first_dropped == 5 && (sticky || (mantissa & 1))))
    {
        mantissa++;
    }
    double value = (double)mantissa;
    if (mantissa && exponent > 308)
    {
        union
        {
            uint64 u;
            double d;
        } infinity = { 0x7ff0000000000000ULL };
        value = infinity.d;
    }
    else if (exponent < -342)
    {
        value = 0.0;
    }
    else if (mantissa)
    {
        unsigned magnitude = (unsigned)(exponent < 0 ? -exponent : exponent);
        // Largest power first keeps subnormal results from underflowing before
        // the significant digits have been included. At most nine operations.
        for (int i = 8; i >= 0; i--)
        {
            if (magnitude & (1u << i))
            {
                value = exponent < 0 ? value / powers[i] : value * powers[i];
            }
        }
    }
    return negative ? -value : value;
}

/**
 * @brief single precision clib__strtod
 *
 * @param const char* s -> text to parse
 * @param char** endptr -> receives the first character not consumed, or s when nothing was parsed; may be NULL_POINTER
 *
 * @return float -> the value, 0.0f when nothing was parsed
 */
float clib__strtof(const char *s, char **endptr)
{
    return (float)clib__strtod(s, endptr);
}

/**
 * @brief parses a decimal floating point number with atof semantics: garbage gives 0.0 and cannot be told apart from a parsed zero, so
 * prefer clib__strtod where that matters
 *
 * @param const char* s -> text to parse
 *
 * @return double -> the value
 */
double clib__atof(const char *s)
{
    return clib__strtod(s, NULL_POINTER);
}

typedef union clib_double_bits_t
{
    double value;
    uint64 bits;
} clib_double_bits_t;

int clib__isfinite(double x)
{
    clib_double_bits_t v;
    v.value = x;
    return (v.bits & 0x7ff0000000000000ULL) != 0x7ff0000000000000ULL;
}

double clib__nan(void)
{
    clib_double_bits_t v;
    v.bits = 0x7ff8000000000000ULL;
    return v.value;
}

static double _clib_internal_double_max(double x, double y)
{
    return x > y ? x : y;
}

double clib__copysign(double magnitude, double sign)
{
    clib_double_bits_t m;
    clib_double_bits_t s;
    m.value = magnitude;
    s.value = sign;
    m.bits = (m.bits & 0x7fffffffffffffffULL) | (s.bits & 0x8000000000000000ULL);
    return m.value;
}

double clib__hypot(double x, double y)
{
    x = clib__fabs(x);
    y = clib__fabs(y);
    clib_double_bits_t a;
    clib_double_bits_t b;
    a.value = x;
    b.value = y;
    if (a.bits == 0x7ff0000000000000ULL || b.bits == 0x7ff0000000000000ULL)
    {
        a.bits = 0x7ff0000000000000ULL;
        return a.value;
    }
    if (!clib__isfinite(x) || !clib__isfinite(y))
    {
        return clib__nan();
    }
    double scale = _clib_internal_double_max(x, y);
    if (scale == 0.0)
    {
        return 0.0;
    }
    x /= scale;
    y /= scale;
    return scale * clib__sqrt(x * x + y * y);
}

double clib__exp(double x)
{
    if (x != x)
    {
        return x;
    }
    if (x < -745.133219101941)
    {
        return 0.0;
    }
    if (x > 709.782712893384)
    {
        clib_double_bits_t infinity;
        infinity.bits = 0x7ff0000000000000ULL;
        return infinity.value;
    }
    int k = (int)clib__floor(x * 1.4426950408889634074 + 0.5);
    /* Split ln(2) reduces cancellation in the argument reduction. */
    double r = (x - k * 0.69314718036912381649) - k * 1.9082149292705877000e-10;
    double sum = 1.0;
    double term = 1.0;
    for (int i = 1; i <= 18; ++i)
    {
        term *= r / i;
        sum += term;
    }
    clib_double_bits_t scale;
    if (k > 1023)
    {
        scale.bits = 0x7fe0000000000000ULL;
        return (sum * 2.0) * scale.value;
    }
    if (k < -1022)
    {
        scale.bits = (uint64)(k + 1022 + 1023) << 52;
        return sum * scale.value * 2.2250738585072013831e-308;
    }
    scale.bits = (uint64)(k + 1023) << 52;
    return sum * scale.value;
}

double clib__log(double x)
{
    if (x < 0.0 || x != x)
    {
        return clib__nan();
    }
    if (x == 0.0)
    {
        clib_double_bits_t infinity;
        infinity.bits = 0xfff0000000000000ULL;
        return infinity.value;
    }
    if (!clib__isfinite(x))
    {
        return x;
    }
    int correction = 0;
    if (x < 2.2250738585072013831e-308)
    {
        x *= 18014398509481984.0;
        correction = -54;
    }
    clib_double_bits_t v;
    v.value = x;
    int exponent = (int)((v.bits >> 52) & 0x7ff) - 1023 + correction;
    v.bits = (v.bits & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL;
    double mantissa = v.value;
    if (mantissa > 1.4142135623730950488)
    {
        mantissa *= 0.5;
        ++exponent;
    }
    double z = (mantissa - 1.0) / (mantissa + 1.0);
    double term = z;
    double sum = z;
    for (int k = 3; k <= 35; k += 2)
    {
        term *= z * z;
        sum += term / k;
    }
    return exponent * 0.69314718055994530942 + 2.0 * sum;
}

double clib__pow_positive(double x, double exponent)
{
    if (x <= 0.0 || !clib__isfinite(x) || !clib__isfinite(exponent))
    {
        return clib__nan();
    }
    return clib__exp(exponent * clib__log(x));
}

double clib__cos_bounded(double x)
{
    const double pi = 3.14159265358979323846;
    if (!clib__isfinite(x) || clib__fabs(x) > 2.0 * pi)
    {
        return clib__nan();
    }
    x -= clib__floor((x + pi) / (2.0 * pi)) * (2.0 * pi);
    double sign = 1.0;
    if (x > pi * 0.5)
    {
        x = pi - x;
        sign = -1.0;
    }
    else if (x < -pi * 0.5)
    {
        x = -pi - x;
        sign = -1.0;
    }
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k <= 12; ++k)
    {
        term *= -x * x / ((2.0 * k - 1.0) * (2.0 * k));
        sum += term;
    }
    return sign * sum;
}

/* Caller-owned buffers. Capacity includes the terminator; these functions never allocate. */
/* -1 means invalid/unterminated. Reads at most capacity bytes. */
int64 clib__string_length(const char *string, uint64 capacity)
{
    uint64 i = 0;
    if (!string || !capacity || capacity > (uint64)INT64_MAX || capacity > SIZE_MAX)
    {
        return -1;
    }
    for (i = 0; i < capacity; i++)
    {
        if (string[i] == 0)
        {
            return (int64)i;
        }
    }
    return -1;
}

boole clib__string_copy_n(char *destination, uint64 capacity, const char *source, uint64 length)
{
    if (!destination || !capacity || capacity > SIZE_MAX || capacity > (uint64)INT64_MAX)
    {
        return FALSE;
    }
    if ((!source && length) || length >= capacity)
    {
        destination[0] = 0;
        return FALSE;
    }
    clib__memmove(destination, source, (int64)length);
    destination[length] = 0;
    return TRUE;
}

/* Copies are all-or-nothing. Failure clears a writable destination. Overlap is supported. */
boole clib__string_copy(char *destination, uint64 capacity, cstring source)
{
    uint64 length = 0;
    if (!destination || !capacity || capacity > SIZE_MAX || capacity > (uint64)INT64_MAX)
    {
        return FALSE;
    }
    if (source)
    {
        while (length < capacity && source[length])
        {
            length++;
        }
    }
    return clib__string_copy_n(destination, capacity, source, length);
}

/* Append leaves the existing string unchanged on failure. */
boole clib__string_append(char *destination, uint64 capacity, cstring source)
{
    int64 length = clib__string_length(destination, capacity);
    uint64 added = 0;
    uint64 remaining = 0;
    if (length < 0 || !source)
    {
        return FALSE;
    }
    remaining = capacity - (uint64)length;
    while (added < remaining && source[added])
    {
        added++;
    }
    if (added >= remaining)
    {
        return FALSE;
    }
    clib__memmove(destination + length, source, (int64)added);
    destination[(uint64)length + added] = 0;
    return TRUE;
}

int64 clib__string_vformat(char *destination, uint64 capacity, cstring format, va_list arguments)
{
    int64 required = 0;
    va_list copy;
    if (!format || capacity > SIZE_MAX || capacity > (uint64)INT64_MAX || (!destination && capacity))
    {
        return -1;
    }
    va_copy(copy, arguments);
    required = clib__vformat(NULL_POINTER, 0, format, copy);
    va_end(copy);
    if (capacity)
    {
        if (required < 0 || (uint64)required >= capacity)
        {
            destination[0] = 0;
        }
        else
        {
            clib__vformat(destination, (int64)capacity, format, arguments);
        }
    }
    return required;
}

/* Returns required bytes excluding NUL, or -1 on invalid input. Overflow clears output. */
int64 clib__string_format(char *destination, uint64 capacity, cstring format, ...)
{
    va_list arguments;
    int64 required = 0;
    va_start(arguments, format);
    required = clib__string_vformat(destination, capacity, format, arguments);
    va_end(arguments);
    return required;
}

void clib__string_trim_newline(char *string, uint64 capacity)
{
    int64 length = clib__string_length(string, capacity);
    if (length > 0 && string[length - 1] == '\n')
    {
        string[--length] = 0;
        if (length > 0 && string[length - 1] == '\r')
        {
            string[length - 1] = 0;
        }
    }
}

/* Compatibility for existing byte-oriented application code, with zero-capacity handling. */
void clib__copy_memory(const void *source, void *destination, uint64 length, uint64 capacity)
{
    if (!source || !destination)
    {
        return;
    }
    if (length > capacity)
    {
        length = capacity;
    }
    if (length <= (uint64)INT64_MAX && length <= SIZE_MAX)
    {
        clib__memmove(destination, source, (int64)length);
    }
}

void clib__set_memory(void *destination, int64 length, ubyte value)
{
    if (destination)
    {
        clib__memset(destination, value, length);
    }
}

boole clib__is_string_equal(cstring first, cstring second)
{
    return first && second && clib__strcmp(first, second) == 0;
}

uint64 clib__utf8_string_length(cstring string)
{
    return (uint64)clib__strlen(string);
}

int64 clib__utf8_string_length_check_max_length(cstring string, int maximum)
{
    int64 i = 0;
    if (!string || maximum < 0)
    {
        return -1;
    }
    for (i = 0; i <= maximum; i++)
    {
        if (!string[i])
        {
            return i;
        }
    }
    return -1;
}

unsigned long clib__strtoul(cstring string, char **end, int base)
{
    cstring start = string;
    unsigned long value = 0;
    boole negative = FALSE;
    boole any = FALSE;
    boole overflow = FALSE;
    if (end)
    {
        *end = (char *)string;
    }
    if (!string || (base && (base < 2 || base > 36)))
    {
        return 0;
    }
    while (clib__isspace((ubyte)*string))
    {
        string++;
    }
    if (*string == '-' || *string == '+')
    {
        negative = *string++ == '-';
    }
    if ((base == 0 || base == 16) && string[0] == '0' && (string[1] == 'x' || string[1] == 'X') && clib__isxdigit((ubyte)string[2]))
    {
        string += 2;
        base = 16;
    }
    if (!base)
    {
        base = *string == '0' ? 8 : 10;
    }
    for (;; string++)
    {
        int c = clib__tolower((ubyte)*string);
        int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : -1;
        if (digit < 0 || digit >= base)
        {
            break;
        }
        any = TRUE;
        if (value > (ULONG_MAX - (unsigned long)digit) / (unsigned long)base)
        {
            overflow = TRUE;
        }
        else if (!overflow)
        {
            value = value * (unsigned long)base + (unsigned long)digit;
        }
    }
    if (end)
    {
        *end = (char *)(any ? string : start);
    }
    return overflow ? ULONG_MAX : negative ? 0ul - value : value;
}
