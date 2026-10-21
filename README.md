# A chat system you can host yourself

## Features:

- lightweight C server! built to run on different operating systems and CPUs
- a client in a single .html file! <br>
  open it from your desktop like an app, or serve it from a website <br>
- user management! (change usernames, kick, ban)
- channel management! (create, delete, and edit channels)
- direct messages (text and pictures)
- channel messages (text and pictures)
- sending files
- voice chat in channels
- end-to-end encryption
- Android app
- screen sharing

<br>

## Other fun features

- select an MP3 file from your computer and play it in a channel
- choose a theme
- rearrange the interface
- change the font size and color of messages for both you and the people receiving them
- add groups/tags to other users
- create music bots

## Live demo -> https://fruitybackendtesting.com/client.html
<br>

## How to start the server:

- clone the repository and run `windows_build_script.bat` or `linux_build_script.sh` from `server-source-code`
- or download a prebuilt release
- watch `how-to-build-and-use-video.mkv`, included in the repository
- for general setup and hosting instructions, see the [self-hosting guide](selfhosting-tools/README.md): Tailscale Funnel, Cloudflare Tunnel, ngrok, direct hosting and WireGuard with a VPS


## How to start the client:

- open `client.html` in your browser



<br>
<br>

## Example pictures

![](https://raw.githubusercontent.com/azc5OQ/lemon-chat/master/example/pic2.png)


![](https://raw.githubusercontent.com/azc5OQ/lemon-chat/master/example/test1.PNG)



![](https://raw.githubusercontent.com/azc5OQ/lemon-chat/master/example/pic4.png)


![](https://raw.githubusercontent.com/azc5OQ/lemon-chat/master/example/pic5.PNG)

![](https://raw.githubusercontent.com/azc5OQ/lemon-chat/master/example/android.jpg)

<br>

### To support further development of this project, consider giving it a ⭐
<br>
<br>
<br>

## More technical notes

I made this chat to be similar to TeamSpeak and Mumble, which you can see in the interface.<br>
Voice chat is optional and can be enabled by the admin in each channel. Text, pictures, and audio sent between users are end-to-end encrypted.<br>
This chat does not rely on TLS/SSL alone. Messages are encrypted between users even when TLS/SSL is not used, and the server relays them without having the keys to read them.<br>
WebSocket is used for text and pictures. Audio and video can use either WebSocket or WebRTC DataChannel. Only the DataChannel part of WebRTC is used; clients connect to the server, not to each other, so they do not share their IP addresses with other users through WebRTC.<br>
The `client.html` file has everything embedded in it. Open it in your browser, enter the server's IP address and port, and connect. It can also be packaged into a desktop app using a tool like Electron if needed.<br>
The server should be easy to build if the correct build tools are installed. The C/C++ libraries needed to build it are already included in this repository.<br>
Use the `.bat` build script on Windows or the `.sh` script for your system.



<br>
<br>


<br>

## Can the client.html file be used on a website?

Yes, just follow the setup instructions when you start the server.

<br />
<br />


# Thanks to these projects and people for providing some of the source code this project uses:

### Client:
cryptico -> https://github.com/wwwtyro/cryptico
<br>
aes-js -> https://github.com/ricmoo/aes-js
<br>
js-sha256 -> https://github.com/emn178/js-sha256
<br>
SHA-256 by Angel Marin and Paul Johnston, from WebToolkit -> [source](client-source-code/client-development/browser/src/scripts/vendor/sha256.js)
<br>
minimp3-wasm -> https://github.com/bashi/minimp3-wasm
<br>
minimp3 -> https://github.com/lieff/minimp3
<br>
Opus -> https://opus-codec.org/
<br>
Speex resampler -> https://github.com/xiph/speexdsp
<br>
ws-audio-api -> https://github.com/Ivan-Feofanov/ws-audio-api
<br>
Node.js Mobile (Android app) -> https://github.com/nodejs-mobile/nodejs-mobile


### Server:
wsServer -> https://github.com/Theldus/wsServer
<br>
cJSON -> https://github.com/DaveGamble/cJSON
<br>
libdatachannel -> https://github.com/paullouisageneau/libdatachannel
<br>
libviolet -> https://github.com/paullouisageneau/violet
<br>
libjuice -> https://github.com/paullouisageneau/libjuice
<br>
usrsctp (used by libdatachannel) -> https://github.com/sctplab/usrsctp
<br>
plog (used by libdatachannel) -> https://github.com/SergiusTheBest/plog
<br>
mbedtls -> https://github.com/Mbed-TLS/mbedtls
<br>
LibTomMath -> https://github.com/libtom/libtommath
<br>
tiny-AES-c -> https://github.com/kokke/tiny-AES-c
<br>
Opus -> https://opus-codec.org/
<br>
dr_mp3 -> https://github.com/mackron/dr_libs
<br>
libmaxminddb -> https://github.com/maxmind/libmaxminddb
<br>
cvector -> https://github.com/eteran/c-vector
<br>
log.c -> https://github.com/rxi/log
<br>
SHA-256 by ITH -> [source](server-source-code/third-party/ITH-sha/sha256.c)
<br>
Base64 by zhicheng -> [source](server-source-code/third-party/zhicheng/base64.c)

### Optional HTTPS/WSS:
stunnel -> https://www.stunnel.org/
<br>
OpenSSL -> https://www.openssl.org/


