"""Run production Java framing/connection/settings code with bounded Android API fixtures.

Socket and Unicode behavior are real JDK implementations. JSONObject uses snapshot tokens,
so these tests exercise settings ownership/commit ordering rather than Android JSON parsing.
The APK compile is a separate check against the real Android API and dependencies.
"""
from pathlib import Path

from test_support import ROOT, JAVA, compile_run

def literal_fixtures(name, variable):
    group = "identity" if "identity" in name else "transport"
    base = Path(__file__).parent / "fixtures" / group
    return {path.relative_to(base).as_posix(): path.read_text(encoding="utf-8") for path in base.rglob("*.java")}


fixtures = literal_fixtures('android-native-identity-probes.py', 'FILES')
fixtures.pop('com/lemonchat/IdentityProbe.java', None)
bridge = literal_fixtures('android-java-bridge-probe.py', 'FIXTURES')
for key in ['android/webkit/WebView.java', 'com/lemonchat/BackgroundService.java', 'com/lemonchat/MainActivity.java', 'com/lemonchat/JavascriptJavaBridge.java']:
    if key in bridge:
        fixtures[key] = bridge[key]
fixtures['android/util/Log.java'] = '''package android.util;
public class Log
{
    public static final StringBuilder captured = new StringBuilder();
    public static int d(String tag, String text)
    {
        captured.append(text);
        return 0;
    }
    public static int i(String tag, String text)
    {
        captured.append(text);
        return 0;
    }
    public static int w(String tag, String text)
    {
        captured.append(text);
        return 0;
    }
    public static int e(String tag, String text)
    {
        captured.append(text);
        return 0;
    }
    public static int e(String tag, String text, Throwable error)
    {
        captured.append(text);
        return 0;
    }
}'''
json = fixtures['org/json/JSONObject.java']
json = json.replace('values = new HashMap<>(saved);', 'values = new HashMap<>(saved);')
json = json.replace('if (saved == null)', 'if (saved == null && token.equals("{}"))\n        {\n            saved = new HashMap<>();\n        }\n        if (saved == null)')
json = json.replace('    @Override public String toString()', '''
    public static String quote(String value)
    {
        return "\\\"" + value + "\\\"";
    }
    public Object get(String key)
    {
        return values.get(key);
    }
    public Object opt(String key)
    {
        return values.get(key);
    }
    public java.util.Iterator<String> keys()
    {
        return values.keySet().iterator();
    }
    public int getInt(String key)
    {
        return ((Number)values.get(key)).intValue();
    }
    public long getLong(String key)
    {
        return ((Number)values.get(key)).longValue();
    }
    public int optInt(String key, int fallback)
    {
        return values.containsKey(key) ? getInt(key) : fallback;
    }
    public long optLong(String key, long fallback)
    {
        return values.containsKey(key) ? getLong(key) : fallback;
    }
    public boolean optBoolean(String key, boolean fallback)
    {
        return values.containsKey(key) ? (Boolean)values.get(key) : fallback;
    }
    @Override public String toString()''')
fixtures['org/json/JSONObject.java'] = json
fixtures['android/webkit/WebView.java'] = '''package android.webkit;
public class WebView
{
    public final java.util.Queue<Runnable> callbacks = new java.util.concurrent.ConcurrentLinkedQueue<>();
    public final java.util.List<String> evaluated = new java.util.ArrayList<>();
    public void post(Runnable callback)
    {
        callbacks.add(callback);
    }
    public void evaluateJavascript(String script, Object callback)
    {
        evaluated.add(script);
    }
}'''
fixtures['com/lemonchat/BackgroundService.java'] = '''package com.lemonchat;
public class BackgroundService
{
    public int canceledCalls;
    public int shownCalls;
    public android.webkit.WebView webView;
    public JavascriptJavaBridge javascriptJavaBridge;
    public boolean isNetworkAvailable()
    {
        return true;
    }
    public long getPageGeneration()
    {
        return 1;
    }
    public boolean isTrustedPage()
    {
        return true;
    }
    public void showUnreadBadge(int count)
    {
    }
    public void showIncomingCall(String name, int channel, String callId, String session, long expires)
    {
        this.shownCalls++;
    }
    public void clearIncomingCall()
    {
        this.canceledCalls++;
    }
    public void showMessageNotification(String title, String message)
    {
    }
    public void showPokeNotification(String title, String message)
    {
    }
}'''
fixtures['com/lemonchat/MainActivity.java'] = '''package com.lemonchat;
public class MainActivity
{
    public static MainActivity instance;
}'''
fixtures['com/lemonchat/JavascriptJavaBridge.java'] = '''package com.lemonchat;
public class JavascriptJavaBridge
{
    public void JavaExportRequestCurrentSettingsFromAndroid()
    {
    }
}'''
fixtures['android/os/Looper.java'] = '''package android.os;
public class Looper
{
    public static Looper getMainLooper()
    {
        return new Looper();
    }
}'''
fixtures['android/os/Handler.java'] = '''package android.os;
public class Handler
{
    public Handler(Looper looper)
    {
    }
    public boolean post(Runnable callback)
    {
        callback.run();
        return true;
    }
}'''
fixtures['com/lemonchat/NativeRegression.java'] = r'''package com.lemonchat;
import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.lang.reflect.*;
import java.util.*;
import java.util.concurrent.*;
import org.json.JSONObject;
import android.content.*;
public class NativeRegression
{
    static void check(boolean condition, String message)
    {
        if (!condition)
        {
            throw new AssertionError(message);
        }
    }
    static class Preferences implements SharedPreferences
    {
        final Map<String, Object> memory = new ConcurrentHashMap<>();
        final Map<String, Object> disk = new ConcurrentHashMap<>();
        boolean fail;
        int commits;
        int failOnCommit = -1;
        public String getString(String key, String fallback)
        {
            return (String)memory.getOrDefault(key, fallback);
        }
        public boolean getBoolean(String key, boolean fallback)
        {
            return (Boolean)memory.getOrDefault(key, fallback);
        }
        public Editor edit()
        {
            return new Editor()
            {
                final Map<String, Object> updates = new HashMap<>();
                public Editor putString(String key, String value)
                {
                    updates.put(key, value);
                    return this;
                }
                public Editor putBoolean(String key, boolean value)
                {
                    updates.put(key, value);
                    return this;
                }
                public boolean commit()
                {
                    commits++;
                    memory.putAll(updates);
                    if (!fail && commits != failOnCommit)
                    {
                        disk.putAll(updates);
                    }
                    return !fail && commits != failOnCommit;
                }
                public void apply()
                {
                    commit();
                }
            };
        }
    }
    static JSONObject state(ChatSettings settings)
    {
        return new JSONObject(settings.getJsonSettings());
    }
    static String seed(char value)
    {
        char[] seed = new char[200];
        Arrays.fill(seed, value);
        return new String(seed);
    }
    static Object current(NodeBridge bridge) throws Exception
    {
        Field field = NodeBridge.class.getDeclaredField("currentConnection");
        field.setAccessible(true);
        return field.get(bridge);
    }
    static void waitForConnection(NodeBridge bridge, Object previous) throws Exception
    {
        long end = System.currentTimeMillis() + 2000;
        while (System.currentTimeMillis() < end)
        {
            if (current(bridge) != null && current(bridge) != previous)
            {
                return;
            }
            Thread.sleep(5);
        }
        throw new AssertionError("Authentication did not complete");
    }
    static Socket connect(NodeBridge bridge) throws Exception
    {
        Socket socket = new Socket("127.0.0.1", bridge.getPort());
        socket.setSoTimeout(2000);
        socket.getOutputStream().write((bridge.getToken() + "\n").getBytes(StandardCharsets.UTF_8));
        return socket;
    }
    static Object field(NodeBridge bridge, String name) throws Exception
    {
        Field field = NodeBridge.class.getDeclaredField(name);
        field.setAccessible(true);
        synchronized (bridge)
        {
            return field.get(bridge);
        }
    }
    static void event(Socket peer, JSONObject value) throws Exception
    {
        peer.getOutputStream().write((value.toString() + "\n").getBytes(StandardCharsets.UTF_8));
        peer.getOutputStream().flush();
    }
    static void barrier(NodeBridge bridge, Socket peer, String name) throws Exception
    {
        event(peer, new JSONObject().put("type", "status_text").put("state", name));
        long deadline = System.currentTimeMillis() + 2000;
        while (System.currentTimeMillis() < deadline)
        {
            if (name.equals(field(bridge, "lastPhaseState")))
            {
                return;
            }
            Thread.sleep(5);
        }
        throw new AssertionError("Control event barrier did not complete");
    }
    static void hello(Socket peer) throws Exception
    {
        event(peer, new JSONObject().put("type", "loopback").put("port", 32123)
            .put("token", "0123456789abcdef0123456789abcdef"));
    }
    static void noMoreLines(Socket peer, BufferedReader reader) throws Exception
    {
        peer.setSoTimeout(100);
        try
        {
            String unexpected = reader.readLine();
            throw new AssertionError("Unexpected control line: " + unexpected);
        }
        catch (SocketTimeoutException expected)
        {
        }
        finally
        {
            peer.setSoTimeout(2000);
        }
    }
    static String intentId(NodeBridge bridge) throws Exception
    {
        Object intent = field(bridge, "pendingConnectIntent");
        Field id = intent.getClass().getDeclaredField("commandId");
        id.setAccessible(true);
        return (String)id.get(intent);
    }
    static class BlockingSettings extends ChatSettings
    {
        volatile String json;
        final CountDownLatch captured = new CountDownLatch(1);
        final CountDownLatch release = new CountDownLatch(1);
        @Override public String getJsonSettings()
        {
            String snapshot = this.json;
            if ("old-settings-publication".equals(Thread.currentThread().getName()))
            {
                this.captured.countDown();
                try
                {
                    this.release.await();
                }
                catch (InterruptedException interrupted)
                {
                    Thread.currentThread().interrupt();
                    throw new IllegalStateException(interrupted);
                }
            }
            return snapshot;
        }
    }
    public static void main(String[] args) throws Exception
    {
        Preferences preferences = new Preferences();
        ChatSettings.attachContext(new Context(preferences));
        ChatSettings settings = new ChatSettings();
        check(settings.getJsonSettings().contains("identity_error"), "Read must not generate identity");
        check(preferences.commits == 0, "Read unexpectedly committed");
        Thread a = new Thread(settings::loadPersistedState);
        Thread b = new Thread(settings::loadPersistedState);
        a.start(); b.start(); a.join(); b.join();
        check(preferences.commits == 1, "Concurrent init must commit once");
        String original = state(settings).getString("identity_string");
        check(original.length() == 200, "Initial identity invalid");
        settings.updateSetting("is_audio_effect_enabled", true);
        settings.updateSetting("is_microphone_always_on", true);
        settings.updateSetting("is_microphone_always_on", false);
        check(settings.getAreAudioEffectsEnabled(), "Mic update changed sound preference");
        check(!settings.isMicrophoneAlwayson(), "Mic update lost");
        check(state(settings).optBoolean("is_audio_effect_enabled", false), "Sound enabled flag inverted");
        int beforeRead = preferences.commits;
        settings.getJsonSettings(); settings.getJsonSettings();
        check(beforeRead == preferences.commits, "Settings reads must be pure");
        String operation = "0000000000000001";
        check(settings.replaceIdentity(seed('B'), 3072, operation, 1).contains("true"), "Identity replace failed");
        int afterReplace = preferences.commits;
        check(settings.replaceIdentity(seed('B'), 3072, operation, 1).contains("true"), "Duplicate identity operation failed");
        check(afterReplace == preferences.commits, "Duplicate operation wrote again");
        check(settings.replaceIdentity(seed('C'), 2048, "0000000000000002", 1).contains("stale"), "Stale identity operation accepted");
        settings.saveJsonSettings(new JSONObject().put("host", "synthetic.invalid").put("future_field", "kept").toString());
        check(state(settings).getInt("rsa_key_bits") == 3072 && state(settings).getString("identity_string").equals(seed('B')), "Connection save changed identity");
        check(new JSONObject(settings.exportIdentity()).getInt("rsa_key_bits") == 3072, "Export dropped actual size");
        final int[] notices = { 0 };
        ChatSettings.onSettingsSaved = () -> notices[0]++;
        int beforeLogging = preferences.commits;
        preferences.failOnCommit = beforeLogging + 2;
        check(settings.setFileLoggingEnabled(true), "Logging preference save failed");
        check(preferences.commits == beforeLogging + 1, "Logging preference requires one atomic commit");
        check(settings.isFileLoggingEnabled() && state(settings).optBoolean("is_file_logging_enabled", false), "Logging state diverged after save");
        check((Boolean)preferences.disk.get("file_logging_enabled"), "Logging preference not committed");
        preferences.failOnCommit = preferences.commits + 1;
        int beforeFailedLogging = notices[0];
        check(!settings.setFileLoggingEnabled(false), "Failed logging save reported success");
        check(settings.isFileLoggingEnabled() && state(settings).optBoolean("is_file_logging_enabled", false), "Failed logging commit changed published state");
        check(notices[0] == beforeFailedLogging, "Failed logging commit notified listeners");
        preferences.failOnCommit = -1;
        notices[0] = 0;
        preferences.fail = true;
        check(!settings.updateSetting("host", "uncommitted.invalid"), "Failed save reported success");
        check(state(settings).getString("host").equals("synthetic.invalid") && notices[0] == 0, "Failed commit published state");
        check(settings.replaceIdentity(seed('C'), 4096, "0000000000000003", 2).contains("storage_failed"), "Failed identity commit reported success");
        check(state(settings).getString("identity_string").equals(seed('B')), "Failed identity commit mutated active seed");
        System.out.println("PASS: serialized initialization, pure reads, partial sound/mic saves, canonical size, operation dedup/CAS, failed-commit retention");

        String unicode = "A\u20ac\ud83d\ude00";
        check(BridgeProtocol.escapeSurrogates(unicode).equals(unicode), "Valid surrogate pair must stay UTF8-encodable without expansion");
        check(unicode.equals(BridgeProtocol.readLine(new ByteArrayInputStream((unicode + "\n").getBytes(StandardCharsets.UTF_8)), 100)), "UTF8 changed");
        boolean malformed = false;
        try
        {
            BridgeProtocol.readLine(new ByteArrayInputStream(new byte[] { (byte)0xc3, 10 }), 100);
        }
        catch (IOException expected)
        {
            malformed = true;
        }
        check(malformed, "Malformed UTF8 accepted");
        boolean oversized = false;
        try
        {
            BridgeProtocol.readLine(new ByteArrayInputStream("12345\n".getBytes(StandardCharsets.UTF_8)), 4);
        }
        catch (IOException expected)
        {
            oversized = true;
        }
        check(oversized, "Oversized line accepted");
        check(BridgeProtocol.escapeSurrogates("\ud800").equals("\\ud800"), "Lone surrogate changed during encoding");
        check(BridgeProtocol.escapeSurrogates("\udfff").equals("\\udfff"), "Lone low surrogate changed during encoding");
        System.out.println("PASS: bounded complete-line decoding, invalid UTF8 rejection, exact surrogate escaping");

        NodeBridge bridge = new NodeBridge(new BackgroundService());
        check(bridge.start(), "Bridge startup failed");
        try (Socket attacker = new Socket("127.0.0.1", bridge.getPort()); Socket first = connect(bridge))
        {
            waitForConnection(bridge, null);
            Object firstOwner = current(bridge);
            try (Socket second = connect(bridge))
            {
                waitForConnection(bridge, firstOwner);
                Thread.sleep(30);
                check(first.getInputStream().read() == -1, "Superseded authenticated socket left open");
                Method send = NodeBridge.class.getDeclaredMethod("send", String.class);
                send.setAccessible(true);
                for (int i = 0; i < 25; i++)
                {
                    send.invoke(bridge, "sequence-" + i);
                }
                BufferedReader reader = new BufferedReader(new InputStreamReader(second.getInputStream(), StandardCharsets.UTF_8));
                for (int i = 0; i < 25; i++)
                {
                    check(("sequence-" + i).equals(reader.readLine()), "Output reordered or new connection cleared by old cleanup");
                }
                bridge.sendUiVisible(false, false, true);
                bridge.sendUiVisible(false, true);
                Method replayVisibility = NodeBridge.class.getDeclaredMethod("sendRetainedUiVisibility");
                replayVisibility.setAccessible(true);
                replayVisibility.invoke(bridge);
                for (int i = 0; i < 3; i++)
                {
                    String visibility = reader.readLine();
                    check(visibility.contains("\"visible\":false") && visibility.contains("\"force_idle\":true")
                        && visibility.contains("\"preserve_idle\":false"), "Hidden replay lost task removal force or created conflicting preserve flag");
                }
                bridge.sendUiVisible(true);
                bridge.sendUiVisible(false);
                check(reader.readLine().contains("\"force_idle\":false"), "Visible transition did not clear old force");
                check(reader.readLine().contains("\"force_idle\":false"), "Ordinary backgrounding unexpectedly forced idle");
                second.shutdownOutput();
                check(reader.readLine() == null, "Authenticated EOF did not close socket");
            }
            try (Socket malformedPeer = connect(bridge))
            {
                waitForConnection(bridge, null);
                malformedPeer.getOutputStream().write(new byte[] { (byte)0xff, 10 });
                check(malformedPeer.getInputStream().read() == -1, "Malformed peer not closed");
            }
        }
        finally
        {
            bridge.close();
        }
        check(!android.util.Log.captured.toString().contains(bridge.getToken()), "Bridge token leaked to diagnostics");
        System.out.println("PASS: silent unauthenticated peer cannot block valid auth, replacement cleanup is scoped, 25 writes ordered, EOF/error closes, token not logged");

        NodeBridge failedRuntime = new NodeBridge(null);
        check(failedRuntime.start(), "Failure fixture bridge startup failed");
        Field eventsField = NodeBridge.class.getDeclaredField("events");
        eventsField.setAccessible(true);
        ThreadPoolExecutor events = (ThreadPoolExecutor)eventsField.get(failedRuntime);
        CountDownLatch ownerBlocked = new CountDownLatch(1);
        CountDownLatch releaseOwner = new CountDownLatch(1);
        try (Socket peer = connect(failedRuntime))
        {
            waitForConnection(failedRuntime, null);
            events.execute(() ->
            {
                ownerBlocked.countDown();
                try
                {
                    releaseOwner.await();
                }
                catch (InterruptedException stopped)
                {
                    Thread.currentThread().interrupt();
                }
            });
            check(ownerBlocked.await(2, TimeUnit.SECONDS), "Event owner did not block");
            String delayedPhase = new JSONObject().put("type", "status_text").put("state", "connected").put("reason", "old phase").toString();
            peer.getOutputStream().write((delayedPhase + "\n").getBytes(StandardCharsets.UTF_8));
            long deadline = System.currentTimeMillis() + 2000;
            while (events.getQueue().isEmpty() && System.currentTimeMillis() < deadline)
            {
                Thread.sleep(5);
            }
            check(!events.getQueue().isEmpty(), "Reader did not queue the delayed phase");
            failedRuntime.reportRuntimeFailure("runtime stopped");
            releaseOwner.countDown();
            check(peer.getInputStream().read() == -1, "Terminal runtime failure left control socket open");
            events.awaitTermination(2, TimeUnit.SECONDS);
            Method handle = NodeBridge.class.getDeclaredMethod("handleEvent", String.class);
            handle.setAccessible(true);
            handle.invoke(failedRuntime, delayedPhase);
            handle.invoke(failedRuntime, new JSONObject().put("type", "loopback").put("port", 32123)
                .put("token", "0123456789abcdef0123456789abcdef").toString());
            Field phase = NodeBridge.class.getDeclaredField("lastPhaseState");
            phase.setAccessible(true);
            check("runtime_failed".equals(phase.get(failedRuntime)), "Late phase overwrote terminal failure");
            check(failedRuntime.getLoopbackPort() == 0 && failedRuntime.getLoopbackToken().isEmpty(), "Late endpoint revived stopped runtime");
            check(!failedRuntime.start(), "Stopped bridge restarted without a runtime");
        }
        finally
        {
            releaseOwner.countDown();
            failedRuntime.close();
        }
        System.out.println("PASS: terminal runtime failure closes transport and rejects queued/late phase and endpoint events");

        BackgroundService phaseService = new BackgroundService();
        phaseService.webView = new android.webkit.WebView();
        NodeBridge phaseBridge = new NodeBridge(phaseService);
        Method handlePhase = NodeBridge.class.getDeclaredMethod("handleEvent", String.class);
        handlePhase.setAccessible(true);
        handlePhase.invoke(phaseBridge, new JSONObject().put("type", "status_text").put("state", "connected").toString());
        Runnable oldPhase = phaseService.webView.callbacks.remove();
        phaseBridge.reportRuntimeFailure("runtime stopped");
        check(phaseService.canceledCalls == 1, "Terminal runtime failure left the native call displayed");
        phaseService.webView.callbacks.remove().run();
        oldPhase.run();
        check(phaseService.webView.evaluated.size() == 1 && phaseService.webView.evaluated.get(0).contains("runtime_failed"),
            "Old queued phase callback overwrote terminal failure in the page");
        System.out.println("PASS: queued page phase is scoped to current phase/reason as well as current document");

        NodeBridge intents = new NodeBridge(null);
        check(intents.start(), "Connect intent fixture bridge startup failed");
        try (Socket failedWriter = connect(intents))
        {
            waitForConnection(intents, null);
            Object connection = current(intents);
            Field socketField = connection.getClass().getDeclaredField("socket");
            socketField.setAccessible(true);
            synchronized (intents)
            {
                // Half-close only native output: its reader remains waiting for this peer,
                // while the production writer's next SocketOutputStream.write must fail.
                ((Socket)socketField.get(connection)).shutdownOutput();
                intents.sendConnect();
            }
            long failureDeadline = System.currentTimeMillis() + 2000;
            while (current(intents) != null && System.currentTimeMillis() < failureDeadline)
            {
                Thread.sleep(5);
            }
            check(current(intents) == null, "Real socket writer failure did not drop control connection");
            check(field(intents, "pendingConnectIntent") != null, "Writer failure lost the queued explicit Connect intent");
            String connectId = intentId(intents);
            check(connectId.matches("[a-f0-9-]{36}"), "Connect intent did not receive bounded UUID scope");
            try (Socket replacement = connect(intents))
            {
                waitForConnection(intents, null);
                Object replacementOwner = current(intents);
                hello(replacement);
                BufferedReader reader = new BufferedReader(new InputStreamReader(replacement.getInputStream(), StandardCharsets.UTF_8));
                String retriedConnect = reader.readLine();
                check(retriedConnect.contains("\"type\":\"connect\"") && retriedConnect.contains(connectId), "Failed Connect not retried with original scope");
                check(reader.readLine().contains("\"type\":\"ui_visible\""), "Intent replay did not preserve visibility command");
                // Node could have accepted the retry before the result was lost with this socket.
                try (Socket lostResultReplacement = connect(intents))
                {
                    waitForConnection(intents, replacementOwner);
                    hello(lostResultReplacement);
                    BufferedReader resultReader = new BufferedReader(new InputStreamReader(lostResultReplacement.getInputStream(), StandardCharsets.UTF_8));
                    check(retriedConnect.equals(resultReader.readLine()), "Lost-result Connect retry changed command scope");
                    check(resultReader.readLine().contains("\"type\":\"ui_visible\""), "Lost-result replay omitted presence");
                    intents.sendDisconnect();
                    String disconnectId = intentId(intents);
                    check(!connectId.equals(disconnectId), "New native intent reused old scope");
                    check(resultReader.readLine().contains("\"type\":\"disconnect\""), "Latest Disconnect was not sent");
                    event(lostResultReplacement, new JSONObject().put("type", "connect_intent_result")
                        .put("command_id", connectId).put("accepted", true));
                    barrier(intents, lostResultReplacement, "old-connect-result");
                    check(disconnectId.equals(intentId(intents)), "Old Connect result cleared newer Disconnect");
                    event(lostResultReplacement, new JSONObject().put("type", "connect_intent_result")
                        .put("command_id", disconnectId).put("accepted", "true"));
                    barrier(intents, lostResultReplacement, "malformed-connect-result");
                    check(disconnectId.equals(intentId(intents)), "Malformed result cleared current intent");
                    event(lostResultReplacement, new JSONObject().put("type", "connect_intent_result")
                        .put("command_id", disconnectId).put("accepted", true));
                    barrier(intents, lostResultReplacement, "current-connect-result");
                    check(field(intents, "pendingConnectIntent") == null, "Matching result did not clear pending intent");
                    hello(lostResultReplacement);
                    check(resultReader.readLine().contains("\"type\":\"ui_visible\""), "Acknowledged intent remained for replay");
                    noMoreLines(lostResultReplacement, resultReader);

                    Object writerOwner = current(intents);
                    Field writerSocket = writerOwner.getClass().getDeclaredField("socket");
                    writerSocket.setAccessible(true);
                    synchronized (intents)
                    {
                        ((Socket)writerSocket.get(writerOwner)).shutdownOutput();
                        intents.sendDisconnect();
                    }
                    long disconnectFailureDeadline = System.currentTimeMillis() + 2000;
                    while (current(intents) != null && System.currentTimeMillis() < disconnectFailureDeadline)
                    {
                        Thread.sleep(5);
                    }
                    check(current(intents) == null && field(intents, "pendingConnectIntent") != null,
                        "Actual writer failure lost explicit Disconnect intent");
                    String failedDisconnectId = intentId(intents);
                    try (Socket finalPeer = connect(intents))
                    {
                        waitForConnection(intents, null);
                        hello(finalPeer);
                        BufferedReader finalReader = new BufferedReader(new InputStreamReader(finalPeer.getInputStream(), StandardCharsets.UTF_8));
                        String finalIntent = finalReader.readLine();
                        check(finalIntent.contains("\"type\":\"disconnect\"") && finalIntent.contains(failedDisconnectId),
                            "Failed Disconnect did not replay with original scope");
                        check(finalReader.readLine().contains("\"type\":\"ui_visible\""), "Final presence not replayed");
                        event(finalPeer, new JSONObject().put("type", "connect_intent_result")
                            .put("command_id", failedDisconnectId).put("accepted", false));
                        barrier(intents, finalPeer, "rejected-connect-result");
                        check(field(intents, "pendingConnectIntent") == null, "Matching rejection left intent pending");
                    }
                }
            }
        }
        finally
        {
            intents.close();
        }
        System.out.println("PASS: actual Connect/Disconnect writer failure retention, scoped retry after lost result, last-intent wins, matching result cleanup");

        NodeBridge settingsBridge = new NodeBridge(null);
        check(settingsBridge.start(), "Settings publication bridge startup failed");
        BlockingSettings publication = new BlockingSettings();
        ChatSettings previousSettings = ChatSettings.instance;
        ChatSettings.instance = publication;
        try (Socket settingsPeer = connect(settingsBridge))
        {
            waitForConnection(settingsBridge, null);
            publication.json = new JSONObject().put("host", "old.invalid").toString();
            Thread oldPublication = new Thread(settingsBridge::sendSettings, "old-settings-publication");
            oldPublication.start();
            check(publication.captured.await(2, TimeUnit.SECONDS), "Old settings snapshot was not captured");
            publication.json = new JSONObject().put("host", "new.invalid").toString();
            CountDownLatch newerFinished = new CountDownLatch(1);
            Thread newPublication = new Thread(() ->
            {
                settingsBridge.sendSettings();
                newerFinished.countDown();
            }, "new-settings-publication");
            newPublication.start();
            // Before the fix, B completes while A is paused and its newer snapshot is written first.
            newerFinished.await(250, TimeUnit.MILLISECONDS);
            publication.release.countDown();
            oldPublication.join(2000);
            newPublication.join(2000);
            check(!oldPublication.isAlive() && !newPublication.isAlive(), "Settings publications did not settle");
            BufferedReader reader = new BufferedReader(new InputStreamReader(settingsPeer.getInputStream(), StandardCharsets.UTF_8));
            JSONObject first = new JSONObject(new JSONObject(reader.readLine()).getString("json"));
            JSONObject second = new JSONObject(new JSONObject(reader.readLine()).getString("json"));
            check("old.invalid".equals(first.getString("host")) && "new.invalid".equals(second.getString("host")),
                "Older settings snapshot overtook the newer committed snapshot");
        }
        finally
        {
            publication.release.countDown();
            ChatSettings.instance = previousSettings;
            settingsBridge.close();
        }
        System.out.println("PASS: concurrent settings capture and publication preserve commit order across the real writer");

        BackgroundService endedService = new BackgroundService();
        NodeBridge endedCalls = new NodeBridge(endedService);
        Method endedEvent = NodeBridge.class.getDeclaredMethod("handleEvent", String.class);
        endedEvent.setAccessible(true);
        String endedId = "00000000000000000000000000000009";
        String endedCall = new JSONObject().put("type", "call").put("call_id", endedId)
            .put("session_id", "ended-session").put("expires_at", System.currentTimeMillis() + 60000).toString();
        endedEvent.invoke(endedCalls, endedCall);
        check(field(endedCalls, "pendingCall") != null, "Current call was not cached");
        JSONObject capturedCall = (JSONObject)field(endedCalls, "pendingCall");
        endedEvent.invoke(endedCalls, new JSONObject().put("type", "status").put("connected", false).toString());
        check(field(endedCalls, "pendingCall") == null && endedService.canceledCalls == 1,
            "Disconnected session retained its cached/displayed incoming call");
        // attachService can pause after capturing its local call and resume after disconnect cleanup.
        Method publish = NodeBridge.class.getDeclaredMethod("publishCall", BackgroundService.class, JSONObject.class);
        publish.setAccessible(true);
        publish.invoke(endedCalls, endedService, capturedCall);
        check(endedService.shownCalls == 1, "Paused service attach republished a retired call");
        endedEvent.invoke(endedCalls, endedCall);
        capturedCall = (JSONObject)field(endedCalls, "pendingCall");
        endedCalls.sendCallAction("accept", endedId, "ended-session");
        publish.invoke(endedCalls, endedService, capturedCall);
        check(endedService.shownCalls == 2, "Paused service attach republished an already-submitted call");
        Object submittedAction = field(endedCalls, "pendingCallAction");
        endedEvent.invoke(endedCalls, new JSONObject().put("type", "status").put("connected", false).toString());
        check(field(endedCalls, "pendingCallAction") == submittedAction,
            "Connection status consumed an already-submitted action without its scoped result");
        endedCalls.close();
        System.out.println("PASS: disconnected status retires cached/displayed incoming calls without consuming a submitted action");

        NodeBridge calls = new NodeBridge(null);
        check(calls.start(), "Call replay fixture bridge startup failed");
        Method handleCall = NodeBridge.class.getDeclaredMethod("handleEvent", String.class);
        handleCall.setAccessible(true);
        String firstId = "00000000000000000000000000000001";
        String secondId = "00000000000000000000000000000002";
        String thirdId = "00000000000000000000000000000003";
        long callExpiry = System.currentTimeMillis() + 60000;
        String firstCall = new JSONObject().put("type", "call").put("call_id", firstId)
            .put("session_id", "session").put("expires_at", callExpiry).toString();
        handleCall.invoke(calls, firstCall);
        calls.sendUiVisible(false, false, true);
        calls.sendCallAction("accept", firstId, "session");
        Object originalAction = field(calls, "pendingCallAction");
        check(field(calls, "pendingCall") == null && originalAction != null, "Disconnected Accept did not retain only its action");
        handleCall.invoke(calls, firstCall);
        check(field(calls, "pendingCall") == null, "Pending action's repeated call replayed notification");
        try (Socket firstPeer = connect(calls))
        {
            waitForConnection(calls, null);
            Object firstOwner = current(calls);
            hello(firstPeer);
            BufferedReader firstReader = new BufferedReader(new InputStreamReader(firstPeer.getInputStream(), StandardCharsets.UTF_8));
            check(firstReader.readLine().contains("\"force_idle\":true"), "Presence must replay before accepting call");
            String firstAction = firstReader.readLine();
            check(firstAction.contains("\"action\":\"accept\"") && firstAction.contains(firstId), "Disconnected Accept not replayed");
            event(firstPeer, new JSONObject().put("type", "call_action_result").put("call_id", firstId)
                .put("session_id", "old-session").put("accepted", true));
            barrier(calls, firstPeer, "stale-result");
            check(field(calls, "pendingCallAction") == originalAction, "Old-session result consumed current action");

            // The first action reached Node, but its result was lost with this control socket.
            try (Socket replacement = connect(calls))
            {
                waitForConnection(calls, firstOwner);
                hello(replacement);
                BufferedReader reader = new BufferedReader(new InputStreamReader(replacement.getInputStream(), StandardCharsets.UTF_8));
                check(reader.readLine().contains("\"force_idle\":true"), "Replacement did not replay retained presence");
                check(firstAction.equals(reader.readLine()), "Unacknowledged action did not retry exactly");
                event(replacement, new JSONObject().put("type", "call_action_result").put("call_id", firstId)
                    .put("session_id", "session").put("accepted", true));
                barrier(calls, replacement, "accepted-result");
                check(field(calls, "pendingCallAction") == null, "Matching result did not consume retained action");
                check(Boolean.FALSE.equals(field(calls, "uiForceIdle")), "Accepted call did not clear retained forced idle");
                hello(replacement);
                check(reader.readLine().contains("\"force_idle\":false"), "Old force replayed after accepted result");
                noMoreLines(replacement, reader);

                event(replacement, new JSONObject().put("type", "call").put("call_id", secondId)
                    .put("session_id", "session").put("expires_at", callExpiry));
                barrier(calls, replacement, "second-call");
                calls.sendCallAction("decline", firstId, "session");
                check(field(calls, "pendingCall") != null, "Stale action erased newer call");
                calls.sendCallAction("decline", secondId, "session");
                check(reader.readLine().contains("\"action\":\"decline\""), "Current Decline not written");
                check(field(calls, "pendingCall") == null, "Declined call remained for service replay");
                Object secondAction = field(calls, "pendingCallAction");
                event(replacement, new JSONObject().put("type", "call_action_result").put("call_id", firstId)
                    .put("session_id", "session").put("accepted", false));
                barrier(calls, replacement, "replaced-result");
                check(field(calls, "pendingCallAction") == secondAction, "Old-call result erased new pending action");
                event(replacement, new JSONObject().put("type", "call").put("call_id", thirdId)
                    .put("session_id", "session").put("expires_at", System.currentTimeMillis() + 500));
                barrier(calls, replacement, "third-call");
                check(field(calls, "pendingCallAction") == null, "New call did not replace old pending action");
                calls.sendCallAction("decline", thirdId, "session");
                check(reader.readLine().contains(thirdId), "Current expiring Decline not written");
                long expiresDeadline = System.currentTimeMillis() + 2000;
                while (field(calls, "pendingCallAction") != null && System.currentTimeMillis() < expiresDeadline)
                {
                    Thread.sleep(5);
                }
                check(field(calls, "pendingCallAction") == null, "Original call expiry did not discard retained action");
                hello(replacement);
                check(reader.readLine().contains("\"type\":\"ui_visible\""), "Expiry fixture did not replay presence");
                noMoreLines(replacement, reader);
                event(replacement, new JSONObject().put("type", "call").put("call_id", secondId)
                    .put("session_id", "replacement-session").put("expires_at", callExpiry));
                barrier(calls, replacement, "decline-result-call");
                calls.sendCallAction("decline", secondId, "replacement-session");
                check(reader.readLine().contains("replacement-session"), "Current Decline did not retain its session");
                event(replacement, new JSONObject().put("type", "call_action_result").put("call_id", secondId)
                    .put("session_id", "replacement-session").put("accepted", "false"));
                barrier(calls, replacement, "invalid-result");
                check(field(calls, "pendingCallAction") != null, "Malformed result consumed retained action");
                event(replacement, new JSONObject().put("type", "call_action_result").put("call_id", secondId)
                    .put("session_id", "replacement-session").put("accepted", false));
                barrier(calls, replacement, "rejected-result");
                check(field(calls, "pendingCallAction") == null, "Matching rejection did not finish retained action");
                event(replacement, new JSONObject().put("type", "call").put("call_id", thirdId)
                    .put("session_id", "replacement-session").put("expires_at", callExpiry));
                barrier(calls, replacement, "superseded-accept-call");
                calls.sendCallAction("accept", thirdId, "replacement-session");
                check(reader.readLine().contains("\"action\":\"accept\""), "Current Accept was not sent before task removal");
                calls.sendUiVisible(false);
                check(reader.readLine().contains("\"visible\":false"), "Ordinary hidden state was not sent");
                check(field(calls, "pendingCallAction") != null, "Ordinary hiding erased pending Accept");
                calls.sendUiVisible(false, false, true);
                check(reader.readLine().contains("\"force_idle\":true"), "Later task removal did not force idle");
                check(field(calls, "pendingCallAction") == null, "Later explicit task removal did not supersede pending Accept");
                event(replacement, new JSONObject().put("type", "call_action_result").put("call_id", thirdId)
                    .put("session_id", "replacement-session").put("accepted", true));
                barrier(calls, replacement, "late-accept-after-swipe");
                check(Boolean.TRUE.equals(field(calls, "uiForceIdle")), "Late Accept result undid newer task removal");
            }
        }
        finally
        {
            calls.close();
        }
        System.out.println("PASS: disconnected call action retained, retry after lost result, scoped acknowledgment, replacement/expiry cleanup, accepted presence, no notification replay");
    }
}'''

for name in ["ChatSettings.java", "NodeBridge.java", "BridgeProtocol.java"]:
    fixtures["com/lemonchat/" + name] = (JAVA / name).read_text(encoding="utf-8")
compile_run(fixtures, "com.lemonchat.NativeRegression")
