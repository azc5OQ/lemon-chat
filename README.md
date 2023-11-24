### chat system that can be self hosted

## how to start server:
- install rust -> https://www.rust-lang.org/tools/install
- cargo run

![](https://raw.githubusercontent.com/azc5OQ/lemon-chat/master/client/other/pic3.png)



## how to start client:
- open "client.html" file in browser


![](https://raw.githubusercontent.com/azc5OQ/lemon-chat/master/client/other/pic2.png)


![](https://raw.githubusercontent.com/azc5OQ/lemon-chat/master/client/other/test1.PNG)



<br>
source code of other people used in this project:

client
<br>
https://github.com/wwwtyro/cryptico
<br>
https://github.com/ricmoo/aes-js
<br>
https://github.com/bashi/minimp3-wasm
<br>
https://github.com/Ivan-Feofanov/ws-audio-api

# video of how it works here:
https://streamable.com/0urmck

<br>

# Question: how is this different from IRC?

<b>A</b>
I made this to be similar to teamspeak, not IRC. So unlike IRC this also contains voice chat.</br>
The messages send in this chat, text messages, images and voice data are all end to end encrypted for both direct and channel communication. This chat does not rely on TLS/SSL and encrypts stuff on its own. <br>
It was made to be hard as possible, even for admin hosting this, to see messages of others that do not belong to him. </br>
This tool is my contribution to free software on internet that no one asked for </br>
<br>

# Question: Isnt like matrix better?
<b>A</b> what do you think?
<br>
<br>

# Question: Can the client.html file be embedded into website?
<b>A</b>
To answer this, it is important to know that the client.html is ment to be run from desktop. This was done to make chat client accessible without the need to run .exe.
It works fine. If really needed, the client.html can be packed to .exe with something like electron. <br>
That being said, yes, the client.html can also be embedded to website so people can connect directly from it. <br />
<br />

with these assumptions: <br />
- apache2 running on ubuntu <br />
- websocket port of choice: 1111 <br />
- stunnel used<br />
- websockets secure (WSS) port of choice: 1112 (set in stunnel configuration file) <br />
- certbot used <br />
- domain name "justsometestchat.com"<br />

steps to do it would be following
- copy client.html to /var/www/html <br />
- apt install stunnel<br />
- apt install apache2<br />
- install certbot (easiest way to get free certificate)<br />
- sudo certbot --apache<br />
- service apache2 start <br />
- vim /etc/stunnel.conf<br />

put this data in /etc/stunnel.conf<br />
[chatserver]<br />
accept = 0.0.0.0:1112 <br />
connect = 127.0.0.1:1111<br />
cert = /etc/letsencrypt/live/justsometestchat.com/cert.pem<br />
key = /etc/letsencrypt/live/justsometestchat.com/privkey.pem<br />


- run "stunnel" command in terminal. If command run with no error, server is probably set up correctly and accessible from https secured site.
  try viewwing https://justsometestchat.com/client.html in browser and test if everything works. The server will be accessible from both desktop and website <br />

<br />
<br />

# Is the client.html using any webassembly files (.wasm) ?
Yes, the client.html has two webassembly files embedded directly in it as base64 string.  <br />
- first webassembly (libopusjs) for encoding/decoding pcm to/from opus <br />
https://github.com/azc5OQ/libopusjs-wasm--build-steps <br />
- second webassembly for encoding mp3 to pcm <br />
https://github.com/azc5OQ/minimp3-wasm-build-steps <br />

<br>

There are two options on how to build and reproduce these exact webasemblies: <br />
- clone it from my repository and setup build enviroment yourself
- download .ova file (exported virtual machine) that contains build enviroment with everything already setup

I provided virtual machine for building these webassemblies because the enviroment needed to build them from scratch was just too complicated to setup.

There is also webassembly-free version of client, client_noaudio.html.

<br />
<br />

# Question: why rust for server?
I got asked why I did not use rust. <br />
I thought about it. I wanted server to be easy to build into binary and rust made that possible. There are pros and cons to everything. <br />
I used C before for server side code and it worked. There are some forks of it here. <br />
I must say I find rust syntax unnecessary complicated. While writing code in rust, I tried to made the server code readable at all costs even if the code was not following "rust standards" <br />


in case you have any feature request open new issue or just fork it and add what you want

<br />

# remote port forwarding setup
posible solution to problem of making server visible (hosting server on windows behind router etc)
 - buy cheap VPS (1 cpu core and 500mb ram more than enough, vps will be access point / router for traffic)
 - ssh -i some_generated_private_key -p 2245 -w 0:0 -R 1234:localhost:1234 -R 3478:localhost:3478 -v root@XXX.XXX.XXX.XXX

some explanation: (its assumed reader knows how this ssh command works)
<br />
1234 is a websocket port that admin chooses when he starts server
<br />
3478 is a stun port used for creating a NON-peer-to-peer webrtc datachannel. Similar to websocket, just UDP. If voice chatting is not needed, this can be ignored.
<br />
todo: add fallback to websocket if webrtc channel fails to create
