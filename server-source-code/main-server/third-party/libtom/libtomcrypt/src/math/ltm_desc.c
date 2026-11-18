/* LibTomCrypt, modular cryptographic library -- Tom St Denis
 *
 * LibTomCrypt is a library that provides various cryptographic
 * algorithms in a highly modular and flexible manner.
 *
 * The library is free for all purposes without any express
 * guarantee it works.
 */

#define DESC_DEF_ONLY
#include "tomcrypt.h"

#ifdef LTM_DESC

#include <tommath.h>
#include "ltm_desc.h"

static const struct {
    int mpi_code, ltc_code;
} mpi_to_ltc_codes[] = {
   { MP_OKAY ,  CRYPT_OK},
   { MP_MEM  ,  CRYPT_MEM},
   { MP_VAL  ,  CRYPT_INVALID_ARG},
};

/**
   Convert a MPI error to a LTC error (Possibly the most powerful function ever!  Oh wait... no)
   @param err    The error to convert
   @return The equivalent LTC error code or CRYPT_ERROR if none found
*/
int ltc_mp_mpi_to_ltc_error(int err)
{
   int x;

   for (x = 0; x < (int)(sizeof(mpi_to_ltc_codes)/sizeof(mpi_to_ltc_codes[0])); x++) {
       if (err == mpi_to_ltc_codes[x].mpi_code) {
          return mpi_to_ltc_codes[x].ltc_code;
       }
   }
   return CRYPT_ERROR;
}

int ltc_mp_init(void **a)
{
   int err;

   LTC_ARGCHK(a != NULL);

   *a = XCALLOC(1, sizeof(mp_int));
   if (*a == NULL) {
      return CRYPT_MEM;
   }

   if ((err = ltc_mp_mpi_to_ltc_error(mp_init(*a))) != CRYPT_OK) {
      XFREE(*a);
   }
   return err;
}

void ltc_mp_deinit(void *a)
{
   LTC_ARGCHKVD(a != NULL);
   mp_clear(a);
   XFREE(a);
}

int ltc_mp_neg(void *a, void *b)
{
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(b != NULL);
   return ltc_mp_mpi_to_ltc_error(ltc_mp_neg(a, b));
}

int ltc_mp_copy(void *a, void *b)
{
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(b != NULL);
   return ltc_mp_mpi_to_ltc_error(ltc_mp_copy(a, b));
}

int ltc_mp_init_copy(void **a, void *b)
{
   if (ltc_mp_init(a) != CRYPT_OK) {
      return CRYPT_MEM;
   }
   return ltc_mp_copy(b, *a);
}

/* ---- trivial ---- */
int ltc_mp_set_int(void *a, ltc_mp_digit b)
{
   LTC_ARGCHK(a != NULL);
   return ltc_mp_mpi_to_ltc_error(ltc_mp_set_int(a, b));
}

unsigned long ltc_mp_get_int(void *a)
{
   LTC_ARGCHK(a != NULL);
   return ltc_mp_get_int(a);
}

ltc_mp_digit ltc_mp_get_digit(void *a, int n)
{
   mp_int *A;
   LTC_ARGCHK(a != NULL);
   A = a;
   return (n >= A->used || n < 0) ? 0 : A->dp[n];
}

int ltc_mp_get_digit_count(void *a)
{
   mp_int *A;
   LTC_ARGCHK(a != NULL);
   A = a;
   return A->used;
}

int ltc_mp_compare(void *a, void *b)
{
   int ret;
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(b != NULL);
   ret = mp_cmp(a, b);
   switch (ret) {
      case MP_LT: return LTC_MP_LT;
      case MP_EQ: return LTC_MP_EQ;
      case MP_GT: return LTC_MP_GT;
      default:    return 0;
   }
}

int ltc_mp_compare_d(void *a, ltc_mp_digit b)
{
   int ret;
   LTC_ARGCHK(a != NULL);
   ret = mp_cmp_d(a, b);
   switch (ret) {
      case MP_LT: return LTC_MP_LT;
      case MP_EQ: return LTC_MP_EQ;
      case MP_GT: return LTC_MP_GT;
      default:    return 0;
   }
}

int ltc_mp_count_bits(void *a)
{
   LTC_ARGCHK(a != NULL);
   return ltc_mp_count_bits(a);
}

int ltc_mp_count_lsb_bits(void *a)
{
   LTC_ARGCHK(a != NULL);
   return ltc_mp_count_lsb_bits(a);
}


int ltc_mp_twoexpt(void *a, int n)
{
   LTC_ARGCHK(a != NULL);
   return ltc_mp_mpi_to_ltc_error(ltc_mp_twoexpt(a, n));
}

/* ---- conversions ---- */

/* read ascii string */
int ltc_mp_read_radix(void *a, const char *b, int radix)
{
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(b != NULL);
   return ltc_mp_mpi_to_ltc_error(ltc_mp_read_radix(a, b, radix));
}

/* write one */
int ltc_mp_write_radix(void *a, char *b, int radix)
{
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(b != NULL);
   return ltc_mp_mpi_to_ltc_error(ltc_mp_write_radix(a, b, radix));
}

/* get size as unsigned char string */
unsigned long ltc_mp_unsigned_size(void *a)
{
   LTC_ARGCHK(a != NULL);
   return ltc_mp_unsigned_size(a); //wtf?
}

/* store */
int ltc_mp_unsigned_write(void *a, unsigned char *b)
{
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(b != NULL);
   return ltc_mp_mpi_to_ltc_error(ltc_mp_unsigned_write(a, b));
}

/* read */

int ltc_mp_unsigned_read(void *a, unsigned char *b, unsigned long len)
{
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(b != NULL);
   return ltc_mp_mpi_to_ltc_error(mp_read_unsigned_bin(a, b, len));
}

/* add */
int ltc_mp_add(void *a, void *b, void *c)
{
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(b != NULL);
   LTC_ARGCHK(c != NULL);
   return ltc_mp_mpi_to_ltc_error(mp_add(a, b, c));
}

int ltc_mp_addi(void *a, ltc_mp_digit b, void *c)
{
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(c != NULL);
   return ltc_mp_mpi_to_ltc_error(mp_add_d(a, b, c));
}

/* sub */
int ltc_mp_sub(void *a, void *b, void *c)
{
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(b != NULL);
   LTC_ARGCHK(c != NULL);
   return ltc_mp_mpi_to_ltc_error(mp_sub(a, b, c));
}

int ltc_mp_subi(void *a, ltc_mp_digit b, void *c)
{
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(c != NULL);
   return ltc_mp_mpi_to_ltc_error(mp_sub_d(a, b, c));
}

/* mul */
int ltc_mp_mul(void *a, void *b, void *c)
{
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(b != NULL);
   LTC_ARGCHK(c != NULL);
   return ltc_mp_mpi_to_ltc_error(mp_mul(a, b, c));
}

int ltc_mp_muli(void *a, ltc_mp_digit b, void *c)
{
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(c != NULL);
   return ltc_mp_mpi_to_ltc_error(mp_mul_d(a, b, c));
}

/* sqr */
int ltc_mp_sqr(void *a, void *b)
{
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(b != NULL);
   return ltc_mp_mpi_to_ltc_error(mp_sqr(a, b));
}

/* div */
int ltc_mp_divide(void *a, void *b, void *c, void *d)
{
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(b != NULL);
   return ltc_mp_mpi_to_ltc_error(mp_div(a, b, c, d));
}

int ltc_mp_div_2(void *a, void *b)
{
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(b != NULL);
   return ltc_mp_mpi_to_ltc_error(mp_div_2(a, b));
}

/* modi */
int ltc_mp_modi(void *a, ltc_mp_digit b, ltc_mp_digit *c)
{
   mp_digit tmp;
   int      err;

   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(c != NULL);

   if ((err = ltc_mp_mpi_to_ltc_error(ltc_mp_modi(a, b, (ltc_mp_digit*)&tmp))) != CRYPT_OK) {
      return err;
   }
   *c = tmp;
   return CRYPT_OK;
}

/* gcd */
int ltc_mp_gcd(void *a, void *b, void *c)
{
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(b != NULL);
   LTC_ARGCHK(c != NULL);
   return ltc_mp_mpi_to_ltc_error(ltc_mp_gcd(a, b, c));
}

/* lcm */
int ltc_mp_lcm(void *a, void *b, void *c)
{
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(b != NULL);
   LTC_ARGCHK(c != NULL);
   return ltc_mp_mpi_to_ltc_error(ltc_mp_lcm(a, b, c));
}

int ltc_mp_addmod(void *a, void *b, void *c, void *d)
{
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(b != NULL);
   LTC_ARGCHK(c != NULL);
   LTC_ARGCHK(d != NULL);
   return ltc_mp_mpi_to_ltc_error(ltc_mp_addmod(a,b,c,d));
}

int ltc_mp_submod(void *a, void *b, void *c, void *d)
{
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(b != NULL);
   LTC_ARGCHK(c != NULL);
   LTC_ARGCHK(d != NULL);
   return ltc_mp_mpi_to_ltc_error(ltc_mp_submod(a,b,c,d));
}

int ltc_mp_mulmod(void *a, void *b, void *c, void *d)
{
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(b != NULL);
   LTC_ARGCHK(c != NULL);
   LTC_ARGCHK(d != NULL);
   return ltc_mp_mpi_to_ltc_error(ltc_mp_mulmod(a,b,c,d));
}

int ltc_mp_sqrmod(void *a, void *b, void *c)
{
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(b != NULL);
   LTC_ARGCHK(c != NULL);
   return ltc_mp_mpi_to_ltc_error(ltc_mp_sqrmod(a,b,c));
}

/* invmod */
int ltc_mp_invmod(void *a, void *b, void *c)
{
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(b != NULL);
   LTC_ARGCHK(c != NULL);
   return ltc_mp_mpi_to_ltc_error(ltc_mp_invmod(a, b, c));
}

/* setup */
int ltc_mp_montgomery_setup(void *a, void **b)
{
   int err;
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(b != NULL);
   *b = XCALLOC(1, sizeof(mp_digit));
   if (*b == NULL) {
      return CRYPT_MEM;
   }
   if ((err = ltc_mp_mpi_to_ltc_error(mp_montgomery_setup(a, (mp_digit *)*b))) != CRYPT_OK) {
      XFREE(*b);
   }
   return err;
}

/* get normalization value */
int ltc_mp_montgomery_normalization(void *a, void *b)
{
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(b != NULL);
   return ltc_mp_mpi_to_ltc_error(mp_montgomery_calc_normalization(a, b));
}

/* reduce */
int ltc_mp_montgomery_reduce(void *a, void *b, void *c)
{
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(b != NULL);
   LTC_ARGCHK(c != NULL);
   return ltc_mp_mpi_to_ltc_error(mp_montgomery_reduce(a, b, *((mp_digit *)c)));
}

/* clean up */
void ltc_mp_montgomery_deinit(void *a)
{
   XFREE(a);
}

int ltc_mp_exptmod(void *a, void *b, void *c, void *d)
{
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(b != NULL);
   LTC_ARGCHK(c != NULL);
   LTC_ARGCHK(d != NULL);
   return ltc_mp_mpi_to_ltc_error(mp_exptmod(a,b,c,d));
}

int ltc_mp_isprime(void *a, int b, int *c)
{
   int err;
   LTC_ARGCHK(a != NULL);
   LTC_ARGCHK(c != NULL);
   if (b == 0) {
       b = LTC_MILLER_RABIN_REPS;
   } /* if */
   err = ltc_mp_mpi_to_ltc_error(ltc_mp_isprime(a, b, c));
   *c = (*c == MP_YES) ? LTC_MP_YES : LTC_MP_NO;
   return err;
}

int ltc_mp_set_rand(void *a, int size)
{
   LTC_ARGCHK(a != NULL);
   return ltc_mp_mpi_to_ltc_error(ltc_mp_set_rand(a, size));
}

const ltc_math_descriptor ltm_desc = {

   "LibTomMath",
   (int)DIGIT_BIT,

   &ltc_mp_init,
   &ltc_mp_init_copy,
   &ltc_mp_deinit,

   &ltc_mp_neg,
   &ltc_mp_copy,

   &ltc_mp_set_int,
   &ltc_mp_get_int,
   &ltc_mp_get_digit,
   &ltc_mp_get_digit_count,
   &ltc_mp_compare,
   &ltc_mp_compare_d,
   &ltc_mp_count_bits,
   &ltc_mp_count_lsb_bits,
   &ltc_mp_twoexpt,

   &ltc_mp_read_radix,
   &ltc_mp_write_radix,
   &ltc_mp_unsigned_size,
   &ltc_mp_unsigned_write,
   &ltc_mp_unsigned_read,

   &ltc_mp_add,
   &ltc_mp_addi,
   &ltc_mp_sub,
   &ltc_mp_subi,
   &ltc_mp_mul,
   &ltc_mp_muli,
   &ltc_mp_sqr,
   &ltc_mp_divide,
   &ltc_mp_div_2,
   &ltc_mp_modi,
   &ltc_mp_gcd,
   &ltc_mp_lcm,

   &ltc_mp_mulmod,
   &ltc_mp_sqrmod,
   &ltc_mp_invmod,

   &ltc_mp_montgomery_setup,
   &ltc_mp_montgomery_normalization,
   &ltc_mp_montgomery_reduce,
   &ltc_mp_montgomery_deinit,

   &ltc_mp_exptmod,
   &ltc_mp_isprime,

#ifdef LTC_MECC
#ifdef LTC_MECC_FP
   &ltc_ecc_fp_mulmod,
#else
   &ltc_ecc_mulmod,
#endif
   &ltc_ecc_projective_add_point,
   &ltc_ecc_projective_dbl_point,
   &ltc_ecc_map,
#ifdef LTC_ECC_SHAMIR
#ifdef LTC_MECC_FP
   &ltc_ecc_fp_mul2add,
#else
   &ltc_ecc_mul2add,
#endif /* LTC_MECC_FP */
#else
   NULL,
#endif /* LTC_ECC_SHAMIR */
#else
   NULL, NULL, NULL, NULL, NULL,
#endif /* LTC_MECC */

#ifdef LTC_MRSA
   &rsa_make_key,
   &rsa_exptmod,
#else
   NULL, NULL,
#endif
   &ltc_mp_addmod,
   &ltc_mp_submod,

   &ltc_mp_set_rand,

};


#endif

/* ref:         HEAD -> master, tag: v1.18.2 */
/* git commit:  7e7eb695d581782f04b24dc444cbfde86af59853 */
/* commit time: 2018-07-01 22:49:01 +0200 */
