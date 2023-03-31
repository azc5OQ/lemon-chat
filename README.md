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


