// Private implementation, included by stdlib.c. Never read outside the caller's
// byte count, and keep optimized loops local instead of becoming external calls.
#if defined(COMPILER_CLANG)
#define STDLIB_NO_LIBCALL __attribute__((no_builtin))
#elif defined(COMPILER_GCC)
#define STDLIB_NO_LIBCALL __attribute__((optimize("no-tree-loop-distribute-patterns")))
#else
#define STDLIB_NO_LIBCALL
#endif

#if STDLIB_USE_SIMD && defined(ARCHITECTURE_SSE2)
#include <emmintrin.h>
typedef __m128i _stdlib_vector;
#define STDLIB_VECTOR_LOAD(p) _mm_loadu_si128((const __m128i*)(p))
#define STDLIB_VECTOR_STORE(p, v) _mm_storeu_si128((__m128i*)(p), (v))
#define STDLIB_VECTOR_FILL(v) _mm_set1_epi8((char)(v))
#define STDLIB_HAS_VECTOR 1
#elif STDLIB_USE_SIMD && defined(ARCHITECTURE_NEON)
#include <arm_neon.h>
typedef uint8x16_t _stdlib_vector;
#define STDLIB_VECTOR_LOAD(p) vld1q_u8((const ubyte*)(p))
#define STDLIB_VECTOR_STORE(p, v) vst1q_u8((ubyte*)(p), (v))
#define STDLIB_VECTOR_FILL(v) vdupq_n_u8((ubyte)(v))
#define STDLIB_HAS_VECTOR 1
#else
#define STDLIB_HAS_VECTOR 0
#endif

static STDLIB_NO_LIBCALL void* stdlib__copy_bytes(void* dst, const void* src, nuint n)
{
    ubyte* d = (ubyte*)dst;
    const ubyte* s = (const ubyte*)src;
    if (!d || !s || !n) return dst;
#if STDLIB_HAS_VECTOR
    while (n >= 64)
    {
        _stdlib_vector a = STDLIB_VECTOR_LOAD(s);
        _stdlib_vector b = STDLIB_VECTOR_LOAD(s + 16);
        _stdlib_vector c = STDLIB_VECTOR_LOAD(s + 32);
        _stdlib_vector e = STDLIB_VECTOR_LOAD(s + 48);
        STDLIB_VECTOR_STORE(d, a);
        STDLIB_VECTOR_STORE(d + 16, b);
        STDLIB_VECTOR_STORE(d + 32, c);
        STDLIB_VECTOR_STORE(d + 48, e);
        s += 64; d += 64; n -= 64;
    }
    while (n >= 16)
    {
        STDLIB_VECTOR_STORE(d, STDLIB_VECTOR_LOAD(s));
        s += 16; d += 16; n -= 16;
    }
#endif
    // MSVC has no per-function no_builtin equivalent. Volatile byte accesses
    // keep its scalar fallback from being rewritten to an external call.
#if defined(COMPILER_MSVC)
    volatile ubyte* tail = d;
    while (n--) *tail++ = *s++;
#else
    while (n--) *d++ = *s++;
#endif
    return dst;
}

static STDLIB_NO_LIBCALL void* stdlib__fill_bytes(void* dst, int value, nuint n)
{
    ubyte* d = (ubyte*)dst;
    if (!d || !n) return dst;
#if STDLIB_HAS_VECTOR
    _stdlib_vector v = STDLIB_VECTOR_FILL(value);
    while (n >= 64)
    {
        STDLIB_VECTOR_STORE(d, v);
        STDLIB_VECTOR_STORE(d + 16, v);
        STDLIB_VECTOR_STORE(d + 32, v);
        STDLIB_VECTOR_STORE(d + 48, v);
        d += 64; n -= 64;
    }
    while (n >= 16)
    {
        STDLIB_VECTOR_STORE(d, v);
        d += 16; n -= 16;
    }
#endif
#if defined(COMPILER_MSVC)
    volatile ubyte* tail = d;
    while (n--) *tail++ = (ubyte)value;
#else
    while (n--) *d++ = (ubyte)value;
#endif
    return dst;
}

static STDLIB_NO_LIBCALL void* stdlib__move_bytes(void* dst, const void* src, nuint n)
{
    ubyte* d = (ubyte*)dst;
    const ubyte* s = (const ubyte*)src;
    if (!d || !s || d == s || !n) return dst;
    // Integer addresses avoid relational comparisons of unrelated pointers.
    if ((nuint)d < (nuint)s || (nuint)d - (nuint)s >= n)
        return stdlib__copy_bytes(dst, src, n);
    d += n; s += n;
#if STDLIB_HAS_VECTOR
    while (n >= 16)
    {
        d -= 16; s -= 16;
        STDLIB_VECTOR_STORE(d, STDLIB_VECTOR_LOAD(s));
        n -= 16;
    }
#endif
    while (n--) *--d = *--s;
    return dst;
}

STDLIB_NO_LIBCALL void* stdlib__memcpy(void* dst, const void* src, int64 count)
{
    return count <= 0 ? dst : stdlib__copy_bytes(dst, src, (nuint)count);
}

STDLIB_NO_LIBCALL void* stdlib__memset(void* dst, int value, int64 count)
{
    return count <= 0 ? dst : stdlib__fill_bytes(dst, value, (nuint)count);
}

STDLIB_NO_LIBCALL void* stdlib__memmove(void* dst, const void* src, int64 count)
{
    return count <= 0 ? dst : stdlib__move_bytes(dst, src, (nuint)count);
}
