#include "clib.h"
// Existing Lemon identity and metadata protocol, using the vendored crypto primitives.
#include <stdlib.h>
#include <stdatomic.h>
#include "crypto/crypto_session.h"
#include "network/protocol.h"
#include "crypto/dh_parameters.h"
#include "platform/system.h"
#include "cJSON.h"
#include "../../third-party/kokke-tiny-aes-c/aes.h"
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
    ubyte session_keys[64];
    ubyte server_keys[DESKTOP_SERVER_KEY_LIMIT][32];
    uint64 server_key_count;
    boole session_ready;
    int server_types[DESKTOP_SERVER_KEY_LIMIT];
    lemon_session_keys negotiated;
    lemon_session_policy policy;
    ubyte transcript[32];
    mbedtls_mpi exponents[LEMON_MAX_DH_EXCHANGES];
    char mixes[LEMON_MAX_DH_EXCHANGES][DESKTOP_DH_TEXT_CAPACITY];
    char first_mix[DESKTOP_DH_TEXT_CAPACITY];
};

// The browser calculation has static scratch storage. Serialize native callers.
static atomic_flag g_identity_lock = ATOMIC_FLAG_INIT;

/**
 * @brief copies only the keys used by the existing media protocol into worker-owned storage.
 * @param desktop_crypto_t* crypto -> authenticated session crypto
 * @param desktop_media_keys_t* keys -> cleared on failure; never publish to the GUI or overlay
 * @return boole -> 1 when the DH session is established
 */
boole crypto_session__media_keys(desktop_crypto_t *crypto, desktop_media_keys_t *keys)
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
    clib__memcpy(keys->server_types, crypto->server_types, sizeof(keys->server_types));
    keys->negotiated = crypto->negotiated;
    return 1;
}

/**
 * @brief draws bounded random bytes from this worker's cryptographic generator.
 * @param desktop_crypto_t* crypto -> worker-owned crypto context
 * @param ubyte* bytes -> destination with size bytes
 * @param uint64 size -> requested byte count, limited to 256
 * @return boole -> 1 when filled
 */
boole crypto_session__random(desktop_crypto_t *crypto, ubyte *bytes, uint64 size)
{
    return crypto && bytes && size <= 256 && mbedtls_ctr_drbg_random(&crypto->random, bytes, size) == 0;
}

// declarations
ubyte *rsa_keygen__get_seed_buffer(void);
ubyte *rsa_keygen__get_result(int index);
int rsa_keygen__generate(int bits, int seed_length);
void rsa_keygen__clear_sensitive(void);
static boole _crypto_session_internal__base64(char *output, uint64 output_capacity, const ubyte *bytes, uint64 length);
static boole _crypto_session_internal__mac(desktop_crypto_t *crypto, const char *iv, const char *data, ubyte output[32], boole receiving);
static void _crypto_session_internal__layer(const ubyte key[32], const ubyte iv[16], ubyte *bytes, uint64 length);
static char *_crypto_session_internal__content(desktop_crypto_t *crypto, const desktop_content_keys_t *keys, const char *text, boole encrypt);
static boole _crypto_session_internal__content_envelope(const char *text, uint64 length, cJSON **root, ubyte iv[16], const char **encoded);
static boole _crypto_session_internal__flat_envelope(const char *text, uint64 length, uint64 maximum_members);
static char *_crypto_session_internal__attachment(desktop_crypto_t *crypto, const desktop_content_keys_t *keys, const char *text, boole encrypt, boole picture, uint64 padding, boole require_authentication);

/**
 * @brief encodes bytes into a capacity-carrying string buffer.
 * @param char* output -> writable destination including terminator space
 * @param uint64 output_capacity -> destination capacity in bytes, including its terminator
 * @param const ubyte* bytes -> source bytes
 * @param uint64 length -> source byte count
 * @return boole -> 1 on success
 */
static boole _crypto_session_internal__base64(char *output, uint64 output_capacity, const ubyte *bytes, uint64 length)
{
    size_t written = 0;

    if (!output || !output_capacity)
    {
        return 0;
    }
    output[0] = 0;
    return mbedtls_base64_encode((ubyte *)output, output_capacity, &written, bytes, length) == 0;
}

/**
 * @brief initializes per-session entropy and crypto contexts.
 * @return desktop_crypto_t* -> owned state, or NULL on allocation/entropy failure
 */
desktop_crypto_t *crypto_session__create(void)
{
    desktop_crypto_t *crypto = calloc(1, sizeof(*crypto));
    const ubyte personalization[] = "lemon-chat-desktop";

    if (!crypto)
    {
        return NULL;
    }
    mbedtls_entropy_init(&crypto->entropy);
    mbedtls_ctr_drbg_init(&crypto->random);
    mbedtls_rsa_init(&crypto->rsa);
    mbedtls_mpi_init(&crypto->modulus);
    mbedtls_mpi_init(&crypto->exponent);
    if (mbedtls_ctr_drbg_seed(&crypto->random, mbedtls_entropy_func, &crypto->entropy, personalization, sizeof(personalization) - 1) != 0)
    {
        crypto_session__destroy(crypto);
        return NULL;
    }
    return crypto;
}

/**
 * @brief gives an attachment worker its own RSA state and random generator without recalculating the identity.
 * @param const desktop_crypto_t* source -> session-owned identity, read only on the session worker
 * @return desktop_crypto_t* -> independent owned context, or NULL
 */
desktop_crypto_t *crypto_session__copy_identity(const desktop_crypto_t *source)
{
    desktop_crypto_t *copy = source ? crypto_session__create() : NULL;
    if (copy && mbedtls_rsa_copy(&copy->rsa, &source->rsa) != 0)
    {
        crypto_session__destroy(copy);
        copy = NULL;
    }
    return copy;
}

/**
 * @brief clears and releases a session's owned cryptographic material.
 * @param desktop_crypto_t* crypto -> owned state, or NULL
 * @return void
 */
void crypto_session__destroy(desktop_crypto_t *crypto)
{
    if (crypto)
    {
        mbedtls_rsa_free(&crypto->rsa);
        mbedtls_mpi_free(&crypto->modulus);
        mbedtls_mpi_free(&crypto->exponent);
        for (unsigned int index = 0; index < LEMON_MAX_DH_EXCHANGES; index++)
        {
            mbedtls_mpi_free(&crypto->exponents[index]);
        }
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
boole crypto_session__new_identity(char *identity, uint64 identity_capacity)
{
    desktop_crypto_t *crypto = NULL;
    ubyte bytes[100] = { 0 };
    const char hex[] = "0123456789abcdef";
    uint64 index = 0;
    boole complete = 0;

    if (!identity || identity_capacity < sizeof(bytes) * 2 + 1)
    {
        return 0;
    }
    identity[0] = 0;
    crypto = crypto_session__create();
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
    crypto_session__destroy(crypto);
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
boole crypto_session__identity(desktop_crypto_t *crypto, const char *identity, int64 bits, char *public_key, uint64 public_key_capacity)
{
    ubyte digest[32] = { 0 };
    ubyte modulus[1024] = { 0 };
    const char hex[] = "0123456789abcdef";
    ubyte *seed = NULL;
    mbedtls_mpi values[5] = { 0 };
    uint64 index = 0;
    uint64 size = 0;
    boole complete = 0;

    if (!crypto || !identity || !identity[0] || bits < 2048 || bits > 8192 || bits % 256)
    {
        return 0;
    }
    if (mbedtls_sha256((const ubyte *)identity, clib__strlen(identity), digest, 0) != 0)
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
        complete = mbedtls_mpi_read_string(&values[index], 16, (const char *)rsa_keygen__get_result((int)index)) == 0;
    }
    rsa_keygen__clear_sensitive();
    atomic_flag_clear_explicit(&g_identity_lock, memory_order_release);
    // Generator order is N, E, D, P, Q; the import API takes N, P, Q, D, E.
    complete = complete && mbedtls_rsa_import(&crypto->rsa, &values[0], &values[3], &values[4], &values[2], &values[1]) == 0 && mbedtls_rsa_complete(&crypto->rsa) == 0 && mbedtls_rsa_check_privkey(&crypto->rsa) == 0 && mbedtls_rsa_set_padding(&crypto->rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE) == 0;
    size = mbedtls_mpi_size(&values[0]);
    complete = complete && size <= sizeof(modulus) && mbedtls_mpi_write_binary(&values[0], modulus, size) == 0 && _crypto_session_internal__base64(public_key, public_key_capacity, modulus, size);
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
boole crypto_session__server_keys(desktop_crypto_t *crypto, const char *keys)
{
    const char *cursor = keys;
    const char *start = NULL;
    uint64 length = 0;
    cJSON *entry;
    cJSON *password;
    cJSON *type;
    int cipher;
    const char *text;
    uint64 text_length;
    boole valid;

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
        entry = length && start[0] == '{' ? cJSON_ParseWithLength(start, length) : NULL;
        password = cJSON_GetObjectItemCaseSensitive(entry, "password");
        type = cJSON_GetObjectItemCaseSensitive(entry, "type");
        cipher = LEMON_AES256;
        text = start;
        text_length = length;
        if (cJSON_IsObject(entry) && (password != NULL || type != NULL))
        {
            cipher = ciphers__type(cJSON_GetStringValue(type));
            text = cJSON_GetStringValue(password);
            text_length = text == NULL ? 0 : clib__strlen(text);
        }
        valid = length && crypto->server_key_count < DESKTOP_SERVER_KEY_LIMIT && cipher && text && mbedtls_sha256((const ubyte *)text, text_length, crypto->server_keys[crypto->server_key_count], 0) == 0;
        crypto_session__free_json(entry);
        if (!valid)
        {
            return 0;
        }
        crypto->server_types[crypto->server_key_count] = cipher;
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
boole crypto_session__begin_dh(desktop_crypto_t *crypto, char *public_mix, uint64 public_mix_capacity)
{
    ubyte bytes[DH_EXPONENT_BITS / 8] = { 0 };
    mbedtls_mpi base = { 0 };
    mbedtls_mpi mix = { 0 };
    size_t written = 0;
    boole complete = 0;

    mbedtls_mpi_init(&base);
    mbedtls_mpi_init(&mix);
    crypto->session_ready = 0;
    complete = mbedtls_ctr_drbg_random(&crypto->random, bytes, sizeof(bytes)) == 0;
    bytes[0] |= 128;
    complete = complete && mbedtls_mpi_read_string(&crypto->modulus, 10, DH_MODULUS_STR) == 0 && mbedtls_mpi_read_binary(&crypto->exponent, bytes, sizeof(bytes)) == 0 && mbedtls_mpi_lset(&base, 2) == 0 && mbedtls_mpi_exp_mod(&mix, &base, &crypto->exponent, &crypto->modulus, NULL) == 0 && mbedtls_mpi_write_string(&mix, 10, public_mix, public_mix_capacity, &written) == 0;
    mbedtls_mpi_free(&base);
    mbedtls_mpi_free(&mix);
    mbedtls_platform_zeroize(bytes, sizeof(bytes));
    if (complete)
    {
        clib__string_copy(crypto->first_mix, sizeof(crypto->first_mix), public_mix);
    }
    return complete;
}

/**
 * @brief validates the server mix and derives the existing AES/HMAC session keys.
 * @param desktop_crypto_t* crypto -> state containing a fresh private exponent
 * @param const char* public_mix -> decimal server public mix
 * @return boole -> 1 on success; invalid public mixes never activate the session layer
 */
boole crypto_session__finish_dh(desktop_crypto_t *crypto, const char *public_mix)
{
    const ubyte salt[] = "lemonchat-hkdf-salt-v1";
    const ubyte info[] = "lemonchat-dh-keys-v1";
    mbedtls_mpi mix = { 0 };
    mbedtls_mpi maximum = { 0 };
    mbedtls_mpi shared = { 0 };
    // Mbed TLS reserves bits / 2 + overhead even for a shorter decimal result.
    char decimal[DESKTOP_DH_TEXT_CAPACITY] = { 0 };
    size_t written = 0;
    boole complete = 0;

    mbedtls_mpi_init(&mix);
    mbedtls_mpi_init(&maximum);
    mbedtls_mpi_init(&shared);
    complete = public_mix && clib__strlen(public_mix) < sizeof(decimal) && mbedtls_mpi_read_string(&mix, 10, public_mix) == 0 && mbedtls_mpi_sub_int(&maximum, &crypto->modulus, 1) == 0 && mbedtls_mpi_cmp_int(&mix, 1) > 0 && mbedtls_mpi_cmp_mpi(&mix, &maximum) < 0 && mbedtls_mpi_exp_mod(&shared, &mix, &crypto->exponent, &crypto->modulus, NULL) == 0 && mbedtls_mpi_write_string(&shared, 10, decimal, sizeof(decimal), &written) == 0 && mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), salt, sizeof(salt) - 1, (const ubyte *)decimal, written - 1, info, sizeof(info) - 1, crypto->session_keys, sizeof(crypto->session_keys)) == 0;
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
boole crypto_session__challenge(desktop_crypto_t *crypto, const char *encoded, char *plaintext, uint64 plaintext_capacity)
{
    ubyte ciphertext[1024] = { 0 };
    size_t length = 0;
    size_t written = 0;
    boole complete = 0;

    if (!encoded || !plaintext || plaintext_capacity < 2)
    {
        return 0;
    }
    clib__null_memory(plaintext, plaintext_capacity);
    complete = mbedtls_base64_decode(ciphertext, sizeof(ciphertext), &length, (const ubyte *)encoded, clib__strlen(encoded)) == 0 && length == mbedtls_rsa_get_len(&crypto->rsa) && mbedtls_rsa_pkcs1_decrypt(&crypto->rsa, mbedtls_ctr_drbg_random, &crypto->random, &written, ciphertext, (ubyte *)plaintext, plaintext_capacity - 1) == 0 && written > 0 && !clib__memchr(plaintext, 0, written);
    if (!complete)
    {
        mbedtls_platform_zeroize(plaintext, plaintext_capacity);
    }
    mbedtls_platform_zeroize(ciphertext, sizeof(ciphertext));
    return complete;
}

/**
 * @brief applies one existing AES-256 CTR layer, resetting its counter for each message.
 * @param const ubyte key[32] -> layer key
 * @param const ubyte iv[16] -> message counter
 * @param ubyte* bytes -> data transformed in place
 * @param uint64 length -> byte count
 * @return void
 */
static void _crypto_session_internal__layer(const ubyte key[32], const ubyte iv[16], ubyte *bytes, uint64 length)
{
    struct AES_ctx aes = { 0 };

    AES_init_ctx_iv(&aes, key, iv);
    AES_CTR_xcrypt_buffer(&aes, bytes, length);
    mbedtls_platform_zeroize(&aes, sizeof(aes));
}

/**
 * @brief computes the protocol HMAC over the base64 IV followed by base64 ciphertext.
 * @param desktop_crypto_t* crypto -> established session
 * @param const char* iv -> encoded IV
 * @param const char* data -> encoded ciphertext
 * @param ubyte output[32] -> destination checksum
 * @return boole -> 1 on success
 */
static boole _crypto_session_internal__mac(desktop_crypto_t *crypto, const char *iv, const char *data, ubyte output[32], boole receiving)
{
    mbedtls_md_context_t context = { 0 };
    boole complete = 0;

    mbedtls_md_init(&context);
    complete = mbedtls_md_setup(&context, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1) == 0 && mbedtls_md_hmac_starts(&context, crypto->negotiated.count ? (receiving ? crypto->negotiated.server_mac : crypto->negotiated.client_mac) : crypto->session_keys + 32, 32) == 0 && mbedtls_md_hmac_update(&context, (const ubyte *)iv, clib__strlen(iv)) == 0 && mbedtls_md_hmac_update(&context, (const ubyte *)data, clib__strlen(data)) == 0 && mbedtls_md_hmac_finish(&context, output) == 0;
    mbedtls_md_free(&context);
    return complete;
}

/**
 * @brief pads, encrypts and authenticates one outgoing metadata message.
 * @param desktop_crypto_t* crypto -> worker-owned state
 * @param const char* plaintext -> complete JSON message
 * @return char* -> owned envelope released with crypto_session__free_text, or NULL
 */
char *crypto_session__encrypt(desktop_crypto_t *crypto, const char *plaintext)
{
    ubyte iv[16] = { 0 };
    ubyte checksum[32] = { 0 };
    char encoded_iv[25] = { 0 };
    char encoded_tag[45] = { 0 };
    ubyte *bytes = NULL;
    char *encoded = NULL;
    char *result = NULL;
    cJSON *root = NULL;
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
        if (crypto->negotiated.count)
        {
            if (!keys_session__crypt(&crypto->negotiated, 0, 1, iv, bytes, padded))
            {
                goto label_finished;
            }
        }
        else
        {
            _crypto_session_internal__layer(crypto->session_keys, iv, bytes, padded);
        }
    }
    for (index = 0; index < crypto->server_key_count; index++)
    {
        if (!ciphers__crypt(crypto->server_types[index], crypto->server_keys[index], iv, bytes, padded))
        {
            goto label_finished;
        }
    }
    complete = _crypto_session_internal__base64(encoded_iv, sizeof(encoded_iv), iv, sizeof(iv)) && _crypto_session_internal__base64(encoded, capacity, bytes, padded) && cJSON_AddStringToObject(root, "iv", encoded_iv) && cJSON_AddStringToObject(root, "data", encoded);
    if (complete && crypto->session_ready)
    {
        complete = _crypto_session_internal__mac(crypto, encoded_iv, encoded, checksum, FALSE) && _crypto_session_internal__base64(encoded_tag, sizeof(encoded_tag), checksum, sizeof(checksum)) && cJSON_AddStringToObject(root, "tag", encoded_tag);
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
    crypto_session__free_json(root);
    return result;
}

/**
 * @brief verifies an incoming envelope before decrypting its metadata layers.
 * @param desktop_crypto_t* crypto -> worker-owned state
 * @param const char* envelope -> input frame
 * @param uint64 length -> exact frame byte count
 * @return char* -> owned plaintext released with crypto_session__free_text, or NULL
 */
char *crypto_session__decrypt(desktop_crypto_t *crypto, const char *envelope, uint64 length)
{
    cJSON *root = NULL;
    const cJSON *iv_value = NULL;
    const cJSON *data_value = NULL;
    const cJSON *tag_value = NULL;
    ubyte iv[16] = { 0 };
    ubyte checksum[32] = { 0 };
    ubyte received_tag[32] = { 0 };
    char *bytes = NULL;
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
    root = desktop_protocol__parse_json(envelope, length, DESKTOP_MESSAGE_LIMIT, CJSON_NESTING_LIMIT);
    if (!root)
    {
        goto label_finished;
    }
    iv_value = cJSON_GetObjectItemCaseSensitive(root, "iv");
    data_value = cJSON_GetObjectItemCaseSensitive(root, "data");
    tag_value = cJSON_GetObjectItemCaseSensitive(root, "tag");
    if (!cJSON_IsString(iv_value) || !cJSON_IsString(data_value) || mbedtls_base64_decode(iv, sizeof(iv), &iv_length, (const ubyte *)iv_value->valuestring, clib__strlen(iv_value->valuestring)) != 0 || iv_length != sizeof(iv))
    {
        goto label_finished;
    }
    if (crypto->session_ready && (!cJSON_IsString(tag_value) || mbedtls_base64_decode(received_tag, sizeof(received_tag), &tag_length, (const ubyte *)tag_value->valuestring, clib__strlen(tag_value->valuestring)) != 0 || tag_length != sizeof(received_tag) || !_crypto_session_internal__mac(crypto, iv_value->valuestring, data_value->valuestring, checksum, TRUE) || mbedtls_ct_memcmp(checksum, received_tag, sizeof(checksum)) != 0))
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
    if (mbedtls_base64_decode((ubyte *)bytes, capacity - 1, &data_length, (const ubyte *)data_value->valuestring, capacity - 1) != 0 || !data_length)
    {
        goto label_finished;
    }
    if (crypto->session_ready)
    {
        if (crypto->negotiated.count)
        {
            if (!keys_session__crypt(&crypto->negotiated, 1, 0, iv, (ubyte *)bytes, data_length))
            {
                goto label_finished;
            }
        }
        else
        {
            _crypto_session_internal__layer(crypto->session_keys, iv, (ubyte *)bytes, data_length);
        }
    }
    for (index = crypto->server_key_count; index > 0; index--)
    {
        if (!ciphers__crypt(crypto->server_types[index - 1], crypto->server_keys[index - 1], iv, (ubyte *)bytes, data_length))
        {
            goto label_finished;
        }
    }
    // Zero padding terminates the JSON; reject hidden nonzero data after it.
    index = clib__strlen(bytes);
    while (index < data_length && !bytes[index])
    {
        index++;
    }
    complete = index == data_length;
label_finished:
    crypto_session__free_json(root);
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
void crypto_session__free_text(char *text)
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
void crypto_session__free_json(cJSON *json)
{
    cJSON *next = NULL;
    while (json)
    {
        next = json->next;
        if (!(json->type & cJSON_IsReference))
        {
            crypto_session__free_json(json->child);
            crypto_session__free_text(json->valuestring);
        }
        if (!(json->type & cJSON_StringIsConst))
        {
            crypto_session__free_text(json->string);
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
boole crypto_session__content_keys(desktop_crypto_t *crypto, desktop_content_keys_t *keys)
{
    clib__null_memory(keys, sizeof(*keys));
    return mbedtls_ctr_drbg_random(&crypto->random, (ubyte *)keys, sizeof(*keys)) == 0;
}

/**
 * @brief serializes channel keys using the existing numeric-array wire format.
 * @param const desktop_content_keys_t* keys -> key set
 * @return char* -> owned JSON string, or NULL
 */
char *crypto_session__keys_json(const desktop_content_keys_t *keys)
{
    cJSON *root = cJSON_CreateArray();
    cJSON *entry = NULL;
    cJSON *key = NULL;
    cJSON *iv = NULL;
    char *result = NULL;
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
    crypto_session__free_json(root);
    return result;
}

/**
 * @brief validates every byte before accepting a received channel or direct-message key set.
 * @param const char* json -> four numeric-array key entries
 * @param desktop_content_keys_t* keys -> output, cleared on failure
 * @return boole -> 1 on success
 */
boole crypto_session__read_keys(const char *json, desktop_content_keys_t *keys)
{
    cJSON *root = NULL;
    const cJSON *entry = NULL;
    const cJSON *array = NULL;
    const cJSON *value = NULL;
    int64 index = 0;
    int64 part = 0;
    int64 byte = 0;
    int64 count = 0;
    boole complete = 0;
    ubyte *output = NULL;

    clib__null_memory(keys, sizeof(*keys));
    root = json ? desktop_protocol__parse_json(json, clib__strlen(json), DESKTOP_MESSAGE_LIMIT, CJSON_NESTING_LIMIT) : NULL;
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
                complete = cJSON_IsNumber(value) && value->valuedouble >= 0 && value->valuedouble <= 255 && value->valuedouble == value->valueint;
                if (complete)
                {
                    output[byte] = (ubyte)value->valueint;
                }
            }
        }
    }
    crypto_session__free_json(root);
    if (!complete)
    {
        mbedtls_platform_zeroize(keys, sizeof(*keys));
    }
    return complete;
}

/**
 * @brief bounds a flat envelope before cJSON allocates any nodes or strings.
 * @param const char* text -> complete, bounded JSON text
 * @param uint64 length -> text byte count excluding its terminator
 * @param uint64 maximum_members -> maximum permitted object fields
 * @return boole -> TRUE when bounded enough for full JSON validation
 */
static boole _crypto_session_internal__flat_envelope(const char *text, uint64 length, uint64 maximum_members)
{
    uint64 index = 0;
    int64 depth = 0;
    uint64 strings = 0;
    uint64 members = 0;
    uint64 separators = 0;
    boole quoted = FALSE;
    boole escaped = FALSE;

    if (!text || !length || text[0] != '{' || length > 40ULL * 1024 * 1024 || !maximum_members)
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
        else if (text[index] == ':')
        {
            members++;
        }
        else if (text[index] == ',')
        {
            separators++;
        }
        if (depth < 0 || depth > 1 || strings > maximum_members * 2 || members > maximum_members || separators >= maximum_members)
        {
            return FALSE;
        }
    }
    return !quoted && !depth;
}

/**
 * @brief validates a flat content envelope before allocating its JSON strings or decoding its IV.
 * @param const char* text -> complete, bounded JSON text
 * @param uint64 length -> text byte count excluding its terminator
 * @param cJSON** root -> owned parsed envelope; caller clears it on success and failure
 * @param ubyte iv[16] -> decoded per-message IV
 * @param const char** encoded -> ciphertext borrowed from the parsed envelope on success
 * @return boole -> TRUE for one IV and one nonempty ciphertext string
 */
static boole _crypto_session_internal__content_envelope(const char *text, uint64 length, cJSON **root, ubyte iv[16], const char **encoded)
{
    const cJSON *iv_value = NULL;
    const cJSON *data_value = NULL;
    size_t written = 0;
    if (!_crypto_session_internal__flat_envelope(text, length, 2))
    {
        return FALSE;
    }
    *root = desktop_protocol__parse_json(text, length, 40ULL * 1024 * 1024, 1);
    iv_value = cJSON_GetObjectItemCaseSensitive(*root, "iv");
    data_value = cJSON_GetObjectItemCaseSensitive(*root, "data");
    if (!cJSON_IsObject(*root) || cJSON_GetArraySize(*root) != 2 || !cJSON_IsString(iv_value) || !cJSON_IsString(data_value) || !data_value->valuestring[0] || clib__strlen(iv_value->valuestring) != 24 || mbedtls_base64_decode(iv, 16, &written, (const ubyte *)iv_value->valuestring, 24) != 0 || written != 16)
    {
        return FALSE;
    }
    *encoded = data_value->valuestring;
    return TRUE;
}

/**
 * @brief implements the existing four-layer content envelope, including legacy bare-base64 reads.
 * @param desktop_crypto_t* crypto -> random source for encryption; unused for decryption
 * @param const desktop_content_keys_t* keys -> channel keys
 * @param const char* text -> plaintext or envelope
 * @param boole encrypt -> nonzero to encrypt
 * @return char* -> owned result, or NULL
 */
static char *_crypto_session_internal__content(desktop_crypto_t *crypto, const desktop_content_keys_t *keys, const char *text, boole encrypt)
{
    cJSON *root = NULL;
    const char *encoded_data = text;
    ubyte iv[16] = { 0 };
    ubyte counter[16] = { 0 };
    char iv_text[25] = { 0 };
    char *bytes = NULL;
    char *encoded = NULL;
    char *result = NULL;
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
            if (!_crypto_session_internal__content_envelope(text, length, &root, iv, &encoded_data))
            {
                goto label_finished;
            }
        }
        if (mbedtls_base64_decode((ubyte *)bytes, size, &written, (const ubyte *)encoded_data, clib__strlen(encoded_data)) != 0 || !written)
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
        _crypto_session_internal__layer(keys->keys[index], counter, (ubyte *)bytes, size);
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
    if (root && encoded && _crypto_session_internal__base64(iv_text, sizeof(iv_text), iv, sizeof(iv)) && _crypto_session_internal__base64(encoded, capacity, (ubyte *)bytes, size) && cJSON_AddStringToObject(root, "iv", iv_text) && cJSON_AddStringToObject(root, "data", encoded))
    {
        result = cJSON_PrintUnformatted(root);
    }
label_finished:
    crypto_session__free_json(root);
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
 * @brief encrypts a channel message body under a fresh per-message IV.
 * @param desktop_crypto_t* crypto -> worker-owned random source
 * @param const desktop_content_keys_t* keys -> content key set
 * @param const char* text -> plaintext
 * @return char* -> owned content envelope, or NULL
 */
char *crypto_session__content_encrypt(desktop_crypto_t *crypto, const desktop_content_keys_t *keys, const char *text)
{
    return _crypto_session_internal__content(crypto, keys, text, 1);
}

/**
 * @brief decrypts an internal channel message body.
 * @param const desktop_content_keys_t* keys -> content key set
 * @param const char* text -> encrypted content
 * @return char* -> owned plaintext, or NULL
 */
char *crypto_session__content_decrypt(const desktop_content_keys_t *keys, const char *text)
{
    return _crypto_session_internal__content(NULL, keys, text, 0);
}

static boole _crypto_session_internal__attachment_derive(const desktop_content_keys_t *keys, const ubyte iv[16], ubyte derived[160])
{
    ubyte material[192];
    const char label[] = "lemon-attachment-v2";
    uint64 i = 0;
    int status = 0;
    for (i = 0; i < 4; i++)
    {
        clib__memcpy(material + i * 48, keys->keys[i], 32);
        clib__memcpy(material + i * 48 + 32, keys->ivs[i], 16);
    }
    status = mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), iv, 16, material, sizeof(material), (const ubyte *)label, sizeof(label) - 1, derived, 160);
    mbedtls_platform_zeroize(material, sizeof(material));
    return status == 0;
}

static boole _crypto_session_internal__attachment_tag(const ubyte derived[160], const ubyte iv[16], const ubyte *data, size_t size, ubyte tag[32])
{
    const char label[] = "lemon-attachment-v2";
    mbedtls_md_context_t context;
    boole result = FALSE;
    mbedtls_md_init(&context);
    result = mbedtls_md_setup(&context, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1) == 0 && mbedtls_md_hmac_starts(&context, derived + 128, 32) == 0 && mbedtls_md_hmac_update(&context, (const ubyte *)label, sizeof(label) - 1) == 0 && mbedtls_md_hmac_update(&context, iv, 16) == 0 && mbedtls_md_hmac_update(&context, data, size) == 0 && mbedtls_md_hmac_finish(&context, tag) == 0;
    mbedtls_md_free(&context);
    return result;
}

/**
 * @brief follows the browser's separate picture and file encryption formats within the attachment size limit.
 * @param desktop_crypto_t* crypto -> random source for file and picture encryption; unused for decoding
 * @param const desktop_content_keys_t* keys -> captured channel keys
 * @param const char* text -> terminated plaintext, current IV envelope or legacy base64 ciphertext
 * @param boole encrypt -> TRUE to encrypt, FALSE to decrypt
 * @param boole picture -> TRUE for the existing picture IV envelope; FALSE for authenticated v2 files
 * @param uint64 padding -> minimum plaintext bytes, at most 4096
 * @param boole require_authentication -> reject content without the authenticated file format
 * @return char* -> owned terminated result; free with crypto_session__free_text
 */
static char *_crypto_session_internal__attachment(desktop_crypto_t *crypto, const desktop_content_keys_t *keys, const char *text, boole encrypt, boole picture, uint64 padding, boole require_authentication)
{
    const uint64 limit = 40ULL * 1024 * 1024;
    cJSON *root = NULL;
    const cJSON *version = NULL;
    const cJSON *nonce = NULL;
    const cJSON *data = NULL;
    const cJSON *code = NULL;
    const char *encoded_data = text;
    ubyte iv[16] = { 0 };
    ubyte derived[160] = { 0 };
    ubyte tag[32] = { 0 };
    ubyte expected_tag[32] = { 0 };
    char tag_text[45] = { 0 };
    boole version2 = encrypt && !picture;
    ubyte counter[16] = { 0 };
    uint64 length = text ? clib__strlen(text) : 0;
    uint64 size = 0;
    uint64 capacity = 0;
    uint64 bytes_capacity = 0;
    uint64 plaintext_length = 0;
    uint64 index = 0;
    uint64 byte = 0;
    size_t written = 0;
    ubyte *bytes = NULL;
    char *result = NULL;
    char *encoded = NULL;
    char iv_text[25] = { 0 };
    if (!keys || !length || length > limit || padding > 4096)
    {
        return NULL;
    }
    if (!encrypt && text[0] == '{')
    {
        if (!_crypto_session_internal__flat_envelope(text, length, 4))
        {
            goto label_finished;
        }
        root = desktop_protocol__parse_json(text, length, limit, 1);
        version = cJSON_GetObjectItemCaseSensitive(root, "v");
        if (version != NULL)
        {
            nonce = cJSON_GetObjectItemCaseSensitive(root, "iv");
            data = cJSON_GetObjectItemCaseSensitive(root, "data");
            code = cJSON_GetObjectItemCaseSensitive(root, "tag");
            if (!cJSON_IsNumber(version) || version->valuedouble != 2 || cJSON_GetArraySize(root) != 4 || !cJSON_IsString(nonce) || clib__strlen(nonce->valuestring) != 24 || !cJSON_IsString(data) || !data->valuestring[0] || !cJSON_IsString(code) || clib__strlen(code->valuestring) != 44 || mbedtls_base64_decode(iv, sizeof(iv), &written, (const ubyte *)nonce->valuestring, 24) != 0 || written != 16 || mbedtls_base64_decode(tag, sizeof(tag), &written, (const ubyte *)code->valuestring, 44) != 0 || written != 32)
            {
                goto label_finished;
            }
            version2 = TRUE;
            encoded_data = data->valuestring;
        }
        else
        {
            crypto_session__free_json(root);
            root = NULL;
            if (!_crypto_session_internal__content_envelope(text, length, &root, iv, &encoded_data))
            {
                goto label_finished;
            }
        }
        length = clib__strlen(encoded_data);
    }
    if (!encrypt && require_authentication && !version2)
    {
        goto label_finished;
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
        if (!crypto_session__random(crypto, iv, sizeof(iv)))
        {
            goto label_finished;
        }
        clib__memcpy(bytes, text, length);
    }
    else if (mbedtls_base64_decode(bytes, size, &written, (const ubyte *)encoded_data, length) != 0 || !written)
    {
        goto label_finished;
    }
    else
    {
        size = written;
    }
    if (version2)
    {
        if (!_crypto_session_internal__attachment_derive(keys, iv, derived))
        {
            goto label_finished;
        }
        if (!encrypt && (!_crypto_session_internal__attachment_tag(derived, iv, bytes, size, expected_tag) || mbedtls_ct_memcmp(tag, expected_tag, sizeof(tag)) != 0))
        {
            goto label_finished;
        }
    }
    for (index = 0; index < 4; index++)
    {
        for (byte = 0; byte < sizeof(counter); byte++)
        {
            counter[byte] = keys->ivs[index][byte] ^ iv[byte];
        }
        _crypto_session_internal__layer(version2 ? derived + index * 32 : keys->keys[index], counter, bytes, size);
    }
    if (encrypt)
    {
        capacity = (size + 2) / 3 * 4 + 1;
        if (capacity > limit + 1)
        {
            goto label_finished;
        }
        root = cJSON_CreateObject();
        encoded = calloc(1, capacity);
        if (root && encoded && _crypto_session_internal__base64(encoded, capacity, bytes, size) && _crypto_session_internal__base64(iv_text, sizeof(iv_text), iv, sizeof(iv)) && cJSON_AddStringToObject(root, "iv", iv_text) && cJSON_AddStringToObject(root, "data", encoded))
        {
            if (version2 && (!_crypto_session_internal__attachment_tag(derived, iv, bytes, size, tag) || !_crypto_session_internal__base64(tag_text, sizeof(tag_text), tag, sizeof(tag)) || !cJSON_AddNumberToObject(root, "v", 2) || !cJSON_AddStringToObject(root, "tag", tag_text)))
            {
                goto label_finished;
            }
            result = cJSON_PrintUnformatted(root);
            if (result && (uint64)clib__strlen(result) > limit)
            {
                crypto_session__free_text(result);
                result = NULL;
            }
        }
    }
    else
    {
        index = clib__strlen((char *)bytes);
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
    mbedtls_platform_zeroize(derived, sizeof(derived));
    mbedtls_platform_zeroize(tag, sizeof(tag));
    mbedtls_platform_zeroize(expected_tag, sizeof(expected_tag));
    crypto_session__free_json(root);
    if (bytes)
    {
        mbedtls_platform_zeroize(bytes, bytes_capacity);
        cJSON_free(bytes);
    }
    if (encoded)
    {
        mbedtls_platform_zeroize(encoded, capacity);
        free(encoded);
    }
    mbedtls_platform_zeroize(iv_text, sizeof(iv_text));
    mbedtls_platform_zeroize(counter, sizeof(counter));
    mbedtls_platform_zeroize(iv, sizeof(iv));
    return result;
}

/**
 * @brief encrypts file headers and bodies with fresh keys and an authenticated v2 envelope.
 * @param const desktop_content_keys_t* keys -> worker-owned content keys
 * @param const char* text -> attachment plaintext
 * @param uint64 padding -> protocol padding floor: 1024 for headers, 4096 for bodies
 * @return char* -> owned ciphertext or NULL
 */
char *crypto_session__attachment_encrypt(desktop_crypto_t *crypto, const desktop_content_keys_t *keys, const char *text, uint64 padding)
{
    return _crypto_session_internal__attachment(crypto, keys, text, TRUE, FALSE, padding, TRUE);
}

/**
 * @brief encrypts a picture using the fresh-IV envelope already supported by the browser.
 * @param desktop_crypto_t* crypto -> attachment worker's random source
 * @param const desktop_content_keys_t* keys -> captured channel keys
 * @param const char* text -> picture data URL
 * @param uint64 padding -> minimum plaintext size
 * @return char* -> owned picture envelope or NULL
 */
char *crypto_session__picture_encrypt(desktop_crypto_t *crypto, const desktop_content_keys_t *keys, const char *text, uint64 padding)
{
    return _crypto_session_internal__attachment(crypto, keys, text, TRUE, TRUE, padding, FALSE);
}

/**
 * @brief decrypts a browser-compatible attachment without raising the chat-message size limit.
 * @param const desktop_content_keys_t* keys -> captured content keys
 * @param const char* text -> bounded current IV envelope or legacy base64 ciphertext
 * @return char* -> owned plaintext or NULL
 */
char *crypto_session__attachment_decrypt(const desktop_content_keys_t *keys, const char *text)
{
    return _crypto_session_internal__attachment(NULL, keys, text, FALSE, FALSE, 0, FALSE);
}

/**
 * @brief Opens a file header or body only when its authenticated format and tag are valid.
 * @param const desktop_content_keys_t* keys -> captured channel keys
 * @param const char* text -> restored internal file envelope
 * @return char* -> owned plaintext, or NULL
 */
char *crypto_session__file_decrypt(const desktop_content_keys_t *keys, const char *text)
{
    return _crypto_session_internal__attachment(NULL, keys, text, FALSE, FALSE, 0, TRUE);
}

/**
 * @brief Derives a separate key for hiding a channel message IV.
 * @param const desktop_content_keys_t* keys -> four channel key and IV pairs
 * @param ubyte derived[32] -> AES key for the IV block
 * @return boole -> TRUE when the key was derived
 */
static boole _crypto_session_internal__channel_derive(const desktop_content_keys_t *keys, ubyte derived[32])
{
    const ubyte label[] = "lemon-channel-envelope-v3";
    ubyte salt[32] = { 0 };
    ubyte material[192] = { 0 };
    uint64 index = 0;
    boole result = FALSE;

    for (index = 0; index < 4; index++)
    {
        clib__memcpy(material + index * 48, keys->keys[index], 32);
        clib__memcpy(material + index * 48 + 32, keys->ivs[index], 16);
    }
    result = mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), salt, sizeof(salt), material, sizeof(material), label, sizeof(label) - 1, derived, 32) == 0;
    mbedtls_platform_zeroize(material, sizeof(material));
    return result;
}

/**
 * @brief Hides the random message IV before sending encrypted channel content.
 * @param const desktop_content_keys_t* keys -> current channel key and IV pairs
 * @param const char* content -> internal text, picture or authenticated file envelope
 * @return char* -> owned channel envelope, or NULL
 */
char *crypto_session__channel_seal(const desktop_content_keys_t *keys, const char *content)
{
    const uint64 limit = 40ULL * 1024 * 1024;
    cJSON *body = NULL;
    cJSON *envelope = NULL;
    const cJSON *version = NULL;
    const cJSON *nonce = NULL;
    const cJSON *data = NULL;
    const cJSON *file_tag = NULL;
    mbedtls_aes_context aes = { 0 };
    ubyte derived[32] = { 0 };
    ubyte iv[16] = { 0 };
    ubyte *packet = NULL;
    char *encoded = NULL;
    char *result = NULL;
    uint64 length = 0;
    uint64 data_length = 0;
    uint64 packet_capacity = 0;
    uint64 packet_length = 0;
    uint64 encoded_capacity = 0;
    uint64 offset = 17;
    size_t written = 0;

    if (!keys || !content)
    {
        return NULL;
    }
    while (length <= limit && content[length])
    {
        length++;
    }
    if (!_crypto_session_internal__flat_envelope(content, length, 4))
    {
        return NULL;
    }
    mbedtls_aes_init(&aes);

    // Read internal content
    body = desktop_protocol__parse_json(content, length, limit, 1);
    version = cJSON_GetObjectItemCaseSensitive(body, "v");
    nonce = cJSON_GetObjectItemCaseSensitive(body, "iv");
    data = cJSON_GetObjectItemCaseSensitive(body, "data");
    file_tag = cJSON_GetObjectItemCaseSensitive(body, "tag");
    if (!cJSON_IsObject(body) || !cJSON_IsString(nonce) || clib__strlen(nonce->valuestring) != 24 || !cJSON_IsString(data) || !data->valuestring[0] || mbedtls_base64_decode(iv, sizeof(iv), &written, (const ubyte *)nonce->valuestring, 24) != 0 || written != 16)
    {
        goto label_finished;
    }
    if (version)
    {
        if (!cJSON_IsNumber(version) || version->valuedouble != 2 || cJSON_GetArraySize(body) != 4 || !cJSON_IsString(file_tag) || clib__strlen(file_tag->valuestring) != 44)
        {
            goto label_finished;
        }
        offset = 49;
    }
    else if (cJSON_GetArraySize(body) != 2)
    {
        goto label_finished;
    }
    data_length = clib__strlen(data->valuestring);
    packet_capacity = offset + data_length / 4 * 3 + 3;
    packet = calloc(1, packet_capacity);
    if (!packet || mbedtls_base64_decode(packet + offset, packet_capacity - offset, &written, (const ubyte *)data->valuestring, data_length) != 0 || !written)
    {
        goto label_finished;
    }
    packet_length = offset + written;
    packet[0] = version ? 1 : 0;
    if (version && (mbedtls_base64_decode(packet + 17, 32, &written, (const ubyte *)file_tag->valuestring, 44) != 0 || written != 32))
    {
        goto label_finished;
    }

    // Encrypt only the IV block
    if (!_crypto_session_internal__channel_derive(keys, derived) || mbedtls_aes_setkey_enc(&aes, derived, 256) != 0 || mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, iv, packet + 1) != 0)
    {
        goto label_finished;
    }

    // Serialize the packet
    encoded_capacity = (packet_length + 2) / 3 * 4 + 1;
    if (encoded_capacity > limit)
    {
        goto label_finished;
    }
    encoded = calloc(1, encoded_capacity);
    envelope = cJSON_CreateObject();
    if (encoded && envelope && _crypto_session_internal__base64(encoded, encoded_capacity, packet, packet_length) && cJSON_AddStringToObject(envelope, "message_value", encoded))
    {
        result = cJSON_PrintUnformatted(envelope);
        if (result && (uint64)clib__strlen(result) > limit)
        {
            crypto_session__free_text(result);
            result = NULL;
        }
    }

label_finished:
    crypto_session__free_json(body);
    crypto_session__free_json(envelope);
    if (packet)
    {
        mbedtls_platform_zeroize(packet, packet_capacity);
        free(packet);
    }
    if (encoded)
    {
        mbedtls_platform_zeroize(encoded, encoded_capacity);
        free(encoded);
    }
    mbedtls_aes_free(&aes);
    mbedtls_platform_zeroize(derived, sizeof(derived));
    mbedtls_platform_zeroize(iv, sizeof(iv));
    return result;
}

/**
 * @brief Recovers a channel packet's hidden IV for internal content decryption.
 * @param const desktop_content_keys_t* keys -> current or previous channel key and IV pairs
 * @param const char* text -> received channel envelope containing its encrypted IV and ciphertext
 * @return char* -> owned internal content envelope, or NULL
 */
char *crypto_session__channel_open(const desktop_content_keys_t *keys, const char *text)
{
    const uint64 limit = 40ULL * 1024 * 1024;
    cJSON *envelope = NULL;
    cJSON *body = NULL;
    const cJSON *value = NULL;
    mbedtls_aes_context aes = { 0 };
    ubyte derived[32] = { 0 };
    ubyte iv[16] = { 0 };
    char iv_text[25] = { 0 };
    char file_tag_text[45] = { 0 };
    ubyte *packet = NULL;
    char *encoded = NULL;
    char *result = NULL;
    uint64 length = 0;
    uint64 data_length = 0;
    uint64 packet_capacity = 0;
    uint64 packet_length = 0;
    uint64 encoded_capacity = 0;
    uint64 offset = 0;
    size_t written = 0;

    if (!keys || !text)
    {
        return NULL;
    }
    while (length <= limit && text[length])
    {
        length++;
    }
    if (!_crypto_session_internal__flat_envelope(text, length, 1))
    {
        return NULL;
    }
    mbedtls_aes_init(&aes);

    // Validate public fields
    envelope = desktop_protocol__parse_json(text, length, limit, 1);
    value = cJSON_GetObjectItemCaseSensitive(envelope, "message_value");
    if (!cJSON_IsObject(envelope) || cJSON_GetArraySize(envelope) != 1 || !cJSON_IsString(value) || !value->valuestring[0])
    {
        goto label_finished;
    }
    data_length = clib__strlen(value->valuestring);
    packet_capacity = data_length / 4 * 3 + 3;
    packet = calloc(1, packet_capacity);
    if (!packet || mbedtls_base64_decode(packet, packet_capacity, &written, (const ubyte *)value->valuestring, data_length) != 0 || written < 18)
    {
        goto label_finished;
    }
    packet_length = written;
    if (packet[0] > 1)
    {
        goto label_finished;
    }
    offset = packet[0] ? 49 : 17;
    if (packet_length <= offset)
    {
        goto label_finished;
    }

    // Recover the random IV
    if (!_crypto_session_internal__channel_derive(keys, derived) || mbedtls_aes_setkey_dec(&aes, derived, 256) != 0 || mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, packet + 1, iv) != 0)
    {
        goto label_finished;
    }

    // Restore the internal envelope
    encoded_capacity = (packet_length - offset + 2) / 3 * 4 + 1;
    encoded = calloc(1, encoded_capacity);
    body = cJSON_CreateObject();
    if (!encoded || !body || !_crypto_session_internal__base64(encoded, encoded_capacity, packet + offset, packet_length - offset) || !_crypto_session_internal__base64(iv_text, sizeof(iv_text), iv, sizeof(iv)) || !cJSON_AddStringToObject(body, "iv", iv_text) || !cJSON_AddStringToObject(body, "data", encoded))
    {
        goto label_finished;
    }
    if (packet[0] && (!_crypto_session_internal__base64(file_tag_text, sizeof(file_tag_text), packet + 17, 32) || !cJSON_AddNumberToObject(body, "v", 2) || !cJSON_AddStringToObject(body, "tag", file_tag_text)))
    {
        goto label_finished;
    }
    result = cJSON_PrintUnformatted(body);
    if (result && (uint64)clib__strlen(result) > limit)
    {
        crypto_session__free_text(result);
        result = NULL;
    }

label_finished:
    crypto_session__free_json(envelope);
    crypto_session__free_json(body);
    if (packet)
    {
        mbedtls_platform_zeroize(packet, packet_capacity);
        free(packet);
    }
    if (encoded)
    {
        mbedtls_platform_zeroize(encoded, encoded_capacity);
        free(encoded);
    }
    mbedtls_aes_free(&aes);
    mbedtls_platform_zeroize(derived, sizeof(derived));
    mbedtls_platform_zeroize(iv, sizeof(iv));
    mbedtls_platform_zeroize(iv_text, sizeof(iv_text));
    mbedtls_platform_zeroize(file_tag_text, sizeof(file_tag_text));
    return result;
}

/**
 * @brief Derives direct file encryption and authentication keys from its four fresh key pairs.
 * @param const desktop_content_keys_t* keys -> four independent key and IV pairs
 * @param ubyte derived[160] -> four AES keys followed by the file MAC key
 * @return boole -> TRUE when derivation succeeded
 */
static boole _crypto_session_internal__direct_file_derive(const desktop_content_keys_t *keys, ubyte derived[160])
{
    const ubyte label[] = "lemon-direct-attachment";
    ubyte salt[32] = { 0 };
    ubyte material[192] = { 0 };
    uint64 index = 0;
    boole result = FALSE;

    for (index = 0; index < 4; index++)
    {
        clib__memcpy(material + index * 48, keys->keys[index], 32);
        clib__memcpy(material + index * 48 + 32, keys->ivs[index], 16);
    }
    result = mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), salt, sizeof(salt), material, sizeof(material), label, sizeof(label) - 1, derived, 160) == 0;
    mbedtls_platform_zeroize(material, sizeof(material));
    return result;
}

/**
 * @brief Computes the file integrity check without an extra direct-message IV.
 * @param const ubyte derived[160] -> derived direct file encryption and MAC keys
 * @param const ubyte* data -> encrypted file header or body
 * @param uint64 length -> ciphertext length
 * @param ubyte tag[32] -> HMAC-SHA256 output
 * @return boole -> TRUE when the tag was computed
 */
static boole _crypto_session_internal__direct_file_tag(const ubyte derived[160], const ubyte *data, uint64 length, ubyte tag[32])
{
    const ubyte label[] = "lemon-direct-attachment";
    mbedtls_md_context_t context = { 0 };
    boole result = FALSE;

    mbedtls_md_init(&context);
    result = mbedtls_md_setup(&context, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1) == 0 && mbedtls_md_hmac_starts(&context, derived + 128, 32) == 0 && mbedtls_md_hmac_update(&context, label, sizeof(label) - 1) == 0 && mbedtls_md_hmac_update(&context, data, length) == 0 && mbedtls_md_hmac_finish(&context, tag) == 0;
    mbedtls_md_free(&context);
    return result;
}

/**
 * @brief Uses each fresh direct key's own IV, retaining authentication only for files.
 * @param const desktop_content_keys_t* keys -> fresh keys for this one direct content item
 * @param const char* text -> plaintext or strict internal direct envelope
 * @param uint64 padding -> encryption padding floor, at most 4096 bytes
 * @param boole authenticated -> TRUE for a file header or body
 * @param boole encrypt -> TRUE to encrypt, FALSE to decrypt
 * @return char* -> owned result, or NULL
 */
static char *_crypto_session_internal__direct_content(const desktop_content_keys_t *keys, const char *text, uint64 padding, boole authenticated, boole encrypt)
{
    const uint64 limit = 40ULL * 1024 * 1024;
    cJSON *body = NULL;
    const cJSON *data = NULL;
    const cJSON *code = NULL;
    const char *encoded_data = NULL;
    ubyte derived[160] = { 0 };
    ubyte tag[32] = { 0 };
    ubyte expected[32] = { 0 };
    char tag_text[45] = { 0 };
    ubyte *bytes = NULL;
    char *encoded = NULL;
    char *result = NULL;
    uint64 length = 0;
    uint64 size = 0;
    uint64 bytes_capacity = 0;
    uint64 encoded_capacity = 0;
    uint64 plaintext_length = 0;
    uint64 index = 0;
    size_t written = 0;

    if (!keys || !text || padding > 4096)
    {
        return NULL;
    }
    while (length <= limit && text[length])
    {
        length++;
    }
    if (!length || length > limit)
    {
        return NULL;
    }

    // Read the exact direct shape
    if (!encrypt)
    {
        if (!_crypto_session_internal__flat_envelope(text, length, authenticated ? 2 : 1))
        {
            goto label_finished;
        }
        body = desktop_protocol__parse_json(text, length, limit, 1);
        data = cJSON_GetObjectItemCaseSensitive(body, "data");
        code = cJSON_GetObjectItemCaseSensitive(body, "tag");
        if (!cJSON_IsObject(body) || cJSON_GetArraySize(body) != (authenticated ? 2 : 1) || !cJSON_IsString(data) || !data->valuestring[0])
        {
            goto label_finished;
        }
        if (authenticated && (!cJSON_IsString(code) || clib__strlen(code->valuestring) != 44 || mbedtls_base64_decode(tag, sizeof(tag), &written, (const ubyte *)code->valuestring, 44) != 0 || written != 32))
        {
            goto label_finished;
        }
        encoded_data = data->valuestring;
        length = clib__strlen(encoded_data);
    }
    size = encrypt ? (length < padding ? padding : length) : length / 4 * 3 + 3;
    bytes_capacity = size + 1;
    bytes = cJSON_malloc(bytes_capacity);
    if (!bytes)
    {
        goto label_finished;
    }
    clib__null_memory(bytes, bytes_capacity);
    if (encrypt)
    {
        clib__memcpy(bytes, text, length);
    }
    else if (mbedtls_base64_decode(bytes, size, &written, (const ubyte *)encoded_data, length) != 0 || !written)
    {
        goto label_finished;
    }
    else
    {
        size = written;
    }

    // Check files before decryption
    if (authenticated)
    {
        if (!_crypto_session_internal__direct_file_derive(keys, derived))
        {
            goto label_finished;
        }
        if (!encrypt && (!_crypto_session_internal__direct_file_tag(derived, bytes, size, expected) || mbedtls_ct_memcmp(tag, expected, sizeof(tag)) != 0))
        {
            goto label_finished;
        }
    }

    // Use the four original IVs
    for (index = 0; index < 4; index++)
    {
        _crypto_session_internal__layer(authenticated ? derived + index * 32 : keys->keys[index], keys->ivs[index], bytes, size);
    }
    if (encrypt)
    {
        encoded_capacity = (size + 2) / 3 * 4 + 1;
        if (encoded_capacity > limit)
        {
            goto label_finished;
        }
        body = cJSON_CreateObject();
        encoded = calloc(1, encoded_capacity);
        if (!body || !encoded || !_crypto_session_internal__base64(encoded, encoded_capacity, bytes, size) || !cJSON_AddStringToObject(body, "data", encoded))
        {
            goto label_finished;
        }
        if (authenticated && (!_crypto_session_internal__direct_file_tag(derived, bytes, size, tag) || !_crypto_session_internal__base64(tag_text, sizeof(tag_text), tag, sizeof(tag)) || !cJSON_AddStringToObject(body, "tag", tag_text)))
        {
            goto label_finished;
        }
        result = cJSON_PrintUnformatted(body);
        if (result && (uint64)clib__strlen(result) > limit)
        {
            crypto_session__free_text(result);
            result = NULL;
        }
    }
    else
    {
        plaintext_length = clib__strlen((char *)bytes);
        index = plaintext_length;
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
    crypto_session__free_json(body);
    if (bytes)
    {
        mbedtls_platform_zeroize(bytes, bytes_capacity);
        cJSON_free(bytes);
    }
    if (encoded)
    {
        mbedtls_platform_zeroize(encoded, encoded_capacity);
        free(encoded);
    }
    mbedtls_platform_zeroize(derived, sizeof(derived));
    mbedtls_platform_zeroize(tag, sizeof(tag));
    mbedtls_platform_zeroize(expected, sizeof(expected));
    mbedtls_platform_zeroize(tag_text, sizeof(tag_text));
    return result;
}

/**
 * @brief Encrypts direct content using its four fresh keys and their own IVs.
 * @param const desktop_content_keys_t* keys -> fresh key and IV pairs for this content item
 * @param const char* text -> plaintext text, picture, file header or file body
 * @param uint64 padding -> minimum plaintext length, at most 4096 bytes
 * @param boole authenticated -> TRUE for file headers and bodies
 * @return char* -> owned internal direct envelope, or NULL
 */
char *crypto_session__direct_content_encrypt(const desktop_content_keys_t *keys, const char *text, uint64 padding, boole authenticated)
{
    return _crypto_session_internal__direct_content(keys, text, padding, authenticated, TRUE);
}

/**
 * @brief Decrypts direct content without accepting an extra message IV or older file formats.
 * @param const desktop_content_keys_t* keys -> recovered direct key and IV pairs
 * @param const char* content -> strict internal direct envelope
 * @param boole authenticated -> TRUE to require and verify the file tag
 * @return char* -> owned plaintext, or NULL
 */
char *crypto_session__direct_content_decrypt(const desktop_content_keys_t *keys, const char *content, boole authenticated)
{
    return _crypto_session_internal__direct_content(keys, content, 0, authenticated, FALSE);
}

/**
 * @brief Encrypts the four fresh key and IV pairs for the direct recipient.
 * @param desktop_crypto_t* crypto -> sender's crypto state
 * @param const char* public_key -> recipient's public key
 * @param const desktop_content_keys_t* keys -> four fresh content keys and their IVs
 * @param const char* content -> encrypted text, picture or authenticated file envelope
 * @return char* -> owned direct envelope, or NULL
 */
char *crypto_session__direct_seal(desktop_crypto_t *crypto, const char *public_key, const desktop_content_keys_t *keys, const char *content)
{
    const uint64 limit = 40ULL * 1024 * 1024;
    cJSON *body = NULL;
    cJSON *protected_keys = NULL;
    cJSON *file_body = NULL;
    cJSON *envelope = NULL;
    const cJSON *data = NULL;
    const cJSON *tag = NULL;
    const char *value = NULL;
    char *keys_json = NULL;
    char *protected_json = NULL;
    char *encrypted_keys = NULL;
    char *file_json = NULL;
    char *result = NULL;
    ubyte decoded_tag[32] = { 0 };
    size_t written = 0;
    uint64 length = content ? clib__strlen(content) : 0;

    if (!crypto || !public_key || !keys || !length || length > limit || !_crypto_session_internal__flat_envelope(content, length, 2))
    {
        return NULL;
    }
    body = desktop_protocol__parse_json(content, length, limit, 1);
    data = cJSON_GetObjectItemCaseSensitive(body, "data");
    tag = cJSON_GetObjectItemCaseSensitive(body, "tag");
    if (!cJSON_IsObject(body) || !cJSON_IsString(data) || !data->valuestring[0])
    {
        goto label_finished;
    }
    value = data->valuestring;

    // Preserve file authentication
    if (tag)
    {
        if (cJSON_GetArraySize(body) != 2 || !cJSON_IsString(tag) || clib__strlen(tag->valuestring) != 44 || mbedtls_base64_decode(decoded_tag, sizeof(decoded_tag), &written, (const ubyte *)tag->valuestring, 44) != 0 || written != 32)
        {
            goto label_finished;
        }
        file_body = cJSON_CreateObject();
        if (!file_body || !cJSON_AddStringToObject(file_body, "data", value) || !cJSON_AddStringToObject(file_body, "tag", tag->valuestring))
        {
            goto label_finished;
        }
        file_json = cJSON_PrintUnformatted(file_body);
        value = file_json;
    }
    else if (cJSON_GetArraySize(body) != 1)
    {
        goto label_finished;
    }

    // Encrypt the four key pairs
    keys_json = crypto_session__keys_json(keys);
    protected_keys = cJSON_CreateObject();
    if (!value || !keys_json || !protected_keys || !cJSON_AddRawToObject(protected_keys, "keys", keys_json))
    {
        goto label_finished;
    }
    protected_json = cJSON_PrintUnformatted(protected_keys);
    encrypted_keys = protected_json ? crypto_session__hybrid_encrypt(crypto, public_key, protected_json) : NULL;
    envelope = cJSON_CreateObject();
    if (envelope && encrypted_keys && cJSON_AddStringToObject(envelope, "message_keys", encrypted_keys) && cJSON_AddStringToObject(envelope, "message_value", value))
    {
        result = cJSON_PrintUnformatted(envelope);
        if (result && (uint64)clib__strlen(result) > limit)
        {
            crypto_session__free_text(result);
            result = NULL;
        }
    }

label_finished:
    crypto_session__free_json(body);
    crypto_session__free_json(protected_keys);
    crypto_session__free_json(file_body);
    crypto_session__free_json(envelope);
    crypto_session__free_text(keys_json);
    crypto_session__free_text(protected_json);
    crypto_session__free_text(encrypted_keys);
    crypto_session__free_text(file_json);
    mbedtls_platform_zeroize(decoded_tag, sizeof(decoded_tag));
    return result;
}

/**
 * @brief Recovers the direct recipient's four key pairs and validates the encrypted content shape.
 * @param desktop_crypto_t* crypto -> recipient's private identity
 * @param const struct cJSON* envelope -> direct envelope containing encrypted keys and content
 * @param desktop_content_keys_t* keys -> recovered content keys; cleared on failure
 * @return char* -> owned internal content envelope, or NULL
 */
char *crypto_session__direct_open(desktop_crypto_t *crypto, const struct cJSON *envelope, desktop_content_keys_t *keys)
{
    const uint64 limit = 40ULL * 1024 * 1024;
    const cJSON *encrypted_keys = cJSON_GetObjectItemCaseSensitive(envelope, "message_keys");
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(envelope, "message_value");
    const cJSON *data = NULL;
    const cJSON *tag = NULL;
    cJSON *protected_keys = NULL;
    cJSON *file_body = NULL;
    cJSON *body = NULL;
    char *protected_json = NULL;
    char *keys_json = NULL;
    char *result = NULL;
    ubyte decoded_tag[32] = { 0 };
    size_t written = 0;
    uint64 length = 0;

    if (!keys)
    {
        return NULL;
    }
    clib__null_memory(keys, sizeof(*keys));
    if (!crypto || !cJSON_IsObject(envelope) || !cJSON_IsString(encrypted_keys) || clib__strlen(encrypted_keys->valuestring) > 16384 || !cJSON_IsString(value) || !value->valuestring[0] || cJSON_GetArraySize(envelope) != 2)
    {
        return NULL;
    }
    length = clib__strlen(value->valuestring);
    if (length > limit)
    {
        return NULL;
    }

    // Recover recipient-only data
    protected_json = crypto_session__hybrid_decrypt(crypto, encrypted_keys->valuestring);
    if (!protected_json)
    {
        goto label_finished;
    }
    protected_keys = desktop_protocol__parse_json(protected_json, clib__strlen(protected_json), 16384, 4);
    if (!cJSON_IsObject(protected_keys) || cJSON_GetArraySize(protected_keys) != 1)
    {
        goto label_finished;
    }
    keys_json = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(protected_keys, "keys"));
    if (!keys_json || !crypto_session__read_keys(keys_json, keys))
    {
        goto label_finished;
    }

    // Restore the internal content shape
    body = cJSON_CreateObject();
    if (!body)
    {
        goto label_finished;
    }
    if (value->valuestring[0] == '{')
    {
        if (!_crypto_session_internal__flat_envelope(value->valuestring, length, 2))
        {
            goto label_finished;
        }
        file_body = desktop_protocol__parse_json(value->valuestring, length, limit, 1);
        data = cJSON_GetObjectItemCaseSensitive(file_body, "data");
        tag = cJSON_GetObjectItemCaseSensitive(file_body, "tag");
        if (!cJSON_IsObject(file_body) || cJSON_GetArraySize(file_body) != 2 || !cJSON_IsString(data) || !data->valuestring[0] || !cJSON_IsString(tag) || clib__strlen(tag->valuestring) != 44 || mbedtls_base64_decode(decoded_tag, sizeof(decoded_tag), &written, (const ubyte *)tag->valuestring, 44) != 0 || written != 32 || !cJSON_AddStringToObject(body, "data", data->valuestring) || !cJSON_AddStringToObject(body, "tag", tag->valuestring))
        {
            goto label_finished;
        }
    }
    else if (!cJSON_AddStringToObject(body, "data", value->valuestring))
    {
        goto label_finished;
    }
    result = cJSON_PrintUnformatted(body);

label_finished:
    crypto_session__free_json(protected_keys);
    crypto_session__free_json(file_body);
    crypto_session__free_json(body);
    crypto_session__free_text(protected_json);
    crypto_session__free_text(keys_json);
    mbedtls_platform_zeroize(decoded_tag, sizeof(decoded_tag));
    if (!result)
    {
        mbedtls_platform_zeroize(keys, sizeof(*keys));
    }
    return result;
}

/**
 * @brief reproduces the browser's RSA plus AES-CBC envelope for protected key JSON.
 * @param desktop_crypto_t* crypto -> worker-owned random source
 * @param const char* public_key -> receiver's base64 modulus, exponent 3
 * @param const char* text -> ASCII key-array JSON
 * @return char* -> owned hybrid envelope, or NULL
 */
char *crypto_session__hybrid_encrypt(desktop_crypto_t *crypto, const char *public_key, const char *text)
{
    mbedtls_rsa_context rsa = { 0 };
    mbedtls_aes_context aes = { 0 };
    mbedtls_mpi modulus = { 0 };
    mbedtls_mpi exponent = { 0 };
    ubyte raw_modulus[1024] = { 0 };
    ubyte key[32] = { 0 };
    ubyte utf8_key[64] = { 0 };
    ubyte iv[16] = { 0 };
    ubyte encrypted_key[1024] = { 0 };
    char key_text[1369] = { 0 };
    ubyte *body = NULL;
    char *body_text = NULL;
    char *result = NULL;
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
    if (!length || length > 16384 || mbedtls_base64_decode(raw_modulus, sizeof(raw_modulus), &modulus_length, (const ubyte *)public_key, clib__strlen(public_key)) != 0 || modulus_length < 256 || mbedtls_mpi_read_binary(&modulus, raw_modulus, modulus_length) != 0 || mbedtls_mpi_lset(&exponent, 3) != 0 || mbedtls_rsa_import(&rsa, &modulus, NULL, NULL, NULL, &exponent) != 0 || mbedtls_rsa_complete(&rsa) != 0 || mbedtls_rsa_check_pubkey(&rsa) != 0 || mbedtls_rsa_set_padding(&rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE) != 0 || mbedtls_ctr_drbg_random(&crypto->random, key, sizeof(key)) != 0 || mbedtls_ctr_drbg_random(&crypto->random, iv, sizeof(iv)) != 0)
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
    if (!body || !body_text || mbedtls_rsa_pkcs1_encrypt(&rsa, mbedtls_ctr_drbg_random, &crypto->random, key_length, utf8_key, encrypted_key) != 0)
    {
        goto label_finished;
    }
    clib__memcpy(body, iv, sizeof(iv));
    clib__memcpy(body + 16, text, length);
    if (mbedtls_aes_setkey_enc(&aes, key, 256) != 0 || mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padded, iv, body + 16, body + 16) != 0 || !_crypto_session_internal__base64(key_text, sizeof(key_text), encrypted_key, modulus_length) || !_crypto_session_internal__base64(body_text, body_capacity, body, padded + 16))
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
char *crypto_session__hybrid_decrypt(desktop_crypto_t *crypto, const char *text)
{
    mbedtls_aes_context aes = { 0 };
    const char *separator = clib__strchr(text, '?');
    ubyte ciphertext[1024] = { 0 };
    ubyte utf8_key[1024] = { 0 };
    ubyte key[32] = { 0 };
    ubyte iv[16] = { 0 };
    ubyte *body = NULL;
    char *result = NULL;
    size_t length = 0;
    uint64 rsa_length = mbedtls_rsa_get_len(&crypto->rsa);
    size_t decoded_length = 0;
    uint64 key_length = 0;
    uint64 capacity = 0;
    size_t body_length = 0;
    uint64 index = 0;
    int64 value = 0;

    mbedtls_aes_init(&aes);
    if (!separator || clib__strlen(text) > 32768 || rsa_length > sizeof(ciphertext) || mbedtls_base64_decode(ciphertext, sizeof(ciphertext), &length, (const ubyte *)text, (uint64)(separator - text)) != 0 || !length || length > rsa_length)
    {
        goto label_finished;
    }
    // The browser's bigint encoder omits leading zero ciphertext bytes.
    if (length < rsa_length)
    {
        clib__memmove(ciphertext + rsa_length - length, ciphertext, length);
        clib__null_memory(ciphertext, rsa_length - length);
    }
    if (mbedtls_rsa_pkcs1_decrypt(&crypto->rsa, mbedtls_ctr_drbg_random, &crypto->random, &decoded_length, ciphertext, utf8_key, sizeof(utf8_key)) != 0)
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
        key[key_length++] = (ubyte)value;
    }
    if (key_length != sizeof(key))
    {
        goto label_finished;
    }
    capacity = clib__strlen(separator + 1) + 1;
    body = calloc(1, capacity);
    if (!body || mbedtls_base64_decode(body, capacity - 1, &body_length, (const ubyte *)(separator + 1), capacity - 1) != 0 || body_length < 32 || body_length % 16)
    {
        goto label_finished;
    }
    clib__memcpy(iv, body, sizeof(iv));
    if (mbedtls_aes_setkey_dec(&aes, key, 256) != 0 || mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, body_length - 16, iv, body + 16, body + 16) != 0)
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

/** @brief Accepts a bounded v2 policy and produces a fresh mix for each requested exchange. */
cJSON *crypto_session__negotiate(desktop_crypto_t *crypto, const cJSON *policy, const char *public_key)
{
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(policy, "version");
    const cJSON *exchanges = cJSON_GetObjectItemCaseSensitive(policy, "dh_exchange_count");
    const cJSON *layers = cJSON_GetObjectItemCaseSensitive(policy, "session_encryption_layer_count");
    const cJSON *random = cJSON_GetObjectItemCaseSensitive(policy, "is_random_cipher_layer_enabled");
    const char *cipher = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(policy, "session_cipher"));
    const char *nonce = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(policy, "nonce"));
    ubyte nonce_bytes[16];
    cJSON *request = NULL;
    cJSON *mixes = NULL;
    cJSON *mix = NULL;
    if (crypto->policy.exchanges || !cJSON_IsNumber(version) || version->valuedouble != 2 || !cJSON_IsNumber(exchanges) || exchanges->valuedouble != exchanges->valueint || exchanges->valueint < 1 || exchanges->valueint > LEMON_MAX_DH_EXCHANGES || !cJSON_IsNumber(layers) || layers->valuedouble != layers->valueint || layers->valueint < 1 || layers->valueint > LEMON_MAX_SESSION_LAYERS || !cJSON_IsBool(random) || !ciphers__type(cipher) || !crypto_media__hex(nonce, nonce_bytes, sizeof(nonce_bytes)))
    {
        return NULL;
    }
    crypto->policy.exchanges = exchanges->valueint;
    crypto->policy.layers = layers->valueint;
    crypto->policy.cipher = ciphers__type(cipher);
    crypto->policy.random_layer = cJSON_IsTrue(random);
    if (!keys_session__begin_transcript(crypto->transcript, &crypto->policy, nonce, public_key))
    {
        return NULL;
    }
    request = cJSON_CreateObject();
    mixes = cJSON_AddArrayToObject(request, "dh_public_mixes");
    if (!mixes || !cJSON_AddStringToObject(request, "type", "public_key_info") || !cJSON_AddStringToObject(request, "value", public_key) || !cJSON_AddStringToObject(request, "verification_string", "welcome") || !cJSON_AddNumberToObject(request, "crypto_version", 2) || !cJSON_AddStringToObject(request, "crypto_nonce", nonce))
    {
        goto failed;
    }
    for (unsigned int index = 0; index < crypto->policy.exchanges; index++)
    {
        if (index == 0)
        {
            clib__string_copy(crypto->mixes[index], sizeof(crypto->mixes[index]), crypto->first_mix);
        }
        else if (!crypto_session__begin_dh(crypto, crypto->mixes[index], sizeof(crypto->mixes[index])))
        {
            goto failed;
        }
        mix = cJSON_CreateString(crypto->mixes[index]);
        if (mbedtls_mpi_copy(&crypto->exponents[index], &crypto->exponent) != 0 || !mix || !cJSON_AddItemToArray(mixes, mix))
        {
            cJSON_Delete(mix);
            goto failed;
        }
    }
    if (!cJSON_AddStringToObject(request, "dh_public_mix", crypto->mixes[0]))
    {
        goto failed;
    }
    return request;
failed:
    crypto_session__free_json(request);
    return NULL;
}

/** @brief Verifies every DH mix, the hidden cipher's inner proof and the final server transcript proof. */
boole crypto_session__finish_negotiation(desktop_crypto_t *crypto, const cJSON *message)
{
    const cJSON *reply = cJSON_GetObjectItemCaseSensitive(message, "crypto");
    const char *first = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(message, "dh_public_mix"));
    const cJSON *mixes = cJSON_GetObjectItemCaseSensitive(reply, "dh_public_mixes");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(reply, "version");
    ubyte secret[32] = { 0 };
    ubyte transcript[32];
    ubyte expected[32];
    ubyte received[32];
    char decimal[DESKTOP_DH_TEXT_CAPACITY];
    mbedtls_mpi mix = { 0 }, maximum = { 0 }, shared = { 0 };
    size_t written;
    const cJSON *probe = cJSON_GetObjectItemCaseSensitive(reply, "random_probe");
    int random_type = 0;
    boole success = FALSE;
    if (!crypto->policy.exchanges)
    {
        return !reply && crypto_session__finish_dh(crypto, first);
    }
    clib__memcpy(transcript, crypto->transcript, sizeof(transcript));
    if (!cJSON_IsNumber(version) || version->valuedouble != 2 || !cJSON_IsArray(mixes) || cJSON_GetArraySize(mixes) != (int)crypto->policy.exchanges || !first || !cJSON_IsString(cJSON_GetArrayItem(mixes, 0)) || clib__strcmp(first, cJSON_GetArrayItem(mixes, 0)->valuestring) != 0 || mbedtls_mpi_sub_int(&maximum, &crypto->modulus, 1) != 0)
    {
        goto done;
    }
    for (unsigned int index = 0; index < crypto->policy.exchanges; index++)
    {
        const char *text = cJSON_GetStringValue(cJSON_GetArrayItem(mixes, index));
        if (!text || !text[0] || clib__strlen(text) >= sizeof(decimal))
        {
            goto done;
        }
        for (const char *digit = text; *digit; digit++)
        {
            if (*digit < '0' || *digit > '9')
            {
                goto done;
            }
        }
        if (mbedtls_mpi_read_string(&mix, 10, text) != 0 || mbedtls_mpi_cmp_int(&mix, 1) <= 0 || mbedtls_mpi_cmp_mpi(&mix, &maximum) >= 0 || mbedtls_mpi_exp_mod(&shared, &mix, &crypto->exponents[index], &crypto->modulus, NULL) != 0 || mbedtls_mpi_write_string(&shared, 10, decimal, sizeof(decimal), &written) != 0 || !keys_session__mix(secret, (ubyte *)decimal, clib__strlen(decimal)) || !keys_session__mix(transcript, (ubyte *)crypto->mixes[index], clib__strlen(crypto->mixes[index])) || !keys_session__mix(transcript, (ubyte *)text, clib__strlen(text)))
        {
            goto done;
        }
    }
    if (crypto->policy.random_layer)
    {
        static const int candidates[] = { LEMON_AES256, LEMON_CHACHA20, LEMON_CAMELLIA256, LEMON_TWOFISH256, LEMON_SERPENT256, LEMON_TRIVIUM80 };
        ubyte iv[16], encrypted[32], plaintext[32];
        unsigned int matches = 0;
        if (!crypto_media__hex(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(probe, "iv")), iv, 16) || !crypto_media__hex(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(probe, "data")), encrypted, 32) || !crypto_media__hex(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(probe, "tag")), received, 32))
        {
            goto done;
        }
        for (unsigned int index = 0; index < sizeof(candidates) / sizeof(candidates[0]); index++)
        {
            clib__memcpy(plaintext, encrypted, 32);
            if (keys_session__random_probe(secret, transcript, candidates[index], iv, plaintext) && keys_session__random_probe_tag(secret, transcript, plaintext, expected) && mbedtls_ct_memcmp(expected, received, 32) == 0)
            {
                random_type = candidates[index];
                matches++;
            }
        }
        ciphers__clear(plaintext, sizeof(plaintext));
        if (matches != 1)
        {
            goto done;
        }
    }
    else if (probe)
    {
        goto done;
    }
    success = keys_session__derive(secret, transcript, crypto->policy.cipher, crypto->policy.layers, random_type, &crypto->negotiated) && crypto_media__hex(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "finished")), received, 32) && ciphers__hmac(crypto->negotiated.server_mac, transcript, 32, expected) && mbedtls_ct_memcmp(expected, received, 32) == 0;
    if (success)
    {
        clib__memcpy(crypto->session_keys, crypto->negotiated.client_to_server[0].bytes, 32);
        clib__memcpy(crypto->session_keys + 32, crypto->negotiated.client_mac, 32);
    }
done:
    mbedtls_mpi_free(&mix);
    mbedtls_mpi_free(&maximum);
    mbedtls_mpi_free(&shared);
    mbedtls_mpi_free(&crypto->exponent);
    for (unsigned int index = 0; index < LEMON_MAX_DH_EXCHANGES; index++)
    {
        mbedtls_mpi_free(&crypto->exponents[index]);
    }
    ciphers__clear(secret, sizeof(secret));
    ciphers__clear(decimal, sizeof(decimal));
    crypto->session_ready = success;
    if (!success)
    {
        ciphers__clear(&crypto->negotiated, sizeof(crypto->negotiated));
    }
    return success;
}
