#ifndef CPU_FEATURES_H
#define CPU_FEATURES_H

#include "types.h"
#include "meek_ui_config.h"

// Runtime CPU feature detection. The vector paths in the library are
// selected per process from what the CPU reports, never from a build-time
// switch: one binary runs its scalar code on a CPU without the feature and
// its vector code on one with it. The build only decides which vector code
// exists at all (x86 intrinsics do not compile for ARM, and so on).
//
// Detection per architecture and operating system:
// - x86 / x86-64 (Windows, Linux, macOS, FreeBSD, OpenBSD, NetBSD, Android):
//   CPUID, with the XGETBV check for AVX state.
// - AArch64 (Windows, Linux, macOS, the BSDs, Android): NEON is the baseline.
// - 32-bit ARM: Windows IsProcessorFeaturePresent, Linux / Android
//   getauxval(AT_HWCAP), FreeBSD elf_aux_info(AT_HWCAP); other systems
//   report no NEON (scalar paths).
// - PowerPC / PowerPC64: Linux getauxval (AltiVec, VSX), FreeBSD
//   elf_aux_info; other systems report no vector unit.
// - RISC-V: Linux getauxval (the V extension bit); other systems report none.
// - anything else: no features (scalar paths).
typedef enum cpu_feature_e
{
    CPU_FEATURE__SSE2    = 1u << 0,
    CPU_FEATURE__SSE41   = 1u << 1,
    CPU_FEATURE__AVX2    = 1u << 2, // with the OS saving the AVX registers
    CPU_FEATURE__NEON    = 1u << 3,
    CPU_FEATURE__ALTIVEC = 1u << 4, // PowerPC
    CPU_FEATURE__VSX     = 1u << 5, // PowerPC (POWER7 and later)
    CPU_FEATURE__RVV     = 1u << 6  // RISC-V vector extension
} cpu_feature_e;

// The detected feature set, computed on the first call and cached.
// MEEK_UI_SIMD=0 in the environment reports no features at all (the
// scalar paths then run everywhere, which lets a test compare both on the
// same machine); MEEK_UI_SIMD=sse2 limits the set to SSE2.
GUI_API uint  cpu_features__get(void);
GUI_API boole cpu_features__has(uint feature);
// Replace the detected set for this process (tests and hosts that want
// the scalar path); zero disables every vector path.
GUI_API void  cpu_features__override(uint features);
// A short description for logs, such as "x86-64 sse2 sse4.1 avx2".
GUI_API const char* cpu_features__describe(void);

#endif
