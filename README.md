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

<b>Q</b>  This also contains audio?
<br>
<b>A</b>  Yes. You can press Q to talk, it will send microphone output over webrtc datachannel. (same like websockets, datachannels here are not peer to peer and client only connects to server and not other clients)

<b>Q</b> How secure is the chat?
<br>
<b>A</b> content of messages and voice is encrypted and can not be read by server, metadata (usernames, channel names) are known to server. But it is always possible to spoof content

<b>Q</b> How does the chat work?
<br>
<b>A</b> text/images are sent using websocket, voice using webrtc datachannels (not peer to peer)

<b>Q</b> Can the client.html be put into website?
<br>
<b>A</b>  Yes, if you know what you are doing. Here is chat running live to show it can be done: https://fruitchattest.click/client.html
To place client.html in some website, [stunnel](https://www.stunnel.org/) (or similar) will need to be placed in front of lemonchat's websocket port. Stunnel will forward data coming to its port to lemonchat websocket port interally. That is because web browsers enforce WSS (websockets secure) to be used instead of plain websockets(WS). client.html already encrypts websocket and webrtc data without help of TLS, because client.html is ment to be run from desktop where https is not present. Still, this "secure" layer needs to be used on top of already existing encryption because webbrowsers dont let you connect to plain websocket(WS) port is you are located on https secured site. 
<br />
<br />
steps to make client.html work within https context with assumptions:<br />
- websocket port: 1111<br />
- websockets secure port: 1112<br />
- letsencrypt used for getting ceritificate <br />
- domain name "fruitchattest.click"<br />
- apache2 server<br />

<br />
1 - in client.html edit connection string from ws:/ to wss:/ <br />
2 - apt install stunnel<br />
3 - sudo certbot --apache<br />
4 - vim /etc/stunnel.conf<br />

put this in /etc/stunnel.conf<br />
[someserver]<br />
accept = 0.0.0.0:1112 <br />
connect = 127.0.0.1:1111<br />
cert = /etc/letsencrypt/live/fruitchattest.click/cert.pem<br />
key = /etc/letsencrypt/live/fruitchattest.click/privkey.pem<br />

5. run "stunnel" command in terminal. If there is no error, stunnel is running. Try to join the server from client.html within the website<br />

its also important to know about certain limitation of WSS
if client.html is put in domain fruitchattest.click, browser will not allow websocket connections to other address than "fruitchattest.click", not even ip address of domain. From desktop this works fine.


