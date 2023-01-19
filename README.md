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
<b>Questions I got</b>
<br>
<b>Q:</b> Eww, why gradient background? What is this 1990?</b>
<br>
A: You can change this by editing one line of css in client.html<br />  

<br>
<b>Q</b> I see you used 127.0.0.1 in screenshot. Does this also work over internet?
<br>
<b>A</b>  Yes..... Yes, you can launch the .html file from desktop and it will also connect to remote server, if you enter its address. That was the point of this. To make secure chat easily available without installation<br/>
<br /><b>Q</b> Why did you use such old unmaintained javascript libraries?
<br>
<b>A</b> I found them to be usable for this project
<br/>
<br/>

<b>Q</b>  Can this be embedded into website?
<br>
<b>A</b>  Yes, if you know what you are doing. Web browsers enforce WSS (websockets secure) on https secured domains to be used instead of plain websockets (WS).
The chat app already has own encryption but SSL will still be needed to be used on top of it because web-browsers enforce it.
You will also need to use https://www.stunnel.org/ in front of the already running server (or similar) and make client connect to stunnel port instead of lemonchat websocket port. Websocket data, sent to stunnel SSL port from browser will then be redirect to server webocket port interally. Embedding this to website will also make it not possible to connect to other servers other then the server of website this is embedded within (limitation of browsers). But this is known to people that work with websockets.


<b>Q</b>  This also contains audio?
<br>
<b>A</b>  Yes. You can press Q to talk, it will send microphone output over webrtc datachannel. (same like websockets, datachannels here are not peer to peer and client only connects to server and not other clients)


<b>Q</b> How secure is the chat?
<br>
<b>A</b> It uses form of end to end encryption. This deserves own documentation . Server cannot access any chat messages by default, private or channel messages or voice data. But this would be clear to you if you checked the code to see how it works. You can suggest possible improvements in security in issues


