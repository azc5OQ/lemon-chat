#include "cpu_features.h"
#include "clib/stdlib.h"
#include "third_party/log.h"
#include <stdlib.h>
#include <string.h>

// architecture
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #define CPU_FEATURES__X86 1
    #if defined(_MSC_VER)
        #include <intrin.h>
    #else
        #include <cpuid.h>
    #endif
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define CPU_FEATURES__AARCH64 1
#elif defined(__arm__) || defined(_M_ARM)
    #define CPU_FEATURES__ARM32 1
#elif defined(__powerpc__) || defined(__ppc__) || defined(__powerpc64__) || defined(__PPC64__)
    #define CPU_FEATURES__POWERPC 1
#elif defined(__riscv)
    #define CPU_FEATURES__RISCV 1
#endif

// operating system queries for the architectures without a user-mode
// feature instruction
#if defined(CPU_FEATURES__ARM32) || defined(CPU_FEATURES__POWERPC) || defined(CPU_FEATURES__RISCV)
    #if defined(_WIN32)
        #include <windows.h>
        #define CPU_FEATURES__OS_WINDOWS 1
    #elif defined(__linux__) || defined(__ANDROID__)
        #include <sys/auxv.h>
        #include <asm/hwcap.h>
        #define CPU_FEATURES__OS_AUXV 1
    #elif defined(__FreeBSD__)
        #include <sys/auxv.h>
        #define CPU_FEATURES__OS_ELF_AUX_INFO 1
    #endif
#endif

static uint  _cpu_features_internal__detect(void);
static void  _cpu_features_internal__ensure(void);
static void  _cpu_features_internal__append(const char* text);

static boole _cpu_features_internal__known = FALSE;
static uint  _cpu_features_internal__set = 0;
static char  _cpu_features_internal__text[96];

#if defined(CPU_FEATURES__X86)
/**
 * @brief run one CPUID leaf
 * @param uint leaf -> EAX input
 * @param uint sub -> ECX input
 * @param uint* out -> EAX, EBX, ECX, EDX
 * @return void
 */
static void _cpu_features_internal__cpuid(uint leaf, uint sub, uint* out)
{
    #if defined(_MSC_VER)
    int registers[4] = {0, 0, 0, 0};

    __cpuidex(registers, (int)leaf, (int)sub);
    out[0] = (uint)registers[0];
    out[1] = (uint)registers[1];
    out[2] = (uint)registers[2];
    out[3] = (uint)registers[3];
    #else
    uint a = 0;
    uint b = 0;
    uint c = 0;
    uint d = 0;

    __cpuid_count(leaf, sub, a, b, c, d);
    out[0] = a;
    out[1] = b;
    out[2] = c;
    out[3] = d;
    #endif
}

/**
 * @brief the low half of XCR0: which register states the OS saves on a context switch
 * @return uint -> XCR0 bits
 */
static uint _cpu_features_internal__xcr0(void)
{
    #if defined(_MSC_VER)
    return (uint)_xgetbv(0);
    #else
    uint low = 0;
    uint high = 0;

    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    return low;
    #endif
}
#endif

#if defined(CPU_FEATURES__OS_AUXV) || defined(CPU_FEATURES__OS_ELF_AUX_INFO)
/**
 * @brief an ELF auxiliary vector entry (AT_HWCAP, AT_HWCAP2), zero when unavailable
 * @param unsigned long type -> AT_* key
 * @return unsigned long -> the value
 */
static unsigned long _cpu_features_internal__auxval(unsigned long type)
{
    #if defined(CPU_FEATURES__OS_AUXV)
    return getauxval(type);
    #else
    unsigned long value = 0;

    if (elf_aux_info((int)type, &value, (int)sizeof(value)) != 0)
    {
        return 0;
    }
    return value;
    #endif
}
#endif

/**
 * @brief read the feature set from the CPU or the operating system
 * @return uint -> cpu_feature_e bits
 */
static uint _cpu_features_internal__detect(void)
{
    uint features = 0;
    #if defined(CPU_FEATURES__X86)
    uint registers[4] = {0, 0, 0, 0};
    uint highest = 0;

    _cpu_features_internal__cpuid(0, 0, registers);
    highest = registers[0];
    if (highest >= 1)
    {
        _cpu_features_internal__cpuid(1, 0, registers);
        if ((registers[3] & (1u << 26)) != 0)
        {
            features |= CPU_FEATURE__SSE2;
        }
        if ((registers[2] & (1u << 19)) != 0)
        {
            features |= CPU_FEATURE__SSE41;
        }
        // AVX2 needs the OSXSAVE bit, the OS saving XMM and YMM state
        // (XCR0 bits 1 and 2) and the leaf 7 AVX2 bit
        if ((registers[2] & (1u << 27)) != 0 && (registers[2] & (1u << 28)) != 0 && highest >= 7)
        {
            if ((_cpu_features_internal__xcr0() & 6u) == 6u)
            {
                _cpu_features_internal__cpuid(7, 0, registers);
                if ((registers[1] & (1u << 5)) != 0)
                {
                    features |= CPU_FEATURE__AVX2;
                }
            }
        }
    }
    #elif defined(CPU_FEATURES__AARCH64)
    // Advanced SIMD is part of the AArch64 baseline on every operating system
    features |= CPU_FEATURE__NEON;
    #elif defined(CPU_FEATURES__ARM32)
        #if defined(CPU_FEATURES__OS_WINDOWS)
    if (IsProcessorFeaturePresent(PF_ARM_NEON_INSTRUCTIONS_AVAILABLE))
    {
        features |= CPU_FEATURE__NEON;
    }
        #elif defined(CPU_FEATURES__OS_AUXV) || defined(CPU_FEATURES__OS_ELF_AUX_INFO)
            #if !defined(HWCAP_NEON)
                #define HWCAP_NEON (1 << 12)
            #endif
    if ((_cpu_features_internal__auxval(AT_HWCAP) & HWCAP_NEON) != 0)
    {
        features |= CPU_FEATURE__NEON;
    }
        #endif
    #elif defined(CPU_FEATURES__POWERPC)
        #if defined(CPU_FEATURES__OS_AUXV) || defined(CPU_FEATURES__OS_ELF_AUX_INFO)
            // PPC_FEATURE_HAS_ALTIVEC (AT_HWCAP) and PPC_FEATURE2_ARCH_2_07 (AT_HWCAP2, VSX on POWER8)
            #if !defined(PPC_FEATURE_HAS_ALTIVEC)
                #define PPC_FEATURE_HAS_ALTIVEC 0x10000000
            #endif
            #if !defined(PPC_FEATURE_HAS_VSX)
                #define PPC_FEATURE_HAS_VSX 0x00000080
            #endif
    if ((_cpu_features_internal__auxval(AT_HWCAP) & PPC_FEATURE_HAS_ALTIVEC) != 0)
    {
        features |= CPU_FEATURE__ALTIVEC;
    }
    if ((_cpu_features_internal__auxval(AT_HWCAP) & PPC_FEATURE_HAS_VSX) != 0)
    {
        features |= CPU_FEATURE__VSX;
    }
        #endif
    #elif defined(CPU_FEATURES__RISCV)
        #if defined(CPU_FEATURES__OS_AUXV) || defined(CPU_FEATURES__OS_ELF_AUX_INFO)
    // the ISA extension letters are AT_HWCAP bits: 'V' - 'A' is the vector extension
    if ((_cpu_features_internal__auxval(AT_HWCAP) & (1ul << ('V' - 'A'))) != 0)
    {
        features |= CPU_FEATURE__RVV;
    }
        #endif
    #endif
    return features;
}

/**
 * @brief append a word to the description
 * @param const char* text -> the word with its leading space
 * @return void
 */
static void _cpu_features_internal__append(const char* text)
{
    stdlib__strcat_safe(_cpu_features_internal__text, sizeof(_cpu_features_internal__text), text);
}

/**
 * @brief detect once, apply the environment override, build the description
 * @return void
 */
static void _cpu_features_internal__ensure(void)
{
    const char* value = NULL_POINTER;

    if (_cpu_features_internal__known == TRUE)
    {
        return;
    }
    _cpu_features_internal__known = TRUE;
    _cpu_features_internal__set = _cpu_features_internal__detect();
    value = stdlib__getenv("MEEK_UI_SIMD");
    if (value != NULL_POINTER)
    {
        if (stdlib__strcmp(value, "0") == 0 || stdlib__strcmp(value, "off") == 0 || stdlib__strcmp(value, "scalar") == 0)
        {
            _cpu_features_internal__set = 0;
        }
        else if (stdlib__strcmp(value, "sse2") == 0)
        {
            _cpu_features_internal__set &= CPU_FEATURE__SSE2;
        }
    }
    stdlib__null_memory(_cpu_features_internal__text, sizeof(_cpu_features_internal__text));
    #if defined(CPU_FEATURES__X86)
    stdlib__strcpy_safe(_cpu_features_internal__text, sizeof(_cpu_features_internal__text), (sizeof(void*) == 8) ? "x86-64" : "x86");
    #elif defined(CPU_FEATURES__AARCH64)
    stdlib__strcpy_safe(_cpu_features_internal__text, sizeof(_cpu_features_internal__text), "aarch64");
    #elif defined(CPU_FEATURES__ARM32)
    stdlib__strcpy_safe(_cpu_features_internal__text, sizeof(_cpu_features_internal__text), "arm");
    #elif defined(CPU_FEATURES__POWERPC)
    stdlib__strcpy_safe(_cpu_features_internal__text, sizeof(_cpu_features_internal__text), (sizeof(void*) == 8) ? "powerpc64" : "powerpc");
    #elif defined(CPU_FEATURES__RISCV)
    stdlib__strcpy_safe(_cpu_features_internal__text, sizeof(_cpu_features_internal__text), (sizeof(void*) == 8) ? "riscv64" : "riscv");
    #else
    stdlib__strcpy_safe(_cpu_features_internal__text, sizeof(_cpu_features_internal__text), "unknown");
    #endif
    if ((_cpu_features_internal__set & CPU_FEATURE__SSE2) != 0)
    {
        _cpu_features_internal__append(" sse2");
    }
    if ((_cpu_features_internal__set & CPU_FEATURE__SSE41) != 0)
    {
        _cpu_features_internal__append(" sse4.1");
    }
    if ((_cpu_features_internal__set & CPU_FEATURE__AVX2) != 0)
    {
        _cpu_features_internal__append(" avx2");
    }
    if ((_cpu_features_internal__set & CPU_FEATURE__NEON) != 0)
    {
        _cpu_features_internal__append(" neon");
    }
    if ((_cpu_features_internal__set & CPU_FEATURE__ALTIVEC) != 0)
    {
        _cpu_features_internal__append(" altivec");
    }
    if ((_cpu_features_internal__set & CPU_FEATURE__VSX) != 0)
    {
        _cpu_features_internal__append(" vsx");
    }
    if ((_cpu_features_internal__set & CPU_FEATURE__RVV) != 0)
    {
        _cpu_features_internal__append(" rvv");
    }
    if (_cpu_features_internal__set == 0)
    {
        _cpu_features_internal__append(" scalar");
    }
    log_info("%s %s", "CPU features:", _cpu_features_internal__text);
}

uint cpu_features__get(void)
{
    _cpu_features_internal__ensure();
    return _cpu_features_internal__set;
}

boole cpu_features__has(uint feature)
{
    _cpu_features_internal__ensure();
    return (boole)((_cpu_features_internal__set & feature) == feature);
}

void cpu_features__override(uint features)
{
    _cpu_features_internal__ensure();
    _cpu_features_internal__set = features;
}

const char* cpu_features__describe(void)
{
    _cpu_features_internal__ensure();
    return _cpu_features_internal__text;
}
