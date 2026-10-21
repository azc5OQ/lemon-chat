// This file checks the size of clients' RSA keys and encrypts the challenge used to prove they own their private key.
#include "channel.h"
#include "client_login.h"
#include "client_session.h"
#include "identity_store.h"
#include "administration.h"
#include "video_channel.h"
#include "../definitions.h"
#include "../../third-party/dave-g-json/cJSON.h"
#include "../../third-party/rxi-log/log.h"
#include "../../third-party/eteran-cvector/cvector.h"
#include "../base.h"

#include "../memory_manager.h"
#include "../clib/clib_memory.h"
#include "../clib/clib_string.h"
#include "../util.h"
#include "../metadata_crypto.h"
#include "login_admission.h"
#include "../dh_primes.h"
#include "../../third-party/libtom/libtommath/tommath.h"
#include "../../third-party/zhicheng/base64.h"
#include "../../third-party/mbedtls-3.6.6/include/mbedtls/rsa.h"
#include "../../third-party/mbedtls-3.6.6/include/mbedtls/entropy.h"
#include "../../third-party/mbedtls-3.6.6/include/mbedtls/ctr_drbg.h"
#include "../../third-party/mbedtls-3.6.6/include/mbedtls/bignum.h"
#include "../../third-party/mbedtls-3.6.6/include/mbedtls/base64.h"
#include "../../third-party/mbedtls-3.6.6/include/mbedtls/platform_util.h"
#include "server_logs.h"
#include "../ip_tools.h"
#include "audio_channel.h"


// declarations

/**
 * @brief bit length of a base64-encoded rsa public key modulus. leading zero bytes are
 *        skipped so a padded key cannot claim a bigger size than it has
 *
 * @param char* public_key_modulus -> the client's base64 public key string
 *
 * @return uint64 modulus bit length, 0 on any invalid input
 */
uint64 client_login__get_public_key_bit_length(char* public_key_modulus)
{
    unsigned char* modulus_binary = 0;
    uint64 modulus_length = 0;
    uint64 first_nonzero = 0;
    uint64 key_string_length = 0;
    uint64 bits = 0;
    unsigned char top_byte = 0;

    if (public_key_modulus == NULL_POINTER)
    {
        return 0;
    }

    // an 8192-bit modulus is 1368 base64 chars; anything longer is not a valid key
    key_string_length = (uint64)strlen(public_key_modulus);
    if (key_string_length == 0 || key_string_length > 1372)
    {
        return 0;
    }

    modulus_binary = (unsigned char*)memorymanager__allocate(2048, MEMALLOC_PUBLIC_KEY_ENCRYPT);
    modulus_length = zchg_base64_decode(public_key_modulus, key_string_length, modulus_binary);

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

    memorymanager__free((nuint)modulus_binary);
    return bits;
}

/**
 * @brief Encrypts the login challenge with the client's RSA modulus and fixed exponent 3.
 * @param char* public_key_modulus -> RSA modulus encoded as base64
 * @param unsigned char* bytes -> challenge bytes to encrypt
 * @param uint64 buffer_length -> challenge length; the input copy supports up to 256 bytes
 * @return char* -> allocated base64 ciphertext, or an empty string for an oversized modulus
 * @attention Release the returned string with memorymanager__free.
 */
char* client_login__encrypt_string_with_public_key(char* public_key_modulus, unsigned char* bytes, uint64 buffer_length)
{
    int status = 0;
    mbedtls_rsa_context rsa;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_mpi N;
    mbedtls_mpi E;
    const char* pers = "rsa_encrypt";
    char* public_key_modulus_binary = NULL_POINTER;
    uint64 buffer_modulus_bin_outsize = 0;
    int ret = 0;
    unsigned char* inputbuffer = NULL_POINTER;
    unsigned char* outbuffer = NULL_POINTER;
    uint64 base64_out_string_size = 0;
    void* base64_out_buffer = NULL_POINTER;

    DBG_ENCRYPTION log_info("%s %s %s", "public_key_modulus_base64 ", public_key_modulus, "\n");

    // an 8192-bit modulus is 1368 base64 chars / 1024 bytes; longer input is not a valid key
    if (strlen(public_key_modulus) > 1372)
    {
        base64_out_buffer = (void*)memorymanager__allocate(1, MEMALLOC_PUBLIC_KEY_ENCRYPT);
        ((char*)base64_out_buffer)[0] = 0;
        return base64_out_buffer;
    }

    public_key_modulus_binary = (char*)memorymanager__allocate(2048, MEMALLOC_PUBLIC_KEY_ENCRYPT);

    buffer_modulus_bin_outsize = zchg_base64_decode(public_key_modulus, strlen(public_key_modulus), (unsigned char*)public_key_modulus_binary);

    DBG_ENCRYPTION log_info("%s %llu %s", "buffer_modulus_bin_outsize ", buffer_modulus_bin_outsize, "\n");

    mbedtls_rsa_init(&rsa);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    mbedtls_entropy_init(&entropy);

    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const unsigned char*)pers, strlen(pers));
    if (ret != 0)
    {
        DBG_ENCRYPTION log_info("%s %d", "failed\n  ! mbedtls_ctr_drbg_seed returned ", ret);
    }

    // modulus and exponent; others are not needed for successful import
    mbedtls_mpi_init(&N);
    mbedtls_mpi_read_binary(&N, public_key_modulus_binary, buffer_modulus_bin_outsize);
    // status = mbedtls_mpi_read_string(&N, 64, public_key_modulus_base64);
    // N -> modulus
    if (status != 0)
    {
        DBG_ENCRYPTION log_info("%s %d %s", " mbedtls_mpi_read_string N failed ", status, "\n");
    }

    mbedtls_mpi_init(&E);
    // E -> exponent

    // load exponent from string (3)
    status = mbedtls_mpi_read_string(&E, 10, "3");

    if (status != 0)
    {
        DBG_ENCRYPTION log_info("%s %d %s", " mbedtls_mpi_read_string E failed ", status, "\n");
    }

    status = mbedtls_rsa_import(&rsa, &N, NULL_POINTER, NULL_POINTER, NULL_POINTER, &E);

    if (status != 0)
    {
        DBG_ENCRYPTION log_info("%s %d %s", " [FAIL] status ", status, "\n");
    }

    inputbuffer = (unsigned char*)memorymanager__allocate(1024, MEMALLOC_PUBLIC_KEY_ENCRYPT);

    clib__copy_memory(bytes, inputbuffer, buffer_length, 256);

    outbuffer = (unsigned char*)memorymanager__allocate(2048, MEMALLOC_PUBLIC_KEY_ENCRYPT);

    status = mbedtls_rsa_pkcs1_encrypt(&rsa, mbedtls_ctr_drbg_random, &ctr_drbg, buffer_length, inputbuffer, outbuffer);

    // status = mbedtls_rsa_public(&rsa, inputbuffer, outbuffer);
    // mbedtls_rsa_pkcs1_encrypt()
    if (status != 0)
    {
        DBG_ENCRYPTION log_info("%s %X %s", "[!] client_login__encrypt_string_with_public_key failed ", status, " \n");
    }
    else
    {
        DBG_ENCRYPTION log_info("%s %d %s", "[!] client_login__encrypt_string_with_public_key succeeded ", status, " \n");
    }

    // the pkcs1 ciphertext is exactly as long as the modulus, not fixed 256 bytes:
    // the old hardcoded 256 silently truncated the challenge for every key over 2048 bits
    base64_out_string_size = ((4 * buffer_modulus_bin_outsize / 3) + 3) & ~3;

    DBG_ENCRYPTION log_info("%s %llu %s", "base64_out_string_size -> ", base64_out_string_size, "\n");

    base64_out_buffer = (void*)memorymanager__allocate(base64_out_string_size * 2, MEMALLOC_PUBLIC_KEY_ENCRYPT);

    zchg_base64_encode(outbuffer, buffer_modulus_bin_outsize, base64_out_buffer);

    memorymanager__free((nuint)public_key_modulus_binary);
    memorymanager__free((nuint)outbuffer);
    memorymanager__free((nuint)inputbuffer);

    return base64_out_buffer;
}


