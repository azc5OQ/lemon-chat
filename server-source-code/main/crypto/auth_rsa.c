// This file checks the size of clients' RSA keys and encrypts the challenge used to prove they own their private key.
#include "../core/channel.h"
#include "auth_rsa.h"
#include "../core/client_session.h"
#include "../core/identity_store.h"
#include "../core/administration.h"
#include "../core/video_channel.h"
#include "../definitions.h"
#include "../../third-party/dave-g-json/cJSON.h"
#include "../../third-party/rxi-log/log.h"
#include "../../third-party/eteran-cvector/cvector.h"
#include "../base.h"

#include "../memory_manager.h"
#include "../clib/clib.h"
#include "../util.h"
#include "crypto_metadata.h"
#include "../core/login_admission.h"
#include "dh_parameters.h"
#include "../../third-party/libtom/libtommath/tommath.h"
#include "../../third-party/zhicheng/base64.h"
#include "../../third-party/mbedtls-3.6.6/include/mbedtls/rsa.h"
#include "../../third-party/mbedtls-3.6.6/include/mbedtls/entropy.h"
#include "../../third-party/mbedtls-3.6.6/include/mbedtls/ctr_drbg.h"
#include "../../third-party/mbedtls-3.6.6/include/mbedtls/bignum.h"
#include "../../third-party/mbedtls-3.6.6/include/mbedtls/base64.h"
#include "../../third-party/mbedtls-3.6.6/include/mbedtls/platform_util.h"
#include "../core/server_logs.h"
#include "../ip_tools.h"
#include "../core/audio_channel.h"

// declarations

/**
 * @brief bit length of a base64-encoded rsa public key modulus. leading zero bytes are skipped so a
 *        padded key cannot claim a bigger size than it has
 *
 * @param char* public_key_modulus -> the client's base64 public key string
 *
 * @return uint64 modulus bit length, 0 on any invalid input
 */
uint64 auth_rsa__get_public_key_bit_length(char *public_key_modulus)
{
    ubyte modulus_binary[1024] = { 0 };
    size_t modulus_length = 0; // mbedTLS writes through size_t*.
    uint64 first_nonzero = 0;
    uint64 key_string_length = 0;
    uint64 bits = 0;
    ubyte top_byte = 0;

    if (public_key_modulus == NULL_POINTER)
    {
        goto cleanup;
    }
    key_string_length = (uint64)clib__strlen(public_key_modulus);
    if (key_string_length == 0 || key_string_length > 1372 || mbedtls_base64_decode(modulus_binary, sizeof(modulus_binary), &modulus_length, (const ubyte *)public_key_modulus, (uint64)key_string_length) != 0)
    {
        goto cleanup;
    }
    while (first_nonzero < modulus_length && modulus_binary[first_nonzero] == 0)
    {
        first_nonzero++;
    }
    if (first_nonzero < modulus_length)
    {
        top_byte = modulus_binary[first_nonzero];
        bits = (modulus_length - first_nonzero - 1) * 8;
        while (top_byte > 0)
        {
            bits++;
            top_byte >>= 1;
        }
    }

cleanup:
    clib__null_memory(modulus_binary, sizeof(modulus_binary));
    return bits;
}

/**
 * @brief Encrypts the login challenge with the client's RSA modulus and fixed exponent 3.
 * @param char* public_key_modulus -> RSA modulus encoded as base64
 * @param ubyte* bytes -> challenge bytes to encrypt
 * @param uint64 buffer_length -> challenge length, at most 256 bytes and fitting the RSA padding
 * @return char* -> allocated base64 ciphertext, or NULL_POINTER on invalid input or failure
 * @attention Release the returned string with memorymanager__free.
 */
char *auth_rsa__encrypt_string_with_public_key(char *public_key_modulus, ubyte *bytes, uint64 buffer_length)
{
    mbedtls_rsa_context rsa;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_mpi modulus;
    mbedtls_mpi exponent;
    const char *personalization = "rsa_encrypt";
    ubyte modulus_binary[1024] = { 0 };
    ubyte *ciphertext = NULL_POINTER;
    char *encoded = NULL_POINTER;
    char *result = NULL_POINTER;
    uint64 key_string_length = 0;
    size_t modulus_length = 0; // mbedTLS writes through size_t*.
    uint64 ciphertext_length = 0;
    uint64 encoded_capacity = 0;
    size_t written = 0; // mbedTLS writes through size_t*.

    mbedtls_rsa_init(&rsa);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_mpi_init(&modulus);
    mbedtls_mpi_init(&exponent);

    if (public_key_modulus == NULL_POINTER || bytes == NULL_POINTER || buffer_length == 0 || buffer_length > 256)
    {
        goto cleanup;
    }
    key_string_length = (uint64)clib__strlen(public_key_modulus);
    if (key_string_length == 0 || key_string_length > 1372 || mbedtls_base64_decode(modulus_binary, sizeof(modulus_binary), &modulus_length, (const ubyte *)public_key_modulus, (uint64)key_string_length) != 0 || modulus_length == 0)
    {
        goto cleanup;
    }
    if (mbedtls_mpi_read_binary(&modulus, modulus_binary, modulus_length) != 0 || mbedtls_mpi_lset(&exponent, 3) != 0 || mbedtls_rsa_import(&rsa, &modulus, NULL_POINTER, NULL_POINTER, NULL_POINTER, &exponent) != 0 || mbedtls_rsa_complete(&rsa) != 0 || mbedtls_rsa_check_pubkey(&rsa) != 0 || mbedtls_rsa_set_padding(&rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE) != 0)
    {
        goto cleanup;
    }
    ciphertext_length = mbedtls_rsa_get_len(&rsa);
    if (ciphertext_length < 11 || ciphertext_length > sizeof(modulus_binary) || buffer_length > ciphertext_length - 11)
    {
        goto cleanup;
    }
    if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const ubyte *)personalization, (uint64)clib__strlen(personalization)) != 0)
    {
        goto cleanup;
    }
    ciphertext = (ubyte *)memorymanager__allocate(ciphertext_length, MEMALLOC_PUBLIC_KEY_ENCRYPT);
    if (ciphertext == NULL_POINTER || mbedtls_rsa_pkcs1_encrypt(&rsa, mbedtls_ctr_drbg_random, &ctr_drbg, (uint64)buffer_length, bytes, ciphertext) != 0)
    {
        goto cleanup;
    }
    encoded_capacity = 4 * ((ciphertext_length + 2) / 3) + 1;
    encoded = (char *)memorymanager__allocate(encoded_capacity, MEMALLOC_PUBLIC_KEY_ENCRYPT);
    if (encoded == NULL_POINTER || mbedtls_base64_encode((ubyte *)encoded, encoded_capacity, &written, ciphertext, ciphertext_length) != 0)
    {
        goto cleanup;
    }
    result = encoded;
    encoded = NULL_POINTER;

cleanup:
    memorymanager__free((nuint)encoded);
    memorymanager__free((nuint)ciphertext);
    mbedtls_mpi_free(&exponent);
    mbedtls_mpi_free(&modulus);
    mbedtls_rsa_free(&rsa);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    clib__null_memory(modulus_binary, sizeof(modulus_binary));
    return result;
}
