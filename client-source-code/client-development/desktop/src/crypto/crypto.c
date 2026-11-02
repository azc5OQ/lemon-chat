#include "clib.h"
// Existing Lemon identity and metadata protocol, using the vendored crypto primitives.
#include <stdlib.h>
#include <stdatomic.h>
#include "crypto/crypto.h"
#include "network/protocol.h"
#include "crypto/protocol_dh.h"
#include "platform/system.h"
#include "cJSON.h"
#include "aes.h"
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "mbedtls/bignum.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/rsa.h"
#include "mbedtls/sha256.h"
#include "mbedtls/constant_time.h"


struct desktop_crypto_t
{
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context random;
    mbedtls_rsa_context rsa;
    mbedtls_mpi modulus;
    mbedtls_mpi exponent;
    unsigned char session_keys[64];
    unsigned char server_keys[DESKTOP_SERVER_KEY_LIMIT][32];
    uint64 server_key_count;
    boole session_ready;
};

// The browser calculation has static scratch storage. Serialize native callers.
static atomic_flag g_identity_lock = ATOMIC_FLAG_INIT;

/**
 * @brief copies only the keys used by the existing media protocol into worker-owned storage.
 * @param desktop_crypto_t* crypto -> authenticated session crypto
 * @param desktop_media_keys_t* keys -> cleared on failure; never publish to the GUI or overlay
 * @return boole -> 1 when the DH session is established
 */
boole desktop_crypto__media_keys(desktop_crypto_t* crypto, desktop_media_keys_t* keys)
{
    clib__null_memory(keys, sizeof(*keys));
    if (!crypto || !crypto->session_ready)
    {
        return 0;
    }
    clib__memcpy(keys->encryption, crypto->session_keys, 32);
    clib__memcpy(keys->authentication, crypto->session_keys + 32, 32);
    clib__memcpy(keys->server, crypto->server_keys, sizeof(keys->server));
    keys->server_count = crypto->server_key_count;
    return 1;
}

/**
 * @brief draws bounded random bytes from this worker's cryptographic generator.
 * @param desktop_crypto_t* crypto -> worker-owned crypto context
 * @param unsigned char* bytes -> destination with size bytes
 * @param uint64 size -> requested byte count, limited to 256
 * @return boole -> 1 when filled
 */
boole desktop_crypto__random(desktop_crypto_t* crypto, unsigned char* bytes, uint64 size)
{
    return crypto && bytes && size <= 256 && mbedtls_ctr_drbg_random(&crypto->random, bytes, size) == 0;
}

// declarations
unsigned char* rsa_keygen__get_seed_buffer(void);
unsigned char* rsa_keygen__get_result(int32_t index);
int32_t rsa_keygen__generate(int32_t bits, int32_t seed_length);
static boole _crypto_internal__base64(char* output, uint64 output_capacity, const unsigned char* bytes, uint64 length);
static boole _crypto_internal__mac(desktop_crypto_t* crypto, const char* iv, const char* data, unsigned char output[32]);
static void _crypto_internal__layer(const unsigned char key[32], const unsigned char iv[16], unsigned char* bytes, uint64 length);
static char* _crypto_internal__content(desktop_crypto_t* crypto, const desktop_content_keys_t* keys, const char* text, boole encrypt);
static boole _crypto_internal__content_envelope(const char* text, uint64 length, cJSON** root, unsigned char iv[16], const char** encoded);
static char* _crypto_internal__attachment(const desktop_content_keys_t* keys, const char* text, boole encrypt, uint64 padding);

/**
 * @brief encodes bytes into a capacity-carrying string buffer.
 * @param char* output -> writable destination including terminator space
 * @param uint64 output_capacity -> destination capacity in bytes, including its terminator
 * @param const unsigned char* bytes -> source bytes
 * @param uint64 length -> source byte count
 * @return boole -> 1 on success
 */
static boole _crypto_internal__base64(char* output, uint64 output_capacity, const unsigned char* bytes, uint64 length)
{
    size_t written = 0;

    if (!output || !output_capacity)
    {
        return 0;
    }
    output[0] = 0;
    return mbedtls_base64_encode((unsigned char*)output, output_capacity, &written, bytes, length) == 0;
}

/**
 * @brief initializes per-session entropy and crypto contexts.
 * @return desktop_crypto_t* -> owned state, or NULL on allocation/entropy failure
 */
desktop_crypto_t* desktop_crypto__create(void)
{
    desktop_crypto_t* crypto = calloc(1, sizeof(*crypto));
    const unsigned char personalization[] = "lemon-chat-desktop";

    if (!crypto)
    {
        return NULL;
    }
    mbedtls_entropy_init(&crypto->entropy);
    mbedtls_ctr_drbg_init(&crypto->random);
    mbedtls_rsa_init(&crypto->rsa);
    mbedtls_mpi_init(&crypto->modulus);
    mbedtls_mpi_init(&crypto->exponent);
    if (mbedtls_ctr_drbg_seed(&crypto->random, mbedtls_entropy_func, &crypto->entropy,
            personalization, sizeof(personalization) - 1) != 0)
    {
        desktop_crypto__destroy(crypto);
        return NULL;
    }
    return crypto;
}

/**
 * @brief gives an attachment worker its own RSA state and random generator without recalculating the identity.
 * @param const desktop_crypto_t* source -> session-owned identity, read only on the session worker
 * @return desktop_crypto_t* -> independent owned context, or NULL
 */
desktop_crypto_t* desktop_crypto__copy_identity(const desktop_crypto_t* source)
{
    desktop_crypto_t* copy = source ? desktop_crypto__create() : NULL;
    if (copy && mbedtls_rsa_copy(&copy->rsa, &source->rsa) != 0)
    {
        desktop_crypto__destroy(copy);
        copy = NULL;
    }
    return copy;
}

/**
 * @brief clears and releases a session's owned cryptographic material.
 * @param desktop_crypto_t* crypto -> owned state, or NULL
 * @return void
 */
void desktop_crypto__destroy(desktop_crypto_t* crypto)
{
    if (crypto)
    {
        mbedtls_rsa_free(&crypto->rsa);
        mbedtls_mpi_free(&crypto->modulus);
        mbedtls_mpi_free(&crypto->exponent);
        mbedtls_ctr_drbg_free(&crypto->random);
        mbedtls_entropy_free(&crypto->entropy);
        mbedtls_platform_zeroize(crypto, sizeof(*crypto));
        free(crypto);
    }
}

/**
 * @brief creates a new random identity string for the existing deterministic key calculation.
 * @param char* identity -> writable buffer of at least 201 bytes
 * @param uint64 identity_capacity -> destination capacity in bytes, including its terminator
 * @return boole -> 1 when a complete identity was written
 */
boole desktop_crypto__new_identity(char* identity, uint64 identity_capacity)
{
    desktop_crypto_t* crypto = NULL;
    unsigned char bytes[100] = {0};
    const char hex[] = "0123456789abcdef";
    uint64 index = 0;
    boole complete = 0;

    if (!identity || identity_capacity < sizeof(bytes) * 2 + 1)
    {
        return 0;
    }
    identity[0] = 0;
    crypto = desktop_crypto__create();
    if (crypto && mbedtls_ctr_drbg_random(&crypto->random, bytes, sizeof(bytes)) == 0)
    {
        for (index = 0; index < sizeof(bytes); index++)
        {
            identity[index * 2] = hex[bytes[index] >> 4];
            identity[index * 2 + 1] = hex[bytes[index] & 15];
        }
        identity[sizeof(bytes) * 2] = 0;
        complete = 1;
    }
    mbedtls_platform_zeroize(bytes, sizeof(bytes));
    desktop_crypto__destroy(crypto);
    return complete;
}

/**
 * @brief derives the same RSA identity as the browser, then imports it for blinded private operations.
 * @param desktop_crypto_t* crypto -> worker-owned crypto state
 * @param const char* identity -> existing identity string
 * @param int64 bits -> key size from 2048 through 8192, in 256-bit increments
 * @param char* public_key -> base64 of the modulus, matching the existing protocol
 * @param uint64 public_key_capacity -> destination capacity in bytes, including its terminator
 * @return boole -> 1 on success
 */
boole desktop_crypto__identity(desktop_crypto_t* crypto, const char* identity, int64 bits, char* public_key, uint64 public_key_capacity)
{
    unsigned char digest[32] = {0};
    unsigned char modulus[1024] = {0};
    const char hex[] = "0123456789abcdef";
    unsigned char* seed = NULL;
    mbedtls_mpi values[5] = {0};
    uint64 index = 0;
    uint64 size = 0;
    boole complete = 0;

    if (!crypto || !identity || !identity[0] || bits < 2048 || bits > 8192 || bits % 256)
    {
        return 0;
    }
    if (mbedtls_sha256((const unsigned char*)identity, clib__strlen(identity), digest, 0) != 0)
    {
        return 0;
    }
    for (index = 0; index < 5; index++)
    {
        mbedtls_mpi_init(&values[index]);
    }
    while (atomic_flag_test_and_set_explicit(&g_identity_lock, memory_order_acquire))
    {
        desktop_system__sleep(1);
    }
    seed = rsa_keygen__get_seed_buffer();
    for (index = 0; index < sizeof(digest); index++)
    {
        seed[index * 2] = hex[digest[index] >> 4];
        seed[index * 2 + 1] = hex[digest[index] & 15];
    }
    complete = rsa_keygen__generate(bits, 64) == 1;
    for (index = 0; index < 5 && complete; index++)
    {
        complete = mbedtls_mpi_read_string(&values[index], 16, (const char*)rsa_keygen__get_result((int32_t)index)) == 0;
    }
    mbedtls_platform_zeroize(seed, 64);
    for (index = 0; index < 8; index++)
    {
        seed = rsa_keygen__get_result((int32_t)index);
        mbedtls_platform_zeroize(seed, clib__strlen((const char*)seed));
    }
    atomic_flag_clear_explicit(&g_identity_lock, memory_order_release);
    // Generator order is N, E, D, P, Q; the import API takes N, P, Q, D, E.
    complete = complete && mbedtls_rsa_import(&crypto->rsa, &values[0], &values[3], &values[4], &values[2], &values[1]) == 0
        && mbedtls_rsa_complete(&crypto->rsa) == 0 && mbedtls_rsa_check_privkey(&crypto->rsa) == 0
        && mbedtls_rsa_set_padding(&crypto->rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE) == 0;
    size = mbedtls_mpi_size(&values[0]);
    complete = complete && size <= sizeof(modulus) && mbedtls_mpi_write_binary(&values[0], modulus, size) == 0
        && _crypto_internal__base64(public_key, public_key_capacity, modulus, size);
    for (index = 0; index < 5; index++)
    {
        mbedtls_mpi_free(&values[index]);
    }
    mbedtls_platform_zeroize(digest, sizeof(digest));
    mbedtls_platform_zeroize(modulus, sizeof(modulus));
    return complete;
}

/**
 * @brief derives configured metadata layers from newline-separated server key strings.
 * @param desktop_crypto_t* crypto -> worker-owned state
 * @param const char* keys -> key strings in the server's configured order; empty means none
 * @return boole -> 1 on success; 0 on an empty interior row or too many keys
 */
boole desktop_crypto__server_keys(desktop_crypto_t* crypto, const char* keys)
{
    const char* cursor = keys;
    const char* start = NULL;
    uint64 length = 0;

    crypto->server_key_count = 0;
    mbedtls_platform_zeroize(crypto->server_keys, sizeof(crypto->server_keys));
    while (*cursor)
    {
        start = cursor;
        while (*cursor && *cursor != '\n')
        {
            cursor++;
        }
        length = (uint64)(cursor - start);
        if (length && start[length - 1] == '\r')
        {
            length--;
        }
        if (!length || crypto->server_key_count == DESKTOP_SERVER_KEY_LIMIT
            || mbedtls_sha256((const unsigned char*)start, length, crypto->server_keys[crypto->server_key_count], 0) != 0)
        {
            return 0;
        }
        crypto->server_key_count++;
        if (*cursor)
        {
            cursor++;
        }
    }
    return 1;
}

/**
 * @brief creates the existing protocol's ephemeral Diffie-Hellman public mix.
 * @param desktop_crypto_t* crypto -> worker-owned state
 * @param char* public_mix -> destination for the decimal public mix
 * @param uint64 public_mix_capacity -> destination capacity in bytes, including its terminator
 * @return boole -> 1 on success
 */
boole desktop_crypto__begin_dh(desktop_crypto_t* crypto, char* public_mix, uint64 public_mix_capacity)
{
    unsigned char bytes[DH_EXPONENT_BITS / 8] = {0};
    mbedtls_mpi base = {0};
    mbedtls_mpi mix = {0};
    size_t written = 0;
    boole complete = 0;

    mbedtls_mpi_init(&base);
    mbedtls_mpi_init(&mix);
    crypto->session_ready = 0;
    complete = mbedtls_ctr_drbg_random(&crypto->random, bytes, sizeof(bytes)) == 0;
    bytes[0] |= 128;
    complete = complete && mbedtls_mpi_read_string(&crypto->modulus, 10, DH_MODULUS_STR) == 0
        && mbedtls_mpi_read_binary(&crypto->exponent, bytes, sizeof(bytes)) == 0
        && mbedtls_mpi_lset(&base, 2) == 0
        && mbedtls_mpi_exp_mod(&mix, &base, &crypto->exponent, &crypto->modulus, NULL) == 0
        && mbedtls_mpi_write_string(&mix, 10, public_mix, public_mix_capacity, &written) == 0;
    mbedtls_mpi_free(&base);
    mbedtls_mpi_free(&mix);
    mbedtls_platform_zeroize(bytes, sizeof(bytes));
    return complete;
}

/**
 * @brief validates the server mix and derives the existing AES/HMAC session keys.
 * @param desktop_crypto_t* crypto -> state containing a fresh private exponent
 * @param const char* public_mix -> decimal server public mix
 * @return boole -> 1 on success; invalid public mixes never activate the session layer
 */
boole desktop_crypto__finish_dh(desktop_crypto_t* crypto, const char* public_mix)
{
    const unsigned char salt[] = "lemonchat-hkdf-salt-v1";
    const unsigned char info[] = "lemonchat-dh-keys-v1";
    mbedtls_mpi mix = {0};
    mbedtls_mpi maximum = {0};
    mbedtls_mpi shared = {0};
    // Mbed TLS reserves bits / 2 + overhead even for a shorter decimal result.
    char decimal[DESKTOP_DH_TEXT_CAPACITY] = {0};
    size_t written = 0;
    boole complete = 0;

    mbedtls_mpi_init(&mix);
    mbedtls_mpi_init(&maximum);
    mbedtls_mpi_init(&shared);
    complete = public_mix && clib__strlen(public_mix) < sizeof(decimal)
        && mbedtls_mpi_read_string(&mix, 10, public_mix) == 0
        && mbedtls_mpi_sub_int(&maximum, &crypto->modulus, 1) == 0
        && mbedtls_mpi_cmp_int(&mix, 1) > 0 && mbedtls_mpi_cmp_mpi(&mix, &maximum) < 0
        && mbedtls_mpi_exp_mod(&shared, &mix, &crypto->exponent, &crypto->modulus, NULL) == 0
        && mbedtls_mpi_write_string(&shared, 10, decimal, sizeof(decimal), &written) == 0
        && mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), salt, sizeof(salt) - 1,
            (const unsigned char*)decimal, written - 1, info, sizeof(info) - 1, crypto->session_keys, sizeof(crypto->session_keys)) == 0;
    crypto->session_ready = complete;
    mbedtls_mpi_free(&mix);
    mbedtls_mpi_free(&maximum);
    mbedtls_mpi_free(&shared);
    mbedtls_mpi_free(&crypto->exponent);
    mbedtls_platform_zeroize(decimal, sizeof(decimal));
    return complete;
}

/**
 * @brief decrypts the server's RSA identity challenge with private-operation blinding.
 * @param desktop_crypto_t* crypto -> initialized identity state
 * @param const char* encoded -> base64 RSA ciphertext
 * @param char* plaintext -> bounded challenge response output
 * @param uint64 plaintext_capacity -> destination capacity in bytes, including its terminator
 * @return boole -> 1 on valid padding and a nonempty, NUL-free response
 */
boole desktop_crypto__challenge(desktop_crypto_t* crypto, const char* encoded, char* plaintext, uint64 plaintext_capacity)
{
    unsigned char ciphertext[1024] = {0};
    size_t length = 0;
    size_t written = 0;
    boole complete = 0;

    if (!encoded || !plaintext || plaintext_capacity < 2)
    {
        return 0;
    }
    clib__null_memory(plaintext, plaintext_capacity);
    complete = mbedtls_base64_decode(ciphertext, sizeof(ciphertext), &length, (const unsigned char*)encoded, clib__strlen(encoded)) == 0
        && length == mbedtls_rsa_get_len(&crypto->rsa)
        && mbedtls_rsa_pkcs1_decrypt(&crypto->rsa, mbedtls_ctr_drbg_random, &crypto->random, &written,
            ciphertext, (unsigned char*)plaintext, plaintext_capacity - 1) == 0
        && written > 0 && !clib__memchr(plaintext, 0, written);
    if (!complete)
    {
        mbedtls_platform_zeroize(plaintext, plaintext_capacity);
    }
    mbedtls_platform_zeroize(ciphertext, sizeof(ciphertext));
    return complete;
}

/**
 * @brief applies one existing AES-256 CTR layer, resetting its counter for each message.
 * @param const unsigned char key[32] -> layer key
 * @param const unsigned char iv[16] -> message counter
 * @param unsigned char* bytes -> data transformed in place
 * @param uint64 length -> byte count
 * @return void
 */
static void _crypto_internal__layer(const unsigned char key[32], const unsigned char iv[16], unsigned char* bytes, uint64 length)
{
    struct AES_ctx aes = {0};

    AES_init_ctx_iv(&aes, key, iv);
    AES_CTR_xcrypt_buffer(&aes, bytes, length);
    mbedtls_platform_zeroize(&aes, sizeof(aes));
}

/**
 * @brief computes the protocol HMAC over the base64 IV followed by base64 ciphertext.
 * @param desktop_crypto_t* crypto -> established session
 * @param const char* iv -> encoded IV
 * @param const char* data -> encoded ciphertext
 * @param unsigned char output[32] -> destination checksum
 * @return boole -> 1 on success
 */
static boole _crypto_internal__mac(desktop_crypto_t* crypto, const char* iv, const char* data, unsigned char output[32])
{
    mbedtls_md_context_t context = {0};
    boole complete = 0;

    mbedtls_md_init(&context);
    complete = mbedtls_md_setup(&context, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1) == 0
        && mbedtls_md_hmac_starts(&context, crypto->session_keys + 32, 32) == 0
        && mbedtls_md_hmac_update(&context, (const unsigned char*)iv, clib__strlen(iv)) == 0
        && mbedtls_md_hmac_update(&context, (const unsigned char*)data, clib__strlen(data)) == 0
        && mbedtls_md_hmac_finish(&context, output) == 0;
    mbedtls_md_free(&context);
    return complete;
}

/**
 * @brief pads, encrypts and authenticates one outgoing metadata message.
 * @param desktop_crypto_t* crypto -> worker-owned state
 * @param const char* plaintext -> complete JSON message
 * @return char* -> owned envelope released with desktop_crypto__free_text, or NULL
 */
char* desktop_crypto__encrypt(desktop_crypto_t* crypto, const char* plaintext)
{
    unsigned char iv[16] = {0};
    unsigned char checksum[32] = {0};
    char encoded_iv[25] = {0};
    char encoded_tag[45] = {0};
    unsigned char* bytes = NULL;
    char* encoded = NULL;
    char* result = NULL;
    cJSON* root = NULL;
    uint64 length = clib__strlen(plaintext);
    uint64 padded = length < 1024 ? 1024 : length;
    uint64 capacity = (padded + 2) / 3 * 4 + 1;
    uint64 index = 0;
    boole complete = 0;

    if (capacity > DESKTOP_MESSAGE_LIMIT - 128)
    {
        return NULL;
    }
    bytes = calloc(1, padded);
    encoded = calloc(1, capacity);
    root = cJSON_CreateObject();
    if (!bytes || !encoded || !root || mbedtls_ctr_drbg_random(&crypto->random, iv, sizeof(iv)) != 0)
    {
        goto label_finished;
    }
    clib__memcpy(bytes, plaintext, length);
    if (crypto->session_ready)
    {
        _crypto_internal__layer(crypto->session_keys, iv, bytes, padded);
    }
    for (index = 0; index < crypto->server_key_count; index++)
    {
        _crypto_internal__layer(crypto->server_keys[index], iv, bytes, padded);
    }
    complete = _crypto_internal__base64(encoded_iv, sizeof(encoded_iv), iv, sizeof(iv))
        && _crypto_internal__base64(encoded, capacity, bytes, padded)
        && cJSON_AddStringToObject(root, "iv", encoded_iv) && cJSON_AddStringToObject(root, "data", encoded);
    if (complete && crypto->session_ready)
    {
        complete = _crypto_internal__mac(crypto, encoded_iv, encoded, checksum)
            && _crypto_internal__base64(encoded_tag, sizeof(encoded_tag), checksum, sizeof(checksum))
            && cJSON_AddStringToObject(root, "tag", encoded_tag);
    }
    if (complete)
    {
        result = cJSON_PrintUnformatted(root);
    }
label_finished:
    if (bytes)
    {
        mbedtls_platform_zeroize(bytes, padded);
    }
    free(bytes);
    if (encoded)
    {
        mbedtls_platform_zeroize(encoded, capacity);
    }
    free(encoded);
    desktop_crypto__free_json(root);
    return result;
}

/**
 * @brief verifies an incoming envelope before decrypting its metadata layers.
 * @param desktop_crypto_t* crypto -> worker-owned state
 * @param const char* envelope -> input frame
 * @param uint64 length -> exact frame byte count
 * @return char* -> owned plaintext released with desktop_crypto__free_text, or NULL
 */
char* desktop_crypto__decrypt(desktop_crypto_t* crypto, const char* envelope, uint64 length)
{
    cJSON* root = NULL;
    const cJSON* iv_value = NULL;
    const cJSON* data_value = NULL;
    const cJSON* tag_value = NULL;
    const char* end = NULL;
    unsigned char iv[16] = {0};
    unsigned char checksum[32] = {0};
    unsigned char received_tag[32] = {0};
    char* bytes = NULL;
    size_t iv_length = 0;
    size_t tag_length = 0;
    size_t data_length = 0;
    uint64 index = 0;
    uint64 capacity = 0;
    boole complete = 0;

    if (!envelope || !length || length > DESKTOP_MESSAGE_LIMIT)
    {
        return NULL;
    }
    root = cJSON_ParseWithLengthOpts(envelope, length, &end, 0);
    if (!root || end != envelope + length)
    {
        goto label_finished;
    }
    iv_value = cJSON_GetObjectItemCaseSensitive(root, "iv");
    data_value = cJSON_GetObjectItemCaseSensitive(root, "data");
    tag_value = cJSON_GetObjectItemCaseSensitive(root, "tag");
    if (!cJSON_IsString(iv_value) || !cJSON_IsString(data_value)
        || mbedtls_base64_decode(iv, sizeof(iv), &iv_length, (const unsigned char*)iv_value->valuestring, clib__strlen(iv_value->valuestring)) != 0
        || iv_length != sizeof(iv))
    {
        goto label_finished;
    }
    if (crypto->session_ready && (!cJSON_IsString(tag_value)
        || mbedtls_base64_decode(received_tag, sizeof(received_tag), &tag_length,
            (const unsigned char*)tag_value->valuestring, clib__strlen(tag_value->valuestring)) != 0
        || tag_length != sizeof(received_tag)
        || !_crypto_internal__mac(crypto, iv_value->valuestring, data_value->valuestring, checksum)
        || mbedtls_ct_memcmp(checksum, received_tag, sizeof(checksum)) != 0))
    {
        goto label_finished;
    }
    capacity = clib__strlen(data_value->valuestring) + 1;
    bytes = cJSON_malloc(capacity);
    if (!bytes)
    {
        goto label_finished;
    }
    clib__null_memory(bytes, capacity);
    if (mbedtls_base64_decode((unsigned char*)bytes, capacity - 1, &data_length,
        (const unsigned char*)data_value->valuestring, capacity - 1) != 0 || !data_length)
    {
        goto label_finished;
    }
    if (crypto->session_ready)
    {
        _crypto_internal__layer(crypto->session_keys, iv, (unsigned char*)bytes, data_length);
    }
    for (index = crypto->server_key_count; index > 0; index--)
    {
        _crypto_internal__layer(crypto->server_keys[index - 1], iv, (unsigned char*)bytes, data_length);
    }
    // Zero padding terminates the JSON; reject hidden nonzero data after it.
    index = clib__strlen(bytes);
    while (index < data_length && !bytes[index])
    {
        index++;
    }
    complete = index == data_length;
label_finished:
    desktop_crypto__free_json(root);
    if (!complete && bytes)
    {
        mbedtls_platform_zeroize(bytes, capacity);
        cJSON_free(bytes);
        bytes = NULL;
    }
    return bytes;
}

/**
 * @brief clears an owned JSON string before releasing it through cJSON's allocator.
 * @param char* text -> owned string, or NULL
 * @return void
 */
void desktop_crypto__free_text(char* text)
{
    if (text)
    {
        mbedtls_platform_zeroize(text, clib__strlen(text) + 1);
        cJSON_free(text);
    }
}

/**
 * @brief erases owned JSON strings and numeric key values before releasing their library allocations.
 * @param cJSON* json -> owned tree; borrowed reference children and constant keys are left untouched
 * @return void
 */
void desktop_crypto__free_json(cJSON* json)
{
    cJSON* next = NULL;
    while (json)
    {
        next = json->next;
        if (!(json->type & cJSON_IsReference))
        {
            desktop_crypto__free_json(json->child);
            desktop_crypto__free_text(json->valuestring);
        }
        if (!(json->type & cJSON_StringIsConst))
        {
            desktop_crypto__free_text(json->string);
        }
        mbedtls_platform_zeroize(json, sizeof(*json));
        cJSON_free(json);
        json = next;
    }
}

/**
 * @brief creates the four key/counter pairs used by existing channel and direct messages.
 * @param desktop_crypto_t* crypto -> worker-owned random source
 * @param desktop_content_keys_t* keys -> writable key set
 * @return boole -> 1 on success
 */
boole desktop_crypto__content_keys(desktop_crypto_t* crypto, desktop_content_keys_t* keys)
{
    clib__null_memory(keys, sizeof(*keys));
    return mbedtls_ctr_drbg_random(&crypto->random, (unsigned char*)keys, sizeof(*keys)) == 0;
}

/**
 * @brief serializes channel keys using the existing numeric-array wire format.
 * @param const desktop_content_keys_t* keys -> key set
 * @return char* -> owned JSON string, or NULL
 */
char* desktop_crypto__keys_json(const desktop_content_keys_t* keys)
{
    cJSON* root = cJSON_CreateArray();
    cJSON* entry = NULL;
    cJSON* key = NULL;
    cJSON* iv = NULL;
    char* result = NULL;
    uint64 index = 0;
    uint64 byte = 0;
    boole complete = root != NULL;

    for (index = 0; index < 4 && complete; index++)
    {
        entry = cJSON_CreateObject();
        if (!entry)
        {
            complete = 0;
            break;
        }
        cJSON_AddItemToArray(root, entry);
        key = cJSON_AddArrayToObject(entry, "key_bytes");
        iv = cJSON_AddArrayToObject(entry, "iv_bytes");
        complete = key && iv;
        for (byte = 0; byte < 32 && complete; byte++)
        {
            complete = cJSON_AddItemToArray(key, cJSON_CreateNumber(keys->keys[index][byte]));
        }
        for (byte = 0; byte < 16 && complete; byte++)
        {
            complete = cJSON_AddItemToArray(iv, cJSON_CreateNumber(keys->ivs[index][byte]));
        }
    }
    if (complete)
    {
        result = cJSON_PrintUnformatted(root);
    }
    desktop_crypto__free_json(root);
    return result;
}

/**
 * @brief validates every byte before accepting a received channel or direct-message key set.
 * @param const char* json -> four numeric-array key entries
 * @param desktop_content_keys_t* keys -> output, cleared on failure
 * @return boole -> 1 on success
 */
boole desktop_crypto__read_keys(const char* json, desktop_content_keys_t* keys)
{
    cJSON* root = NULL;
    const cJSON* entry = NULL;
    const cJSON* array = NULL;
    const cJSON* value = NULL;
    int64 index = 0;
    int64 part = 0;
    int64 byte = 0;
    int64 count = 0;
    boole complete = 0;
    unsigned char* output = NULL;

    clib__null_memory(keys, sizeof(*keys));
    root = json ? cJSON_Parse(json) : NULL;
    complete = cJSON_IsArray(root) && cJSON_GetArraySize(root) == 4;
    for (index = 0; index < 4 && complete; index++)
    {
        entry = cJSON_GetArrayItem(root, index);
        for (part = 0; part < 2 && complete; part++)
        {
            array = cJSON_GetObjectItemCaseSensitive(entry, part ? "iv_bytes" : "key_bytes");
            count = part ? 16 : 32;
            output = part ? keys->ivs[index] : keys->keys[index];
            complete = cJSON_IsArray(array) && cJSON_GetArraySize(array) == count;
            for (byte = 0; byte < count && complete; byte++)
            {
                value = cJSON_GetArrayItem(array, byte);
                complete = cJSON_IsNumber(value) && value->valuedouble >= 0 && value->valuedouble <= 255
                    && value->valuedouble == value->valueint;
                if (complete)
                {
                    output[byte] = (unsigned char)value->valueint;
                }
            }
        }
    }
    desktop_crypto__free_json(root);
    if (!complete)
    {
        mbedtls_platform_zeroize(keys, sizeof(*keys));
    }
    return complete;
}

/**
 * @brief validates a flat content envelope before allocating its JSON strings or decoding its IV.
 * @param const char* text -> complete, bounded JSON text
 * @param uint64 length -> text byte count excluding its terminator
 * @param cJSON** root -> owned parsed envelope; caller clears it on success and failure
 * @param unsigned char iv[16] -> decoded per-message IV
 * @param const char** encoded -> ciphertext borrowed from the parsed envelope on success
 * @return boole -> TRUE for one IV and one nonempty ciphertext string
 */
static boole _crypto_internal__content_envelope(const char* text, uint64 length, cJSON** root, unsigned char iv[16], const char** encoded)
{
    const cJSON* iv_value = NULL;
    const cJSON* data_value = NULL;
    uint64 index = 0;
    int64 depth = 0;
    int64 strings = 0;
    size_t written = 0;
    boole quoted = FALSE;
    boole escaped = FALSE;

    if (!text || !length || length > 40ULL * 1024 * 1024)
    {
        return FALSE;
    }
    for (index = 0; index < length; index++)
    {
        if (quoted)
        {
            if (escaped)
            {
                if (length - index >= 5 && clib__strncmp(text + index, "u0000", 5) == 0)
                {
                    return FALSE;
                }
                escaped = FALSE;
            }
            else if (text[index] == '\\')
            {
                escaped = TRUE;
            }
            else if (text[index] == '"')
            {
                quoted = FALSE;
            }
        }
        else if (text[index] == '"')
        {
            quoted = TRUE;
            strings++;
        }
        else if (text[index] == '{')
        {
            depth++;
        }
        else if (text[index] == '}')
        {
            depth--;
        }
        else if (text[index] == '[' || text[index] == ']')
        {
            return FALSE;
        }
        if (depth < 0 || depth > 1 || strings > 4)
        {
            return FALSE;
        }
    }
    if (quoted || depth || strings != 4)
    {
        return FALSE;
    }
    *root = cJSON_ParseWithLengthOpts(text, length + 1, NULL, TRUE);
    iv_value = cJSON_GetObjectItemCaseSensitive(*root, "iv");
    data_value = cJSON_GetObjectItemCaseSensitive(*root, "data");
    if (!cJSON_IsObject(*root) || cJSON_GetArraySize(*root) != 2 || !cJSON_IsString(iv_value)
        || !cJSON_IsString(data_value) || !data_value->valuestring[0] || clib__strlen(iv_value->valuestring) != 24
        || mbedtls_base64_decode(iv, 16, &written, (const unsigned char*)iv_value->valuestring, 24) != 0 || written != 16)
    {
        return FALSE;
    }
    *encoded = data_value->valuestring;
    return TRUE;
}

/**
 * @brief implements the existing four-layer content envelope, including legacy bare-base64 reads.
 * @param desktop_crypto_t* crypto -> random source for encryption; unused for decryption
 * @param const desktop_content_keys_t* keys -> channel or direct-message keys
 * @param const char* text -> plaintext or envelope
 * @param boole encrypt -> nonzero to encrypt
 * @return char* -> owned result, or NULL
 */
static char* _crypto_internal__content(desktop_crypto_t* crypto, const desktop_content_keys_t* keys, const char* text, boole encrypt)
{
    cJSON* root = NULL;
    const char* encoded_data = text;
    unsigned char iv[16] = {0};
    unsigned char counter[16] = {0};
    char iv_text[25] = {0};
    char* bytes = NULL;
    char* encoded = NULL;
    char* result = NULL;
    uint64 length = clib__strlen(text);
    uint64 size = 0;
    size_t written = 0;
    uint64 capacity = 0;
    uint64 bytes_capacity = 0;
    uint64 plaintext_length = 0;
    uint64 index = 0;
    uint64 byte = 0;

    if (length > DESKTOP_MESSAGE_LIMIT / 2)
    {
        return NULL;
    }
    size = encrypt ? (length < 1024 ? 1024 : length) : length;
    bytes_capacity = size + 1;
    bytes = cJSON_malloc(bytes_capacity);
    if (!bytes)
    {
        return NULL;
    }
    clib__null_memory(bytes, size + 1);
    if (encrypt)
    {
        if (mbedtls_ctr_drbg_random(&crypto->random, iv, sizeof(iv)) != 0)
        {
            goto label_finished;
        }
        clib__memcpy(bytes, text, length);
    }
    else
    {
        if (text[0] == '{')
        {
            if (!_crypto_internal__content_envelope(text, length, &root, iv, &encoded_data))
            {
                goto label_finished;
            }
        }
        if (mbedtls_base64_decode((unsigned char*)bytes, size, &written, (const unsigned char*)encoded_data,
            clib__strlen(encoded_data)) != 0 || !written)
        {
            goto label_finished;
        }
        size = written;
    }
    for (index = 0; index < 4; index++)
    {
        for (byte = 0; byte < sizeof(counter); byte++)
        {
            counter[byte] = keys->ivs[index][byte] ^ iv[byte];
        }
        _crypto_internal__layer(keys->keys[index], counter, (unsigned char*)bytes, size);
    }
    if (!encrypt)
    {
        // Wrong keys generally leave nonzero bytes after the first terminator.
        index = clib__strlen(bytes);
        plaintext_length = index;
        while (index < size && !bytes[index])
        {
            index++;
        }
        if (index == size)
        {
            result = cJSON_malloc(plaintext_length + 1);
            if (result)
            {
                clib__null_memory(result, plaintext_length + 1);
                clib__memcpy(result, bytes, plaintext_length);
            }
        }
        goto label_finished;
    }
    root = cJSON_CreateObject();
    capacity = (size + 2) / 3 * 4 + 1;
    encoded = calloc(1, capacity);
    if (root && encoded && _crypto_internal__base64(iv_text, sizeof(iv_text), iv, sizeof(iv))
        && _crypto_internal__base64(encoded, capacity, (unsigned char*)bytes, size)
        && cJSON_AddStringToObject(root, "iv", iv_text) && cJSON_AddStringToObject(root, "data", encoded))
    {
        result = cJSON_PrintUnformatted(root);
    }
label_finished:
    desktop_crypto__free_json(root);
    if (bytes)
    {
        mbedtls_platform_zeroize(bytes, bytes_capacity);
        cJSON_free(bytes);
    }
    if (encoded)
    {
        mbedtls_platform_zeroize(encoded, capacity);
    }
    free(encoded);
    mbedtls_platform_zeroize(counter, sizeof(counter));
    mbedtls_platform_zeroize(iv, sizeof(iv));
    mbedtls_platform_zeroize(iv_text, sizeof(iv_text));
    return result;
}

/**
 * @brief encrypts a channel/direct-message body under a fresh per-message IV.
 * @param desktop_crypto_t* crypto -> worker-owned random source
 * @param const desktop_content_keys_t* keys -> content key set
 * @param const char* text -> plaintext
 * @return char* -> owned content envelope, or NULL
 */
char* desktop_crypto__content_encrypt(desktop_crypto_t* crypto, const desktop_content_keys_t* keys, const char* text)
{
    return _crypto_internal__content(crypto, keys, text, 1);
}

/**
 * @brief decrypts a current or legacy channel/direct-message body.
 * @param const desktop_content_keys_t* keys -> content key set
 * @param const char* text -> encrypted content
 * @return char* -> owned plaintext, or NULL
 */
char* desktop_crypto__content_decrypt(const desktop_content_keys_t* keys, const char* text)
{
    return _crypto_internal__content(NULL, keys, text, 0);
}

/**
 * @brief reproduces the browser attachment wire format with a separate bounded payload limit.
 * @param const desktop_content_keys_t* keys -> captured channel keys or fresh direct attachment keys
 * @param const char* text -> terminated plaintext, current IV envelope or legacy base64 ciphertext
 * @param boole encrypt -> TRUE to encrypt, FALSE to decrypt
 * @param uint64 padding -> minimum plaintext bytes, at most 4096
 * @return char* -> owned terminated result; free with desktop_crypto__free_text
 */
static char* _crypto_internal__attachment(const desktop_content_keys_t* keys, const char* text, boole encrypt, uint64 padding)
{
    const uint64 limit = 40ULL * 1024 * 1024;
    cJSON* root = NULL;
    const char* encoded_data = text;
    unsigned char iv[16] = {0};
    unsigned char counter[16] = {0};
    uint64 length = text ? clib__strlen(text) : 0;
    uint64 size = 0;
    uint64 capacity = 0;
    uint64 bytes_capacity = 0;
    uint64 plaintext_length = 0;
    uint64 index = 0;
    uint64 byte = 0;
    size_t written = 0;
    unsigned char* bytes = NULL;
    char* result = NULL;
    if (!keys || !length || length > limit || padding > 4096)
    {
        return NULL;
    }
    if (!encrypt && text[0] == '{')
    {
        if (!_crypto_internal__content_envelope(text, length, &root, iv, &encoded_data))
        {
            goto label_finished;
        }
        length = clib__strlen(encoded_data);
    }
    size = encrypt ? (length < padding ? padding : length) : length / 4 * 3 + 3;
    bytes_capacity = size + 1;
    bytes = cJSON_malloc(bytes_capacity);
    if (!bytes)
    {
        goto label_finished;
    }
    clib__null_memory(bytes, size + 1);
    if (encrypt)
    {
        clib__memcpy(bytes, text, length);
    }
    else if (mbedtls_base64_decode(bytes, size, &written, (const unsigned char*)encoded_data, length) != 0 || !written)
    {
        goto label_finished;
    }
    else
    {
        size = written;
    }
    for (index = 0; index < 4; index++)
    {
        for (byte = 0; byte < sizeof(counter); byte++)
        {
            counter[byte] = keys->ivs[index][byte] ^ iv[byte];
        }
        _crypto_internal__layer(keys->keys[index], counter, bytes, size);
    }
    if (encrypt)
    {
        capacity = (size + 2) / 3 * 4 + 1;
        if (capacity > limit + 1)
        {
            goto label_finished;
        }
        result = cJSON_malloc(capacity);
        if (result)
        {
            clib__null_memory(result, capacity);
            if (!_crypto_internal__base64(result, capacity, bytes, size))
            {
                mbedtls_platform_zeroize(result, capacity);
                cJSON_free(result);
                result = NULL;
            }
        }
    }
    else
    {
        index = clib__strlen((char*)bytes);
        plaintext_length = index;
        while (index < size && !bytes[index])
        {
            index++;
        }
        if (index == size)
        {
            result = cJSON_malloc(plaintext_length + 1);
            if (result)
            {
                clib__null_memory(result, plaintext_length + 1);
                clib__memcpy(result, bytes, plaintext_length);
            }
        }
    }
label_finished:
    desktop_crypto__free_json(root);
    if (bytes)
    {
        mbedtls_platform_zeroize(bytes, bytes_capacity);
        cJSON_free(bytes);
    }
    mbedtls_platform_zeroize(counter, sizeof(counter));
    mbedtls_platform_zeroize(iv, sizeof(iv));
    return result;
}

/**
 * @brief encrypts the existing bare-base64 attachment format understood by browser clients.
 * @param const desktop_content_keys_t* keys -> worker-owned content keys
 * @param const char* text -> attachment plaintext
 * @param uint64 padding -> protocol padding floor: 1024 for headers, 4096 for bodies
 * @return char* -> owned ciphertext or NULL
 */
char* desktop_crypto__attachment_encrypt(const desktop_content_keys_t* keys, const char* text, uint64 padding)
{
    return _crypto_internal__attachment(keys, text, TRUE, padding);
}

/**
 * @brief decrypts a browser-compatible attachment without raising the chat-message size limit.
 * @param const desktop_content_keys_t* keys -> captured content keys
 * @param const char* text -> bounded current IV envelope or legacy base64 ciphertext
 * @return char* -> owned plaintext or NULL
 */
char* desktop_crypto__attachment_decrypt(const desktop_content_keys_t* keys, const char* text)
{
    return _crypto_internal__attachment(keys, text, FALSE, 0);
}

/**
 * @brief reproduces the browser's RSA plus AES-CBC envelope for numeric key-array JSON.
 * @param desktop_crypto_t* crypto -> worker-owned random source
 * @param const char* public_key -> receiver's base64 modulus, exponent 3
 * @param const char* text -> ASCII key-array JSON
 * @return char* -> owned hybrid envelope, or NULL
 */
char* desktop_crypto__hybrid_encrypt(desktop_crypto_t* crypto, const char* public_key, const char* text)
{
    mbedtls_rsa_context rsa = {0};
    mbedtls_aes_context aes = {0};
    mbedtls_mpi modulus = {0};
    mbedtls_mpi exponent = {0};
    unsigned char raw_modulus[1024] = {0};
    unsigned char key[32] = {0};
    unsigned char utf8_key[64] = {0};
    unsigned char iv[16] = {0};
    unsigned char encrypted_key[1024] = {0};
    char key_text[1369] = {0};
    unsigned char* body = NULL;
    char* body_text = NULL;
    char* result = NULL;
    uint64 length = clib__strlen(text);
    uint64 padded = (length + 15) / 16 * 16;
    size_t modulus_length = 0;
    uint64 key_length = 0;
    uint64 body_capacity = (padded + 18) / 3 * 4 + 1;
    uint64 capacity = 0;
    uint64 index = 0;

    mbedtls_rsa_init(&rsa);
    mbedtls_aes_init(&aes);
    mbedtls_mpi_init(&modulus);
    mbedtls_mpi_init(&exponent);
    if (!length || length > 16384 || mbedtls_base64_decode(raw_modulus, sizeof(raw_modulus), &modulus_length,
        (const unsigned char*)public_key, clib__strlen(public_key)) != 0 || modulus_length < 256
        || mbedtls_mpi_read_binary(&modulus, raw_modulus, modulus_length) != 0 || mbedtls_mpi_lset(&exponent, 3) != 0
        || mbedtls_rsa_import(&rsa, &modulus, NULL, NULL, NULL, &exponent) != 0 || mbedtls_rsa_complete(&rsa) != 0
        || mbedtls_rsa_check_pubkey(&rsa) != 0 || mbedtls_rsa_set_padding(&rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE) != 0
        || mbedtls_ctr_drbg_random(&crypto->random, key, sizeof(key)) != 0
        || mbedtls_ctr_drbg_random(&crypto->random, iv, sizeof(iv)) != 0)
    {
        goto label_finished;
    }
    // cryptico encodes each binary key byte as a Unicode character before RSA padding.
    for (index = 0; index < sizeof(key); index++)
    {
        if (key[index] >= 128)
        {
            utf8_key[key_length++] = 0xc0 | (key[index] >> 6);
            utf8_key[key_length++] = 0x80 | (key[index] & 63);
        }
        else
        {
            utf8_key[key_length++] = key[index];
        }
    }
    body = calloc(1, padded + 16);
    body_text = calloc(1, body_capacity);
    if (!body || !body_text || mbedtls_rsa_pkcs1_encrypt(&rsa, mbedtls_ctr_drbg_random, &crypto->random,
        key_length, utf8_key, encrypted_key) != 0)
    {
        goto label_finished;
    }
    clib__memcpy(body, iv, sizeof(iv));
    clib__memcpy(body + 16, text, length);
    if (mbedtls_aes_setkey_enc(&aes, key, 256) != 0
        || mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padded, iv, body + 16, body + 16) != 0
        || !_crypto_internal__base64(key_text, sizeof(key_text), encrypted_key, modulus_length)
        || !_crypto_internal__base64(body_text, body_capacity, body, padded + 16))
    {
        goto label_finished;
    }
    capacity = clib__strlen(key_text) + clib__strlen(body_text) + 2;
    result = cJSON_malloc(capacity);
    if (result)
    {
        clib__null_memory(result, capacity);
        clib__string_format(result, capacity, "%s?%s", key_text, body_text);
    }
label_finished:
    mbedtls_rsa_free(&rsa);
    mbedtls_aes_free(&aes);
    mbedtls_mpi_free(&modulus);
    mbedtls_mpi_free(&exponent);
    mbedtls_platform_zeroize(key, sizeof(key));
    mbedtls_platform_zeroize(utf8_key, sizeof(utf8_key));
    if (body)
    {
        mbedtls_platform_zeroize(body, padded + 16);
    }
    free(body);
    if (body_text)
    {
        mbedtls_platform_zeroize(body_text, body_capacity);
    }
    free(body_text);
    mbedtls_platform_zeroize(raw_modulus, sizeof(raw_modulus));
    mbedtls_platform_zeroize(iv, sizeof(iv));
    mbedtls_platform_zeroize(encrypted_key, sizeof(encrypted_key));
    mbedtls_platform_zeroize(key_text, sizeof(key_text));
    return result;
}

/**
 * @brief unwraps the browser's hybrid envelope without treating its binary AES key as a C string.
 * @param desktop_crypto_t* crypto -> initialized local RSA identity
 * @param const char* text -> encrypted key-array JSON
 * @return char* -> owned ASCII plaintext, or NULL
 */
char* desktop_crypto__hybrid_decrypt(desktop_crypto_t* crypto, const char* text)
{
    mbedtls_aes_context aes = {0};
    const char* separator = clib__strchr(text, '?');
    unsigned char ciphertext[1024] = {0};
    unsigned char utf8_key[1024] = {0};
    unsigned char key[32] = {0};
    unsigned char iv[16] = {0};
    unsigned char* body = NULL;
    char* result = NULL;
    size_t length = 0;
    uint64 rsa_length = mbedtls_rsa_get_len(&crypto->rsa);
    size_t decoded_length = 0;
    uint64 key_length = 0;
    uint64 capacity = 0;
    size_t body_length = 0;
    uint64 index = 0;
    int64 value = 0;

    mbedtls_aes_init(&aes);
    if (!separator || clib__strlen(text) > 32768 || rsa_length > sizeof(ciphertext)
        || mbedtls_base64_decode(ciphertext, sizeof(ciphertext), &length, (const unsigned char*)text,
            (uint64)(separator - text)) != 0 || !length || length > rsa_length)
    {
        goto label_finished;
    }
    // The browser's bigint encoder omits leading zero ciphertext bytes.
    if (length < rsa_length)
    {
        clib__memmove(ciphertext + rsa_length - length, ciphertext, length);
        clib__null_memory(ciphertext, rsa_length - length);
    }
    if (mbedtls_rsa_pkcs1_decrypt(&crypto->rsa, mbedtls_ctr_drbg_random, &crypto->random,
        &decoded_length, ciphertext, utf8_key, sizeof(utf8_key)) != 0)
    {
        goto label_finished;
    }
    for (index = 0; index < decoded_length; index++)
    {
        value = utf8_key[index];
        if (value >= 128)
        {
            if ((value != 0xc2 && value != 0xc3) || index + 1 >= decoded_length || (utf8_key[index + 1] & 0xc0) != 0x80)
            {
                goto label_finished;
            }
            value = ((value & 3) << 6) | (utf8_key[++index] & 63);
        }
        if (key_length == sizeof(key))
        {
            goto label_finished;
        }
        key[key_length++] = (unsigned char)value;
    }
    if (key_length != sizeof(key))
    {
        goto label_finished;
    }
    capacity = clib__strlen(separator + 1) + 1;
    body = calloc(1, capacity);
    if (!body || mbedtls_base64_decode(body, capacity - 1, &body_length, (const unsigned char*)(separator + 1), capacity - 1) != 0
        || body_length < 32 || body_length % 16)
    {
        goto label_finished;
    }
    clib__memcpy(iv, body, sizeof(iv));
    if (mbedtls_aes_setkey_dec(&aes, key, 256) != 0
        || mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, body_length - 16, iv, body + 16, body + 16) != 0)
    {
        goto label_finished;
    }
    // Reject hidden nonzero bytes after a terminator before returning an exact-size string.
    length = 0;
    while (length < body_length - 16 && body[16 + length])
    {
        length++;
    }
    for (index = length; index < body_length - 16; index++)
    {
        if (body[16 + index])
        {
            goto label_finished;
        }
    }
    result = cJSON_malloc(length + 1);
    if (result)
    {
        clib__null_memory(result, length + 1);
        clib__memcpy(result, body + 16, length);
    }
label_finished:
    mbedtls_aes_free(&aes);
    mbedtls_platform_zeroize(key, sizeof(key));
    mbedtls_platform_zeroize(utf8_key, sizeof(utf8_key));
    mbedtls_platform_zeroize(ciphertext, sizeof(ciphertext));
    mbedtls_platform_zeroize(iv, sizeof(iv));
    if (body)
    {
        mbedtls_platform_zeroize(body, capacity);
    }
    free(body);
    return result;
}
