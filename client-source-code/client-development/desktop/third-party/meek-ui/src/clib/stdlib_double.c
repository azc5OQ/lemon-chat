#include "stdlib.h"

typedef union StdlibDoubleBits
{
    double value;
    uint64 bits;
} StdlibDoubleBits;

int stdlib__isfinite(double x)
{
    StdlibDoubleBits v;
    v.value = x;
    return (v.bits & 0x7ff0000000000000ULL) != 0x7ff0000000000000ULL;
}

double stdlib__nan(void)
{
    StdlibDoubleBits v;
    v.bits = 0x7ff8000000000000ULL;
    return v.value;
}

static double stdlib_double_max(double x, double y)
{
    return x > y ? x : y;
}

double stdlib__copysign(double magnitude, double sign)
{
    StdlibDoubleBits m;
    StdlibDoubleBits s;
    m.value = magnitude;
    s.value = sign;
    m.bits = (m.bits & 0x7fffffffffffffffULL) | (s.bits & 0x8000000000000000ULL);
    return m.value;
}

double stdlib__hypot(double x, double y)
{
    x = stdlib__fabs(x);
    y = stdlib__fabs(y);
    StdlibDoubleBits a;
    StdlibDoubleBits b;
    a.value = x;
    b.value = y;
    if (a.bits == 0x7ff0000000000000ULL || b.bits == 0x7ff0000000000000ULL)
    {
        a.bits = 0x7ff0000000000000ULL;
        return a.value;
    }
    if (!stdlib__isfinite(x) || !stdlib__isfinite(y))
    {
        return stdlib__nan();
    }
    double scale = stdlib_double_max(x, y);
    if (scale == 0.0)
    {
        return 0.0;
    }
    x /= scale;
    y /= scale;
    return scale * stdlib__sqrt(x * x + y * y);
}

double stdlib__exp(double x)
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
        StdlibDoubleBits infinity;
        infinity.bits = 0x7ff0000000000000ULL;
        return infinity.value;
    }
    int k = (int)stdlib__floor(x * 1.4426950408889634074 + 0.5);
    /* Split ln(2) reduces cancellation in the argument reduction. */
    double r = (x - k * 0.69314718036912381649) - k * 1.9082149292705877000e-10;
    double sum = 1.0;
    double term = 1.0;
    for (int i = 1; i <= 18; ++i)
    {
        term *= r / i;
        sum += term;
    }
    StdlibDoubleBits scale;
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

double stdlib__log(double x)
{
    if (x < 0.0 || x != x)
    {
        return stdlib__nan();
    }
    if (x == 0.0)
    {
        StdlibDoubleBits infinity;
        infinity.bits = 0xfff0000000000000ULL;
        return infinity.value;
    }
    if (!stdlib__isfinite(x))
    {
        return x;
    }
    int correction = 0;
    if (x < 2.2250738585072013831e-308)
    {
        x *= 18014398509481984.0;
        correction = -54;
    }
    StdlibDoubleBits v;
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

double stdlib__pow_positive(double x, double exponent)
{
    if (x <= 0.0 || !stdlib__isfinite(x) || !stdlib__isfinite(exponent))
    {
        return stdlib__nan();
    }
    return stdlib__exp(exponent * stdlib__log(x));
}

double stdlib__cos_bounded(double x)
{
    const double pi = 3.14159265358979323846;
    if (!stdlib__isfinite(x) || stdlib__fabs(x) > 2.0 * pi)
    {
        return stdlib__nan();
    }
    x -= stdlib__floor((x + pi) / (2.0 * pi)) * (2.0 * pi);
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
