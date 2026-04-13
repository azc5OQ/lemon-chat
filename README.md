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

![](https://raw.githubusercontent.com/azc5OQ/lemon-chat/master/example/android.jpg)


<br>\
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
# Yes. <br />
with following assumptions : <br />
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


## Remote Port Forwarding Setup

This guide shows how to make a local Windows server accessible from the public internet, even if it’s behind a router or firewall. The method uses a cheap VPS as a relay.

---

## Requirements

- A VPS (1 CPU core, 500 MB RAM is enough)
- SSH client (OpenSSH)
- SSH key pair for authentication

The VPS will act as a public access point for your local server traffic.

---

## Step 1: Connect via SSH with Remote Port Forwarding

Run this command from your local Windows server:

```bash
ssh -i /path/to/private_key -p 2245 -w 0:0 \
    -R 1234:localhost:1234 \
    -R 3478:localhost:3478 \
    -v root@VPS_IP_ADDRESS
```
### Explanation of flags:

- `-i /path/to/private_key` → SSH private key for authentication  
- `-p 2245` → SSH port on the VPS  
- `-w 0:0` → Creates a TUN/TAP interface (required for some setups)  
- `-R 1234:localhost:1234` → Forward local WebSocket port 1234 to the VPS  
- `-R 3478:localhost:3478` → Forward STUN/UDP port 3478 (used for WebRTC data channels)  
- `-v` → Verbose mode for debugging  

`root@VPS_IP_ADDRESS` → Replace with your VPS root user and IP  

---

### Notes

**WebSocket Port (1234):**  
- Chosen by the admin when starting the server  
- Used for WebSocket connections  

**STUN/UDP Port (3478):**  
- Used for creating a non-peer-to-peer WebRTC data channel (UDP-based)  
- Optional if voice chat or WebRTC isn’t needed  

---

✅ With this setup, your local server becomes accessible through the VPS without needing to configure router port forwarding.


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


Its worth noting that there is also a rust version that is older and not in development
https://github.com/azc5OQ/lemon-chat-rust-version



