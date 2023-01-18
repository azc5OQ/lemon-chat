### chat system that can be self hosted

## how to start server:
- install rust -> https://www.rust-lang.org/tools/install
- cargo run

![](https://raw.githubusercontent.com/azc5OQ/lemon-chat/master/client/other/pic3.png)



## how to start client:
- open "client.html" file in browser


![](https://raw.githubusercontent.com/azc5OQ/lemon-chat/master/client/other/pic2.png)


![](https://raw.githubusercontent.com/azc5OQ/lemon-chat/master/client/other/pic.png)


<br>
Used libraries:

client
<br>
https://github.com/wwwtyro/cryptico
<br>
https://github.com/ricmoo/aes-js
<br>
https://github.com/Ivan-Feofanov/ws-audio-api


<br>
<br>
<br>
<br>
<br>

<b>Questions I got</b>

<br>
Q I see you have 127.0.0.1 (local address) in screenshot. Does this also work over internet?
<br>
A Yes. You can launch the .html file from desktop and it will also connect to remote server, if you enter its address. That was the point of this app. To make secure chat easily available.

Q Why did you use such old unmaintained javascript libraries?
<br>
A I found them to work

Q Can this be embedded into website?
<br>
A Yes, if you know what you are doing. Web browsers enforce WSS (websockets secure) on https secured domains to be used instead of websockets (WS).
The chat app already has own encryption but SSL will still be needed to be used on top of it because browsers enforce it.
You will also need to use https://www.stunnel.org/ in front of the already running server (or similar) and make client connect to stunnel port instead of lemonchat websocket port. Websocket data, sent to stunnel SSL port from browser will then be redirect to server webocket port interally. Embedding this to website will also make it not possible to connect to other servers other then the server of website this is embedded within (limitation of browsers). But this is known to people that work with websockets.

<br>
Q This also contains audio?
<br>
A Yes. You can press Q to talk, it will send microphone data over webrtc datachannel. (same as websocket, in this app, datachannel is not peer to peer and client only connects to server and not other clients)
