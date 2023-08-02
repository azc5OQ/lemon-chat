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

# Question: how is this different from IRC?

<b>A</b>
I made this to be similar to teamspeak, not IRC. So unlike IRC this also contains voice chat. But I actually got asked this question.</br>
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
- second webassembly for encoding mp3 to pcm (supports streaming music from mp3 file) taken from here https://github.com/bashi/minimp3-wasm <br />

There are two options on how to build and reproduce these exact webasemblies: <br />
- clone it from my repository and setup build enviroment yourself
- download .ova file (exported virtual machine) that contains build enviroment with everything already setup

I provided virtual machine for building the webassemblies, because the enviroment needed to build from scratch it was just too complicated to setup.
the pcm to mp3 encoder webassembly is easy to build but the opus encoder is not easy to build

<br />
<br />

# Question: why rust for server?
I wanted server to be easy to build into binary. There are pros and cons to everything.
I used C before for server side code. It was complicated to build and only could run on linux. There are some forks of it here.
I dont like rust syntax but I tried to made the server code readable for anyone.
if someone wants to maintain C version of it I dont mind, there are some forks of it here


in case you have any feature request open new issue or just fork it and add what you want
