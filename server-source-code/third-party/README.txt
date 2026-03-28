The third-party directory contains libraries used by the LemonChat server.
Libraries in this directory can be divided into categories based on how they are compiled and how they are used.


Based on how they are compiled:
Libraries that contain their own CMakeLists.txt file.
Libraries that do not contain their own CMakeLists.txt file.

Based on how they are used:
Libraries that are direct dependencies of the main project.
Libraries that are dependencies of other libraries.
Libraries that are dependencies of both the main project and other libraries.


Libraries that contain their own CMakeLists.txt file
These libraries are independent projects, separate from the main LemonChat project. They are built into either static or dynamic libraries.

Static libraries have the extensions .a or .lib.
Dynamic libraries have the extensions .so or .dll.

On Windows, dynamic libraries use .dll files. On Linux, they use .so files. For static libraries, .a files are common on both platforms depending on the toolchain (MinGW typically produces .a files, while MSVC produces .lib files).
List of libraries in this category:

libopus-1.5.2 – static
mbedtls-3.5.1 – static
libviolet – static
theldus-websocket – static
libtom – static
libdatachannel – dynamic
libmaxminddb-1.12.2 – static


Libraries that do not contain their own CMakeLists.txt file
These libraries are usually small (often just a few files) and are included directly into the main project. They were previously located in the main directory but were moved here for better organization and cleanliness.
List of libraries in this category:

zhicheng
rxi-log
ITH-SHA
dave-g-json
eteran-cvector
dr_mp3 (single header file)
kokke-tiny-aes-c


As mentioned earlier, libraries can also be categorized based on how they are used. For example, libdatachannel depends on libviolet and mbedtls, while the main project also uses libviolet directly.
TODO: Create a dependency tree showing the relationships between libraries.
Purpose of each library

dave-g-json → JSON parsing and handling
dr_mp3 → Used by music bots to decode MP3 files to PCM (when streaming songs)
eteran-cvector → A lightweight vector-like container (similar to std::vector), currently experimental
ITH-SHA → Cryptographic hash functions (SHA)
kokke-tiny-aes-c → Small AES encryption implementation
libdatachannel → Enables the server to send WebRTC DataChannel data (used for audio transmission)
libmaxminddb-1.12.2 → Allows local identification of an IP address’s country from a .mmdb file. Used to display country flags when enabled.
libopus-1.5.2 → Used by music bots to encode PCM audio to Opus format.
libtom → Cryptographic functionality
libviolet → Used for TURN/STUN functionality in the main project and is also a dependency of libdatachannel
mbedtls-3.5.1 → Dependency of libdatachannel and also used for general cryptographic operations
rxi-log → Logging functionality
theldus-websocket → WebSocket functionality
zhicheng → Base64 encoding/decoding