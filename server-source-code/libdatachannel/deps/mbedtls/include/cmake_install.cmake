# Install script for directory: C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "C:/Program Files (x86)/test0s_solution")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set default install directory permissions.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "C:/Program Files/JetBrains/CLion 2022.2/bin/mingw/bin/objdump.exe")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/mbedtls" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ FILES
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/aes.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/aria.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/asn1.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/asn1write.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/base64.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/bignum.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/build_info.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/camellia.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/ccm.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/chacha20.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/chachapoly.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/check_config.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/cipher.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/cmac.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/compat-2.x.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/config_adjust_legacy_crypto.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/config_adjust_legacy_from_psa.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/config_adjust_psa_from_legacy.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/config_adjust_psa_superset_legacy.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/config_adjust_ssl.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/config_adjust_x509.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/config_psa.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/constant_time.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/ctr_drbg.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/debug.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/des.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/dhm.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/ecdh.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/ecdsa.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/ecjpake.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/ecp.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/entropy.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/error.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/gcm.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/hkdf.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/hmac_drbg.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/lms.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/mbedtls_config.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/md.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/md5.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/memory_buffer_alloc.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/net_sockets.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/nist_kw.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/oid.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/pem.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/pk.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/pkcs12.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/pkcs5.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/pkcs7.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/platform.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/platform_time.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/platform_util.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/poly1305.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/private_access.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/psa_util.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/ripemd160.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/rsa.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/sha1.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/sha256.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/sha3.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/sha512.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/ssl.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/ssl_cache.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/ssl_ciphersuites.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/ssl_cookie.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/ssl_ticket.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/threading.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/timing.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/version.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/x509.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/x509_crl.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/x509_crt.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/mbedtls/x509_csr.h"
    )
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/psa" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ FILES
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/psa/build_info.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/psa/crypto.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/psa/crypto_adjust_auto_enabled.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/psa/crypto_adjust_config_key_pair_types.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/psa/crypto_adjust_config_synonyms.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/psa/crypto_builtin_composites.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/psa/crypto_builtin_key_derivation.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/psa/crypto_builtin_primitives.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/psa/crypto_compat.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/psa/crypto_config.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/psa/crypto_driver_common.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/psa/crypto_driver_contexts_composites.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/psa/crypto_driver_contexts_key_derivation.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/psa/crypto_driver_contexts_primitives.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/psa/crypto_extra.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/psa/crypto_legacy.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/psa/crypto_platform.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/psa/crypto_se_driver.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/psa/crypto_sizes.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/psa/crypto_struct.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/psa/crypto_types.h"
    "C:/Users/user/Desktop/lemon-chat-server-nice-build/project-root/mbedtls-3.5.1/include/psa/crypto_values.h"
    )
endif()

