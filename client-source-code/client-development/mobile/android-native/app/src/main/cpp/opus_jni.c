#include <jni.h>
#include <stdint.h>
#include <opus.h>

/**
 * @brief creates the bounded voice encoder or stereo decoder
 *
 * @param JNIEnv* env -> JNI environment
 * @param jclass type -> codec class
 * @param jboolean encoder -> true for encoding
 * @param jint rate -> sample rate
 * @param jint channels -> channel count
 *
 * @return jlong -> codec handle or zero
 */
JNIEXPORT jlong JNICALL Java_com_lemonchat_audio_OpusCodec_nativeCreate(JNIEnv *env, jclass type, jboolean encoder, jint rate, jint channels)
{
    int error = OPUS_OK;
    (void)env;
    (void)type;
    if (encoder)
    {
        OpusEncoder *codec = NULL;
        if ((rate != 48000 && rate != 16000 && rate != 8000) || (channels != 1 && channels != 2))
        {
            return 0;
        }
        codec = opus_encoder_create(rate, channels, channels == 2 ? OPUS_APPLICATION_AUDIO : OPUS_APPLICATION_VOIP, &error);
        if (codec == NULL || error != OPUS_OK)
        {
            if (codec != NULL)
            {
                opus_encoder_destroy(codec);
            }
            return 0;
        }
        if (opus_encoder_ctl(codec, OPUS_SET_BITRATE(channels == 2 ? 128000 : 32000)) != OPUS_OK || opus_encoder_ctl(codec, OPUS_SET_COMPLEXITY(5)) != OPUS_OK || opus_encoder_ctl(codec, OPUS_SET_SIGNAL(channels == 2 ? OPUS_SIGNAL_MUSIC : OPUS_SIGNAL_VOICE)) != OPUS_OK || opus_encoder_ctl(codec, OPUS_SET_VBR(1)) != OPUS_OK)
        {
            opus_encoder_destroy(codec);
            return 0;
        }
        return (jlong)(intptr_t)codec;
    }
    if (rate != 48000 || channels != 2)
    {
        return 0;
    }
    OpusDecoder *codec = opus_decoder_create(rate, channels, &error);
    if (codec == NULL || error != OPUS_OK)
    {
        if (codec != NULL)
        {
            opus_decoder_destroy(codec);
        }
        return 0;
    }
    return (jlong)(intptr_t)codec;
}

/**
 * @brief encodes one bounded frame without holding Java array pointers
 *
 * @param JNIEnv* env -> JNI environment
 * @param jclass type -> codec class
 * @param jlong handle -> live encoder
 * @param jshortArray pcm -> mono samples
 * @param jint samples -> frame sample count
 * @param jint channels -> channel count used when creating the encoder
 *
 * @return jbyteArray -> encoded packet or null
 */
JNIEXPORT jbyteArray JNICALL Java_com_lemonchat_audio_OpusCodec_nativeEncode(JNIEnv *env, jclass type, jlong handle, jshortArray pcm, jint samples, jint channels)
{
    opus_int16 input[1920];
    unsigned char packet[1275];
    jbyteArray result = NULL;
    int size = 0;
    (void)type;
    if (handle == 0 || pcm == NULL || samples < 1 || samples > 960 || (channels != 1 && channels != 2) || (*env)->GetArrayLength(env, pcm) < samples * channels)
    {
        return NULL;
    }
    (*env)->GetShortArrayRegion(env, pcm, 0, samples * channels, input);
    if ((*env)->ExceptionCheck(env))
    {
        return NULL;
    }
    size = opus_encode((OpusEncoder *)(intptr_t)handle, input, samples, packet, sizeof(packet));
    if (size <= 0)
    {
        return NULL;
    }
    result = (*env)->NewByteArray(env, size);
    if (result != NULL)
    {
        (*env)->SetByteArrayRegion(env, result, 0, size, (const jbyte *)packet);
    }
    return result;
}

/**
 * @brief decodes a bounded packet to at most 120 ms of stereo audio
 *
 * @param JNIEnv* env -> JNI environment
 * @param jclass type -> codec class
 * @param jlong handle -> live decoder
 * @param jbyteArray packet -> encoded packet
 *
 * @return jshortArray -> stereo samples or null for invalid audio
 */
JNIEXPORT jshortArray JNICALL Java_com_lemonchat_audio_OpusCodec_nativeDecode(JNIEnv *env, jclass type, jlong handle, jbyteArray packet)
{
    unsigned char input[3838];
    opus_int16 output[5760 * 2];
    jshortArray result = NULL;
    jsize size = 0;
    int samples = 0;
    (void)type;
    if (handle == 0 || packet == NULL)
    {
        return NULL;
    }
    size = (*env)->GetArrayLength(env, packet);
    if (size < 1 || size > (jsize)sizeof(input))
    {
        return NULL;
    }
    (*env)->GetByteArrayRegion(env, packet, 0, size, (jbyte *)input);
    if ((*env)->ExceptionCheck(env))
    {
        return NULL;
    }
    samples = opus_decode((OpusDecoder *)(intptr_t)handle, input, size, output, 5760, 0);
    if (samples <= 0 || samples > 5760)
    {
        return NULL;
    }
    result = (*env)->NewShortArray(env, samples * 2);
    if (result != NULL)
    {
        (*env)->SetShortArrayRegion(env, result, 0, samples * 2, output);
    }
    return result;
}

/**
 * @brief releases a codec after its Java owner stops using it
 *
 * @param JNIEnv* env -> JNI environment
 * @param jclass type -> codec class
 * @param jlong handle -> codec handle
 * @param jboolean encoder -> codec kind
 *
 * @return void
 */
JNIEXPORT void JNICALL Java_com_lemonchat_audio_OpusCodec_nativeDestroy(JNIEnv *env, jclass type, jlong handle, jboolean encoder)
{
    (void)env;
    (void)type;
    if (handle == 0)
    {
        return;
    }
    if (encoder)
    {
        opus_encoder_destroy((OpusEncoder *)(intptr_t)handle);
    }
    else
    {
        opus_decoder_destroy((OpusDecoder *)(intptr_t)handle);
    }
}
