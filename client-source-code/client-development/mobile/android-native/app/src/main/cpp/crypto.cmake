set(native_desktop "${LEMON_ROOT}/client-source-code/client-development/desktop")
set(native_third_party "${native_desktop}/third-party")

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
add_subdirectory("${LEMON_ROOT}/server-source-code/third-party/mbedtls-3.6.6" "${CMAKE_CURRENT_BINARY_DIR}/mbedtls")

# mbedtls reads random bytes with the getrandom system call. A kernel older than 3.17 has no getrandom,
# and mbedtls then reads /dev/random, which blocks on such a kernel until the kernel has collected enough
# entropy; the connection then stays at "Preparing your identity" (seen on a phone with kernel 3.4).
# /dev/urandom does not block and is the random source of Android's own libraries.
target_compile_definitions(mbedcrypto PRIVATE MBEDTLS_PLATFORM_DEV_RANDOM="/dev/urandom")

add_library(MbedTLS::MbedTLS INTERFACE IMPORTED GLOBAL)
set_target_properties(MbedTLS::MbedTLS PROPERTIES INTERFACE_LINK_LIBRARIES "mbedtls;mbedx509;mbedcrypto")

target_sources(lemonnative PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/native_crypto.c"
    "${CMAKE_CURRENT_LIST_DIR}/native_media.c"
    "${CMAKE_CURRENT_LIST_DIR}/native_platform.c"
    "${native_desktop}/main/clib/clib.c"
    "${native_desktop}/main/platform/memory.c"
    "${native_desktop}/main/network/protocol.c"
    "${native_desktop}/main/crypto/crypto_session.c"
    "${native_desktop}/main/crypto/crypto_media.c"
    "${native_desktop}/main/crypto/ciphers.c"
    "${native_desktop}/main/crypto/keys_session.c"
    "${native_desktop}/main/crypto/cipher_aes.c"
    "${native_desktop}/main/crypto/cipher_camellia.c"
    "${native_desktop}/main/crypto/cipher_chacha20.c"
    "${native_desktop}/main/crypto/cipher_twofish.c"
    "${native_desktop}/main/crypto/cipher_serpent.c"
    "${native_desktop}/main/crypto/cipher_trivium.c"
    "${native_third_party}/identity_keygen.c"
    "${native_third_party}/dave-g-json/cJSON.c"
    "${native_third_party}/kokke-tiny-aes-c/aes.c")

target_include_directories(lemonnative PRIVATE
    "${native_desktop}/main"
    "${native_desktop}/main/clib"
    "${native_third_party}"
    "${native_third_party}/dave-g-json"
    "${native_third_party}/libtomcrypt/src/headers")
target_link_libraries(lemonnative PRIVATE mbedtls mbedx509 mbedcrypto)

if(WIN32)
    target_link_libraries(lemonnative PRIVATE bcrypt)
else()
    target_link_libraries(lemonnative PRIVATE m)
endif()
