# Change built-in sounds

Both clients store their notification sounds as editable Base64 text.

## Browser client

The browser recordings are in `sounds.js`.

1. Open `client-source-code/client-development/browser/main/scripts/app/audio/sounds.js`.
   Find the sound you want to replace, such as `channel_switched`.

2. Convert your replacement MP3 to Base64. In PowerShell, from the folder containing
   `new-sound.mp3`, run:

   ```powershell
   $bytes = [IO.File]::ReadAllBytes((Resolve-Path '.\new-sound.mp3').Path)
   [Convert]::ToBase64String($bytes) | Set-Content -Encoding ASCII new-sound-base64.txt
   ```

   Copy the Base64 line from `new-sound-base64.txt`. Replace only the old text after
   `base64,` inside the chosen `new Audio("...")` entry. Keep the sound name,
   quotation marks and surrounding code.

   For MP3, use the prefix `data:audio/mpeg;base64,`. Existing `audio/mp3` entries
   can keep their prefix. An Ogg recording uses `data:audio/ogg;base64,`; change
   that prefix too if your replacement is MP3. Base64 does not convert audio formats.

3. Save the file. From `client-source-code/client-development/browser`, run
   `.\build.bat` on Windows or `sh build.sh` on Linux/macOS. Open the rebuilt
   `client-source-code/client.html` to use the new sounds.

## Native desktop client

Open `assets/sounds.json` beside `lemon-chat-desktop.exe`. It contains the same sound effects,
with names such as `connected`, `channel_switched` and `message_received`.

Convert your MP3 using step 2 above. Replace the Base64 text after `base64,` in the chosen entry,
keeping its name, quotes and comma:

```json
"channel_switched": "data:audio/mpeg;base64,<your MP3 as Base64>"
```

Save the file. The running desktop client reloads it automatically; no rebuild is needed.
To include the change in future builds, also edit
`client-source-code/client-development/desktop/assets/sounds.json`.

If the sounds are silent, enable **Play notification sounds** in **Audio settings** and save.
