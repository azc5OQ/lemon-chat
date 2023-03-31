### chat system that can be self hosted

## how to start server:
- install rust -> https://www.rust-lang.org/tools/install
- cargo run

![](https://raw.githubusercontent.com/azc5OQ/lemon-chat/master/client/other/pic3.png)



## how to start client:
- open "client.html" file in browser


![](https://raw.githubusercontent.com/azc5OQ/lemon-chat/master/client/other/pic2.png)


![](https://raw.githubusercontent.com/azc5OQ/lemon-chat/master/client/other/test1.PNG)

## how to stream music in channel
First, you need to convert your mp3 to .wav file.
<br>
use https://convertio.co/ or simillar to convert to .wav
<br>
for output .wav, select these desired parameters:
- codec: PCM_S16LE (uncompressed)
- audiochannels: mono
- frequency: 48000 hz

after its done, activate mic, select .wav file, it will play
<br>


<br>
Used libraries:

client
<br>
https://github.com/wwwtyro/cryptico
<br>
https://github.com/ricmoo/aes-js
<br>
https://github.com/azc5OQ/libopusjs


# video of how it works here:
https://streamable.com/0urmck

<br>
<br>


# Question: Can the client.html file be embedded into website?
<br>
<b>A</b>  Yes, if you know what you are doing. Here is chat running live to show it can be done: https://fruitchattest.click/client.html
To embed client.html into website, [stunnel](https://www.stunnel.org/) (or similar) will need to be placed in front of lemonchat's websocket port. Stunnel will then forward data coming to its WSS port to lemonchat WS port interally. Stunnel (or similar) needs to be used because web browsers enforce WSS (websockets secure) to be used instead of plain websockets (WS) and this is something lemonchat server cannot yet handle. client.html already encrypts websocket and webrtc data without help of websockets secure, because client.html is also ment to be run from desktop where https is not present. Still, this "secure" layer needs to be used on top of already existing encryption because webbrowsers dont let you connect to plain websocket(WS) port is you are located on https secured site.  Basically stunnel needs to be properly configured and running in front of lemon chat server if you also want to use client.html from https context and not just Desktop.
<br />
<br />

with these assumptions: <br />
- websocket port of choice: 1111<br />
- websockets secure port of choice: 1112<br />
- certbot used for obtaining free ceritificate <br />
- domain name "fruitchattest.click"<br />
- apache2 server running on linux<br />

steps for embedding client.html to https secured site would be following:
- apt install stunnel<br />
- sudo certbot --apache<br />
- vim /etc/stunnel.conf<br />

put this data in /etc/stunnel.conf<br />
[chatserver]<br />
accept = 0.0.0.0:1112 <br />
connect = 127.0.0.1:1111<br />
cert = /etc/letsencrypt/live/fruitchattest.click/cert.pem<br />
key = /etc/letsencrypt/live/fruitchattest.click/privkey.pem<br />

- run "stunnel" command in terminal. If there is no error, stunnel is running and client.html should now work within https secured site <br />

