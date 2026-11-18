#ifndef LEMON_CIPHER_VENDOR_CONFIG_H
#define LEMON_CIPHER_VENDOR_CONFIG_H
#define LTC_NOTHING
#define LTC_TWOFISH
#define LTC_SERPENT
#define LTC_ECB_MODE
#define LTC_NO_TEST
#if defined(__wasm__)
#define LTC_NO_ASM
#endif
#define ARGTYPE 4
#define twofish_setup lemon_twofish_setup
#define twofish_ecb_encrypt lemon_twofish_ecb_encrypt
#define twofish_ecb_decrypt lemon_twofish_ecb_decrypt
#define twofish_done lemon_twofish_done
#define twofish_test lemon_twofish_test
#define twofish_keysize lemon_twofish_keysize
#define twofish_desc lemon_twofish_desc
#define serpent_setup lemon_serpent_setup
#define serpent_ecb_encrypt lemon_serpent_ecb_encrypt
#define serpent_ecb_decrypt lemon_serpent_ecb_decrypt
#define serpent_done lemon_serpent_done
#define serpent_test lemon_serpent_test
#define serpent_keysize lemon_serpent_keysize
#define serpent_desc lemon_serpent_desc
#endif
