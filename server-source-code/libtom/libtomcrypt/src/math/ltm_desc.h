#ifndef LTM_DESC_H

#define LTM_DESC_H 1


int ltc_mp_mpi_to_ltc_error(int err);

int ltc_mp_init(void **a);

void ltc_mp_deinit(void *a);

int ltc_mp_neg(void *a, void *b);

int ltc_mp_copy(void *a, void *b);


int ltc_mp_init_copy(void **a, void *b);

/* ---- trivial ---- */
int ltc_mp_set_int(void *a, ltc_mp_digit b);

unsigned long ltc_mp_get_int(void *a);


ltc_mp_digit ltc_mp_get_digit(void *a, int n);

int ltc_mp_get_digit_count(void *a);

int ltc_mp_compare(void *a, void *b);

int ltc_mp_compare_d(void *a, ltc_mp_digit b);


int ltc_mp_count_bits(void *a);

int ltc_mp_count_lsb_bits(void *a);

int ltc_mp_twoexpt(void *a, int n);


/* ---- conversions ---- */

/* read ascii string */
int ltc_mp_read_radix(void *a, const char *b, int radix);

/* write one */
int ltc_mp_write_radix(void *a, char *b, int radix);

/* get size as unsigned char string */
unsigned long ltc_mp_unsigned_size(void *a);

/* store */
int ltc_mp_unsigned_write(void *a, unsigned char *b);


/* read */
int ltc_mp_unsigned_read(void *a, unsigned char *b, unsigned long len);

/* add */
int ltc_mp_add(void *a, void *b, void *c);

int ltc_mp_addi(void *a, ltc_mp_digit b, void *c);

/* sub */
int ltc_mp_sub(void *a, void *b, void *c);

int ltc_mp_subi(void *a, ltc_mp_digit b, void *c);

/* mul */
int ltc_mp_mul(void *a, void *b, void *c);

int ltc_mp_muli(void *a, ltc_mp_digit b, void *c);

/* sqr */
int ltc_mp_sqr(void *a, void *b);


/* div */
int ltc_mp_divide(void *a, void *b, void *c, void *d);

int ltc_mp_div_2(void *a, void *b);


/* modi */
int ltc_mp_modi(void *a, ltc_mp_digit b, ltc_mp_digit *c);

/* gcd */
int ltc_mp_gcd(void *a, void *b, void *c);

/* lcm */
int ltc_mp_lcm(void *a, void *b, void *c);

int ltc_mp_addmod(void *a, void *b, void *c, void *d);

int ltc_mp_submod(void *a, void *b, void *c, void *d);


int ltc_mp_mulmod(void *a, void *b, void *c, void *d);

int ltc_mp_sqrmod(void *a, void *b, void *c);

/* invmod */
int ltc_mp_invmod(void *a, void *b, void *c);

/* setup */
int ltc_mp_montgomery_setup(void *a, void **b);

    
/* get normalization value */
int ltc_mp_montgomery_normalization(void *a, void *b);


/* reduce */
int ltc_mp_montgomery_reduce(void *a, void *b, void *c);

/* clean up */
void ltc_mp_montgomery_deinit(void *a);


int ltc_mp_exptmod(void *a, void *b, void *c, void *d);

int ltc_mp_isprime(void *a, int b, int *c);

int ltc_mp_set_rand(void *a, int size);



#endif