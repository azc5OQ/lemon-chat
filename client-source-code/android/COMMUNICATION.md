# Who talks to whom inside the Android app

The Android app is not one program. It is four parts working together.
This file explains every connection between them, in simple words.

## The four parts

| part | what it is | its job |
|---|---|---|
| **Server** | the chat server, on another machine | the meeting place all clients connect to |
| **Node.js** | a JavaScript engine embedded inside the apk, no screen | holds the one real connection to the server, always, even when the app is closed |
| **WebView** | a built-in browser showing `client.html` | the screen. Everything the user sees and taps |
| **Java** | the normal Android part of the app | starts everything, owns settings, shows notifications, watches the phone (network, screen, boot) |

One rule to remember: **Node owns the server connection. The WebView is only the screen. Java is the manager.**

## The four connections

```
                 (1) websocket - the internet
   Server  <────────────────────────────────────────────>  Node.js
                                                            ^    ^
                    (2) local websocket "loopback"          |    |
   WebView <────────────────────────────────────────────────┘    |
      ^                                                          |
      |             (3) local socket "the bridge"                |
      |    Java <────────────────────────────────────────────────┘
      |     ^
      └─────┘
        (4) built-in Android calls
```

### (1) Node.js <-> Server - the real connection

- A websocket over the internet. The pipe itself is usually plain `ws://` - the chat
  content inside it is what is encrypted (with the keys). Only a client page served
  over https uses an encrypted `wss://` pipe (on a separate stunnel port).
- This is the ONLY connection to the server. There is never a second one.
- Node keeps it alive in a foreground service, so messages and calls arrive
  even when the app is closed or the screen is off.
- Node also owns the identity (the key pair). The WebView has no keys.

### (2) WebView <-> Node.js - the "loopback"

- A websocket too, but local only: `127.0.0.1`, it never leaves the phone.
- Plain text on purpose (it never leaves the phone). A random token protects it,
  because every app on the phone can reach 127.0.0.1.
- Purpose: the WebView is only the screen, but `client.html` was written to talk
  to a real server. So Node pretends to be one. When the WebView attaches, Node
  replays the login it already did (the "burst": auth, channels, clients, tags, icons).
  To the page it looks exactly like connecting to the real server.
- After the burst, everything flows live in both directions:
  server frames go down to the page, page requests go up and Node forwards them
  to the real server (encrypting them on the way).
- Node also sends its own status over this wire (`loopback_status`), so the
  connect page can show what Node is doing while the page is not logged in yet.

### (3) Node.js <-> Java - the "bridge"

- A local TCP socket speaking one JSON message per line. Also token-protected.
- Java is the server side, Node dials in when it starts
  (it reads the port and token from `bridge.json`, written by Java).
- Java sends DOWN to Node: the settings (host, port, keys - "connect with this"),
  network on/off, "the user is looking at the app" (ui_visible), log on/off,
  "accept this call" (come_from_idle).
- Node sends UP to Java: its loopback port+token (so Java can point the WebView
  at it), connection status, the connect-page status text (see (4) below), unread
  count (for the app icon badge), incoming calls and new messages (so Java can
  show notifications when the app is closed).
- Why it exists: Node has no screen and no Android rights. Java has both.

### (4) Java <-> WebView - built-in Android calls

No socket here. Android gives two built-in ways, one per direction:

- **Java -> page**: Java runs a line of JavaScript inside the page
  (`evaluateJavascript`). All these entry points are page functions named
  `JavascriptJavaBridge__*`. Used for: handing over the settings json,
  go-to-idle / come-from-idle, "reattach to node now" (nudge), and the
  connect-page status text ("connecting...", "failed: ...") - that text is
  born in Node and travels Node -> Java (3) -> page (4).
- **Page -> Java**: the page calls methods on a magic `Android` object that Java
  injected into it. All these methods are named `JavaExport*`. Used for:
  asking for the settings, opening the native settings screen, showing
  notifications, toasts, the unread badge.

## What happens when... (short stories)

**The app starts.**
Java starts the service, starts Node, and creates the WebView.
Node dials the bridge (3) and announces its loopback port.
Java sends the settings to Node (3) - Node connects to the server (1).
Java pushes the same settings into the page (4) - the page attaches to Node (2).
When Node's login is complete, it replays it to the page (2). The page shows the chat.

**A message arrives while the app is closed.**
It arrives at Node (1). Node tells Java (3). Java shows a notification.
The WebView knows nothing - it may not even exist.

**Somebody calls while the app is closed.**
Same road: Node (1) -> Java (3) -> Android call notification with accept/decline.
Accepting goes back down: Java (3) -> Node -> server (1). The app opens after.

**The user opens the app again.**
Java tells Node "the user is looking" (3), and pokes the page (4) so it
re-attaches to Node (2) right away instead of waiting for its retry timer.
Node replays the login (2), the page shows the chat.

**The user saves server settings.**
Java saves them, then sends them to BOTH sides: to Node over (3) - Node
reconnects to the new server - and into the page over (4), so it re-attaches.

## Where the code lives

| connection | file(s) |
|---|---|
| (1) server connection | inside `bundle.js` (built from `client-development/src/scripts`), started by `nodejs-client/android-main.js` |
| (2) loopback, Node side | `nodejs-client/loopback-ui-server.js` (+ `mini-ws.js`, the websocket implementation) |
| (2) loopback, page side | the connection driver in `client-development/src/scripts/app/connection-driver.js` |
| (3) bridge, Java side | `android/.../NodeBridge.java` |
| (3) bridge, Node side | `nodejs-client/android-bridge-client.js` |
| (4) Java -> page | `evaluateJavascript` calls in `MainActivity.java` / `NodeBridge.java`; page functions in `client-development/src/scripts/app/android-bridge.js` |
| (4) page -> Java | `android/.../JavascriptJavaBridge.java` (the `Android` object) |

More background on WHY it is built this way: `client-source-code/nodejs-client/README.md`.
