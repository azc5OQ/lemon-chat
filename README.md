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

<br /><b>Q</b> How is this supposed to be simple to use, if client has to enter connection details?
<br>
<b>A</b> client.html can be set to automatically connect on start and then handed over to user who is lazy to enter details
<br/>

<b>Q</b>  Can this be embedded into website?
<br>
<b>A</b>  Yes, if you know what you are doing. To place client.html in some website, [stunnel](https://www.stunnel.org/) (or similar) will need to be placed in front of lemonchat's websocket port. Stunnel will forward data coming to its port to lemonchat websocket port interally. That is because web browsers enforce WSS (websockets secure) to be used instead of plain websockets(WS). client.html already encrypts websocket and webrtc data without help of TLS, because client.html is ment to be run from desktop where https is not present. Still, this "secure" layer needs to be used on top of already existing encryption because webbrowsers dont let you connect to WS port is you are located on https domain. You need to use WSS. Of course, if you are running client.html from desktop, you can use websocket port.
That being said, here is client running on website to show that it can be done:
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


