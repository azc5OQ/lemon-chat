// rsa_keygen.c is the c source of rsa_keygen.wasm, the webassembly that does the heavy rsa math for the lemon chat client
// build.bat compiles it with clang (freestanding, no libc) and the client build embeds the wasm into rsa-crypto.js, which
// runs in the data processing worker. rsa-crypto.js is the only caller: it seeds the generator with the sha256 hex of the
// identity passphrase, calls rsa_keygen__generate to derive the identity keypair, and calls rsa_keygen__modpow for the
// big modular exponentiation behind every rsa encrypt and decrypt
//
// a lemon chat identity IS this calculation: the same passphrase must produce the same keypair on every machine, forever.
// the old client derived keys in javascript (cryptico's seeded Math.random + jsbn's RSAGenerate), so this file clones
// that old path step by step - same random generator, same prime search, same COUNT of random draws - because even one
// extra random() call would steer the prime search elsewhere and every existing identity would derive different keys
//
// the cloned pipeline, in order:
//   1. Math.seedrandom(seed)        - the seed keys an arc4 stream cipher that replaces Math.random
//   2. Math.random()                - each call turns arc4 bytes into one random double between 0 and 1
//   3. SeededRandom.nextBytes       - one random byte = floor(Math.random() * 256)
//   4. new BigInteger(bits, 1, rng) - draw a random number, force the top bit, make it odd, then
//                                     step +2 until a candidate passes the primality test
//   5. RSAGenerate(bits, "03")      - repeat that for two primes p and q (each with p-1 coprime to
//                                     e=3), then compute the key parts n, d, dmp1, dmq1, coeff
// the primality test picks its random witnesses with Math.random() too, so every random() call in
// this file happens at exactly the same point of the walk as it did in the js

typedef unsigned char uint8;
typedef unsigned int uint32;
typedef int int32;
typedef unsigned long long uint64;
typedef long long int64;
typedef int boole;

#define TRUE 1
#define FALSE 0

// enough limbs for an 8192-bit modulus and its double-width products
#define BN_LIMBS 520

// ---------------------------------------------------------------------------
// the random number generator: a clone of the seedrandom js library. the seed keys
// an arc4 stream cipher, and every bit of randomness below is drawn from that stream
// ---------------------------------------------------------------------------

static uint8 g_arc4_s[256];
static uint32 g_arc4_i = 0;
static uint32 g_arc4_j = 0;

// the next byte of the arc4 stream (swap two table entries, serve a third);
// byte for byte the same as seedrandom's internal g() step
static uint32 _rsa_internal__arc4_byte(void)
{
    uint8 t, u;

    g_arc4_i = (g_arc4_i + 1) & 255;
    t = g_arc4_s[g_arc4_i];
    g_arc4_j = (g_arc4_j + t) & 255;
    u = g_arc4_s[g_arc4_j];
    g_arc4_s[g_arc4_i] = u;
    g_arc4_s[g_arc4_j] = t;

    return g_arc4_s[(t + u) & 255];
}

// keys arc4 from the seed string the way Math.seedrandom does: the seed characters
// become the key, the normal arc4 key schedule shuffles the table with it, and the
// first 256 output bytes are thrown away
static void _rsa_internal__seedrandom(const uint8* seed, int32 seed_length)
{
    int32 key[256];
    boole key_defined[256];
    int32 key_length;
    int32 smear = 0;
    int32 i, j, slot;
    uint8 t;

    for (i = 0; i < 256; i++)
    {
        key_defined[i] = FALSE;
        key[i] = 0;
    }

    for (j = 0; j < seed_length; j++)
    {
        slot = j & 255;

        if (key_defined[slot] == TRUE)
        {
            smear = smear ^ (key[slot] * 19);
        }
        // an undefined slot contributes NaN*19 in js, and ToInt32(NaN) is 0, so smear is untouched

        key[slot] = (smear + seed[j]) & 255;
        key_defined[slot] = TRUE;
    }

    key_length = (seed_length < 256) ? seed_length : 256;
    if (key_length == 0)
    {
        key[0] = 0;
        key_length = 1;
    }

    for (i = 0; i < 256; i++)
    {
        g_arc4_s[i] = (uint8)i;
    }

    j = 0;
    for (i = 0; i < 256; i++)
    {
        j = (j + g_arc4_s[i] + key[i % key_length]) & 255;
        t = g_arc4_s[i];
        g_arc4_s[i] = g_arc4_s[j];
        g_arc4_s[j] = t;
    }

    g_arc4_i = 0;
    g_arc4_j = 0;

    // seedrandom's constructor throws away 256 bytes before first use; only the state stepping matters
    for (i = 0; i < 256; i++)
    {
        _rsa_internal__arc4_byte();
    }
}

// the seeded Math.random(): one random double between 0 and 1 built from arc4 bytes,
// with the exact floating point steps of the js closure, so both sides match bit for bit
static double _rsa_internal__random(void)
{
    double n;
    double d = 281474976710656.0;            // 256^6
    uint32 x = 0;
    int32 i;

    n = (double)_rsa_internal__arc4_byte();
    for (i = 0; i < 5; i++)
    {
        n = n * 256.0 + (double)_rsa_internal__arc4_byte();
    }

    while (n < 4503599627370496.0)           // 2^52
    {
        n = (n + x) * 256.0;
        d = d * 256.0;
        x = _rsa_internal__arc4_byte();
    }

    while (n >= 9007199254740992.0)          // 2^53
    {
        n = n / 2.0;
        d = d / 2.0;
        x = x >> 1;
    }

    return (n + x) / d;
}

// one random byte, exactly how cryptico's SeededRandom.nextBytes made one: floor(Math.random() * 256)
static uint32 _rsa_internal__random_byte(void)
{
    return (uint32)(_rsa_internal__random() * 256.0);
}

// ---------------------------------------------------------------------------
// big numbers: rsa numbers are thousands of bits, so they live in arrays of 32-bit
// words ("limbs"), least significant limb first, in fixed-size buffers (no malloc)
// ---------------------------------------------------------------------------

typedef struct
{
    int32 used;                 // limbs in use; 0 means the value 0
    uint32 limb[BN_LIMBS];
} bignum;

static void _rsa_internal__bn_zero(bignum* a)
{
    int32 i;
    for (i = 0; i < BN_LIMBS; i++) { a->limb[i] = 0; }
    a->used = 0;
}

static void _rsa_internal__bn_trim(bignum* a)
{
    while (a->used > 0 && a->limb[a->used - 1] == 0) { a->used--; }
}

static void _rsa_internal__bn_copy(bignum* out, const bignum* a)
{
    int32 i;
    for (i = 0; i < a->used; i++) { out->limb[i] = a->limb[i]; }
    for (i = a->used; i < BN_LIMBS; i++) { out->limb[i] = 0; }
    out->used = a->used;
}

static void _rsa_internal__bn_set_word(bignum* a, uint32 w)
{
    _rsa_internal__bn_zero(a);
    if (w > 0)
    {
        a->limb[0] = w;
        a->used = 1;
    }
}

static int32 _rsa_internal__bn_compare(const bignum* a, const bignum* b)
{
    int32 i;

    if (a->used != b->used) { return (a->used > b->used) ? 1 : -1; }
    for (i = a->used - 1; i >= 0; i--)
    {
        if (a->limb[i] != b->limb[i]) { return (a->limb[i] > b->limb[i]) ? 1 : -1; }
    }
    return 0;
}

static boole _rsa_internal__bn_is_word(const bignum* a, uint32 w)
{
    if (w == 0) { return (a->used == 0) ? TRUE : FALSE; }
    return (a->used == 1 && a->limb[0] == w) ? TRUE : FALSE;
}

static int32 _rsa_internal__bn_bit_length(const bignum* a)
{
    uint32 top;
    int32 bits;

    if (a->used == 0) { return 0; }
    top = a->limb[a->used - 1];
    bits = (a->used - 1) * 32;
    while (top > 0)
    {
        bits++;
        top >>= 1;
    }
    return bits;
}

static boole _rsa_internal__bn_test_bit(const bignum* a, int32 bit)
{
    int32 limb_index = bit >> 5;
    if (limb_index >= a->used) { return FALSE; }
    return ((a->limb[limb_index] >> (bit & 31)) & 1) ? TRUE : FALSE;
}

static void _rsa_internal__bn_set_bit(bignum* a, int32 bit)
{
    int32 limb_index = bit >> 5;
    a->limb[limb_index] |= (uint32)1 << (bit & 31);
    if (a->used < limb_index + 1) { a->used = limb_index + 1; }
}

// a += w, one machine word wide; the carry ripples up as far as it needs to
static void _rsa_internal__bn_add_word(bignum* a, uint32 w)
{
    uint64 carry = w;
    int32 i = 0;

    while (carry != 0)
    {
        carry += a->limb[i];
        a->limb[i] = (uint32)carry;
        carry >>= 32;
        i++;
    }
    if (a->used < i) { a->used = i; }
    _rsa_internal__bn_trim(a);
}

// a -= b, requires a >= b (there are no negative numbers in this file)
static void _rsa_internal__bn_sub(bignum* a, const bignum* b)
{
    int64 borrow = 0;
    int32 i;

    for (i = 0; i < a->used; i++)
    {
        int64 v = (int64)a->limb[i] - (i < b->used ? (int64)b->limb[i] : 0) + borrow;
        if (v < 0)
        {
            v += ((int64)1 << 32);
            borrow = -1;
        }
        else
        {
            borrow = 0;
        }
        a->limb[i] = (uint32)v;
    }
    _rsa_internal__bn_trim(a);
}

// a += b
static void _rsa_internal__bn_add(bignum* a, const bignum* b)
{
    uint64 carry = 0;
    int32 i;
    int32 top = (a->used > b->used) ? a->used : b->used;

    for (i = 0; i < top; i++)
    {
        carry += (uint64)(i < a->used ? a->limb[i] : 0) + (uint64)(i < b->used ? b->limb[i] : 0);
        a->limb[i] = (uint32)carry;
        carry >>= 32;
    }
    if (carry != 0)
    {
        a->limb[top] = (uint32)carry;
        top++;
    }
    a->used = top;
}

// out = a * b, schoolbook long multiplication; out must be a different bignum than a and b
static void _rsa_internal__bn_mul(bignum* out, const bignum* a, const bignum* b)
{
    int32 i, j;

    _rsa_internal__bn_zero(out);
    for (i = 0; i < a->used; i++)
    {
        uint64 carry = 0;
        uint64 av = a->limb[i];

        for (j = 0; j < b->used; j++)
        {
            carry += (uint64)out->limb[i + j] + av * b->limb[j];
            out->limb[i + j] = (uint32)carry;
            carry >>= 32;
        }
        while (carry != 0)
        {
            carry += out->limb[i + j];
            out->limb[i + j] = (uint32)carry;
            carry >>= 32;
            j++;
        }
    }
    out->used = a->used + b->used;
    _rsa_internal__bn_trim(out);
}

// remainder of a divided by the small word m (m < 2^31); a itself is untouched
static uint32 _rsa_internal__bn_mod_word(const bignum* a, uint32 m)
{
    uint64 r = 0;
    int32 i;

    for (i = a->used - 1; i >= 0; i--)
    {
        r = ((r << 32) | a->limb[i]) % m;
    }
    return (uint32)r;
}

// a /= m for a small word m; its one job is the exact division by 3 that produces d
static void _rsa_internal__bn_div_word(bignum* a, uint32 m)
{
    uint64 r = 0;
    int32 i;

    for (i = a->used - 1; i >= 0; i--)
    {
        r = (r << 32) | a->limb[i];
        a->limb[i] = (uint32)(r / m);
        r = r % m;
    }
    _rsa_internal__bn_trim(a);
}

// a <<= 1 (doubles a)
static void _rsa_internal__bn_shl1(bignum* a)
{
    uint32 carry = 0;
    int32 i;

    for (i = 0; i < a->used; i++)
    {
        uint32 next = a->limb[i] >> 31;
        a->limb[i] = (a->limb[i] << 1) | carry;
        carry = next;
    }
    if (carry != 0)
    {
        a->limb[a->used] = carry;
        a->used++;
    }
}

// a >>= 1 (halves a, dropping the low bit)
static void _rsa_internal__bn_shr1(bignum* a)
{
    int32 i;

    for (i = 0; i < a->used; i++)
    {
        uint32 next = (i + 1 < a->used) ? (a->limb[i + 1] & 1) : 0;
        a->limb[i] = (a->limb[i] >> 1) | (next << 31);
    }
    _rsa_internal__bn_trim(a);
}

// out = a mod m the schoolbook way: line m up under a, subtract wherever it fits,
// shift m back down, repeat. slow, but only a handful of calls per key need it
static void _rsa_internal__bn_mod(bignum* out, const bignum* a, const bignum* m)
{
    static bignum shifted;
    int32 diff;

    _rsa_internal__bn_copy(out, a);
    if (_rsa_internal__bn_compare(out, m) < 0) { return; }

    diff = _rsa_internal__bn_bit_length(out) - _rsa_internal__bn_bit_length(m);
    _rsa_internal__bn_copy(&shifted, m);
    while (diff > 0)
    {
        _rsa_internal__bn_shl1(&shifted);
        diff--;
    }

    for (;;)
    {
        if (_rsa_internal__bn_compare(out, &shifted) >= 0)
        {
            _rsa_internal__bn_sub(out, &shifted);
        }
        if (_rsa_internal__bn_compare(&shifted, m) == 0) { break; }
        _rsa_internal__bn_shr1(&shifted);
    }
}

// ---------------------------------------------------------------------------
// montgomery arithmetic, where nearly all key generation time is spent. numbers are
// moved into a special form (the "montgomery domain") in which reducing mod the
// modulus needs no division at all, only multiplies, adds and shifts
// ---------------------------------------------------------------------------

static bignum g_mont_modulus;
static int32 g_mont_len;
static uint32 g_mont_n0;         // -modulus^-1 mod 2^32
static bignum g_mont_r2;         // R^2 mod modulus, R = 2^(32*len)

// -modulus^-1 mod 2^32, the one helper constant montgomery reduction needs
static uint32 _rsa_internal__mont_n0_of(uint32 odd)
{
    uint32 x = odd;              // 3-bit start, doubles correct bits each round
    x *= 2 - odd * x;
    x *= 2 - odd * x;
    x *= 2 - odd * x;
    x *= 2 - odd * x;
    return (uint32)(0 - x);
}

// montgomery multiplication: out = a * b * R^-1 mod modulus, with multiply and
// reduction interleaved limb by limb. out must be a different bignum than a and b
static void _rsa_internal__mont_mul(bignum* out, const bignum* a, const bignum* b)
{
    static uint32 t[BN_LIMBS + 2];
    int32 i, j;
    int32 len = g_mont_len;

    for (i = 0; i <= len + 1; i++) { t[i] = 0; }

    for (i = 0; i < len; i++)
    {
        uint64 carry = 0;
        uint64 av = (i < a->used) ? a->limb[i] : 0;
        uint32 m;

        for (j = 0; j < len; j++)
        {
            carry += (uint64)t[j] + av * (j < b->used ? b->limb[j] : 0);
            t[j] = (uint32)carry;
            carry >>= 32;
        }
        carry += t[len];
        t[len] = (uint32)carry;
        t[len + 1] = (uint32)(carry >> 32);

        m = t[0] * g_mont_n0;

        carry = (uint64)t[0] + (uint64)m * g_mont_modulus.limb[0];
        carry >>= 32;
        for (j = 1; j < len; j++)
        {
            carry += (uint64)t[j] + (uint64)m * g_mont_modulus.limb[j];
            t[j - 1] = (uint32)carry;
            carry >>= 32;
        }
        carry += t[len];
        t[len - 1] = (uint32)carry;
        t[len] = t[len + 1] + (uint32)(carry >> 32);
    }

    for (i = 0; i < len; i++) { out->limb[i] = t[i]; }
    for (i = len; i < BN_LIMBS; i++) { out->limb[i] = 0; }

    // keep all len limbs (no trim) until after the subtract: when the carry limb
    // t[len] is set, bn_sub dropping its final borrow is exactly what cancels it
    out->used = len;
    if (t[len] != 0 || _rsa_internal__bn_compare(out, &g_mont_modulus) >= 0)
    {
        _rsa_internal__bn_sub(out, &g_mont_modulus);
    }
    _rsa_internal__bn_trim(out);
}

// montgomery squaring: the same value as mont_mul(out, a, a) in about half the raw
// multiplies (each cross product computed once, then doubled). only mont_setup calls
// it - the hot loop squares via mont_mul instead (see mont_pow). out must not alias a
static void _rsa_internal__mont_sqr(bignum* out, const bignum* a)
{
    static uint32 t[2 * BN_LIMBS + 2];
    int32 i, j;
    int32 len = g_mont_len;
    uint64 carry;
    uint32 topbit;

    for (i = 0; i < 2 * len + 2; i++) { t[i] = 0; }

    // every cross product a[i] * a[j] with i < j, each counted once for now
    for (i = 0; i < len; i++)
    {
        uint64 av = (i < a->used) ? a->limb[i] : 0;
        if (av == 0) { continue; }

        carry = 0;
        for (j = i + 1; j < len; j++)
        {
            carry += (uint64)t[i + j] + av * ((j < a->used) ? a->limb[j] : 0);
            t[i + j] = (uint32)carry;
            carry >>= 32;
        }
        j = i + len;
        while (carry != 0)
        {
            carry += t[j];
            t[j] = (uint32)carry;
            carry >>= 32;
            j++;
        }
    }

    // double the cross sum, then add the diagonal squares
    topbit = 0;
    for (i = 0; i <= 2 * len; i++)
    {
        uint32 next = t[i] >> 31;
        t[i] = (t[i] << 1) | topbit;
        topbit = next;
    }

    for (i = 0; i < len; i++)
    {
        uint64 av = (i < a->used) ? a->limb[i] : 0;
        uint64 sq = av * av;

        carry = (uint64)t[2 * i] + (sq & 0xffffffffu);
        t[2 * i] = (uint32)carry;
        carry = (carry >> 32) + (uint64)t[2 * i + 1] + (sq >> 32);
        t[2 * i + 1] = (uint32)carry;
        carry >>= 32;
        j = 2 * i + 2;
        while (carry != 0)
        {
            carry += t[j];
            t[j] = (uint32)carry;
            carry >>= 32;
            j++;
        }
    }

    // the reduction pass; the reduced result collects in the top half, t[len .. 2*len]
    for (i = 0; i < len; i++)
    {
        uint32 m = t[i] * g_mont_n0;

        carry = 0;
        for (j = 0; j < len; j++)
        {
            carry += (uint64)t[i + j] + (uint64)m * g_mont_modulus.limb[j];
            t[i + j] = (uint32)carry;
            carry >>= 32;
        }
        j = i + len;
        while (carry != 0)
        {
            carry += t[j];
            t[j] = (uint32)carry;
            carry >>= 32;
            j++;
        }
    }

    for (i = 0; i < len; i++) { out->limb[i] = t[len + i]; }
    for (i = len; i < BN_LIMBS; i++) { out->limb[i] = 0; }

    // same rule as in mont_mul: subtract at full width, t[2*len] is the carry bn_sub cancels
    out->used = len;
    if (t[2 * len] != 0 || _rsa_internal__bn_compare(out, &g_mont_modulus) >= 0)
    {
        _rsa_internal__bn_sub(out, &g_mont_modulus);
    }
    _rsa_internal__bn_trim(out);
}

// prepares the montgomery constants for one odd modulus (its length, the n0 word and
// R^2 mod m). must run before mont_mul, mont_sqr or mont_pow can be used
static void _rsa_internal__mont_setup(const bignum* m)
{
    static bignum r, t2;
    int32 bit;
    int32 target;

    _rsa_internal__bn_copy(&g_mont_modulus, m);
    g_mont_len = m->used;
    g_mont_n0 = _rsa_internal__mont_n0_of(m->limb[0]);

    // r = R mod m, R = 2^(32*len)
    _rsa_internal__bn_zero(&r);
    _rsa_internal__bn_set_bit(&r, g_mont_len * 32 - 1);
    _rsa_internal__bn_mod(&r, &r, m);
    _rsa_internal__bn_shl1(&r);
    if (_rsa_internal__bn_compare(&r, m) >= 0) { _rsa_internal__bn_sub(&r, m); }
    _rsa_internal__bn_copy(&g_mont_r2, &r);

    // builds R^2 = 2^(32*len) * R mod m: squaring 2^k * R via mont_sqr gives 2^(2k) * R,
    // one modular doubling gives 2^(k+1) * R, so the bits of 32*len grow k up to 32*len
    target = g_mont_len * 32;
    _rsa_internal__bn_copy(&r, &g_mont_r2);
    for (bit = 31; bit >= 0; bit--)
    {
        if ((target >> bit) == 0) { continue; }
        _rsa_internal__mont_sqr(&t2, &r);
        _rsa_internal__bn_copy(&r, &t2);
        if (((target >> bit) & 1) != 0)
        {
            _rsa_internal__bn_shl1(&r);
            if (_rsa_internal__bn_compare(&r, m) >= 0) { _rsa_internal__bn_sub(&r, m); }
        }
    }
    _rsa_internal__bn_copy(&g_mont_r2, &r);
}

// out = base^exp mod the modulus given to mont_setup. the exponent is eaten 4 bits at a
// time against a table of the first 15 powers of base; same value as square-and-multiply
static void _rsa_internal__mont_pow(bignum* out, const bignum* base, const bignum* exp)
{
    static bignum table[16], acc, tmp, one;
    bignum* cur = &acc;
    bignum* nxt = &tmp;
    bignum* swap;
    int32 chunk_count = (_rsa_internal__bn_bit_length(exp) + 3) >> 2;
    int32 c, i;
    uint32 digit;

    _rsa_internal__mont_mul(&table[1], base, &g_mont_r2);  // base into the domain
    for (i = 2; i < 16; i++)
    {
        // mont_mul beats mont_sqr here: at these limb counts the separated squaring
        // pass costs more than it saves, so squares go through mont_mul too
        if ((i & 1) == 0) { _rsa_internal__mont_mul(&table[i], &table[i >> 1], &table[i >> 1]); }
        else { _rsa_internal__mont_mul(&table[i], &table[i - 1], &table[1]); }
    }

    _rsa_internal__bn_set_word(&one, 1);
    _rsa_internal__mont_mul(cur, &one, &g_mont_r2);        // 1 into the domain = R mod m

    for (c = chunk_count - 1; c >= 0; c--)
    {
        if (c != chunk_count - 1)
        {
            for (i = 0; i < 4; i++)
            {
                _rsa_internal__mont_mul(nxt, cur, cur);
                swap = cur; cur = nxt; nxt = swap;
            }
        }

        digit = 0;
        for (i = 3; i >= 0; i--)
        {
            digit = (digit << 1) | (_rsa_internal__bn_test_bit(exp, c * 4 + i) == TRUE ? 1u : 0u);
        }
        if (digit != 0)
        {
            _rsa_internal__mont_mul(nxt, cur, &table[digit]);
            swap = cur; cur = nxt; nxt = swap;
        }
    }

    _rsa_internal__mont_mul(out, cur, &one);               // back out of the domain
}

// ---------------------------------------------------------------------------
// the primality test, verdict-identical to jsbn's isProbablePrime: cheap trial division
// by the small primes first, then miller-rabin rounds. each round draws its witness from
// the seeded Math.random stream, which keeps the wasm walk in sync with the js walk
// ---------------------------------------------------------------------------

// the constant one, because subtracting 1 happens all over the walk
static const bignum g_bn_one = { .used = 1, .limb = { 1 } };

// the 168 primes below 1000, the same table jsbn uses for trial division and witnesses
static const uint32 g_lowprimes[] = {
    2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67, 71, 73, 79, 83, 89,
    97, 101, 103, 107, 109, 113, 127, 131, 137, 139, 149, 151, 157, 163, 167, 173, 179, 181, 191,
    193, 197, 199, 211, 223, 227, 229, 233, 239, 241, 251, 257, 263, 269, 271, 277, 281, 283, 293,
    307, 311, 313, 317, 331, 337, 347, 349, 353, 359, 367, 373, 379, 383, 389, 397, 401, 409, 419,
    421, 431, 433, 439, 443, 449, 457, 461, 463, 467, 479, 487, 491, 499, 503, 509, 521, 523, 541,
    547, 557, 563, 569, 571, 577, 587, 593, 599, 601, 607, 613, 617, 619, 631, 641, 643, 647, 653,
    659, 661, 673, 677, 683, 691, 701, 709, 719, 727, 733, 739, 743, 751, 757, 761, 769, 773, 787,
    797, 809, 811, 821, 823, 827, 829, 839, 853, 857, 859, 863, 877, 881, 883, 887, 907, 911, 919,
    929, 937, 941, 947, 953, 967, 971, 977, 983, 991, 997
};
#define LOWPRIMES_COUNT 168

// miller-rabin: writes x-1 as 2^k * r with r odd, then for t rounds raises a random
// witness to the power r and squares upward, checking the pattern a prime must show
static boole _rsa_internal__miller_rabin(const bignum* x, int32 t)
{
    static bignum n1, r, y, base, tmp;
    int32 k, i, j;
    uint32 witness_index;

    _rsa_internal__bn_copy(&n1, x);
    _rsa_internal__bn_sub(&n1, &g_bn_one);

    // lowest set bit of x-1
    k = 0;
    _rsa_internal__bn_copy(&r, &n1);
    while (r.used > 0 && (r.limb[0] & 1) == 0)
    {
        _rsa_internal__bn_shr1(&r);
        k++;
    }
    if (k <= 0) { return FALSE; }

    t = (t + 1) >> 1;
    if (t > LOWPRIMES_COUNT) { t = LOWPRIMES_COUNT; }

    _rsa_internal__mont_setup(x);

    for (i = 0; i < t; i++)
    {
        // the witness pick consumes one Math.random(), exactly like jsbn does
        witness_index = (uint32)(_rsa_internal__random() * (double)LOWPRIMES_COUNT);
        _rsa_internal__bn_set_word(&base, g_lowprimes[witness_index]);

        _rsa_internal__mont_pow(&y, &base, &r);

        if (_rsa_internal__bn_is_word(&y, 1) == FALSE && _rsa_internal__bn_compare(&y, &n1) != 0)
        {
            j = 1;
            while (j++ < k && _rsa_internal__bn_compare(&y, &n1) != 0)
            {
                // y = y^2 mod x: lift one factor into the domain, then one mont multiply
                _rsa_internal__mont_mul(&tmp, &y, &g_mont_r2);
                _rsa_internal__mont_mul(&y, &tmp, &y);
                if (_rsa_internal__bn_is_word(&y, 1) == TRUE) { return FALSE; }
            }
            if (_rsa_internal__bn_compare(&y, &n1) != 0) { return FALSE; }
        }
    }

    return TRUE;
}

// jsbn's isProbablePrime: trial-divide by every small prime, then hand over to miller-rabin
static boole _rsa_internal__is_probable_prime(const bignum* x, int32 t)
{
    int32 i;

    if (x->used > 0 && (x->limb[0] & 1) == 0) { return FALSE; }

    // trial division, same verdicts as jsbn's batched modInt version
    for (i = 1; i < LOWPRIMES_COUNT; i++)
    {
        if (_rsa_internal__bn_mod_word(x, g_lowprimes[i]) == 0) { return FALSE; }
    }

    return _rsa_internal__miller_rabin(x, t);
}

// ---------------------------------------------------------------------------
// the prime walk: how a random starting number becomes a prime, cloned from jsbn's
// new BigInteger(bits, 1, rng) constructor and cryptico's RSAGenerate
// ---------------------------------------------------------------------------

// draws the random candidate exactly like new BigInteger(bits, rng): (bits>>3)+1 bytes
// consumed, top byte masked (or zeroed when bits is a multiple of 8), big-endian
static void _rsa_internal__draw_candidate(bignum* out, int32 bits)
{
    static uint8 bytes[1032];
    int32 count = (bits >> 3) + 1;
    int32 top_bits = bits & 7;
    int32 i;

    for (i = 0; i < count; i++)
    {
        bytes[i] = (uint8)_rsa_internal__random_byte();
    }

    if (top_bits > 0) { bytes[0] &= (uint8)((1 << top_bits) - 1); }
    else { bytes[0] = 0; }

    _rsa_internal__bn_zero(out);
    for (i = 0; i < count; i++)
    {
        int32 pos = count - 1 - i;             // byte position from the least significant end
        out->limb[pos >> 2] |= (uint32)bytes[i] << ((pos & 3) * 8);
    }
    out->used = (count + 3) >> 2;
    _rsa_internal__bn_trim(out);
}

// walk state: the candidate's remainder against each odd small prime, stepped by 2 along
// the walk so no candidate ever needs the full trial division again; verdicts identical
static uint32 g_walk_residues[LOWPRIMES_COUNT];
static uint32 g_walk_half_residues[LOWPRIMES_COUNT];

// finds a prime like new BigInteger(bits, 1, rng) does: random start, top bit forced so
// the prime has full size, made odd, then +2 steps until a candidate survives both the
// stepped remainders and miller-rabin; same verdicts as running the full test each step
static void _rsa_internal__random_prime(bignum* out, int32 bits, int32 certainty)
{
    static bignum half;
    int32 i;
    boole composite;

    _rsa_internal__draw_candidate(out, bits);

    if (_rsa_internal__bn_test_bit(out, bits - 1) == FALSE)
    {
        _rsa_internal__bn_set_bit(out, bits - 1);
    }
    if (out->used == 0 || (out->limb[0] & 1) == 0)
    {
        _rsa_internal__bn_add_word(out, 1);
    }

    _rsa_internal__bn_zero(&half);
    _rsa_internal__bn_set_bit(&half, bits - 1);

    for (i = 1; i < LOWPRIMES_COUNT; i++)
    {
        g_walk_residues[i] = _rsa_internal__bn_mod_word(out, g_lowprimes[i]);
        g_walk_half_residues[i] = _rsa_internal__bn_mod_word(&half, g_lowprimes[i]);
    }

    for (;;)
    {
        composite = FALSE;
        for (i = 1; i < LOWPRIMES_COUNT; i++)
        {
            if (g_walk_residues[i] == 0) { composite = TRUE; break; }
        }

        if (composite == FALSE && _rsa_internal__miller_rabin(out, certainty) == TRUE) { break; }

        _rsa_internal__bn_add_word(out, 2);
        for (i = 1; i < LOWPRIMES_COUNT; i++)
        {
            g_walk_residues[i] += 2;
            if (g_walk_residues[i] >= g_lowprimes[i]) { g_walk_residues[i] -= g_lowprimes[i]; }
        }

        if (_rsa_internal__bn_bit_length(out) > bits)
        {
            _rsa_internal__bn_sub(out, &half);
            for (i = 1; i < LOWPRIMES_COUNT; i++)
            {
                uint32 r = g_walk_residues[i] + g_lowprimes[i] - g_walk_half_residues[i];
                if (r >= g_lowprimes[i]) { r -= g_lowprimes[i]; }
                g_walk_residues[i] = r;
            }
        }
    }
}

// modular inverse for an odd modulus (the binary extended euclid algorithm); its one
// job here is coeff = q^-1 mod p, the last of the eight key parts
static void _rsa_internal__bn_mod_inverse_odd(bignum* out, const bignum* a, const bignum* m)
{
    static bignum u, v, x1, x2;

    _rsa_internal__bn_mod(&u, a, m);
    _rsa_internal__bn_copy(&v, m);
    _rsa_internal__bn_set_word(&x1, 1);
    _rsa_internal__bn_zero(&x2);

    while (_rsa_internal__bn_is_word(&u, 1) == FALSE && _rsa_internal__bn_is_word(&v, 1) == FALSE)
    {
        while (u.used > 0 && (u.limb[0] & 1) == 0)
        {
            _rsa_internal__bn_shr1(&u);
            if ((x1.limb[0] & 1) == 0) { _rsa_internal__bn_shr1(&x1); }
            else
            {
                _rsa_internal__bn_add(&x1, m);
                _rsa_internal__bn_shr1(&x1);
            }
        }
        while (v.used > 0 && (v.limb[0] & 1) == 0)
        {
            _rsa_internal__bn_shr1(&v);
            if ((x2.limb[0] & 1) == 0) { _rsa_internal__bn_shr1(&x2); }
            else
            {
                _rsa_internal__bn_add(&x2, m);
                _rsa_internal__bn_shr1(&x2);
            }
        }
        if (_rsa_internal__bn_compare(&u, &v) >= 0)
        {
            _rsa_internal__bn_sub(&u, &v);
            if (_rsa_internal__bn_compare(&x1, &x2) >= 0) { _rsa_internal__bn_sub(&x1, &x2); }
            else
            {
                _rsa_internal__bn_add(&x1, m);
                _rsa_internal__bn_sub(&x1, &x2);
            }
        }
        else
        {
            _rsa_internal__bn_sub(&v, &u);
            if (_rsa_internal__bn_compare(&x2, &x1) >= 0) { _rsa_internal__bn_sub(&x2, &x1); }
            else
            {
                _rsa_internal__bn_add(&x2, m);
                _rsa_internal__bn_sub(&x2, &x1);
            }
        }
    }

    if (_rsa_internal__bn_is_word(&u, 1) == TRUE) { _rsa_internal__bn_mod(out, &x1, m); }
    else { _rsa_internal__bn_mod(out, &x2, m); }
}

// ---------------------------------------------------------------------------
// the exported key generation interface. rsa-crypto.js copies the seed into the seed
// buffer, calls rsa_keygen__generate, then reads the eight key parts back as hex
// ---------------------------------------------------------------------------

#define RESULT_COUNT 8
#define RESULT_HEX_MAX 2100

static uint8 g_seed_buffer[256];
static uint8 g_results[RESULT_COUNT][RESULT_HEX_MAX];

// writes a bignum as minimal lowercase hex, same shape jsbn's toString(16) produces
static void _rsa_internal__bn_to_hex(const bignum* a, uint8* out)
{
    static const char digits[] = "0123456789abcdef";
    int32 started = FALSE;
    int32 i, shift;
    int32 pos = 0;

    if (a->used == 0)
    {
        out[0] = '0';
        out[1] = 0;
        return;
    }

    for (i = a->used - 1; i >= 0; i--)
    {
        for (shift = 28; shift >= 0; shift -= 4)
        {
            uint32 nibble = (a->limb[i] >> shift) & 15;
            if (started == FALSE && nibble == 0) { continue; }
            started = TRUE;
            out[pos++] = (uint8)digits[nibble];
        }
    }
    out[pos] = 0;
}

__attribute__((export_name("rsa_keygen__get_seed_buffer")))
uint8* rsa_keygen__get_seed_buffer(void)
{
    return g_seed_buffer;
}

__attribute__((export_name("rsa_keygen__get_result")))
uint8* rsa_keygen__get_result(int32 index)
{
    if (index < 0 || index >= RESULT_COUNT) { return g_results[0]; }
    return g_results[index];
}

// the whole RSAGenerate(bits, "03") walk. results land as hex strings in the order
// n, e, d, p, q, dmp1, dmq1, coeff. returns 1 on success, 0 on a bad input
__attribute__((export_name("rsa_keygen__generate")))
int32 rsa_keygen__generate(int32 bits, int32 seed_length)
{
    static bignum p, q, p1, q1, phi, n, d, dmp1, dmq1, coeff, tmp;
    int32 qs = bits >> 1;
    uint32 k;

    if (bits < 256 || bits > 8192 || seed_length <= 0 || seed_length > 256) { return 0; }

    _rsa_internal__seedrandom(g_seed_buffer, seed_length);

    for (;;)
    {
        // p: RSAGenerate keeps walking until p-1 is coprime to e=3 (else there is no
        // private exponent) and p also survives the stricter certainty-10 test
        for (;;)
        {
            _rsa_internal__random_prime(&p, bits - qs, 1);
            _rsa_internal__bn_copy(&p1, &p);
            _rsa_internal__bn_sub(&p1, &g_bn_one);
            if (_rsa_internal__bn_mod_word(&p1, 3) != 0 && _rsa_internal__is_probable_prime(&p, 10) == TRUE) { break; }
        }

        for (;;)
        {
            _rsa_internal__random_prime(&q, qs, 1);
            _rsa_internal__bn_copy(&q1, &q);
            _rsa_internal__bn_sub(&q1, &g_bn_one);
            if (_rsa_internal__bn_mod_word(&q1, 3) != 0 && _rsa_internal__is_probable_prime(&q, 10) == TRUE) { break; }
        }

        if (_rsa_internal__bn_compare(&p, &q) <= 0)
        {
            _rsa_internal__bn_copy(&tmp, &p);
            _rsa_internal__bn_copy(&p, &q);
            _rsa_internal__bn_copy(&q, &tmp);
        }

        _rsa_internal__bn_copy(&p1, &p);
        _rsa_internal__bn_sub(&p1, &g_bn_one);
        _rsa_internal__bn_copy(&q1, &q);
        _rsa_internal__bn_sub(&q1, &g_bn_one);
        _rsa_internal__bn_mul(&phi, &p1, &q1);

        if (_rsa_internal__bn_mod_word(&phi, 3) != 0)
        {
            _rsa_internal__bn_mul(&n, &p, &q);

            // d = 3^-1 mod phi as (k*phi + 1) / 3, because with e fixed at 3 the inverse
            // has that closed form; k is whichever of 1 or 2 makes it divide evenly
            k = (_rsa_internal__bn_mod_word(&phi, 3) == 1) ? 2 : 1;
            _rsa_internal__bn_copy(&d, &phi);
            if (k == 2) { _rsa_internal__bn_add(&d, &phi); }
            _rsa_internal__bn_add_word(&d, 1);
            _rsa_internal__bn_div_word(&d, 3);

            _rsa_internal__bn_mod(&dmp1, &d, &p1);
            _rsa_internal__bn_mod(&dmq1, &d, &q1);
            _rsa_internal__bn_mod_inverse_odd(&coeff, &q, &p);
            break;
        }
        // phi divisible by 3 never happens when both gcd checks passed, but the js
        // loops again here, so this path stays for exactness
    }

    _rsa_internal__bn_to_hex(&n, g_results[0]);
    g_results[1][0] = '3';
    g_results[1][1] = 0;
    _rsa_internal__bn_to_hex(&d, g_results[2]);
    _rsa_internal__bn_to_hex(&p, g_results[3]);
    _rsa_internal__bn_to_hex(&q, g_results[4]);
    _rsa_internal__bn_to_hex(&dmp1, g_results[5]);
    _rsa_internal__bn_to_hex(&dmq1, g_results[6]);
    _rsa_internal__bn_to_hex(&coeff, g_results[7]);

    return 1;
}

// ---------------------------------------------------------------------------
// the runtime half of this module: generic base^exp mod m, exported so rsa-crypto.js's
// doPublic and doPrivate (every rsa encrypt and decrypt) run on wasm too. hex strings
// in and out, odd modulus only - rsa moduli always are
// ---------------------------------------------------------------------------

#define MODPOW_BUFFERS 3

static uint8 g_modpow_input[MODPOW_BUFFERS][RESULT_HEX_MAX];
static uint8 g_modpow_result[RESULT_HEX_MAX];

// parses a hex string (either case) into a bignum; FALSE on empty, oversized or junk input
static boole _rsa_internal__bn_from_hex(bignum* out, const uint8* hex)
{
    int32 length = 0;
    int32 i;

    while (hex[length] != 0 && length < RESULT_HEX_MAX) { length++; }
    if (length == 0 || length >= RESULT_HEX_MAX) { return FALSE; }

    _rsa_internal__bn_zero(out);
    for (i = 0; i < length; i++)
    {
        uint8 ch = hex[length - 1 - i];
        uint32 nibble;

        if (ch >= '0' && ch <= '9') { nibble = (uint32)(ch - '0'); }
        else if (ch >= 'a' && ch <= 'f') { nibble = (uint32)(ch - 'a' + 10); }
        else if (ch >= 'A' && ch <= 'F') { nibble = (uint32)(ch - 'A' + 10); }
        else { return FALSE; }

        out->limb[i >> 3] |= nibble << ((i & 7) * 4);
    }
    out->used = (length + 7) / 8;
    _rsa_internal__bn_trim(out);
    return TRUE;
}

__attribute__((export_name("rsa_keygen__get_modpow_buffer")))
uint8* rsa_keygen__get_modpow_buffer(int32 index)
{
    if (index < 0 || index >= MODPOW_BUFFERS) { return g_modpow_input[0]; }
    return g_modpow_input[index];
}

__attribute__((export_name("rsa_keygen__get_modpow_result")))
uint8* rsa_keygen__get_modpow_result(void)
{
    return g_modpow_result;
}

// base^exp mod modulus from the hex input buffers (0=base, 1=exp, 2=modulus);
// result lands as hex in the result buffer, returns 1 on success
__attribute__((export_name("rsa_keygen__modpow")))
int32 rsa_keygen__modpow(void)
{
    static bignum base, exp, modulus, reduced, result;

    if (_rsa_internal__bn_from_hex(&base, g_modpow_input[0]) == FALSE) { return 0; }
    if (_rsa_internal__bn_from_hex(&exp, g_modpow_input[1]) == FALSE) { return 0; }
    if (_rsa_internal__bn_from_hex(&modulus, g_modpow_input[2]) == FALSE) { return 0; }

    if (modulus.used == 0 || (modulus.limb[0] & 1) == 0) { return 0; }
    if (modulus.used > BN_LIMBS / 2) { return 0; }

    if (_rsa_internal__bn_is_word(&modulus, 1) == TRUE)
    {
        g_modpow_result[0] = '0';
        g_modpow_result[1] = 0;
        return 1;
    }

    _rsa_internal__bn_mod(&reduced, &base, &modulus);
    _rsa_internal__mont_setup(&modulus);
    _rsa_internal__mont_pow(&result, &reduced, &exp);
    _rsa_internal__bn_to_hex(&result, g_modpow_result);

    return 1;
}
