how things work

development
grandle build script copies lemon-chat-master/client/client.html to its own assets folder on every build
to develop client.html modify the one in client folder

if for some reason you want to debug client.html itself
start debug through android studio, start chrome on desktop, enter chrome://inspect to address bar and attach to client.html running in android device

Purpose of the app

This app is made to replace phone calls (for me)
Users that are on the same server should be able to call each other. user clicks on another user, clicks "call" on second user, phone starts ringing, user accepts the call, he is invited to room and they can talk to each other. Of course multiple people can be called and invited to channel that way. if both users are idle then server automatically creates some temp chat room just for them.
.

For these "calls" to work, server must have idle clients enabled.
Idle client can only respond to keep alive messages from server and to call requests from users. He cannot be sent image or text picture until he goes out of idle mode.

Because of this, the app needs to run in background, constantly. The websocket needs to be connected

There are obstacles, the client app is written as one large client.html file (originally was ment to be used for desktop only).
And having one large client.html file running in background constantly is something android does not want. 

Some of the problems include
* keeping html running in background even if user kills the app from UI
* keeping up-todate html when app is in background, and resuming the app some time later
* bypassing App Standby - android freezes the app after some time
* bypassing doze mode - android kills the app after some time in background
* auto starting the app - 
* handling unexpected webview anrs-
* requesting mic access from WebView
* accessing file system from webview (if user wants)

How problem was approached

The background connection moved out of the WebView into a Node.js runtime embedded in the apk.
Who talks to whom (WebView / Java / Node / server) is explained plainly in COMMUNICATION.md here.
Why Node was chosen is explained in client-source-code/client-development/nodejs-client/README.md.


Other Android things

Sometimes, so that changes in code can take effect, the app has to be manually uninstalled from app manager. Yes, ridiculous but true. Debugger wont upload everything
for example this change got ignored. I was trying the phone to get the new callSoundUri working, no success. Then 

			//			this.callSoundUri = new Uri.Builder()
			//					.scheme(ContentResolver.SCHEME_ANDROID_RESOURCE)
			//					.authority(this.getPackageName())
			//					.path(Integer.toString(R.raw.incoming_call_ringtone))
			//					.build();

			this.callSoundUri = RingtoneManager.getActualDefaultRingtoneUri(this, RingtoneManager.TYPE_RINGTONE);