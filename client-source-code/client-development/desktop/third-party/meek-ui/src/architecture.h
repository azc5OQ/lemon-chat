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
