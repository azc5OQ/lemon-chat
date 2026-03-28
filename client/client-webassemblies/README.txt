client.html uses two WebAssembly files that are directly embedded as Base64 strings.
This way it does not need to load any external resources.  Everything in the client is contained within a single client.html file.

Two two webassemlies are mp3topcm and opus

Purpose of mp3topcm WebAssembly
This allows the client to select an MP3 file from disk, similar to selecting an image, and stream it to other clients in the channel.
This feature was added before music bots feature. Now that music bots are available, it raises the question of whether this is still necessary.
The original idea was to let users easily select an MP3 file from their system and send it to others in the channel—similar to the old days of TeamSpeak, where people streamed music through their microphone.


Purpose of Opus WebAssembly
This enables the client to use the microphone or receive audio from other clients.
Received audio is decrypted using channel keys into Opus format, then decoded into PCM, which is played back.


Questions and Answers
******** What is WebAssembly? ********
A file with a .wasm extension that modern browsers can load for tasks requiring near-native performance. It is often used for things like web-based emulators (e.g., Flash emulators).
Audio processing would be difficult to implement efficiently in pure JavaScript, so WebAssembly is used instead.


******** Is it possible to remove them from client.html ? ********
Yes, quite easy.
Open client.html in VS Code or a similar editor
Locate the function instantiate_libopus_webassembly_from_base64
Remove or comment out libopus_webassembly_base64
This will break audio sending and receiving in client.html, but chat functionality will still work.

To remove MP3-to-PCM (which is likely unnecessary anyway):
Locate the function mp3_to_pcm_worker_onmessage
similar to opus, remove our comment out the webassembly variable



******** How large are they? ********
The libopus WebAssembly file (Base64-encoded) takes about 380 KB in client.html
The mp3topcm WebAssembly file is much smaller, around 30 KB

******** How to build these WebAssembly files? ********
I only know how to build them on a Linux system. 
The build process must be done on Linux, but the resulting files can be used anywhere.

Steps:

Download Emscripten (emsdk)
Follow the setup instructions
Extract the WebAssembly source archives from lemonchat and build them

The build output will be a .wasm file.
This file can be converted into a Base64 string using a Python script (included here).

I have also included the archives of the two projects from which these WebAssembly files were built