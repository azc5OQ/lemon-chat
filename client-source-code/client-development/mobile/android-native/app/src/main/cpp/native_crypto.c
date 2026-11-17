#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include "clib.h"
#include "cJSON.h"
#include "crypto/crypto_session.h"
#include "network/protocol.h"
#include "mbedtls/platform_util.h"
#include "native_media.h"

#define JNI_CRYPTO(name) Java_com_lemonchat_crypto_CryptoSession_##name
#define NATIVE_TEXT_LIMIT (64U * 1024U * 1024U)

typedef struct native_crypto_t
{
    desktop_crypto_t *crypto;
    char public_key[1400];
} native_crypto_t;

/**
 * @brief copies bounded Java bytes into terminated native storage
 *
 * @param JNIEnv* env -> current JNI environment
 * @param jbyteArray source -> Java input bytes
 * @param uint64 limit -> maximum accepted byte count
 * @param boole text -> reject embedded zero bytes
 * @param uint64* length -> receives the copied byte count
 *
 * @return ubyte* -> owned copy or NULL
 */
static ubyte *native_crypto__input(JNIEnv *env, jbyteArray source, uint64 limit, boole text, uint64 *length)
{
    *length = 0;
    if (!source)
    {
        return NULL;
    }
    jsize count = (*env)->GetArrayLength(env, source);
    if (count < 0 || (uint64)count > limit)
    {
        return NULL;
    }
    ubyte *bytes = calloc(1, (uint64)count + 1);
    if (!bytes)
    {
        return NULL;
    }
    (*env)->GetByteArrayRegion(env, source, 0, count, (jbyte *)bytes);
    if ((*env)->ExceptionCheck(env))
    {
        free(bytes);
        return NULL;
    }
    if (text)
    {
        for (jsize index = 0; index < count; index++)
        {
            if (bytes[index] == 0)
            {
                mbedtls_platform_zeroize(bytes, count);
                free(bytes);
                return NULL;
            }
        }
    }
    *length = (uint64)count;
    return bytes;
}

/**
 * @brief clears a temporary input allocation
 *
 * @param ubyte* bytes -> owned input or NULL
 * @param uint64 length -> allocation length without its terminator
 *
 * @return void
 */
static void native_crypto__release(ubyte *bytes, uint64 length)
{
    if (bytes)
    {
        mbedtls_platform_zeroize(bytes, length + 1);
        free(bytes);
    }
}

/**
 * @brief copies native output into a Java byte array
 *
 * @param JNIEnv* env -> current JNI environment
 * @param const void* bytes -> output bytes
 * @param uint64 length -> output byte count
 *
 * @return jbyteArray -> Java copy or NULL
 */
static jbyteArray native_crypto__output(JNIEnv *env, const void *bytes, uint64 length)
{
    if (!bytes || length > NATIVE_TEXT_LIMIT)
    {
        return NULL;
    }
    jbyteArray result = (*env)->NewByteArray(env, (jsize)length);
    if (result)
    {
        (*env)->SetByteArrayRegion(env, result, 0, (jsize)length, (const jbyte *)bytes);
    }
    return result;
}

/**
 * @brief creates a connection with the supplied identity digest
 *
 * @param JNIEnv* env -> current JNI environment
 * @param jclass type -> owning Java class
 * @param jbyteArray digest -> browser compatible SHA256 seed
 * @param jint bits -> RSA key size
 * @param jbyteArray server_keys -> ordered server key rows
 *
 * @return jlong -> owned native handle or zero
 */
JNIEXPORT jlong JNICALL JNI_CRYPTO(nativeCreate)(JNIEnv *env, jclass type, jbyteArray digest, jint bits, jbyteArray server_keys)
{
    (void)type;
    uint64 seed_length = 0;
    uint64 keys_length = 0;
    ubyte *seed = native_crypto__input(env, digest, 32, FALSE, &seed_length);
    ubyte *keys = native_crypto__input(env, server_keys, 65536, TRUE, &keys_length);
    native_crypto_t *state = NULL;
    if (seed && seed_length == 32 && keys)
    {
        state = calloc(1, sizeof(*state));
        if (state)
        {
            state->crypto = crypto_session__create();
            if (!state->crypto || !crypto_session__server_keys(state->crypto, (char *)keys) || !crypto_session__identity_seed(state->crypto, seed, bits, state->public_key, sizeof(state->public_key)))
            {
                crypto_session__destroy(state->crypto);
                mbedtls_platform_zeroize(state, sizeof(*state));
                free(state);
                state = NULL;
            }
        }
    }
    native_crypto__release(seed, seed_length);
    native_crypto__release(keys, keys_length);
    return (jlong)(intptr_t)state;
}

/**
 * @brief clears the identity and connection keys
 *
 * @param JNIEnv* env -> current JNI environment
 * @param jclass type -> owning Java class
 * @param jlong handle -> handle owned by the synchronized Java wrapper
 *
 * @return void
 */
JNIEXPORT void JNICALL JNI_CRYPTO(nativeDestroy)(JNIEnv *env, jclass type, jlong handle)
{
    (void)env;
    (void)type;
    native_crypto_t *state = (native_crypto_t *)(intptr_t)handle;
    if (state)
    {
        crypto_session__destroy(state->crypto);
        mbedtls_platform_zeroize(state, sizeof(*state));
        free(state);
    }
}

/**
 * @brief applies a bounded text operation to this connection
 *
 * @param JNIEnv* env -> current JNI environment
 * @param jclass type -> owning Java class
 * @param jlong handle -> synchronized connection handle
 * @param jint operation -> operation selected by the Java wrapper
 * @param jbyteArray first -> first UTF8 input or NULL
 * @param jbyteArray second -> second UTF8 input or NULL
 *
 * @return jbyteArray -> result bytes or NULL when rejected
 */
JNIEXPORT jbyteArray JNICALL JNI_CRYPTO(nativeText)(JNIEnv *env, jclass type, jlong handle, jint operation, jbyteArray first, jbyteArray second)
{
    (void)type;
    native_crypto_t *state = (native_crypto_t *)(intptr_t)handle;
    uint64 first_length = 0;
    uint64 second_length = 0;
    char *a = (char *)native_crypto__input(env, first, NATIVE_TEXT_LIMIT, TRUE, &first_length);
    char *b = (char *)native_crypto__input(env, second, NATIVE_TEXT_LIMIT, TRUE, &second_length);
    char buffer[DESKTOP_DH_TEXT_CAPACITY] = { 0 };
    desktop_content_keys_t keys = { 0 };
    char *result = NULL;
    char *inner = NULL;
    cJSON *json = NULL;
    cJSON *request = NULL;
    jbyteArray output = NULL;

    if (!state || (*env)->ExceptionCheck(env) || (first && !a) || (second && !b))
    {
        goto done;
    }
    if (operation == 0)
    {
        output = native_crypto__output(env, state->public_key, clib__strlen(state->public_key));
    }
    else if (operation == 1)
    {
        if (crypto_session__begin_dh(state->crypto, buffer, sizeof(buffer)))
        {
            output = native_crypto__output(env, buffer, clib__strlen(buffer));
        }
    }
    else if ((operation == 2 || operation == 3) && a)
    {
        json = desktop_protocol__parse_json(a, first_length, 65536, 8);
        if (cJSON_IsObject(json))
        {
            if (operation == 2)
            {
                request = crypto_session__negotiate(state->crypto, json, state->public_key);
                result = request ? cJSON_PrintUnformatted(request) : NULL;
            }
            else if (crypto_session__finish_negotiation(state->crypto, json))
            {
                output = native_crypto__output(env, "1", 1);
            }
        }
    }
    else if (operation == 4 && a)
    {
        if (crypto_session__challenge(state->crypto, a, buffer, sizeof(buffer)))
        {
            output = native_crypto__output(env, buffer, clib__strlen(buffer));
        }
    }
    else if (operation == 5 && a)
    {
        result = crypto_session__encrypt(state->crypto, a);
    }
    else if (operation == 6 && a)
    {
        result = crypto_session__decrypt(state->crypto, a, first_length);
    }
    else if (operation == 7)
    {
        if (crypto_session__content_keys(state->crypto, &keys))
        {
            result = crypto_session__keys_json(&keys);
        }
    }
    else if ((operation == 8 || operation == 9 || (operation >= 12 && operation <= 15)) && a && b)
    {
        if (!crypto_session__read_keys(a, &keys))
        {
            goto done;
        }
        if (operation == 8 || operation == 12 || operation == 14)
        {
            if (operation == 8)
            {
                inner = crypto_session__content_encrypt(state->crypto, &keys, b);
            }
            else if (operation == 12)
            {
                inner = crypto_session__picture_encrypt(state->crypto, &keys, b, 0);
            }
            else
            {
                inner = crypto_session__attachment_encrypt(state->crypto, &keys, b, 0);
            }
            result = inner ? crypto_session__channel_seal(&keys, inner) : NULL;
        }
        else
        {
            inner = crypto_session__channel_open(&keys, b);
            if (inner)
            {
                if (operation == 9)
                {
                    result = crypto_session__content_decrypt(&keys, inner);
                }
                else if (operation == 13)
                {
                    result = crypto_session__attachment_decrypt(&keys, inner);
                }
                else
                {
                    result = crypto_session__file_decrypt(&keys, inner);
                }
            }
        }
    }
    else if ((operation == 10 || operation == 16 || operation == 18) && a && b)
    {
        if (crypto_session__content_keys(state->crypto, &keys))
        {
            inner = crypto_session__direct_content_encrypt(&keys, b, operation == 10 ? 1024 : 0, operation == 18);
            result = inner ? crypto_session__direct_seal(state->crypto, a, &keys, inner) : NULL;
        }
    }
    else if ((operation == 11 || operation == 17 || operation == 19) && a)
    {
        json = desktop_protocol__parse_json(a, first_length, 40U * 1024U * 1024U, 4);
        inner = json ? crypto_session__direct_open(state->crypto, json, &keys) : NULL;
        result = inner ? crypto_session__direct_content_decrypt(&keys, inner, operation == 19) : NULL;
    }
    if (result)
    {
        output = native_crypto__output(env, result, clib__strlen(result));
    }
done:
    native_crypto__release((ubyte *)a, first_length);
    native_crypto__release((ubyte *)b, second_length);
    crypto_session__free_text(result);
    crypto_session__free_text(inner);
    crypto_session__free_json(json);
    crypto_session__free_json(request);
    mbedtls_platform_zeroize(buffer, sizeof(buffer));
    mbedtls_platform_zeroize(&keys, sizeof(keys));
    return output;
}

/**
 * @brief writes a counter for the Java media result
 *
 * @param ubyte* destination -> eight writable bytes
 * @param uint64 value -> unsigned counter bits
 *
 * @return void
 */
static void native_crypto__write_counter(ubyte *destination, uint64 value)
{
    for (uint64 index = 0; index < 8; index++)
    {
        destination[index] = (ubyte)(value >> (56 - index * 8));
    }
}

/**
 * @brief copies authenticated media keys while the Java chat context is locked
 *
 * @param JNIEnv* env -> current JNI environment
 * @param jclass type -> owning Java class
 * @param jlong handle -> synchronized chat context
 *
 * @return jlong -> independent media handle or zero
 */
JNIEXPORT jlong JNICALL JNI_CRYPTO(nativeCreateMedia)(JNIEnv *env, jclass type, jlong handle)
{
    (void)env;
    (void)type;
    native_crypto_t *state = (native_crypto_t *)(intptr_t)handle;
    desktop_media_keys_t keys = { 0 };
    jlong result = 0;
    if (state && crypto_session__media_keys(state->crypto, &keys))
    {
        result = native_media__create(&keys);
    }
    mbedtls_platform_zeroize(&keys, sizeof(keys));
    return result;
}

/**
 * @brief adds authenticated connection protection to outgoing media
 *
 * @param JNIEnv* env -> current JNI environment
 * @param jclass type -> owning Java class
 * @param jlong handle -> synchronized connection handle
 * @param jint sender -> authenticated local client ID
 * @param jlong counter -> unique outgoing counter
 * @param jbyteArray content -> channel encrypted audio
 *
 * @return jbyteArray -> sealed frame or NULL
 */
JNIEXPORT jbyteArray JNICALL JNI_CRYPTO(nativeSealMedia)(JNIEnv *env, jclass type, jlong handle, jint sender, jlong counter, jbyteArray content)
{
    (void)type;
    native_crypto_t *state = (native_crypto_t *)(intptr_t)handle;
    desktop_media_keys_t keys = { 0 };
    ubyte frame[DESKTOP_MEDIA_FRAME] = { 0 };
    uint64 length = 0;
    ubyte *bytes = native_crypto__input(env, content, DESKTOP_MEDIA_CONTENT, FALSE, &length);
    jbyteArray result = NULL;
    if (state && bytes && crypto_session__media_keys(state->crypto, &keys))
    {
        uint64 written = crypto_media__seal(&keys, (uint)sender, (uint64)counter, bytes, length, frame, sizeof(frame));
        if (written)
        {
            result = native_crypto__output(env, frame, written);
        }
    }
    native_crypto__release(bytes, length);
    mbedtls_platform_zeroize(&keys, sizeof(keys));
    mbedtls_platform_zeroize(frame, sizeof(frame));
    return result;
}

/**
 * @brief opens media and returns its authenticated counters
 *
 * @param JNIEnv* env -> current JNI environment
 * @param jclass type -> owning Java class
 * @param jlong handle -> synchronized connection handle
 * @param jbyteArray frame -> received version 2 frame
 *
 * @return jbyteArray -> sender, counters and content or NULL
 */
JNIEXPORT jbyteArray JNICALL JNI_CRYPTO(nativeOpenMedia)(JNIEnv *env, jclass type, jlong handle, jbyteArray frame)
{
    (void)type;
    native_crypto_t *state = (native_crypto_t *)(intptr_t)handle;
    desktop_media_keys_t keys = { 0 };
    ubyte output[DESKTOP_MEDIA_CONTENT + 20] = { 0 };
    uint sender = 0;
    uint64 sender_counter = 0;
    uint64 frame_counter = 0;
    uint64 length = 0;
    ubyte *bytes = native_crypto__input(env, frame, DESKTOP_MEDIA_FRAME, FALSE, &length);
    jbyteArray result = NULL;
    if (state && bytes && crypto_session__media_keys(state->crypto, &keys))
    {
        uint64 written = crypto_media__open(&keys, bytes, length, &sender, &sender_counter, &frame_counter, output + 20, DESKTOP_MEDIA_CONTENT);
        if (written)
        {
            output[0] = (ubyte)(sender >> 24);
            output[1] = (ubyte)(sender >> 16);
            output[2] = (ubyte)(sender >> 8);
            output[3] = (ubyte)sender;
            native_crypto__write_counter(output + 4, sender_counter);
            native_crypto__write_counter(output + 12, frame_counter);
            result = native_crypto__output(env, output, written + 20);
        }
    }
    native_crypto__release(bytes, length);
    mbedtls_platform_zeroize(&keys, sizeof(keys));
    mbedtls_platform_zeroize(output, sizeof(output));
    return result;
}

/**
 * @brief creates the one use audio socket proof
 *
 * @param JNIEnv* env -> current JNI environment
 * @param jclass type -> owning Java class
 * @param jbyteArray key_text -> hexadecimal authentication key
 * @param jbyteArray ticket_text -> hexadecimal ticket
 * @param jint id -> local client ID
 *
 * @return jbyteArray -> proof bytes or NULL
 */
JNIEXPORT jbyteArray JNICALL JNI_CRYPTO(nativeMediaProof)(JNIEnv *env, jclass type, jbyteArray key_text, jbyteArray ticket_text, jint id)
{
    (void)type;
    uint64 key_length = 0;
    uint64 ticket_length = 0;
    ubyte *key_string = native_crypto__input(env, key_text, 64, TRUE, &key_length);
    ubyte *ticket_string = native_crypto__input(env, ticket_text, 32, TRUE, &ticket_length);
    ubyte key[32] = { 0 };
    ubyte ticket[16] = { 0 };
    ubyte proof[53] = { 0 };
    jbyteArray result = NULL;
    if (crypto_media__hex((char *)key_string, key, sizeof(key)) && crypto_media__hex((char *)ticket_string, ticket, sizeof(ticket)) && crypto_media__proof(key, ticket, (uint)id, proof))
    {
        result = native_crypto__output(env, proof, sizeof(proof));
    }
    native_crypto__release(key_string, key_length);
    native_crypto__release(ticket_string, ticket_length);
    mbedtls_platform_zeroize(key, sizeof(key));
    mbedtls_platform_zeroize(ticket, sizeof(ticket));
    mbedtls_platform_zeroize(proof, sizeof(proof));
    return result;
}

/**
 * @brief verifies the expected audio ticket acknowledgement
 *
 * @param JNIEnv* env -> current JNI environment
 * @param jclass type -> owning Java class
 * @param jbyteArray key_text -> hexadecimal authentication key
 * @param jbyteArray ticket_text -> hexadecimal ticket
 * @param jbyteArray frame -> received acknowledgement
 *
 * @return jboolean -> true for an authentic matching ticket
 */
JNIEXPORT jboolean JNICALL JNI_CRYPTO(nativeVerifyMediaAcknowledgement)(JNIEnv *env, jclass type, jbyteArray key_text, jbyteArray ticket_text, jbyteArray frame)
{
    (void)type;
    uint64 key_length = 0;
    uint64 ticket_length = 0;
    uint64 frame_length = 0;
    ubyte *key_string = native_crypto__input(env, key_text, 64, TRUE, &key_length);
    ubyte *ticket_string = native_crypto__input(env, ticket_text, 32, TRUE, &ticket_length);
    ubyte *bytes = native_crypto__input(env, frame, 49, FALSE, &frame_length);
    ubyte key[32] = { 0 };
    ubyte ticket[16] = { 0 };
    boole valid = crypto_media__hex((char *)key_string, key, sizeof(key)) && crypto_media__hex((char *)ticket_string, ticket, sizeof(ticket)) && crypto_media__acknowledgement(key, ticket, bytes, frame_length);
    native_crypto__release(key_string, key_length);
    native_crypto__release(ticket_string, ticket_length);
    native_crypto__release(bytes, frame_length);
    mbedtls_platform_zeroize(key, sizeof(key));
    mbedtls_platform_zeroize(ticket, sizeof(ticket));
    return valid ? JNI_TRUE : JNI_FALSE;
}

/**
 * @brief applies the current channel's four inner audio layers
 *
 * @param JNIEnv* env -> current JNI environment
 * @param jclass type -> owning Java class
 * @param jbyteArray keys_text -> channel key array JSON
 * @param jint sender -> audio sender ID
 * @param jlong counter -> sender counter
 * @param jbyteArray content -> input audio bytes
 * @param jboolean encrypt -> true for sending
 *
 * @return jbyteArray -> transformed copy or NULL
 */
JNIEXPORT jbyteArray JNICALL JNI_CRYPTO(nativeContentMediaLayer)(JNIEnv *env, jclass type, jbyteArray keys_text, jint sender, jlong counter, jbyteArray content, jboolean encrypt)
{
    (void)type;
    uint64 keys_length = 0;
    uint64 content_length = 0;
    ubyte *keys_string = native_crypto__input(env, keys_text, 16384, TRUE, &keys_length);
    ubyte *bytes = native_crypto__input(env, content, DESKTOP_MEDIA_CONTENT, FALSE, &content_length);
    desktop_content_keys_t keys = { 0 };
    jbyteArray result = NULL;
    if (keys_string && bytes && crypto_session__read_keys((char *)keys_string, &keys))
    {
        for (int index = 0; index < 4; index++)
        {
            int selected = encrypt ? index : 3 - index;
            crypto_media__layer(keys.keys[selected], keys.ivs[selected], (uint)sender, (uint64)counter, bytes, content_length);
        }
        result = native_crypto__output(env, bytes, content_length);
    }
    native_crypto__release(keys_string, keys_length);
    native_crypto__release(bytes, content_length);
    mbedtls_platform_zeroize(&keys, sizeof(keys));
    return result;
}

/**
 * @brief applies one channel or music bot AES layer
 *
 * @param JNIEnv* env -> current JNI environment
 * @param jclass type -> owning Java class
 * @param jbyteArray key_array -> 32 byte AES key
 * @param jbyteArray iv_array -> channel IV or NULL for music bots
 * @param jint sender -> audio sender ID
 * @param jlong counter -> sender counter
 * @param jbyteArray content -> input audio bytes
 *
 * @return jbyteArray -> transformed copy or NULL
 */
JNIEXPORT jbyteArray JNICALL JNI_CRYPTO(nativeAesMediaLayer)(JNIEnv *env, jclass type, jbyteArray key_array, jbyteArray iv_array, jint sender, jlong counter, jbyteArray content)
{
    (void)type;
    uint64 key_length = 0;
    uint64 iv_length = 0;
    uint64 content_length = 0;
    ubyte *key = native_crypto__input(env, key_array, 32, FALSE, &key_length);
    ubyte *iv = native_crypto__input(env, iv_array, 16, FALSE, &iv_length);
    ubyte *bytes = native_crypto__input(env, content, DESKTOP_MEDIA_CONTENT, FALSE, &content_length);
    jbyteArray result = NULL;
    if (key && key_length == 32 && (!iv_array || (iv && iv_length == 16)) && bytes)
    {
        crypto_media__layer(key, iv, (uint)sender, (uint64)counter, bytes, content_length);
        result = native_crypto__output(env, bytes, content_length);
    }
    native_crypto__release(key, key_length);
    native_crypto__release(iv, iv_length);
    native_crypto__release(bytes, content_length);
    return result;
}
