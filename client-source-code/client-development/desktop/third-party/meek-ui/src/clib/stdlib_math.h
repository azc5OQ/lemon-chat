// Local math implementation included only by stdlib.c.
// Adapted from selected musl 1.2.5 numerical kernels; no external library.
// Copyright/license notices are retained below and in stdlib_math.LICENSE.
// Configuration is documented at the top of stdlib.h. CPU checks use only
// architecture.h's normalized feature macros; all integer types are local.

// Private amalgamation: preserves existing stdlib.c-only integration.
// Names are remapped before inclusion; no host libm is linked.
// Private numeric helpers. See stdlib_math.LICENSE for source licenses.
#ifndef MEEK_STDLIB_MATH_HELPERS_H
#define MEEK_STDLIB_MATH_HELPERS_H
#ifndef TYPES_H
#define TYPES_H

// types.h - project-wide fixed-width and native-size typedefs
// used in place of stdint. every .c/.h in this project includes this.
// we pull in <stddef.h> so NULL, size_t, ptrdiff_t, and offsetof are
// available wherever types.h is included. avoids needing each
// widget file to remember to drag in stdlib/stdio just for NULL.

#include <stddef.h>

#include "architecture.h"

// byte-aliasing helper
typedef union safe_byte_t
{
    signed char   safe1;   // 0x0
    unsigned char saf2;    // 0x0
} safe_byte_t;

// Native pointer integers also cover ILP32 ABIs on 64-bit instruction sets.
#if ARCHITECTURE_POINTER_BYTES == 8
 typedef unsigned long long nuint;
 typedef signed long long nint;
#elif ARCHITECTURE_POINTER_BYTES == 4
 typedef unsigned int nuint;
 typedef signed int nint;
#else
 typedef unsigned short nuint;
 typedef signed short nint;
#endif

// fixed-width convenience types. use these across the project so that
// number sizes are unambiguous without dragging stdint everywhere.
typedef signed char        boole;
typedef unsigned char      ubyte;
typedef unsigned int       uint;
typedef unsigned short     ushort;
typedef unsigned long long uint64;
typedef signed long long   int64;
typedef unsigned long long timestamp;
typedef const char* cstring;

// These names have fixed contracts; never silently accept a mismatched ABI.
#if defined(__cplusplus)
static_assert(sizeof(nuint) == sizeof(void*) && sizeof(nint) == sizeof(void*), "native integer width");
static_assert(sizeof(ubyte) == 1 && sizeof(ushort) == 2 && sizeof(uint) == 4 && sizeof(uint64) == 8 && sizeof(int64) == 8, "project integer widths");
#else
_Static_assert(sizeof(nuint) == sizeof(void*) && sizeof(nint) == sizeof(void*), "native integer width");
_Static_assert(sizeof(ubyte) == 1 && sizeof(ushort) == 2 && sizeof(uint) == 4 && sizeof(uint64) == 8 && sizeof(int64) == 8, "project integer widths");
#endif

// boolean literals. boole holds 0 or 1; any non-zero is "true" if tested,
// but we always write these two values explicitly.
// guarded with #ifndef because windows.h (via minwindef.h) already
// defines FALSE / TRUE as plain 0 / 1 ints, and our redefinitions
// trigger -Wmacro-redefined warnings in any TU that pulls in both
// headers (platform_win32.c, fs_win32.c). the Win32 defs evaluate to
// the same values our casts would produce (0 and 1 implicitly
// convertible to boole), so skipping the redefine is safe.
#ifndef FALSE
#define FALSE ((boole)0)
#endif
#ifndef TRUE
#define TRUE  ((boole)1)
#endif

// null address literal; pointers are compared against this, never against a bare 0 or NULL
#ifndef NULL_POINTER
#define NULL_POINTER 0
#endif

#endif

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
static double _stdlib_math_scalbn(double, int);
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

#ifndef _STDLIB_INTERNAL_TOINT_INTRINSICS
#define _STDLIB_INTERNAL_TOINT_INTRINSICS 0
#endif

#if _STDLIB_INTERNAL_TOINT_INTRINSICS
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

#define FORCE_EVAL(x) do {                        \
	if (sizeof(x) == sizeof(float)) {         \
		fp_force_evalf(x);                \
	} else if (sizeof(x) == sizeof(double)) { \
		fp_force_eval(x);                 \
	} else {                                  \
		fp_force_evall(x);                \
	}                                         \
} while(0)

#define asuint(f) ((union{float _f; uint _i;}){f})._i
#define asfloat(i) ((union{uint _i; float _f;}){i})._f
#define asuint64(f) ((union{double _f; uint64 _i;}){f})._i
#define asdouble(i) ((union{uint64 _i; double _f;}){i})._f

#define EXTRACT_WORDS(hi,lo,d)                    \
do {                                              \
  uint64 __u = asuint64(d);                     \
  (hi) = __u >> 32;                               \
  (lo) = (uint)__u;                           \
} while (0)

#define GET_HIGH_WORD(hi,d)                       \
do {                                              \
  (hi) = asuint64(d) >> 32;                       \
} while (0)

#define GET_LOW_WORD(lo,d)                        \
do {                                              \
  (lo) = (uint)asuint64(d);                   \
} while (0)

#define INSERT_WORDS(d,hi,lo)                     \
do {                                              \
  (d) = asdouble(((uint64)(hi)<<32) | (uint)(lo)); \
} while (0)

#define SET_HIGH_WORD(d,hi)                       \
  INSERT_WORDS(d, hi, (uint)asuint64(d))

#define SET_LOW_WORD(d,lo)                        \
  INSERT_WORDS(d, asuint64(d)>>32, lo)

#define GET_FLOAT_WORD(w,d)                       \
do {                                              \
  (w) = asuint(d);                                \
} while (0)

#define SET_FLOAT_WORD(d,w)                       \
do {                                              \
  (d) = asfloat(w);                               \
} while (0)

static int    _stdlib_math__rem_pio2_large(double*,double*,int,int,int);


static int    _stdlib_math__rem_pio2f(float,double*);
static float  _stdlib_math__sindf(double);
static float  _stdlib_math__cosdf(double);
static float  _stdlib_math__tandf(double,int);




/* error handling functions */
static float _stdlib_math__math_xflowf(uint, float);
static float _stdlib_math__math_uflowf(uint);
static float _stdlib_math__math_oflowf(uint);
static float _stdlib_math__math_divzerof(uint);
static float _stdlib_math__math_invalidf(float);
static double _stdlib_math__math_invalid(double);
#if LDBL_MANT_DIG != DBL_MANT_DIG
#endif

#endif

/*
 * Shared data between stdlib__expf, exp2f and stdlib__powf.
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




/* Shared between stdlib__expf, exp2f and stdlib__powf.  */
#define EXP2F_TABLE_BITS 5
#define EXP2F_POLY_ORDER 3
static const struct exp2f_data {
	uint64 tab[1 << EXP2F_TABLE_BITS];
	double shift_scaled;
	double poly[EXP2F_POLY_ORDER];
	double shift;
	double invln2_scaled;
	double poly_scaled[EXP2F_POLY_ORDER];
} _stdlib_math__exp2f_data;

#endif


#define N (1 << EXP2F_TABLE_BITS)

static const struct exp2f_data _stdlib_math__exp2f_data = {
  /* tab[i] = uint(2^(i/N)) - (i << 52-BITS)
     used for computing 2^(k/N) for an int |k| < 150 N as
     double(tab[k%N] + (k << 52-BITS)) */
  .tab = {
0x3ff0000000000000, 0x3fefd9b0d3158574, 0x3fefb5586cf9890f, 0x3fef9301d0125b51,
0x3fef72b83c7d517b, 0x3fef54873168b9aa, 0x3fef387a6e756238, 0x3fef1e9df51fdee1,
0x3fef06fe0a31b715, 0x3feef1a7373aa9cb, 0x3feedea64c123422, 0x3feece086061892d,
0x3feebfdad5362a27, 0x3feeb42b569d4f82, 0x3feeab07dd485429, 0x3feea47eb03a5585,
0x3feea09e667f3bcd, 0x3fee9f75e8ec5f74, 0x3feea11473eb0187, 0x3feea589994cce13,
0x3feeace5422aa0db, 0x3feeb737b0cdc5e5, 0x3feec49182a3f090, 0x3feed503b23e255d,
0x3feee89f995ad3ad, 0x3feeff76f2fb5e47, 0x3fef199bdd85529c, 0x3fef3720dcef9069,
0x3fef5818dcfba487, 0x3fef7c97337b9b5f, 0x3fefa4afa2a490da, 0x3fefd0765b6e4540,
  },
  .shift_scaled = 0x1.8p+52 / N,
  .poly = {
  0x1.c6af84b912394p-5, 0x1.ebfce50fac4f3p-3, 0x1.62e42ff0c52d6p-1,
  },
  .shift = 0x1.8p+52,
  .invln2_scaled = 0x1.71547652b82fep+0 * N,
  .poly_scaled = {
  0x1.c6af84b912394p-5/N/N/N, 0x1.ebfce50fac4f3p-3/N/N, 0x1.62e42ff0c52d6p-1/N,
  },
};

#undef N
/*
 * Data definition for stdlib__powf.
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
#if _STDLIB_INTERNAL_TOINT_INTRINSICS
#define POWF_SCALE_BITS EXP2F_TABLE_BITS
#else
#define POWF_SCALE_BITS 0
#endif
#define POWF_SCALE ((double)(1 << POWF_SCALE_BITS))
static const struct powf_log2_data {
	struct {
		double invc, logc;
	} tab[1 << POWF_LOG2_TABLE_BITS];
	double poly[POWF_LOG2_POLY_ORDER];
} _stdlib_math__powf_log2_data;

#endif


static const struct powf_log2_data _stdlib_math__powf_log2_data = {
  .tab = {
  { 0x1.661ec79f8f3bep+0, -0x1.efec65b963019p-2 * POWF_SCALE },
  { 0x1.571ed4aaf883dp+0, -0x1.b0b6832d4fca4p-2 * POWF_SCALE },
  { 0x1.49539f0f010bp+0, -0x1.7418b0a1fb77bp-2 * POWF_SCALE },
  { 0x1.3c995b0b80385p+0, -0x1.39de91a6dcf7bp-2 * POWF_SCALE },
  { 0x1.30d190c8864a5p+0, -0x1.01d9bf3f2b631p-2 * POWF_SCALE },
  { 0x1.25e227b0b8eap+0, -0x1.97c1d1b3b7afp-3 * POWF_SCALE },
  { 0x1.1bb4a4a1a343fp+0, -0x1.2f9e393af3c9fp-3 * POWF_SCALE },
  { 0x1.12358f08ae5bap+0, -0x1.960cbbf788d5cp-4 * POWF_SCALE },
  { 0x1.0953f419900a7p+0, -0x1.a6f9db6475fcep-5 * POWF_SCALE },
  { 0x1p+0, 0x0p+0 * POWF_SCALE },
  { 0x1.e608cfd9a47acp-1, 0x1.338ca9f24f53dp-4 * POWF_SCALE },
  { 0x1.ca4b31f026aap-1, 0x1.476a9543891bap-3 * POWF_SCALE },
  { 0x1.b2036576afce6p-1, 0x1.e840b4ac4e4d2p-3 * POWF_SCALE },
  { 0x1.9c2d163a1aa2dp-1, 0x1.40645f0c6651cp-2 * POWF_SCALE },
  { 0x1.886e6037841edp-1, 0x1.88e9c2c1b9ff8p-2 * POWF_SCALE },
  { 0x1.767dcf5534862p-1, 0x1.ce0a44eb17bccp-2 * POWF_SCALE },
  },
  .poly = {
  0x1.27616c9496e0bp-2 * POWF_SCALE, -0x1.71969a075c67ap-2 * POWF_SCALE,
  0x1.ec70a6ca7baddp-2 * POWF_SCALE, -0x1.7154748bef6c8p-1 * POWF_SCALE,
  0x1.71547652ab82bp0 * POWF_SCALE,
  }
};

/*
 * Data definition for stdlib__logf.
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
static const struct logf_data {
	struct {
		double invc, logc;
	} tab[1 << LOGF_TABLE_BITS];
	double ln2;
	double poly[LOGF_POLY_ORDER - 1]; /* First order coefficient is 1.  */
} _stdlib_math__logf_data;

#endif


static const struct logf_data _stdlib_math__logf_data = {
  .tab = {
  { 0x1.661ec79f8f3bep+0, -0x1.57bf7808caadep-2 },
  { 0x1.571ed4aaf883dp+0, -0x1.2bef0a7c06ddbp-2 },
  { 0x1.49539f0f010bp+0, -0x1.01eae7f513a67p-2 },
  { 0x1.3c995b0b80385p+0, -0x1.b31d8a68224e9p-3 },
  { 0x1.30d190c8864a5p+0, -0x1.6574f0ac07758p-3 },
  { 0x1.25e227b0b8eap+0, -0x1.1aa2bc79c81p-3 },
  { 0x1.1bb4a4a1a343fp+0, -0x1.a4e76ce8c0e5ep-4 },
  { 0x1.12358f08ae5bap+0, -0x1.1973c5a611cccp-4 },
  { 0x1.0953f419900a7p+0, -0x1.252f438e10c1ep-5 },
  { 0x1p+0, 0x0p+0 },
  { 0x1.e608cfd9a47acp-1, 0x1.aa5aa5df25984p-5 },
  { 0x1.ca4b31f026aap-1, 0x1.c5e53aa362eb4p-4 },
  { 0x1.b2036576afce6p-1, 0x1.526e57720db08p-3 },
  { 0x1.9c2d163a1aa2dp-1, 0x1.bc2860d22477p-3 },
  { 0x1.886e6037841edp-1, 0x1.1058bc8a07ee1p-2 },
  { 0x1.767dcf5534862p-1, 0x1.4043057b6ee09p-2 },
  },
  .ln2 = 0x1.62e42fefa39efp-1,
  .poly = {
  -0x1.00ea348b88334p-2, 0x1.5575b0be00b6ap-2, -0x1.ffffef20a4123p-2,
  }
};

#ifndef _SQRT_DATA_H
#define _SQRT_DATA_H




/* if x in [1,2): i = (int)(64*x);
   if x in [2,4): i = (int)(32*x-64);
   _stdlib_math__rsqrt_tab[i]*2^-16 is estimating 1/stdlib__sqrt(x) with small relative error:
   |_stdlib_math__rsqrt_tab[i]*0x1p-16*stdlib__sqrt(x) - 1| < -0x1.fdp-9 < 2^-8 */
static const ushort _stdlib_math__rsqrt_tab[128];

#endif

static const ushort _stdlib_math__rsqrt_tab[128] = {
0xb451,0xb2f0,0xb196,0xb044,0xaef9,0xadb6,0xac79,0xab43,
0xaa14,0xa8eb,0xa7c8,0xa6aa,0xa592,0xa480,0xa373,0xa26b,
0xa168,0xa06a,0x9f70,0x9e7b,0x9d8a,0x9c9d,0x9bb5,0x9ad1,
0x99f0,0x9913,0x983a,0x9765,0x9693,0x95c4,0x94f8,0x9430,
0x936b,0x92a9,0x91ea,0x912e,0x9075,0x8fbe,0x8f0a,0x8e59,
0x8daa,0x8cfe,0x8c54,0x8bac,0x8b07,0x8a64,0x89c4,0x8925,
0x8889,0x87ee,0x8756,0x86c0,0x862b,0x8599,0x8508,0x8479,
0x83ec,0x8361,0x82d8,0x8250,0x81c9,0x8145,0x80c2,0x8040,
0xff02,0xfd0e,0xfb25,0xf947,0xf773,0xf5aa,0xf3ea,0xf234,
0xf087,0xeee3,0xed47,0xebb3,0xea27,0xe8a3,0xe727,0xe5b2,
0xe443,0xe2dc,0xe17a,0xe020,0xdecb,0xdd7d,0xdc34,0xdaf1,
0xd9b3,0xd87b,0xd748,0xd61a,0xd4f1,0xd3cd,0xd2ad,0xd192,
0xd07b,0xcf69,0xce5b,0xcd51,0xcc4a,0xcb48,0xca4a,0xc94f,
0xc858,0xc764,0xc674,0xc587,0xc49d,0xc3b7,0xc2d4,0xc1f4,
0xc116,0xc03c,0xbf65,0xbe90,0xbdbe,0xbcef,0xbc23,0xbb59,
0xba91,0xb9cc,0xb90a,0xb84a,0xb78c,0xb6d0,0xb617,0xb560,
};



static float _stdlib_math__math_xflowf(uint sign, float y)
{
	return eval_as_float(fp_barrierf(sign ? -y : y) * y);
}



static float _stdlib_math__math_oflowf(uint sign)
{
	return _stdlib_math__math_xflowf(sign, 0x1p97f);
}



static float _stdlib_math__math_uflowf(uint sign)
{
	return _stdlib_math__math_xflowf(sign, 0x1p-95f);
}



static float _stdlib_math__math_divzerof(uint sign)
{
	return fp_barrierf(sign ? -1.0f : 1.0f) / 0.0f;
}



static float _stdlib_math__math_invalidf(float x)
{
	return (x - x) / (x - x);
}



static double _stdlib_math__math_invalid(double x)
{
	return (x - x) / (x - x);
}




static double _stdlib_math_scalbn(double x, int n)
{
	union {double f; uint64 i;} u;
	double_t y = x;

	if (n > 1023) {
		y *= 0x1p1023;
		n -= 1023;
		if (n > 1023) {
			y *= 0x1p1023;
			n -= 1023;
			if (n > 1023)
				n = 1023;
		}
	} else if (n < -1022) {
		/* make sure final n < -53 to avoid double
		   rounding in the subnormal range */
		y *= 0x1p-1022 * 0x1p53;
		n += 1022 - 53;
		if (n < -1022) {
			y *= 0x1p-1022 * 0x1p53;
			n += 1022 - 53;
			if (n < -1022)
				n = -1022;
		}
	}
	u.i = (uint64)(0x3ff+n)<<52;
	x = y * u.f;
	return x;
}




float stdlib__fabsf(float x)
{
	union {float f; uint i;} u = {x};
	u.i &= 0x7fffffff;
	return u.f;
}




double stdlib__fabs(double x)
{
	union {double f; uint64 i;} u = {x};
	u.i &= -1ULL/2;
	return u.f;
}



float stdlib__floorf(float x)
{
	union {float f; uint i;} u = {x};
	int e = (int)(u.i >> 23 & 0xff) - 0x7f;
	uint m;

	if (e >= 23)
		return x;
	if (e >= 0) {
		m = 0x007fffff >> e;
		if ((u.i & m) == 0)
			return x;
		FORCE_EVAL(x + 0x1p120f);
		if (u.i >> 31)
			u.i += m;
		u.i &= ~m;
	} else {
		FORCE_EVAL(x + 0x1p120f);
		if (u.i >> 31 == 0)
			u.i = 0;
		else if (u.i << 1)
			u.f = -1.0;
	}
	return u.f;
}



#if FLT_EVAL_METHOD==0 || FLT_EVAL_METHOD==1
#define EPS DBL_EPSILON
#elif FLT_EVAL_METHOD==2
#define EPS LDBL_EPSILON
#endif
static const double_t _stdlib_math_floor_toint = 1/EPS;

double stdlib__floor(double x)
{
	union {double f; uint64 i;} u = {x};
	int e = u.i >> 52 & 0x7ff;
	double_t y;

	if (e >= 0x3ff+52 || x == 0)
		return x;
	/* y = int(x) - x, where int(x) is an integer neighbor of x */
	if (u.i >> 63)
		y = x - _stdlib_math_floor_toint + _stdlib_math_floor_toint - x;
	else
		y = x + _stdlib_math_floor_toint - _stdlib_math_floor_toint - x;
	/* special case because of non-nearest rounding modes */
	if (e <= 0x3ff-1) {
		FORCE_EVAL(y);
		return u.i >> 63 ? -1 : 0;
	}
	if (y > 0)
		return x + y - 1;
	return x + y;
}

#undef EPS


float stdlib__ceilf(float x)
{
	union {float f; uint i;} u = {x};
	int e = (int)(u.i >> 23 & 0xff) - 0x7f;
	uint m;

	if (e >= 23)
		return x;
	if (e >= 0) {
		m = 0x007fffff >> e;
		if ((u.i & m) == 0)
			return x;
		FORCE_EVAL(x + 0x1p120f);
		if (u.i >> 31 == 0)
			u.i += m;
		u.i &= ~m;
	} else {
		FORCE_EVAL(x + 0x1p120f);
		if (u.i >> 31)
			u.f = -0.0;
		else if (u.i << 1)
			u.f = 1.0;
	}
	return u.f;
}



#if FLT_EVAL_METHOD==0 || FLT_EVAL_METHOD==1
#define EPS DBL_EPSILON
#elif FLT_EVAL_METHOD==2
#define EPS LDBL_EPSILON
#endif
static const double_t _stdlib_math_ceil_toint = 1/EPS;

double stdlib__ceil(double x)
{
	union {double f; uint64 i;} u = {x};
	int e = u.i >> 52 & 0x7ff;
	double_t y;

	if (e >= 0x3ff+52 || x == 0)
		return x;
	/* y = int(x) - x, where int(x) is an integer neighbor of x */
	if (u.i >> 63)
		y = x - _stdlib_math_ceil_toint + _stdlib_math_ceil_toint - x;
	else
		y = x + _stdlib_math_ceil_toint - _stdlib_math_ceil_toint - x;
	/* special case because of non-nearest rounding modes */
	if (e <= 0x3ff-1) {
		FORCE_EVAL(y);
		return u.i >> 63 ? -0.0 : 1;
	}
	if (y < 0)
		return x + y + 1;
	return x + y;
}

#undef EPS



float stdlib__fmodf(float x, float y)
{
	union {float f; uint i;} ux = {x}, uy = {y};
	int ex = ux.i>>23 & 0xff;
	int ey = uy.i>>23 & 0xff;
	uint sx = ux.i & 0x80000000;
	uint i;
	uint uxi = ux.i;

	if (uy.i<<1 == 0 || isnan(y) || ex == 0xff)
		return (x*y)/(x*y);
	if (uxi<<1 <= uy.i<<1) {
		if (uxi<<1 == uy.i<<1)
			return 0*x;
		return x;
	}

#if defined(ARCHITECTURE_FAST_DOUBLE)
	/* meek-ui: for normal operands and quotient < 2^24, a 24-bit
	 * significand times the integer quotient fits exactly in binary64.
	 * The subtraction is exact, avoiding the bit-at-a-time reduction. */
	if (ex && ey && ex - ey < 24) {
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
	if (!ex) {
		for (i = uxi<<9; i>>31 == 0; ex--, i <<= 1);
		uxi <<= -ex + 1;
	} else {
		uxi &= -1U >> 9;
		uxi |= 1U << 23;
	}
	if (!ey) {
		for (i = uy.i<<9; i>>31 == 0; ey--, i <<= 1);
		uy.i <<= -ey + 1;
	} else {
		uy.i &= -1U >> 9;
		uy.i |= 1U << 23;
	}

	/* x mod y */
	for (; ex > ey; ex--) {
		i = uxi - uy.i;
		if (i >> 31 == 0) {
			if (i == 0)
				return 0*x;
			uxi = i;
		}
		uxi <<= 1;
	}
	i = uxi - uy.i;
	if (i >> 31 == 0) {
		if (i == 0)
			return 0*x;
		uxi = i;
	}
	for (; uxi>>23 == 0; uxi <<= 1, ex--);

	/* scale result up */
	if (ex > 0) {
		uxi -= 1U << 23;
		uxi |= (uint)ex << 23;
	} else {
		uxi >>= -ex + 1;
	}
	uxi |= sx;
	ux.i = uxi;
	return ux.f;
}




double stdlib__fmod(double x, double y)
{
	union {double f; uint64 i;} ux = {x}, uy = {y};
	int ex = ux.i>>52 & 0x7ff;
	int ey = uy.i>>52 & 0x7ff;
	int sx = ux.i>>63;
	uint64 i;

	/* in the followings uxi should be ux.i, but then gcc wrongly adds */
	/* float load/store to inner loops ruining performance and code size */
	uint64 uxi = ux.i;

	if (uy.i<<1 == 0 || isnan(y) || ex == 0x7ff)
		return (x*y)/(x*y);
	if (uxi<<1 <= uy.i<<1) {
		if (uxi<<1 == uy.i<<1)
			return 0*x;
		return x;
	}

	/* normalize x and y */
	if (!ex) {
		for (i = uxi<<12; i>>63 == 0; ex--, i <<= 1);
		uxi <<= -ex + 1;
	} else {
		uxi &= -1ULL >> 12;
		uxi |= 1ULL << 52;
	}
	if (!ey) {
		for (i = uy.i<<12; i>>63 == 0; ey--, i <<= 1);
		uy.i <<= -ey + 1;
	} else {
		uy.i &= -1ULL >> 12;
		uy.i |= 1ULL << 52;
	}

	/* x mod y */
	for (; ex > ey; ex--) {
		i = uxi - uy.i;
		if (i >> 63 == 0) {
			if (i == 0)
				return 0*x;
			uxi = i;
		}
		uxi <<= 1;
	}
	i = uxi - uy.i;
	if (i >> 63 == 0) {
		if (i == 0)
			return 0*x;
		uxi = i;
	}
	for (; uxi>>52 == 0; uxi <<= 1, ex--);

	/* scale result */
	if (ex > 0) {
		uxi -= 1ULL << 52;
		uxi |= (uint64)ex << 52;
	} else {
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
static const double
S1 = -0x15555554cbac77.0p-55, /* -0.166666666416265235595 */
S2 =  0x111110896efbb2.0p-59, /*  0.0083333293858894631756 */
S3 = -0x1a00f9e2cae774.0p-65, /* -0.000198393348360966317347 */
S4 =  0x16cd878c3b46a7.0p-71; /*  0.0000027183114939898219064 */

static float _stdlib_math__sindf(double x)
{
	double_t r, s, w, z;

	/* Try to optimize for parallel evaluation as in _stdlib_math__tandf.c. */
	z = x*x;
	w = z*z;
	r = S3 + z*S4;
	s = z*x;
	return (x + s*(S1 + z*S2)) + s*w*r;
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
static const double
C0  = -0x1ffffffd0c5e81.0p-54, /* -0.499999997251031003120 */
C1  =  0x155553e1053a42.0p-57, /*  0.0416666233237390631894 */
C2  = -0x16c087e80f1e27.0p-62, /* -0.00138867637746099294692 */
C3  =  0x199342e0ee5069.0p-68; /*  0.0000243904487962774090654 */

static float _stdlib_math__cosdf(double x)
{
	double_t r, w, z;

	/* Try to optimize for parallel evaluation as in _stdlib_math__tandf.c. */
	z = x*x;
	w = z*z;
	r = C2+z*C3;
	return ((1.0+z*C0) + w*C1) + (w*z)*r;
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

static float _stdlib_math__tandf(double x, int odd)
{
	double_t z,r,w,s,t,u;

	z = x*x;
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
	r = T[4] + z*T[5];
	t = T[2] + z*T[3];
	w = z*z;
	s = z*x;
	u = T[0] + z*T[1];
	r = (x + s*u) + (s*w)*(t + w*r);
	return odd ? -1.0/r : r;
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
 * _stdlib_math__rem_pio2_large(x,y,e0,nx,prec)
 * double x[],y[]; int e0,nx,prec;
 *
 * _stdlib_math__rem_pio2_large return the last three digits of N with
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
 *                      z  = _stdlib_math_scalbn(z,-e0)
 *              for i = 0,1,2
 *                      x[i] = stdlib__floor(z)
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
 *      double _stdlib_math_scalbn(), stdlib__floor();
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



static const int init_jk[] = {3,4,4,6}; /* initial value for jk */

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
0xA2F983, 0x6E4E44, 0x1529FC, 0x2757D1, 0xF534DD, 0xC0DB62,
0x95993C, 0x439041, 0xFE5163, 0xABDEBB, 0xC561B7, 0x246E3A,
0x424DD2, 0xE00649, 0x2EEA09, 0xD1921C, 0xFE1DEB, 0x1CB129,
0xA73EE8, 0x8235F5, 0x2EBB44, 0x84E99C, 0x7026B4, 0x5F7E41,
0x3991D6, 0x398353, 0x39F49C, 0x845F8B, 0xBDF928, 0x3B1FF8,
0x97FFDE, 0x05980F, 0xEF2F11, 0x8B5A0A, 0x6D1F6D, 0x367ECF,
0x27CB09, 0xB74F46, 0x3F669E, 0x5FEA2D, 0x7527BA, 0xC7EBE5,
0xF17B3D, 0x0739F7, 0x8A5292, 0xEA6BFB, 0x5FB11F, 0x8D5D08,
0x560330, 0x46FC7B, 0x6BABF0, 0xCFBC20, 0x9AF436, 0x1DA9E3,
0x91615E, 0xE61B08, 0x659985, 0x5F14A0, 0x68408D, 0xFFD880,
0x4D7327, 0x310606, 0x1556CA, 0x73A8C9, 0x60E27B, 0xC08C6B,

#if LDBL_MAX_EXP > 1024
0x47C419, 0xC367CD, 0xDCE809, 0x2A8359, 0xC4768B, 0x961CA6,
0xDDAF44, 0xD15719, 0x053EA5, 0xFF0705, 0x3F7E33, 0xE832C2,
0xDE4F98, 0x327DBB, 0xC33D26, 0xEF6B1E, 0x5EF89F, 0x3A1F35,
0xCAF27F, 0x1D87F1, 0x21907C, 0x7C246A, 0xFA6ED5, 0x772D30,
0x433B15, 0xC614B5, 0x9D19C3, 0xC2C4AD, 0x414D2C, 0x5D000C,
0x467D86, 0x2D71E3, 0x9AC69B, 0x006233, 0x7CD2B4, 0x97A7B4,
0xD55537, 0xF63ED7, 0x1810A3, 0xFC764D, 0x2A9D64, 0xABD770,
0xF87C63, 0x57B07A, 0xE71517, 0x5649C0, 0xD9D63B, 0x3884A7,
0xCB2324, 0x778AD6, 0x23545A, 0xB91F00, 0x1B0AF1, 0xDFCE19,
0xFF319F, 0x6A1E66, 0x615799, 0x47FBAC, 0xD87F7E, 0xB76522,
0x89E832, 0x60BFE6, 0xCDC4EF, 0x09366C, 0xD43F5D, 0xD7DE16,
0xDE3B58, 0x929BDE, 0x2822D2, 0xE88628, 0x4D58E2, 0x32CAC6,
0x16E308, 0xCB7DE0, 0x50C017, 0xA71DF3, 0x5BE018, 0x34132E,
0x621283, 0x014883, 0x5B8EF5, 0x7FB0AD, 0xF2E91E, 0x434A48,
0xD36710, 0xD8DDAA, 0x425FAE, 0xCE616A, 0xA4280A, 0xB499D3,
0xF2A606, 0x7F775C, 0x83C2A3, 0x883C61, 0x78738A, 0x5A8CAF,
0xBDD76F, 0x63A62D, 0xCBBFF4, 0xEF818D, 0x67C126, 0x45CA55,
0x36D9CA, 0xD2A828, 0x8D61C2, 0x77C912, 0x142604, 0x9B4612,
0xC459C4, 0x44C5C8, 0x91B24D, 0xF31700, 0xAD43D4, 0xE54929,
0x10D5FD, 0xFCBE00, 0xCC941E, 0xEECE70, 0xF53E13, 0x80F1EC,
0xC3E7B3, 0x28F8C7, 0x940593, 0x3E71C1, 0xB3092E, 0xF3450B,
0x9C1288, 0x7B20AB, 0x9FB52E, 0xC29247, 0x2F327B, 0x6D550C,
0x90A772, 0x1FE76B, 0x96CB31, 0x4A1679, 0xE27941, 0x89DFF4,
0x9794E8, 0x84E6E2, 0x973199, 0x6BED88, 0x365F5F, 0x0EFDBB,
0xB49A48, 0x6CA467, 0x427271, 0x325D8D, 0xB8159F, 0x09E5BC,
0x25318D, 0x3974F7, 0x1C0530, 0x010C0D, 0x68084B, 0x58EE2C,
0x90AA47, 0x02E774, 0x24D6BD, 0xA67DF7, 0x72486E, 0xEF169F,
0xA6948E, 0xF691B4, 0x5153D1, 0xF20ACF, 0x339820, 0x7E4BF5,
0x6863B2, 0x5F3EDD, 0x035D40, 0x7F8985, 0x295255, 0xC06437,
0x10D86D, 0x324832, 0x754C5B, 0xD4714E, 0x6E5445, 0xC1090B,
0x69F52A, 0xD56614, 0x9D0727, 0x50045D, 0xDB3BB4, 0xC576EA,
0x17F987, 0x7D6B49, 0xBA271D, 0x296996, 0xACCCC6, 0x5414AD,
0x6AE290, 0x89D988, 0x50722C, 0xBEA404, 0x940777, 0x7030F3,
0x27FC00, 0xA871EA, 0x49C266, 0x3DE064, 0x83DD97, 0x973FA3,
0xFD9443, 0x8C860D, 0xDE4131, 0x9D3992, 0x8C70DD, 0xE7B717,
0x3BDF08, 0x2B3715, 0xA0805C, 0x93805A, 0x921110, 0xD8E80F,
0xAF806C, 0x4BFFDB, 0x0F9038, 0x761859, 0x15A562, 0xBBCB61,
0xB989C7, 0xBD4010, 0x04F2D2, 0x277549, 0xF6B6EB, 0xBB22DB,
0xAA140A, 0x2F2689, 0x768364, 0x333B09, 0x1A940E, 0xAA3A51,
0xC2A31D, 0xAEEDAF, 0x12265C, 0x4DC26D, 0x9C7A2D, 0x9756C0,
0x833F03, 0xF6F009, 0x8C402B, 0x99316D, 0x07B439, 0x15200C,
0x5BC3D8, 0xC492F5, 0x4BADC6, 0xA5CA4E, 0xCD37A7, 0x36A9E6,
0x9492AB, 0x6842DD, 0xDE6319, 0xEF8C76, 0x528B68, 0x37DBFC,
0xABA1AE, 0x3115DF, 0xA1AE00, 0xDAFB0C, 0x664D64, 0xB705ED,
0x306529, 0xBF5657, 0x3AFF47, 0xB9F96A, 0xF3BE75, 0xDF9328,
0x3080AB, 0xF68C66, 0x15CB04, 0x0622FA, 0x1DE4D9, 0xA4B33D,
0x8F1B57, 0x09CD36, 0xE9424E, 0xA4BE13, 0xB52333, 0x1AAAF0,
0xA8654F, 0xA5C1D2, 0x0F3F0B, 0xCD785B, 0x76F923, 0x048B7B,
0x721789, 0x53A6C6, 0xE26E6F, 0x00EBEF, 0x584A9B, 0xB7DAC4,
0xBA66AA, 0xCFCF76, 0x1D02D1, 0x2DF1B1, 0xC1998C, 0x77ADC3,
0xDA4886, 0xA05DF7, 0xF480C6, 0x2FF0AC, 0x9AECDD, 0xBC5C3F,
0x6DDED0, 0x1FC790, 0xB6DB2A, 0x3A25A3, 0x9AAF00, 0x9353AD,
0x0457B6, 0xB42D29, 0x7E804B, 0xA707DA, 0x0EAA76, 0xA1597B,
0x2A1216, 0x2DB7DC, 0xFDE5FA, 0xFEDB89, 0xFDBE89, 0x6C76E4,
0xFCA906, 0x70803E, 0x156E85, 0xFF87FD, 0x073E28, 0x336761,
0x86182A, 0xEABD4D, 0xAFE7B3, 0x6E6D8F, 0x396795, 0x5BBF31,
0x48D784, 0x16DF30, 0x432DC7, 0x356125, 0xCE70C9, 0xB8CB30,
0xFD6CBF, 0xA200A4, 0xE46C05, 0xA0DD5A, 0x476F21, 0xD21262,
0x845CB9, 0x496170, 0xE0566B, 0x015299, 0x375550, 0xB7D51E,
0xC4F133, 0x5F6E13, 0xE4305D, 0xA92E85, 0xC3B21D, 0x3632A1,
0xA4B708, 0xD4B1EA, 0x21F716, 0xE4698F, 0x77FF27, 0x80030C,
0x2D408D, 0xA0CD4F, 0x99A520, 0xD3A2B3, 0x0A5D2F, 0x42F9B4,
0xCBDA11, 0xD0BE7D, 0xC1DB9B, 0xBD17AB, 0x81A2CA, 0x5C6A08,
0x17552E, 0x550027, 0xF0147F, 0x8607E1, 0x640B14, 0x8D4196,
0xDEBE87, 0x2AFDDA, 0xB6256B, 0x34897B, 0xFEF305, 0x9EBFB9,
0x4F6A68, 0xA82A4A, 0x5AC44F, 0xBCF82D, 0x985AD7, 0x95C7F4,
0x8D4D0D, 0xA63A20, 0x5F57A4, 0xB13F14, 0x953880, 0x0120CC,
0x86DD71, 0xB6DEC9, 0xF560BF, 0x11654D, 0x6B0701, 0xACB08C,
0xD0C0B2, 0x485551, 0x0EFB1E, 0xC37295, 0x3B06A3, 0x3540C0,
0x7BDC06, 0xCC45E0, 0xFA294E, 0xC8CAD6, 0x41F3E8, 0xDE647C,
0xD8649B, 0x31BED9, 0xC397A4, 0xD45877, 0xC5E369, 0x13DAF0,
0x3C3ABA, 0x461846, 0x5F7555, 0xF5BDD2, 0xC6926E, 0x5D2EAC,
0xED440E, 0x423E1C, 0x87C461, 0xE9FD29, 0xF3D6E7, 0xCA7C22,
0x35916F, 0xC5E008, 0x8DD7FF, 0xE26A6E, 0xC6FDB0, 0xC10893,
0x745D7C, 0xB2AD6B, 0x9D6ECD, 0x7B723E, 0x6A11C6, 0xA9CFF7,
0xDF7329, 0xBAC9B5, 0x5100B7, 0x0DB2E2, 0x24BA74, 0x607DE5,
0x8AD874, 0x2C150D, 0x0C1881, 0x94667E, 0x162901, 0x767A9F,
0xBEFDFD, 0xEF4556, 0x367ED9, 0x13D9EC, 0xB9BA8B, 0xFC97C4,
0x27A831, 0xC36EF1, 0x36C594, 0x56A8D8, 0xB5A8B4, 0x0ECCCF,
0x2D8912, 0x34576F, 0x89562C, 0xE3CE99, 0xB920D6, 0xAA5E6B,
0x9C2A3E, 0xCC5F11, 0x4A0BFD, 0xFBF4E1, 0x6D3B8E, 0x2C86E2,
0x84D4E9, 0xA9B4FC, 0xD1EEEF, 0xC9352E, 0x61392F, 0x442138,
0xC8D91B, 0x0AFC81, 0x6A4AFB, 0xD81C2F, 0x84B453, 0x8C994E,
0xCC2254, 0xDC552A, 0xD6C6C0, 0x96190B, 0xB8701A, 0x649569,
0x605A26, 0xEE523F, 0x0F117F, 0x11B5F4, 0xF5CBFC, 0x2DBC34,
0xEEBC34, 0xCC5DE8, 0x605EDD, 0x9B8E67, 0xEF3392, 0xB817C9,
0x9B5861, 0xBC57E1, 0xC68351, 0x103ED8, 0x4871DD, 0xDD1C2D,
0xA118AF, 0x462C21, 0xD7F359, 0x987AD9, 0xC0549E, 0xFA864F,
0xFC0656, 0xAE79E5, 0x362289, 0x22AD38, 0xDC9367, 0xAAE855,
0x382682, 0x9BE7CA, 0xA40D51, 0xB13399, 0x0ED7A9, 0x480569,
0xF0B265, 0xA7887F, 0x974C88, 0x36D1F9, 0xB39221, 0x4A827B,
0x21CF98, 0xDC9F40, 0x5547DC, 0x3A74E1, 0x42EB67, 0xDF9DFE,
0x5FD45E, 0xA4677B, 0x7AACBA, 0xA2F655, 0x23882B, 0x55BA41,
0x086E59, 0x862A21, 0x834739, 0xE6E389, 0xD49EE5, 0x40FB49,
0xE956FF, 0xCA0F1C, 0x8A59C5, 0x2BFA94, 0xC5C1D3, 0xCFC50F,
0xAE5ADB, 0x86C547, 0x624385, 0x3B8621, 0x94792C, 0x876110,
0x7B4C2A, 0x1A2C80, 0x12BF43, 0x902688, 0x893C78, 0xE4C4A8,
0x7BDBE5, 0xC23AC4, 0xEAF426, 0x8A67F7, 0xBF920D, 0x2BA365,
0xB1933D, 0x0B7CBD, 0xDC51A4, 0x63DD27, 0xDDE169, 0x19949A,
0x9529A8, 0x28CE68, 0xB4ED09, 0x209F44, 0xCA984E, 0x638270,
0x237C7E, 0x32B90F, 0x8EF5A7, 0xE75614, 0x08F121, 0x2A9DB5,
0x4D7E6F, 0x5119A5, 0xABF9B5, 0xD6DF82, 0x61DD96, 0x023616,
0x9F3AC4, 0xA1A283, 0x6DED72, 0x7A8D39, 0xA9B882, 0x5C326B,
0x5B2746, 0xED3400, 0x7700D2, 0x55F4FC, 0x4D5901, 0x8071E0,
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

static int _stdlib_math__rem_pio2_large(double *x, double *y, int e0, int nx, int prec)
{
	int jz,jx,jv,jp,jk,carry,n,iq[20],i,j,k,m,q0,ih;
	double z,fw,f[20],fq[20],q[20];

	/* initialize jk*/
	jk = init_jk[prec];
	jp = jk;

	/* determine jx,jv,q0, note that 3>q0 */
	jx = nx-1;
	jv = (e0-3)/24;  if(jv<0) jv=0;
	q0 = e0-24*(jv+1);

	/* set up f[0] to f[jx+jk] where f[jx+jk] = ipio2[jv+jk] */
	j = jv-jx; m = jx+jk;
	for (i=0; i<=m; i++,j++)
		f[i] = j<0 ? 0.0 : (double)ipio2[j];

	/* compute q[0],q[1],...q[jk] */
	for (i=0; i<=jk; i++) {
		for (j=0,fw=0.0; j<=jx; j++)
			fw += x[j]*f[jx+i-j];
		q[i] = fw;
	}

	jz = jk;
recompute:
	/* distill q[] into iq[] reversingly */
	for (i=0,j=jz,z=q[jz]; j>0; i++,j--) {
		fw    = (double)(int)(0x1p-24*z);
		iq[i] = (int)(z - 0x1p24*fw);
		z     = q[j-1]+fw;
	}

	/* compute n */
	z  = _stdlib_math_scalbn(z,q0);       /* actual value of z */
	z -= 8.0*stdlib__floor(z*0.125); /* trim off integer >= 8 */
	n  = (int)z;
	z -= (double)n;
	ih = 0;
	if (q0 > 0) {  /* need iq[jz-1] to determine n */
		i  = iq[jz-1]>>(24-q0); n += i;
		iq[jz-1] -= i<<(24-q0);
		ih = iq[jz-1]>>(23-q0);
	}
	else if (q0 == 0) ih = iq[jz-1]>>23;
	else if (z >= 0.5) ih = 2;

	if (ih > 0) {  /* q > 0.5 */
		n += 1; carry = 0;
		for (i=0; i<jz; i++) {  /* compute 1-q */
			j = iq[i];
			if (carry == 0) {
				if (j != 0) {
					carry = 1;
					iq[i] = 0x1000000 - j;
				}
			} else
				iq[i] = 0xffffff - j;
		}
		if (q0 > 0) {  /* rare case: chance is 1 in 12 */
			switch(q0) {
			case 1:
				iq[jz-1] &= 0x7fffff; break;
			case 2:
				iq[jz-1] &= 0x3fffff; break;
			}
		}
		if (ih == 2) {
			z = 1.0 - z;
			if (carry != 0)
				z -= _stdlib_math_scalbn(1.0,q0);
		}
	}

	/* check if recomputation is needed */
	if (z == 0.0) {
		j = 0;
		for (i=jz-1; i>=jk; i--) j |= iq[i];
		if (j == 0) {  /* need recomputation */
			for (k=1; iq[jk-k]==0; k++);  /* k = no. of terms needed */

			for (i=jz+1; i<=jz+k; i++) {  /* add q[jz+1] to q[jz+k] */
				f[jx+i] = (double)ipio2[jv+i];
				for (j=0,fw=0.0; j<=jx; j++)
					fw += x[j]*f[jx+i-j];
				q[i] = fw;
			}
			jz += k;
			goto recompute;
		}
	}

	/* chop off zero terms */
	if (z == 0.0) {
		jz -= 1;
		q0 -= 24;
		while (iq[jz] == 0) {
			jz--;
			q0 -= 24;
		}
	} else { /* break z into 24-bit if necessary */
		z = _stdlib_math_scalbn(z,-q0);
		if (z >= 0x1p24) {
			fw = (double)(int)(0x1p-24*z);
			iq[jz] = (int)(z - 0x1p24*fw);
			jz += 1;
			q0 += 24;
			iq[jz] = (int)fw;
		} else
			iq[jz] = (int)z;
	}

	/* convert integer "bit" chunk to floating-point value */
	fw = _stdlib_math_scalbn(1.0,q0);
	for (i=jz; i>=0; i--) {
		q[i] = fw*(double)iq[i];
		fw *= 0x1p-24;
	}

	/* compute PIo2[0,...,jp]*q[jz,...,0] */
	for(i=jz; i>=0; i--) {
		for (fw=0.0,k=0; k<=jp && k<=jz-i; k++)
			fw += PIo2[k]*q[i+k];
		fq[jz-i] = fw;
	}

	/* compress fq[] into y[] */
	switch(prec) {
	case 0:
		fw = 0.0;
		for (i=jz; i>=0; i--)
			fw += fq[i];
		y[0] = ih==0 ? fw : -fw;
		break;
	case 1:
	case 2:
		fw = 0.0;
		for (i=jz; i>=0; i--)
			fw += fq[i];
		// TODO: drop excess precision here once double_t is used
		fw = (double)fw;
		y[0] = ih==0 ? fw : -fw;
		fw = fq[0]-fw;
		for (i=1; i<=jz; i++)
			fw += fq[i];
		y[1] = ih==0 ? fw : -fw;
		break;
	case 3:  /* painful */
		for (i=jz; i>0; i--) {
			fw      = fq[i-1]+fq[i];
			fq[i]  += fq[i-1]-fw;
			fq[i-1] = fw;
		}
		for (i=jz; i>1; i--) {
			fw      = fq[i-1]+fq[i];
			fq[i]  += fq[i-1]-fw;
			fq[i-1] = fw;
		}
		for (fw=0.0,i=jz; i>=2; i--)
			fw += fq[i];
		if (ih==0) {
			y[0] =  fq[0]; y[1] =  fq[1]; y[2] =  fw;
		} else {
			y[0] = -fq[0]; y[1] = -fq[1]; y[2] = -fw;
		}
	}
	return n&7;
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
/* _stdlib_math__rem_pio2f(x,y)
 *
 * return the remainder of x rem pi/2 in *y
 * use double precision for everything except passing x
 * use _stdlib_math__rem_pio2_large() for large x
 */



#if FLT_EVAL_METHOD==0 || FLT_EVAL_METHOD==1
#define EPS DBL_EPSILON
#elif FLT_EVAL_METHOD==2
#define EPS LDBL_EPSILON
#endif

/*
 * invpio2:  53 bits of 2/pi
 * pio2_1:   first 25 bits of pi/2
 * pio2_1t:  pi/2 - pio2_1
 */
static const double
_stdlib_math___rem_pio2f_toint   = 1.5/EPS,
pio4    = 0x1.921fb6p-1,
invpio2 = 6.36619772367581382433e-01, /* 0x3FE45F30, 0x6DC9C883 */
pio2_1  = 1.57079631090164184570e+00, /* 0x3FF921FB, 0x50000000 */
pio2_1t = 1.58932547735281966916e-08; /* 0x3E5110b4, 0x611A6263 */

static int _stdlib_math__rem_pio2f(float x, double *y)
{
	union {float f; uint i;} u = {x};
	double tx[1],ty[1];
	double_t fn;
	uint ix;
	int n, sign, e0;

	ix = u.i & 0x7fffffff;
	/* 25+53 bit pi is good enough for medium size */
	if (ix < 0x4dc90fdb) {  /* |x| ~< 2^28*(pi/2), medium size */
		/* Use a specialized rint() to get fn. */
		fn = (double_t)x*invpio2 + _stdlib_math___rem_pio2f_toint - _stdlib_math___rem_pio2f_toint;
		n  = (int)fn;
		*y = x - fn*pio2_1 - fn*pio2_1t;
		/* Matters with directed rounding. */
		if (predict_false(*y < -pio4)) {
			n--;
			fn--;
			*y = x - fn*pio2_1 - fn*pio2_1t;
		} else if (predict_false(*y > pio4)) {
			n++;
			fn++;
			*y = x - fn*pio2_1 - fn*pio2_1t;
		}
		return n;
	}
	if(ix>=0x7f800000) {  /* x is inf or NaN */
		*y = x-x;
		return 0;
	}
	/* scale x into [2^23, 2^24-1] */
	sign = u.i>>31;
	e0 = (ix>>23) - (0x7f+23);  /* e0 = ilogb(|x|)-23, positive */
	u.i = ix - (e0<<23);
	tx[0] = u.f;
	n  =  _stdlib_math__rem_pio2_large(tx,ty,e0,1,0);
	if (sign) {
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
static const double
s1pio2 = 1*M_PI_2, /* 0x3FF921FB, 0x54442D18 */
s2pio2 = 2*M_PI_2, /* 0x400921FB, 0x54442D18 */
s3pio2 = 3*M_PI_2, /* 0x4012D97C, 0x7F3321D2 */
s4pio2 = 4*M_PI_2; /* 0x401921FB, 0x54442D18 */

float stdlib__sinf(float x)
{
	double y;
	uint ix;
	int n, sign;

	GET_FLOAT_WORD(ix, x);
	sign = ix >> 31;
	ix &= 0x7fffffff;

	if (ix <= 0x3f490fda) {  /* |x| ~<= pi/4 */
		if (ix < 0x39800000) {  /* |x| < 2**-12 */
			/* raise inexact if x!=0 and underflow if subnormal */
			FORCE_EVAL(ix < 0x00800000 ? x/0x1p120f : x+0x1p120f);
			return x;
		}
		return _stdlib_math__sindf(x);
	}
	if (ix <= 0x407b53d1) {  /* |x| ~<= 5*pi/4 */
		if (ix <= 0x4016cbe3) {  /* |x| ~<= 3pi/4 */
			if (sign)
				return -_stdlib_math__cosdf(x + s1pio2);
			else
				return _stdlib_math__cosdf(x - s1pio2);
		}
		return _stdlib_math__sindf(sign ? -(x + s2pio2) : -(x - s2pio2));
	}
	if (ix <= 0x40e231d5) {  /* |x| ~<= 9*pi/4 */
		if (ix <= 0x40afeddf) {  /* |x| ~<= 7*pi/4 */
			if (sign)
				return _stdlib_math__cosdf(x + s3pio2);
			else
				return -_stdlib_math__cosdf(x - s3pio2);
		}
		return _stdlib_math__sindf(sign ? x + s4pio2 : x - s4pio2);
	}

	/* sin(Inf or NaN) is NaN */
	if (ix >= 0x7f800000)
		return x - x;

	/* general argument reduction needed */
	n = _stdlib_math__rem_pio2f(x, &y);
	switch (n&3) {
	case 0: return  _stdlib_math__sindf(y);
	case 1: return  _stdlib_math__cosdf(y);
	case 2: return  _stdlib_math__sindf(-y);
	default:
		return -_stdlib_math__cosdf(y);
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
static const double
c1pio2 = 1*M_PI_2, /* 0x3FF921FB, 0x54442D18 */
c2pio2 = 2*M_PI_2, /* 0x400921FB, 0x54442D18 */
c3pio2 = 3*M_PI_2, /* 0x4012D97C, 0x7F3321D2 */
c4pio2 = 4*M_PI_2; /* 0x401921FB, 0x54442D18 */

float stdlib__cosf(float x)
{
	double y;
	uint ix;
	unsigned n, sign;

	GET_FLOAT_WORD(ix, x);
	sign = ix >> 31;
	ix &= 0x7fffffff;

	if (ix <= 0x3f490fda) {  /* |x| ~<= pi/4 */
		if (ix < 0x39800000) {  /* |x| < 2**-12 */
			/* raise inexact if x != 0 */
			FORCE_EVAL(x + 0x1p120f);
			return 1.0f;
		}
		return _stdlib_math__cosdf(x);
	}
	if (ix <= 0x407b53d1) {  /* |x| ~<= 5*pi/4 */
		if (ix > 0x4016cbe3)  /* |x|  ~> 3*pi/4 */
			return -_stdlib_math__cosdf(sign ? x+c2pio2 : x-c2pio2);
		else {
			if (sign)
				return _stdlib_math__sindf(x + c1pio2);
			else
				return _stdlib_math__sindf(c1pio2 - x);
		}
	}
	if (ix <= 0x40e231d5) {  /* |x| ~<= 9*pi/4 */
		if (ix > 0x40afeddf)  /* |x| ~> 7*pi/4 */
			return _stdlib_math__cosdf(sign ? x+c4pio2 : x-c4pio2);
		else {
			if (sign)
				return _stdlib_math__sindf(-x - c3pio2);
			else
				return _stdlib_math__sindf(x - c3pio2);
		}
	}

	/* cos(Inf or NaN) is NaN */
	if (ix >= 0x7f800000)
		return x-x;

	/* general argument reduction needed */
	n = _stdlib_math__rem_pio2f(x,&y);
	switch (n&3) {
	case 0: return  _stdlib_math__cosdf(y);
	case 1: return  _stdlib_math__sindf(-y);
	case 2: return -_stdlib_math__cosdf(y);
	default:
		return  _stdlib_math__sindf(y);
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
static const double
t1pio2 = 1*M_PI_2, /* 0x3FF921FB, 0x54442D18 */
t2pio2 = 2*M_PI_2, /* 0x400921FB, 0x54442D18 */
t3pio2 = 3*M_PI_2, /* 0x4012D97C, 0x7F3321D2 */
t4pio2 = 4*M_PI_2; /* 0x401921FB, 0x54442D18 */

float stdlib__tanf(float x)
{
	double y;
	uint ix;
	unsigned n, sign;

	GET_FLOAT_WORD(ix, x);
	sign = ix >> 31;
	ix &= 0x7fffffff;

	if (ix <= 0x3f490fda) {  /* |x| ~<= pi/4 */
		if (ix < 0x39800000) {  /* |x| < 2**-12 */
			/* raise inexact if x!=0 and underflow if subnormal */
			FORCE_EVAL(ix < 0x00800000 ? x/0x1p120f : x+0x1p120f);
			return x;
		}
		return _stdlib_math__tandf(x, 0);
	}
	if (ix <= 0x407b53d1) {  /* |x| ~<= 5*pi/4 */
		if (ix <= 0x4016cbe3)  /* |x| ~<= 3pi/4 */
			return _stdlib_math__tandf((sign ? x+t1pio2 : x-t1pio2), 1);
		else
			return _stdlib_math__tandf((sign ? x+t2pio2 : x-t2pio2), 0);
	}
	if (ix <= 0x40e231d5) {  /* |x| ~<= 9*pi/4 */
		if (ix <= 0x40afeddf)  /* |x| ~<= 7*pi/4 */
			return _stdlib_math__tandf((sign ? x+t3pio2 : x-t3pio2), 1);
		else
			return _stdlib_math__tandf((sign ? x+t4pio2 : x-t4pio2), 0);
	}

	/* tan(Inf or NaN) is NaN */
	if (ix >= 0x7f800000)
		return x - x;

	/* argument reduction */
	n = _stdlib_math__rem_pio2f(x, &y);
	return _stdlib_math__tandf(y, n&1);
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
  3.3333328366e-01,
 -1.9999158382e-01,
  1.4253635705e-01,
 -1.0648017377e-01,
  6.1687607318e-02,
};

float stdlib__atanf(float x)
{
	float_t w,s1,s2,z;
	uint ix,sign;
	int id;

	GET_FLOAT_WORD(ix, x);
	sign = ix>>31;
	ix &= 0x7fffffff;
	if (ix >= 0x4c800000) {  /* if |x| >= 2**26 */
		if (isnan(x))
			return x;
		z = atanhi[3] + 0x1p-120f;
		return sign ? -z : z;
	}
	if (ix < 0x3ee00000) {   /* |x| < 0.4375 */
		if (ix < 0x39800000) {  /* |x| < 2**-12 */
			if (ix < 0x00800000)
				/* raise underflow for subnormal x */
				FORCE_EVAL(x*x);
			return x;
		}
		id = -1;
	} else {
		x = stdlib__fabsf(x);
		if (ix < 0x3f980000) {  /* |x| < 1.1875 */
			if (ix < 0x3f300000) {  /*  7/16 <= |x| < 11/16 */
				id = 0;
				x = (2.0f*x - 1.0f)/(2.0f + x);
			} else {                /* 11/16 <= |x| < 19/16 */
				id = 1;
				x = (x - 1.0f)/(x + 1.0f);
			}
		} else {
			if (ix < 0x401c0000) {  /* |x| < 2.4375 */
				id = 2;
				x = (x - 1.5f)/(1.0f + 1.5f*x);
			} else {                /* 2.4375 <= |x| < 2**26 */
				id = 3;
				x = -1.0f/x;
			}
		}
	}
	/* end of argument reduction */
	z = x*x;
	w = z*z;
	/* break sum from i=0 to 10 aT[i]z**(i+1) into odd and even poly */
	s1 = z*(aT[0]+w*(aT[2]+w*aT[4]));
	s2 = w*(aT[1]+w*aT[3]);
	if (id < 0)
		return x - x*(s1+s2);
	z = atanhi[id] - ((x*(s1+s2) - atanlo[id]) - x);
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



static const float
pi     = 3.1415927410e+00, /* 0x40490fdb */
pi_lo  = -8.7422776573e-08; /* 0xb3bbbd2e */

float stdlib__atan2f(float y, float x)
{
	float z;
	uint m,ix,iy;

	if (isnan(x) || isnan(y))
		return x+y;
	GET_FLOAT_WORD(ix, x);
	GET_FLOAT_WORD(iy, y);
	if (ix == 0x3f800000)  /* x=1.0 */
		return stdlib__atanf(y);
	m = ((iy>>31)&1) | ((ix>>30)&2);  /* 2*sign(x)+sign(y) */
	ix &= 0x7fffffff;
	iy &= 0x7fffffff;

	/* when y = 0 */
	if (iy == 0) {
		switch (m) {
		case 0:
		case 1: return y;   /* atan(+-0,+anything)=+-0 */
		case 2: return  pi; /* atan(+0,-anything) = pi */
		case 3: return -pi; /* atan(-0,-anything) =-pi */
		}
	}
	/* when x = 0 */
	if (ix == 0)
		return m&1 ? -pi/2 : pi/2;
	/* when x is INF */
	if (ix == 0x7f800000) {
		if (iy == 0x7f800000) {
			switch (m) {
			case 0: return  pi/4; /* atan(+INF,+INF) */
			case 1: return -pi/4; /* atan(-INF,+INF) */
			case 2: return 3*pi/4;  /*atan(+INF,-INF)*/
			case 3: return -3*pi/4; /*atan(-INF,-INF)*/
			}
		} else {
			switch (m) {
			case 0: return  0.0f;    /* atan(+...,+INF) */
			case 1: return -0.0f;    /* atan(-...,+INF) */
			case 2: return  pi; /* atan(+...,-INF) */
			case 3: return -pi; /* atan(-...,-INF) */
			}
		}
	}
	/* |y/x| > 0x1p26 */
	if (ix+(26<<23) < iy || iy == 0x7f800000)
		return m&1 ? -pi/2 : pi/2;

	/* z = atan(|y/x|) with correct underflow */
	if ((m&2) && iy+(26<<23) < ix)  /*|y/x| < 0x1p-26, x < 0 */
		z = 0.0;
	else
		z = stdlib__atanf(stdlib__fabsf(y/x));
	switch (m) {
	case 0: return z;              /* atan(+,+) */
	case 1: return -z;             /* atan(-,+) */
	case 2: return pi - (z-pi_lo); /* atan(+,-) */
	default: /* case 3 */
		return (z-pi_lo) - pi; /* atan(-,-) */
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



static const float
pio2_hi = 1.5707962513e+00, /* 0x3fc90fda */
pio2_lo = 7.5497894159e-08, /* 0x33a22168 */
pS0 =  1.6666586697e-01,
pS1 = -4.2743422091e-02,
pS2 = -8.6563630030e-03,
qS1 = -7.0662963390e-01;

static float R(float z)
{
	float_t p, q;
	p = z*(pS0+z*(pS1+z*pS2));
	q = 1.0f+z*qS1;
	return p/q;
}

float stdlib__acosf(float x)
{
	float z,w,s,c,df;
	uint hx,ix;

	GET_FLOAT_WORD(hx, x);
	ix = hx & 0x7fffffff;
	/* |x| >= 1 or nan */
	if (ix >= 0x3f800000) {
		if (ix == 0x3f800000) {
			if (hx >> 31)
				return 2*pio2_hi + 0x1p-120f;
			return 0;
		}
		return 0/(x-x);
	}
	/* |x| < 0.5 */
	if (ix < 0x3f000000) {
		if (ix <= 0x32800000) /* |x| < 2**-26 */
			return pio2_hi + 0x1p-120f;
		return pio2_hi - (x - (pio2_lo-x*R(x*x)));
	}
	/* x < -0.5 */
	if (hx >> 31) {
		z = (1+x)*0.5f;
		s = stdlib__sqrtf(z);
		w = R(z)*s-pio2_lo;
		return 2*(pio2_hi - (s+w));
	}
	/* x > 0.5 */
	z = (1-x)*0.5f;
	s = stdlib__sqrtf(z);
	GET_FLOAT_WORD(hx,s);
	SET_FLOAT_WORD(df,hx&0xfffff000);
	c = (z-df*df)/(s+df);
	w = R(z)*s+c;
	return 2*(df+w);
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
#define InvLn2N _stdlib_math__exp2f_data.invln2_scaled
#define T _stdlib_math__exp2f_data.tab
#define C _stdlib_math__exp2f_data.poly_scaled

static inline uint top12(float x)
{
	return asuint(x) >> 20;
}

float stdlib__expf(float x)
{
	uint abstop;
	uint64 ki, t;
	double_t kd, xd, z, r, r2, y, s;

	xd = (double_t)x;
	abstop = top12(x) & 0x7ff;
	if (predict_false(abstop >= top12(88.0f))) {
		/* |x| >= 88 or x is nan.  */
		if (asuint(x) == asuint(-INFINITY))
			return 0.0f;
		if (abstop >= top12(INFINITY))
			return x + x;
		if (x > 0x1.62e42ep6f) /* x > log(0x1p128) ~= 88.72 */
			return _stdlib_math__math_oflowf(0);
		if (x < -0x1.9fe368p6f) /* x < log(0x1p-150) ~= -103.97 */
			return _stdlib_math__math_uflowf(0);
	}

	/* x*N/Ln2 = k + r with r in [-1/2, 1/2] and int k.  */
	z = InvLn2N * xd;

	/* Round and convert z to int, the result is in [-150*N, 128*N] and
	   ideally ties-to-even rule is used, otherwise the magnitude of r
	   can be bigger which gives larger approximation error.  */
#if _STDLIB_INTERNAL_TOINT_INTRINSICS
	kd = roundtoint(z);
	ki = converttoint(z);
#else
# define SHIFT _stdlib_math__exp2f_data.shift
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

#define T _stdlib_math__logf_data.tab
#define A _stdlib_math__logf_data.poly
#define Ln2 _stdlib_math__logf_data.ln2
#define N (1 << LOGF_TABLE_BITS)
#define OFF 0x3f330000

float stdlib__logf(float x)
{
	double_t z, r, r2, y, y0, invc, logc;
	uint ix, iz, tmp;
	int k, i;

	ix = asuint(x);
	/* Fix sign of zero with downward rounding when x==1.  */
	if (WANT_ROUNDING && predict_false(ix == 0x3f800000))
		return 0;
	if (predict_false(ix - 0x00800000 >= 0x7f800000 - 0x00800000)) {
		/* x < 0x1p-126 or inf or nan.  */
		if (ix * 2 == 0)
			return _stdlib_math__math_divzerof(1);
		if (ix == 0x7f800000) /* log(inf) == inf.  */
			return x;
		if ((ix & 0x80000000) || ix * 2 >= 0xff000000)
			return _stdlib_math__math_invalidf(x);
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
#define T _stdlib_math__powf_log2_data.tab
#define A _stdlib_math__powf_log2_data.poly
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
#define T _stdlib_math__exp2f_data.tab
#define SIGN_BIAS (1 << (EXP2F_TABLE_BITS + 11))

/* The output of log2 and thus the input of exp2 is either scaled by N
   (in case of fast _stdlib_math_powf_toint intrinsics) or not.  The unscaled xd must be
   in [-1021,1023], sign_bias sets the sign of the result.  */
static inline float exp2_inline(double_t xd, uint sign_bias)
{
	uint64 ki, ski, t;
	double_t kd, z, r, r2, y, s;

#if _STDLIB_INTERNAL_TOINT_INTRINSICS
#define C _stdlib_math__exp2f_data.poly_scaled
	/* N*x = k + r with r in [-1/2, 1/2] */
	kd = roundtoint(xd); /* k */
	ki = converttoint(xd);
#else
#define C _stdlib_math__exp2f_data.poly
#define SHIFT _stdlib_math__exp2f_data.shift_scaled
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
		return 0;
	if (e > 0x7f + 23)
		return 2;
	if (iy & ((1 << (0x7f + 23 - e)) - 1))
		return 0;
	if (iy & (1 << (0x7f + 23 - e)))
		return 1;
	return 2;
}

static inline int zeroinfnan(uint ix)
{
	return 2 * ix - 1 >= 2u * 0x7f800000 - 1;
}

float stdlib__powf(float x, float y)
{
	uint sign_bias = 0;
	uint ix, iy;

	ix = asuint(x);
	iy = asuint(y);
	if (predict_false(ix - 0x00800000 >= 0x7f800000 - 0x00800000 ||
			  zeroinfnan(iy))) {
		/* Either (x < 0x1p-126 or inf or nan) or (y is 0 or inf or nan).  */
		if (predict_false(zeroinfnan(iy))) {
			if (2 * iy == 0)
				return issignalingf_inline(x) ? x + y : 1.0f;
			if (ix == 0x3f800000)
				return issignalingf_inline(y) ? x + y : 1.0f;
			if (2 * ix > 2u * 0x7f800000 ||
			    2 * iy > 2u * 0x7f800000)
				return x + y;
			if (2 * ix == 2 * 0x3f800000)
				return 1.0f;
			if ((2 * ix < 2 * 0x3f800000) == !(iy & 0x80000000))
				return 0.0f; /* |x|<1 && y==inf or |x|>1 && y==-inf.  */
			return y * y;
		}
		if (predict_false(zeroinfnan(ix))) {
			float_t x2 = x * x;
			if (ix & 0x80000000 && checkint(iy) == 1)
				x2 = -x2;
			/* Without the barrier some versions of clang hoist the 1/x2 and
			   thus division by zero exception can be signaled spuriously.  */
			return iy & 0x80000000 ? fp_barrierf(1 / x2) : x2;
		}
		/* x and y are non-zero finite.  */
		if (ix & 0x80000000) {
			/* Finite x < 0.  */
			int yint = checkint(iy);
			if (yint == 0)
				return _stdlib_math__math_invalidf(x);
			if (yint == 1)
				sign_bias = SIGN_BIAS;
			ix &= 0x7fffffff;
		}
		if (ix < 0x00800000) {
			/* Normalize subnormal x so exponent becomes negative.  */
			ix = asuint(x * 0x1p23f);
			ix &= 0x7fffffff;
			ix -= 23 << 23;
		}
	}
	double_t logx = log2_inline(ix);
	double_t ylogx = y * logx; /* cannot overflow, y is single prec.  */
	if (predict_false((asuint64(ylogx) >> 47 & 0xffff) >=
			  asuint64(126.0 * POWF_SCALE) >> 47)) {
		/* |y*log(x)| >= 126.  */
		if (ylogx > 0x1.fffffffd1d571p+6 * POWF_SCALE)
			return _stdlib_math__math_oflowf(sign_bias);
		if (ylogx <= -150.0 * POWF_SCALE)
			return _stdlib_math__math_uflowf(sign_bias);
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
#if STDLIB_USE_HARDWARE_SQRT && defined(ARCHITECTURE_SSE2)
#include <emmintrin.h>
float stdlib__sqrtf(float x) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))); }
double stdlib__sqrt(double x) { return _mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(x))); }
#elif STDLIB_ENABLE_ASSEMBLY_OPTIMIZATIONS && STDLIB_USE_HARDWARE_SQRT && defined(ARCHITECTURE_ARM64_SQRT_ASM)
float stdlib__sqrtf(float x) { float y; __asm__("fsqrt %s0, %s1" : "=w"(y) : "w"(x)); return y; }
double stdlib__sqrt(double x) { double y; __asm__("fsqrt %d0, %d1" : "=w"(y) : "w"(x)); return y; }
#else





#define FENV_SUPPORT 1

static inline uint _stdlib_math_sqrtf_mul32(uint a, uint b)
{
	return (uint64)a*b >> 32;
}

/* see stdlib__sqrt.c for more detailed comments.  */

float stdlib__sqrtf(float x)
{
	uint ix, m, m1, m0, even, ey;

	ix = asuint(x);
	if (predict_false(ix - 0x00800000 >= 0x7f800000 - 0x00800000)) {
		/* x < 0x1p-126 or inf or nan.  */
		if (ix * 2 == 0)
			return x;
		if (ix == 0x7f800000)
			return x;
		if (ix > 0x7f800000)
			return _stdlib_math__math_invalidf(x);
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

	/* compute r ~ 1/stdlib__sqrt(m), s ~ stdlib__sqrt(m) with 2 goldschmidt iterations.  */
	static const uint three = 0xc0000000;
	uint r, s, d, u, i;
	i = (ix >> 17) % 128;
	r = (uint)_stdlib_math__rsqrt_tab[i] << 16;
	/* |r*stdlib__sqrt(m) - 1| < 0x1p-8 */
	s = _stdlib_math_sqrtf_mul32(m, r);
	/* |s/stdlib__sqrt(m) - 1| < 0x1p-8 */
	d = _stdlib_math_sqrtf_mul32(s, r);
	u = three - d;
	r = _stdlib_math_sqrtf_mul32(r, u) << 1;
	/* |r*stdlib__sqrt(m) - 1| < 0x1.7bp-16 */
	s = _stdlib_math_sqrtf_mul32(s, u) << 1;
	/* |s/stdlib__sqrt(m) - 1| < 0x1.7bp-16 */
	d = _stdlib_math_sqrtf_mul32(s, r);
	u = three - d;
	s = _stdlib_math_sqrtf_mul32(s, u);
	/* -0x1.03p-28 < s/stdlib__sqrt(m) - 1 < 0x1.fp-31 */
	s = (s - 1)>>6;
	/* s < stdlib__sqrt(m) < s + 0x1.08p-23 */

	/* compute nearest rounded result.  */
	uint d0, d1, d2;
	float y, t;
	d0 = (m << 16) - s*s;
	d1 = s - d0;
	d2 = d1 + s + 1;
	s += d1 >> 31;
	s &= 0x007fffff;
	s |= ey;
	y = asfloat(s);
	if (FENV_SUPPORT) {
		/* handle rounding and inexact exception. */
		uint tiny = predict_false(d2==0) ? 0 : 0x01000000;
		tiny |= (d1^d2) & 0x80000000;
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
	return (uint64)a*b >> 32;
}

/* returns a*b*2^-64 - e, with error 0 <= e < 3.  */
static inline uint64 mul64(uint64 a, uint64 b)
{
	uint64 ahi = a>>32;
	uint64 alo = a&0xffffffff;
	uint64 bhi = b>>32;
	uint64 blo = b&0xffffffff;
	return ahi*bhi + (ahi*blo >> 32) + (alo*bhi >> 32);
}

double stdlib__sqrt(double x)
{
	uint64 ix, top, m;

	/* special case handling.  */
	ix = asuint64(x);
	top = ix >> 52;
	if (predict_false(top - 0x001 >= 0x7ff - 0x001)) {
		/* x < 0x1p-1022 or inf or nan.  */
		if (ix * 2 == 0)
			return x;
		if (ix == 0x7ff0000000000000)
			return x;
		if (ix > 0x7ff0000000000000)
			return _stdlib_math__math_invalid(x);
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
	if (even) m >>= 1;
	top = (top + 0x3ff) >> 1;

	/* approximate r ~ 1/stdlib__sqrt(m) and s ~ stdlib__sqrt(m) when m in [1,4)

	   initial estimate:
	   7bit table lookup (1bit exponent and 6bit significand).

	   iterative approximation:
	   using 2 goldschmidt iterations with 32bit int arithmetics
	   and a final iteration with 64bit int arithmetics.

	   details:

	   the relative error (e = r0 stdlib__sqrt(m)-1) of a linear estimate
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
	     r1 stdlib__sqrt(m) - 1 = e, then
	     r2 stdlib__sqrt(m) - 1 = -3/2 e^2 - 1/2 e^3
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
	r = (uint)_stdlib_math__rsqrt_tab[i] << 16;
	/* |r stdlib__sqrt(m) - 1| < 0x1.fdp-9 */
	s = mul32(m>>32, r);
	/* |s/stdlib__sqrt(m) - 1| < 0x1.fdp-9 */
	d = mul32(s, r);
	u = three - d;
	r = mul32(r, u) << 1;
	/* |r stdlib__sqrt(m) - 1| < 0x1.7bp-16 */
	s = mul32(s, u) << 1;
	/* |s/stdlib__sqrt(m) - 1| < 0x1.7bp-16 */
	d = mul32(s, r);
	u = three - d;
	r = mul32(r, u) << 1;
	/* |r stdlib__sqrt(m) - 1| < 0x1.3704p-29 (measured worst-case) */
	r = r << 32;
	s = mul64(m, r);
	d = mul64(s, r);
	u = (three<<32) - d;
	s = mul64(s, u);  /* repr: 3.61 */
	/* -0x1p-57 < s - stdlib__sqrt(m) < 0x1.8001p-61 */
	s = (s - 2) >> 9; /* repr: 12.52 */
	/* -0x1.09p-52 < s - stdlib__sqrt(m) < -0x1.fffcp-63 */

	/* s < stdlib__sqrt(m) < s + 0x1.09p-52,
	   compute nearest rounded result:
	   the nearest result to 52 bits is either s or s+0x1p-52,
	   we can decide by comparing (2^52 s + 0.5)^2 to 2^104 m.  */
	uint64 d0, d1, d2;
	double y, t;
	d0 = (m << 42) - s*s;
	d1 = s - d0;
	d2 = d1 + s + 1;
	s += d1 >> 63;
	s &= 0x000fffffffffffff;
	s |= top << 52;
	y = asdouble(s);
	if (FENV_SUPPORT) {
		/* handle rounding modes and inexact exception:
		   only (s+1)^2 == 2^42 m case is exact otherwise
		   add a tiny value to cause the fenv effects.  */
		uint64 tiny = predict_false(d2==0) ? 0 : 0x0010000000000000;
		tiny |= (d1^d2) & 0x8000000000000000;
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
