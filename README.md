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

<br /><b>Q</b> The client.html contains some minified javascript. This is intended to be updated by hand?
<br>
<b>A</b> Yes, the client is intended to be updated by hand.
<br/>
<br/>

<br /><b>Q</b> How is this supposed tobe simple, if client has to enter connection details?
<br>
<b>A</b> client.html can have autoconnect set
<br/>
<br/>

<b>Q</b>  Can this be embedded into website?
<br>
<b>A</b>  Yes, if you know what you are doing. To place client.html in some website, stunnel (or similar) will need to be placed of websocket port and then data from stunnel port will need to redirected internally to lemonchat's websocket port. That is because web browsers enforce WSS (websockets secure) to be used instead of plain websockets.
Here is app running on website:
https://fruitchattest.click/client.html



<b>Q</b>  This also contains audio?
<br>
<b>A</b>  Yes. You can press Q to talk, it will send microphone output over webrtc datachannel. (same like websockets, datachannels here are not peer to peer and client only connects to server and not other clients)

<b>Q</b> How secure is the chat?
<br>
<b>A</b> content of messages and voice is encrypted and can not be read by server, metadata (usernames, channel names) are known to server


<b>Q</b> How does the chat work?
<br>
<b>A</b> text/images are sent using websocket, voice using webrtc datachannels (not peer to peer)


