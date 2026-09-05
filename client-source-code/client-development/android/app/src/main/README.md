# android app/src/main

The android wrapper around the chat client.

**`assets/client.html` and `assets/nodejs-project/` are GENERATED - do not edit them here.**
Anything you change in those is overwritten by the next build.

They come from:

| generated file | built by | from |
|---|---|---|
| `assets/client.html` | `client-development/browser/build.py` | `client-development/browser/src/` (template.html, scripts/, styles/) |
| `assets/nodejs-project/` | `client-development/nodejs-client/build-node.py` | `nodejs-client/` + the same `browser/src/scripts/` |

The gradle build runs both scripts before packaging, so a normal `assembleDebug` always
ships a fresh copy. Edit the sources under `client-source-code/`, never the assets.

Hand-written and safe to edit: `java/` (the activity, the foreground service, the node
bridge), `res/` (the android settings screen and app resources), and `AndroidManifest.xml`.

`java/` owns the phone side of things: a foreground service that keeps the embedded node
runtime alive so the connection survives with no UI, and a WebView that renders
`client.html` and talks to node over a local websocket.
