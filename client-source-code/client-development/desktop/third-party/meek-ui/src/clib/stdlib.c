// Local memory, strings, parsing, formatting and math. Configuration and
// contracts live at the top of stdlib.h; architecture detection in architecture.h.
// No allocation, operating-system calls, or external runtime forwarding.

#include "stdlib.h"
#include <limits.h>
#include "stdlib_memory.h"

#if STDLIB_DEFINE_MEMORY_HELPERS
// Compiler-generated aggregate copies/zeroing use these fixed symbol names.
// size_t is required only at this ABI boundary; the local cores use nuint.
#if defined(COMPILER_MSVC_ABI)
#define STDLIB_MEMORY_ABI __cdecl
#else
#define STDLIB_MEMORY_ABI
#endif
void* STDLIB_MEMORY_ABI memcpy(void* dst, const void* src, size_t count);
void* STDLIB_MEMORY_ABI memmove(void* dst, const void* src, size_t count);
void* STDLIB_MEMORY_ABI memset(void* dst, int value, size_t count);
#if defined(COMPILER_MSVC)
#pragma function(memcpy, memmove, memset)
#endif
STDLIB_NO_LIBCALL void* STDLIB_MEMORY_ABI memcpy(void* dst, const void* src, size_t count)
{
    return stdlib__copy_bytes(dst, src, (nuint)count);
}
STDLIB_NO_LIBCALL void* STDLIB_MEMORY_ABI memmove(void* dst, const void* src, size_t count)
{
    return stdlib__move_bytes(dst, src, (nuint)count);
}
STDLIB_NO_LIBCALL void* STDLIB_MEMORY_ABI memset(void* dst, int value, size_t count)
{
    return stdlib__fill_bytes(dst, value, (nuint)count);
}
#undef STDLIB_MEMORY_ABI
#endif

#if STDLIB_DEFINE_FLOAT_MARKER && defined(COMPILER_MSVC_ABI)
// C name has one underscore; the x86 object name gains a second one. COMDAT
// allows an existing strong host definition to take precedence at link time.
__declspec(selectany) int _fltused = 0;
#endif

// ===== Windows stack probes ===============================================
// Touch every 4 KiB page so Windows can advance a thread's stack guard page.
// These entry points have compiler-defined register/stack conventions, not a
// C calling convention. Keep their bodies naked and entirely in assembly.
#if STDLIB_ENABLE_ASSEMBLY_OPTIMIZATIONS && defined(COMPILER_MSVC_ABI)
#if defined(ARCHITECTURE_I386) && (defined(COMPILER_CLANG) || defined(COMPILER_MSVC))
// EAX is the allocation size. x86 also adjusts ESP before returning.
__declspec(naked) void _chkstk(void)
{
    __asm {
        push ecx
        lea ecx, [esp + 8]
        cmp eax, 4096
        jb stack_probe_tail
    stack_probe_page:
        sub ecx, 4096
        test byte ptr [ecx], 0
        sub eax, 4096
        cmp eax, 4096
        jae stack_probe_page
    stack_probe_tail:
        sub ecx, eax
        test byte ptr [ecx], 0
        mov eax, esp
        mov esp, ecx
        mov ecx, dword ptr [eax]
        mov eax, dword ptr [eax + 4]
        push eax
        ret
    }
}
#elif defined(ARCHITECTURE_AMD64) && defined(COMPILER_CLANG)
// RAX is the allocation size and must survive; the caller adjusts RSP.
// Native MSVC does not support x64 inline assembly.
__attribute__((naked)) void __chkstk(void)
{
    __asm {
        push rcx
        push rax
        lea rcx, [rsp + 24]
        cmp rax, 4096
        jb stack_probe_tail
    stack_probe_page:
        sub rcx, 4096
        test byte ptr [rcx], 0
        sub rax, 4096
        cmp rax, 4096
        jae stack_probe_page
    stack_probe_tail:
        sub rcx, rax
        test byte ptr [rcx], 0
        pop rax
        pop rcx
        ret
    }
}
#elif defined(ARCHITECTURE_ARM64) && defined(COMPILER_CLANG) && !defined(COMPILER_ARM64EC_ABI)
// X15 holds the allocation size divided by 16. Preserve it and SP; only the
// scratch registers X16/X17 and flags may change. The caller adjusts SP.
__attribute__((naked)) void __chkstk(void)
{
    __asm__(
        "lsl x16, x15, #4\n"
        "mov x17, sp\n"
        "cmp x16, #4096\n"
        "b.lo 2f\n"
        "1:\n"
        "sub x17, x17, #4096\n"
        "ldr xzr, [x17]\n"
        "sub x16, x16, #4096\n"
        "cmp x16, #4096\n"
        "b.hs 1b\n"
        "2:\n"
        "cbz x16, 3f\n"
        "sub x17, x17, x16\n"
        "ldr xzr, [x17]\n"
        "3:\n"
        "ret\n"
    );
}
#endif
#endif

// ===== integer division ===================================================
uint64 stdlib__udivmod64(uint64 numerator, uint64 denominator, uint64* remainder)
{
#if defined(ARCHITECTURE_NATIVE_DIV64)
    uint64 quotient = numerator / denominator;
    if (remainder) *remainder = numerator % denominator;
    return quotient;
#else
    uint64 quotient = 0;
    uint64 bit = 1;
    uint64 divisor = denominator;
    // Zero is outside the API contract; keep the normalization loop bounded
    // even for that invalid input. No platform-specific trap is required.
    if (!denominator || numerator < denominator)
    {
        if (remainder) *remainder = numerator;
        return 0;
    }
    if ((denominator & (denominator - 1)) == 0)
    {
        // Common row widths and binary units need only shifts and a mask.
        // All shift counts are constants, including on narrow targets.
        quotient = numerator;
        if (divisor >> 32) { divisor >>= 32; quotient >>= 32; }
        if (divisor >> 16) { divisor >>= 16; quotient >>= 16; }
        if (divisor >> 8) { divisor >>= 8; quotient >>= 8; }
        if (divisor >> 4) { divisor >>= 4; quotient >>= 4; }
        if (divisor >> 2) { divisor >>= 2; quotient >>= 2; }
        if (divisor >> 1) quotient >>= 1;
        if (remainder) *remainder = numerator & (denominator - 1);
        return quotient;
    }
#if defined(ARCHITECTURE_NATIVE_DIV32)
    if ((numerator >> 32) == 0)
    {
        uint n = (uint)numerator, d = (uint)denominator;
        quotient = n / d;
        if (remainder) *remainder = n % d;
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
    if (remainder) *remainder = numerator;
    return quotient;
#endif
}

int64 stdlib__divmod64(int64 numerator, int64 denominator, int64* remainder)
{
#if defined(ARCHITECTURE_NATIVE_DIV64)
    int64 quotient = numerator / denominator;
    if (remainder) *remainder = numerator % denominator;
    return quotient;
#else
    // Unsigned magnitudes also represent INT64_MIN without signed overflow.
    uint64 n = numerator < 0 ? 0ULL - (uint64)numerator : (uint64)numerator;
    uint64 d = denominator < 0 ? 0ULL - (uint64)denominator : (uint64)denominator;
    uint64 r;
    uint64 q = stdlib__udivmod64(n, d, remainder ? &r : NULL_POINTER);
    if (remainder) *remainder = numerator < 0 && r ? -(int64)(r - 1) - 1 : (int64)r;
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
void stdlib__null_memory(void* dst, int64 count)
{
    stdlib__memset(dst, 0, count);
}


/**
 * @brief byte-wise compare `count` bytes. Returns 0 on equality, negative if first differing byte in `a` is less than the one in `b`, positive otherwise. Matches memcmp.
 *
 * @param const void* a
 * @param const void* b
 * @param int64 count
 *
 * @return int -> void
 */
int stdlib__memcmp(const void* a, const void* b, int64 count)
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
        return  1;
    }
    const ubyte* pa = (const ubyte*)a;
    const ubyte* pb = (const ubyte*)b;
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

static void _stdlib_sort_swap(ubyte* a, ubyte* b, nuint size)
{
    while (size--) { ubyte v = *a; *a++ = *b; *b++ = v; }
}

static void _stdlib_sort_sift(ubyte* base, nuint parent, nuint count, nuint size,
                            int (*compare)(const void*, const void*))
{
    // parent < count/2 also keeps parent*2+1 from overflowing.
    while (parent < count / 2)
    {
        nuint child = parent * 2 + 1;
        if (child + 1 < count && compare(base + child*size, base + (child+1)*size) < 0) child++;
        if (compare(base + parent*size, base + child*size) >= 0) break;
        _stdlib_sort_swap(base + parent*size, base + child*size, size);
        parent = child;
    }
}

void stdlib__qsort(void* base, nuint count, nuint size, int (*compare)(const void*, const void*))
{
    if (!base || !compare || !size || count < 2 || count > (nuint)-1 / size) return;
    ubyte* bytes = (ubyte*)base;
    for (nuint i = count / 2; i > 0; i--) _stdlib_sort_sift(bytes, i-1, count, size, compare);
    while (--count)
    {
        _stdlib_sort_swap(bytes, bytes + count*size, size);
        _stdlib_sort_sift(bytes, 0, count, size, compare);
    }
}

/**
 * @brief length of a null-terminated C string, NOT counting the terminator. No arbitrary cap (the existing purelib__strlen capped at 1000, which is a silent-truncation hazard this implementation avoids).
 *
 * @param const char* s -> null-terminated string.
 *
 * @return int64 -> byte count before the terminator.
 */
STDLIB_NO_LIBCALL int64 stdlib__strlen(const char* s)
{
    if (s == NULL_POINTER)
    {
        return 0;
    }
    const char* p = s;
    while (*p != 0)
    {
        p++;
    }
    return (int64)(p - s);
}

/**
 * @brief lexicographic compare of two null-terminated strings. Returns 0 on equality, negative if `a < b`, positive if `a > b`. Matches strcmp semantics.
 *
 * @param const char* a
 * @param const char* b
 *
 * @return int -> void
 */
int stdlib__strcmp(const char* a, const char* b)
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
        return  1;
    }
    // Cast to unsigned char for the subtract so byte values >= 128
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
int stdlib__strncmp(const char* a, const char* b, int64 n)
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
        return  1;
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
 * @brief find the first occurrence of `needle` inside `haystack`. Returns a pointer to the start of the match, or NULL. Matches strstr. Both arguments must be null-terminated.
 *
 * @param const char* haystack
 * @param const char* needle
 *
 * @return const char* -> void
 */
const char* stdlib__strstr(const char* haystack, const char* needle)
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
    for (const char* p = haystack; *p != 0; p++)
    {
        const char* h = p;
        const char* n = needle;
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
 * @brief find the first occurrence of `ch` in the null-terminated string `s`. Returns a pointer to it or NULL. Matches strchr. The terminator IS matched if you pass `ch = 0`.
 *
 * @param const char* s
 * @param int ch
 *
 * @return const char* -> void
 */
const char* stdlib__strchr(const char* s, int ch)
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
 * @brief copy at most `dst_cap` bytes from `src` into `dst`. Always null-terminates `dst` (provided dst_cap >= 1). Truncates if `src` doesn't fit. Returns the number of source bytes that WOULD have been copied (not including the terminator), so callers can detect truncation: `if (written >= dst_cap) { ... }`. This is the strlcpy contract -- safer than strncpy (which doesn't always terminate) and safer than strcpy (no cap).
 *
 * @param char* dst
 * @param int64 dst_cap -> total bytes in dst (including terminator room).
 * @param const char* src
 *
 * @return int64 -> strlen(src) at time of call.
 */
int64 stdlib__strcpy_safe(char* dst, int64 dst_cap, const char* src)
{
    // Always returns strlen(src) (so the caller can detect truncation
    // via the result). Even if dst_cap is 0 we still walk src to get
    // that length; fast paths are available if the caller doesn't
    // care about detecting truncation.
    int64 src_len = stdlib__strlen(src);
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
int stdlib__isspace(int c)
{
    return (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') ? 1 : 0;
}

/**
 * @brief aSCII decimal digit. @function stdlib__isdigit
 *
 * @return int -> void
 */
int stdlib__isdigit(int c)
{
    return (c >= '0' && c <= '9') ? 1 : 0;
}

/**
 * @brief aSCII hexadecimal digit (0-9, a-f, A-F). @function stdlib__isxdigit
 *
 * @return int -> void
 */
int stdlib__isxdigit(int c)
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
 * @brief aSCII alphabetic letter. @function stdlib__isalpha
 *
 * @return int -> void
 */
int stdlib__isalpha(int c)
{
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) ? 1 : 0;
}

/**
 * @brief aSCII alphanumeric. @function stdlib__isalnum
 *
 * @return int -> void
 */
int stdlib__isalnum(int c)
{
    return (stdlib__isalpha(c) || stdlib__isdigit(c)) ? 1 : 0;
}

/**
 * @brief aSCII lowercase conversion; non-letters pass through. @function stdlib__tolower
 *
 * @return int -> void
 */
int stdlib__tolower(int c)
{
    if (c >= 'A' && c <= 'Z')
    {
        return c + ('a' - 'A');
    }
    return c;
}

/**
 * @brief aSCII uppercase conversion; non-letters pass through. @function stdlib__toupper
 *
 * @return int -> void
 */
int stdlib__toupper(int c)
{
    if (c >= 'a' && c <= 'z')
    {
        return c - ('a' - 'A');
    }
    return c;
}

// ===== number parsing =====================================================

/**
 * @brief parse a base-10 integer from `s`, consuming leading whitespace + optional sign + contiguous digits. Matches atoi semantics (including "returns 0 on garbage input" — so prefer `stdlib__strtol` if you need to distinguish "parsed zero" from "couldn't parse").
 *
 * @param const char* s
 *
 * @return int -> void
 */
// Parse using an unsigned magnitude so LONG_MIN/INT64_MIN never require
// overflowing signed arithmetic. Keep consuming digits after saturation.
static int64 _stdlib_internal__parse_integer(const char* s, char** endptr, int base, uint64 maximum)
{
    const char* p = s;
    uint64 value = 0, limit, cutoff, cutlim;
    int negative = 0, any = 0, overflow = 0;
    if (endptr) *endptr = (char*)s;
    if (!s || (base != 0 && (base < 2 || base > 36))) return 0;
    while (stdlib__isspace((ubyte)*p)) p++;
    if (*p == '-' || *p == '+') negative = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' &&
        (p[1] == 'x' || p[1] == 'X') && stdlib__isxdigit((ubyte)p[2]))
    {
        p += 2; base = 16;
    }
    if (base == 0) base = *p == '0' ? 8 : 10;
    limit = maximum + (uint64)negative;
    cutoff = stdlib__udivmod64(limit, (uint)base, &cutlim);
    for (;; p++)
    {
        int c = (ubyte)*p;
        int digit = c >= '0' && c <= '9' ? c - '0' :
                    c >= 'a' && c <= 'z' ? c - 'a' + 10 :
                    c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 36;
        if (digit >= base) break;
        any = 1;
        if (value > cutoff || (value == cutoff && (uint)digit > cutlim)) overflow = 1;
        else if (!overflow) value = value * (uint)base + (uint)digit;
    }
    if (!any) return 0;
    if (endptr) *endptr = (char*)p;
    if (overflow) value = limit;
    // -(magnitude - 1) - 1 also handles the most negative signed value.
    return negative && value ? -(int64)(value - 1) - 1 : (int64)value;
}

int stdlib__atoi(const char* s)
{
    return (int)_stdlib_internal__parse_integer(s, NULL_POINTER, 10, INT_MAX);
}
long stdlib__strtol(const char* s, char** endptr, int base)
{
    return (long)_stdlib_internal__parse_integer(s, endptr, base, LONG_MAX);
}
int64 stdlib__strtoll(const char* s, char** endptr, int base)
{
    return _stdlib_internal__parse_integer(s, endptr, base, 0x7fffffffffffffffULL);
}

// ===== local math =========================================================
#include "stdlib_math.h"

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
int64 stdlib__strcat_safe(char* dst, int64 dst_cap, const char* src)
{
    int64 dst_len = 0;
    int64 src_len = stdlib__strlen(src);
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
const char* stdlib__strrchr(const char* s, int ch)
{
    const char* found = NULL_POINTER;

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

const void* stdlib__memchr(const void* block, int ch, int64 count)
{
    const ubyte* bytes = (const ubyte*)block;
    int64 i = 0;

    if (block == NULL_POINTER || count <= 0)
    {
        return NULL_POINTER;
    }
    for (i = 0; i < count; i++)
    {
        if (bytes[i] == (ubyte)ch)
        {
            return (const void*)(bytes + i);
        }
    }
    return NULL_POINTER;
}

int64 stdlib__strncpy_safe(char* dst, int64 dst_cap, const char* src, int64 count)
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
#include "stdlib_format.h"

int64 stdlib__format(char* dst, int64 dst_cap, const char* fmt, ...)
{
    va_list args;
    int64 written = 0;

    va_start(args, fmt);
    written = stdlib__vformat(dst, dst_cap, fmt, args);
    va_end(args);
    return written;
}

int64 stdlib__u64toa_hex(char* dst, int64 dst_cap, uint64 value)
{
    char digits[17];
    int64 length = 0;
    int64 i = 0;

    stdlib__null_memory(digits, sizeof(digits));
    if (dst == NULL_POINTER || dst_cap <= 0) return 0;
    dst[0] = 0;
    if (dst_cap == 1) return 0;
    // the digits come out backwards, so they are collected and reversed
    do
    {
        digits[length] = "0123456789abcdef"[(int)(value & 0xFull)];
        value >>= 4;
        length++;
    }
    while (value != 0 && length < 16);
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

const char* stdlib__getenv(const char* name)
{
    (void)name;
    return NULL_POINTER;
}

int64 stdlib__i64toa(char* dst, int64 dst_cap, int64 value)
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
        magnitude = stdlib__udivmod64(magnitude, 10, &digit);
        reversed[n] = (char)('0' + (int)digit);
        n = n + 1;
    }
    while (magnitude != 0ull);

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
double stdlib__strtod(const char* s, char** endptr)
{
    const char* p = s;
    uint64 mantissa = 0;
    int64 exponent = 0, explicit_exponent = 0;
    int negative = 0, fractional = 0, any = 0, significant = 0;
    int first_dropped = -1, sticky = 0, exponent_negative = 0;
    static const double powers[] = {1e1, 1e2, 1e4, 1e8, 1e16, 1e32, 1e64, 1e128, 1e256};
    if (endptr) *endptr = (char*)s;
    if (!s) return 0.0;
    while (stdlib__isspace((ubyte)*p)) p++;
    if (*p == '-' || *p == '+') negative = *p++ == '-';
    for (;; p++)
    {
        if (*p == '.' && !fractional) { fractional = 1; continue; }
        if (*p < '0' || *p > '9') break;
        int digit = *p - '0';
        any = 1;
        if (!significant && !digit)
        {
            if (fractional && exponent > -1000000000) exponent--;
        }
        else if (significant < 19)
        {
            mantissa = mantissa * 10 + (uint)digit;
            significant++;
            if (fractional) exponent--;
        }
        else
        {
            if (!fractional && exponent < 1000000000) exponent++;
            if (first_dropped < 0) first_dropped = digit;
            else sticky |= digit;
        }
    }
    if (!any) return 0.0;
    if (*p == 'e' || *p == 'E')
    {
        const char* mark = p++;
        if (*p == '-' || *p == '+') exponent_negative = *p++ == '-';
        const char* digits = p;
        while (*p >= '0' && *p <= '9')
        {
            if (explicit_exponent < 1000000000) explicit_exponent = explicit_exponent * 10 + (*p - '0');
            p++;
        }
        if (p == digits) p = mark;
        else exponent += exponent_negative ? -explicit_exponent : explicit_exponent;
    }
    if (endptr) *endptr = (char*)p;
    if (first_dropped > 5 || (first_dropped == 5 && (sticky || (mantissa & 1)))) mantissa++;
    double value = (double)mantissa;
    if (mantissa && exponent > 308)
    {
        union { uint64 u; double d; } infinity = {0x7ff0000000000000ULL};
        value = infinity.d;
    }
    else if (exponent < -342) value = 0.0;
    else if (mantissa)
    {
        unsigned magnitude = (unsigned)(exponent < 0 ? -exponent : exponent);
        // Largest power first keeps subnormal results from underflowing before
        // the significant digits have been included. At most nine operations.
        for (int i = 8; i >= 0; i--)
            if (magnitude & (1u << i)) value = exponent < 0 ? value / powers[i] : value * powers[i];
    }
    return negative ? -value : value;
}

/**
 * @brief single precision stdlib__strtod
 *
 * @param const char* s -> text to parse
 * @param char** endptr -> receives the first character not consumed, or s when nothing was parsed; may be NULL_POINTER
 *
 * @return float -> the value, 0.0f when nothing was parsed
 */
float stdlib__strtof(const char* s, char** endptr)
{
    return (float)stdlib__strtod(s, endptr);
}

/**
 * @brief parses a decimal floating point number with atof semantics: garbage gives 0.0 and cannot be told apart from a parsed zero, so prefer stdlib__strtod where that matters
 *
 * @param const char* s -> text to parse
 *
 * @return double -> the value
 */
double stdlib__atof(const char* s)
{
    return stdlib__strtod(s, NULL_POINTER);
}
