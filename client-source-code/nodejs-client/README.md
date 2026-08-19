# why Node.js for the Android client

At first the idea was to wrap client.html (the same one the desktop and the website use) in
Android's WebView, customize and tweak it until it just worked, with functionality such as calls
routed through the JS-Java bridge. That worked, somehow.

The downside was that client.html had to run constantly in the background, and so did the WebView.
Keeping a WebView alive in the background required an ugly hack: attaching it to an invisible 0x0
window, which produced a permanent "lemon chat is displaying over other apps" alert in the
notifications. Surprisingly, client.html stayed connected even over long periods - but the alert
was always there. 

The user also had to grant the draw-over-other-apps permission, which made setup more complicated.

Moving the core websocket functionality into Java would have split the codebase further and made
everything even more complicated than it already was. So the choice fell on a JavaScript
environment without HTML - Node.js for Android - which can run in the background without needing
to be "displayed over other apps" and without the WebView, while reusing the existing client's
JavaScript files.

When the app actually needs to render something, the Node.js environment hands the data to it. On
Android it is therefore NOT client.html that speaks to the server over the websocket - it is the
Node.js environment. client.html connects to a loopback websocket served by Node and receives the
same protocol replayed; Node holds the one real connection, permanently.

# what is in this directory

- `build-node.py` + `node-template.js` - glue the client sources into `bundle.js` (same @@INCLUDE
  mechanism as client.html's build.py). Run after changing any client source.
- `dom-shim.js` - headless stand-ins for the browser globals, plus the node platform flags.
- `load-bundle.js` - loads the bundle the way a browser would (vm, not require).
- `mini-ws.js` - zero-dependency websocket client + server; no npm anywhere.
- `android-main.js` - entry point inside the android service.
- `android-bridge-client.js` - JSON-lines TCP bridge to the java side (settings in, events out).
- `loopback-ui-server.js` - the loopback websocket the ui connects to instead of the real server.
- `fetch-nodejs-mobile.py` - downloads the prebuilt node runtime the apk embeds (once, ~120 MB).
- `build.bat` - wraps build-node.py.
- `tests/` - one gate per file, shared plumbing in `tests/test-helpers.js`; run after touching
  anything here.