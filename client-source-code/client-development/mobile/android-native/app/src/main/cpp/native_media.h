#ifndef LEMON_NATIVE_MEDIA_H
#define LEMON_NATIVE_MEDIA_H

#include <jni.h>
#include "crypto/crypto_media.h"

jlong native_media__create(const desktop_media_keys_t *keys);

#endif
