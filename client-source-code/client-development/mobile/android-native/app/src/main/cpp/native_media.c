#include "native_media.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include "clib.h"
#include "mbedtls/platform_util.h"

#define JNI_MEDIA(name) Java_com_lemonchat_crypto_MediaCrypto_##name
#define MEDIA_CONTEXT_LIMIT 64

typedef struct native_media_t
{
    desktop_media_keys_t keys;
    uint64 id;
    atomic_uint references;
} native_media_t;

static native_media_t *g_media_contexts[MEDIA_CONTEXT_LIMIT];
static atomic_flag g_media_lock = ATOMIC_FLAG_INIT;
static uint64 g_media_next_id = 1;

/**
 * @brief locks the short handle lookup
 *
 * @return void
 */
static void native_media__lock(void)
{
    while (atomic_flag_test_and_set_explicit(&g_media_lock, memory_order_acquire))
    {
    }
}

/**
 * @brief releases the handle lookup lock
 *
 * @return void
 */
static void native_media__unlock(void)
{
    atomic_flag_clear_explicit(&g_media_lock, memory_order_release);
}

/**
 * @brief drops one context reference and clears its keys after the last operation
 *
 * @param native_media_t* state -> context owned by this caller
 *
 * @return void
 */
static void native_media__release(native_media_t *state)
{
    if (state && atomic_fetch_sub_explicit(&state->references, 1, memory_order_acq_rel) == 1)
    {
        mbedtls_platform_zeroize(state, sizeof(*state));
        free(state);
    }
}

/**
 * @brief looks up an opaque handle without dereferencing caller supplied pointers
 *
 * @param jlong handle -> live registry identifier
 *
 * @return native_media_t* -> retained context or NULL
 */
static native_media_t *native_media__acquire(jlong handle)
{
    native_media_t *found = NULL;
    if (handle <= 0)
    {
        return NULL;
    }
    native_media__lock();
    for (uint index = 0; index < MEDIA_CONTEXT_LIMIT; index++)
    {
        native_media_t *state = g_media_contexts[index];
        if (state && state->id == (uint64)handle)
        {
            atomic_fetch_add_explicit(&state->references, 1, memory_order_relaxed);
            found = state;
            break;
        }
    }
    native_media__unlock();
    return found;
}

/**
 * @brief copies negotiated media keys into an independent bounded context
 *
 * @param const desktop_media_keys_t* keys -> authenticated connection media keys
 *
 * @return jlong -> owned opaque handle or zero
 */
jlong native_media__create(const desktop_media_keys_t *keys)
{
    if (!keys || keys->server_count > 8 || keys->negotiated.count > LEMON_MAX_CONNECTION_LAYERS)
    {
        return 0;
    }
    native_media_t *state = calloc(1, sizeof(*state));
    if (!state)
    {
        return 0;
    }
    clib__copy_memory(keys, &state->keys, sizeof(*keys), sizeof(state->keys));
    atomic_init(&state->references, 1);
    uint64 id = 0;
    native_media__lock();
    for (uint index = 0; index < MEDIA_CONTEXT_LIMIT && g_media_next_id <= INT64_MAX; index++)
    {
        if (!g_media_contexts[index])
        {
            state->id = g_media_next_id++;
            id = state->id;
            g_media_contexts[index] = state;
            break;
        }
    }
    native_media__unlock();
    if (!id)
    {
        native_media__release(state);
        return 0;
    }
    return (jlong)id;
}

/**
 * @brief retires a handle while existing operations finish with retained references
 *
 * @param JNIEnv* env -> current JNI environment
 * @param jclass type -> owning Java class
 * @param jlong handle -> opaque media handle
 *
 * @return void
 */
JNIEXPORT void JNICALL JNI_MEDIA(nativeDestroy)(JNIEnv *env, jclass type, jlong handle)
{
    (void)env;
    (void)type;
    native_media_t *found = NULL;
    native_media__lock();
    for (uint index = 0; index < MEDIA_CONTEXT_LIMIT; index++)
    {
        if (g_media_contexts[index] && g_media_contexts[index]->id == (uint64)handle)
        {
            found = g_media_contexts[index];
            g_media_contexts[index] = NULL;
            break;
        }
    }
    native_media__unlock();
    native_media__release(found);
}

/**
 * @brief copies a bounded frame to Java
 *
 * @param JNIEnv* env -> current JNI environment
 * @param const ubyte* bytes -> bounded native frame
 * @param uint64 size -> copied byte count
 *
 * @return jbyteArray -> Java frame or NULL
 */
static jbyteArray native_media__output(JNIEnv *env, const ubyte *bytes, uint64 size)
{
    jbyteArray result = (*env)->NewByteArray(env, (jsize)size);
    if (result)
    {
        (*env)->SetByteArrayRegion(env, result, 0, (jsize)size, (const jbyte *)bytes);
    }
    return result;
}

/**
 * @brief seals audio with copied connection keys
 *
 * @param JNIEnv* env -> current JNI environment
 * @param jclass type -> owning Java class
 * @param jlong handle -> opaque media handle
 * @param jint sender -> authenticated local client ID
 * @param jlong counter -> unique unsigned frame counter
 * @param jbyteArray content -> channel encrypted audio
 *
 * @return jbyteArray -> sealed frame or NULL
 */
JNIEXPORT jbyteArray JNICALL JNI_MEDIA(nativeSeal)(JNIEnv *env, jclass type, jlong handle, jint sender, jlong counter, jbyteArray content)
{
    (void)type;
    if (!content || sender < 0)
    {
        return NULL;
    }
    jsize size = (*env)->GetArrayLength(env, content);
    if (size < 3 || size > DESKTOP_MEDIA_CONTENT)
    {
        return NULL;
    }
    native_media_t *state = native_media__acquire(handle);
    if (!state)
    {
        return NULL;
    }
    ubyte bytes[DESKTOP_MEDIA_CONTENT] = { 0 };
    ubyte frame[DESKTOP_MEDIA_FRAME] = { 0 };
    jbyteArray result = NULL;
    (*env)->GetByteArrayRegion(env, content, 0, size, (jbyte *)bytes);
    if (!(*env)->ExceptionCheck(env))
    {
        uint64 written = crypto_media__seal(&state->keys, (uint)sender, (uint64)counter, bytes, (uint64)size, frame, sizeof(frame));
        if (written)
        {
            result = native_media__output(env, frame, written);
        }
    }
    mbedtls_platform_zeroize(bytes, sizeof(bytes));
    mbedtls_platform_zeroize(frame, sizeof(frame));
    native_media__release(state);
    return result;
}

/**
 * @brief opens authenticated audio without accessing the chat context
 *
 * @param JNIEnv* env -> current JNI environment
 * @param jclass type -> owning Java class
 * @param jlong handle -> opaque media handle
 * @param jbyteArray frame -> received version two frame
 *
 * @return jbyteArray -> authenticated sender, counters and content or NULL
 */
JNIEXPORT jbyteArray JNICALL JNI_MEDIA(nativeOpen)(JNIEnv *env, jclass type, jlong handle, jbyteArray frame)
{
    (void)type;
    if (!frame)
    {
        return NULL;
    }
    jsize size = (*env)->GetArrayLength(env, frame);
    if (size < DESKTOP_MEDIA_OVERHEAD + 3 || size > DESKTOP_MEDIA_FRAME)
    {
        return NULL;
    }
    native_media_t *state = native_media__acquire(handle);
    if (!state)
    {
        return NULL;
    }
    ubyte bytes[DESKTOP_MEDIA_FRAME] = { 0 };
    ubyte output[DESKTOP_MEDIA_CONTENT + 20] = { 0 };
    uint sender = 0;
    uint64 sender_counter = 0;
    uint64 frame_counter = 0;
    jbyteArray result = NULL;
    (*env)->GetByteArrayRegion(env, frame, 0, size, (jbyte *)bytes);
    if (!(*env)->ExceptionCheck(env))
    {
        uint64 written = crypto_media__open(&state->keys, bytes, (uint64)size, &sender, &sender_counter, &frame_counter, output + 20, DESKTOP_MEDIA_CONTENT);
        if (written)
        {
            for (uint index = 0; index < 4; index++)
            {
                output[index] = (ubyte)(sender >> (24 - index * 8));
            }
            for (uint index = 0; index < 8; index++)
            {
                output[4 + index] = (ubyte)(sender_counter >> (56 - index * 8));
                output[12 + index] = (ubyte)(frame_counter >> (56 - index * 8));
            }
            result = native_media__output(env, output, written + 20);
        }
    }
    mbedtls_platform_zeroize(bytes, sizeof(bytes));
    mbedtls_platform_zeroize(output, sizeof(output));
    native_media__release(state);
    return result;
}
