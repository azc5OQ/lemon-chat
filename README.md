# chat system that can be self hosted

## features:
- lightweight C server ! run on any OS and any CPU
- single .html file client ! <br>
  run it from desktop like if you had .exe or embed it to website <br>
- user management system! (change username, kick, ban)
- channel management system! (create , delete, edit channels)
- direct messages (text and pictures)
- channel messages (text and pictures)
- audio in channel
- end to end encryption
- android app

<br>

## other fun features
- select .mp3 file from disk and play in channel
- set different theme
- change font size and color of message
- add groups/tags to other users
- create music bots

## live demo -> https://fruitybackendtesting.com/client.html
<br>

## how to start server:
- clone repository and run windows_build_script.bat or linux_build_script.sh
- or use prebuild release
- watch how-to-build-and-use-video.mkv included in repository


## how to start client:
- open "client.html" file in browser



<br>
<br>

## example pictures
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

I made this communication system to be similar to tools such as Teamspeak or Mumble, which is evident from its interface.<br>
It supports optional voice chat if the admin enables audio in individual channel settings, and all messages—including text, images, and audio data—are end-to-end encrypted for both direct and channel communication.<br>
This chat does not rely solely on TLS/SSL and applies its own encryption on top if TLS/SSL is used, making it extremely difficult—even for the hosting admin—to view messages that do not belong to them.<br>
WebSocket is used for text and images, while WebRTC data channels are used for audio (only the data channel part is used; communication is client → server like WebSocket, so clients cannot leak IP addresses).<br>
The `client.html` contains everything embedded. It can be used like a regular `.exe` by clicking it, entering the IP address and port, and connecting to a server, or it can be packaged into an actual `.exe` using a tool like Electron if needed. <br>
Server should be easy to built, (if system has correct build tools installed) every dependency for building server is already in this repository, there are no extra C/C++ libraries to download.
There is .bat file that can be launched to build it.



<br>
<br>


<br>

## Can the client.html file be embedded into website?
Yes, just follow the instructions when server is started

<br />
<br />


# Thanks to these projects and people for providing some of the source code this project uses:

### client:
cryptico -> https://github.com/wwwtyro/cryptico
<br>
aes-js -> https://github.com/ricmoo/aes-js
<br>
minimp3-wasm -> https://github.com/bashi/minimp3-wasm
<br>
ws-audio-api -> https://github.com/Ivan-Feofanov/ws-audio-api


### server:
wsServer -> https://github.com/Theldus/wsServer
<br>
cJSON -> https://github.com/DaveGamble/cJSON
<br>
libdatachannel -> https://github.com/paullouisageneau/libdatachannel
<br>
libviolet -> https://github.com/paullouisageneau/violet
<br>
mbedtls -> https://github.com/Mbed-TLS/mbedtls
<br>
libtom -> https://github.com/libtom/libtomcrypt


