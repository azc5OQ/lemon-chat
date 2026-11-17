"""Saving connection settings must not override an explicit Disconnect."""
from test_support import JAVA, FIXTURES, extract, compile_run

activity = (JAVA / "MainActivity.java").read_text(encoding="utf-8")
bridge = (JAVA / "JavascriptJavaBridge.java").read_text(encoding="utf-8")
fixture = r'''package com.lemonchat;
import java.util.HashMap;
import org.json.JSONObject;
import org.json.JSONArray;
@interface JavascriptInterface
{
}
class View
{
}
class WebView
{
    final java.util.ArrayDeque<Runnable> work = new java.util.ArrayDeque<>();
    void post(Runnable action)
    {
        work.add(action);
    }
    void drain()
    {
        while (!work.isEmpty())
        {
            work.remove().run();
        }
    }
}
class LinearLayout extends View
{
}
class EditText extends View
{
    String text;
    EditText(String text)
    {
        this.text = text;
    }
    String getText()
    {
        return this.text;
    }
}
class SwitchMaterial extends View
{
    boolean isChecked()
    {
        return true;
    }
}
class R
{
    static class id
    {
        static final int keys_container = 1;
        static final int ip_address = 2;
        static final int websocket_port = 3;
        static final int default_username = 4;
        static final int settings_autoconnect_enabled = 5;
    }
}
class Preferences
{
    String getString(String key, String fallback)
    {
        return fallback;
    }
}
class Settings
{
    int saves;
    boolean succeeds = true;
    boolean saveJsonSettings(String json)
    {
        this.saves++;
        return this.succeeds;
    }
}
class NodeBridge
{
    int connections;
    int disconnections;
    void sendConnect()
    {
        this.connections++;
    }
    void sendDisconnect()
    {
        this.disconnections++;
    }
}
class BackgroundService
{
    WebView webView = new WebView();
    long generation = 1;
    long getPageGeneration()
    {
        return generation;
    }
    NodeBridge nodeBridge = new NodeBridge();
    boolean trusted = true;
    boolean isTrustedPage()
    {
        return this.trusted;
    }
}
class Toast
{
    static final int LENGTH_LONG = 1;
    static Toast makeText(Object activity, String text, int duration)
    {
        return new Toast();
    }
    void show()
    {
    }
}
class Log
{
    static void e(String tag, String text, Exception error)
    {
        throw new AssertionError(text, error);
    }
}
class MainActivity
{
    static MainActivity instance;
    boolean visible = true;
    boolean isMediaActivityVisible()
    {
        return visible;
    }
    void requestConnection()
    {
        if (backgroundService.nodeBridge != null)
        {
            backgroundService.nodeBridge.sendConnect();
        }
    }
    BackgroundService backgroundService = new BackgroundService();
    Settings settings = new Settings();
    Preferences preferences = new Preferences();
    HashMap<Integer, View> views = new HashMap<>();
    int restored;
    MainActivity()
    {
        instance = this;
        this.views.put(R.id.keys_container, new LinearLayout());
        this.views.put(R.id.ip_address, new EditText("synthetic.invalid"));
        this.views.put(R.id.websocket_port, new EditText("12345"));
        this.views.put(R.id.default_username, new EditText("Maya"));
        this.views.put(R.id.settings_autoconnect_enabled, new SwitchMaterial());
    }
    @SuppressWarnings("unchecked")
    <T extends View> T findViewById(int id)
    {
        return (T)this.views.get(id);
    }
    JSONArray readKeyFields()
    {
        return new JSONArray();
    }
    void restoreWebViewToForeground()
    {
        this.restored++;
    }
__SAVE_METHOD__
}
class JavascriptJavaBridge
{
    BackgroundService backgroundServiceInstance;
__CONNECT_METHOD__
__DISCONNECT_METHOD__
}
public class SettingsActionProbe
{
    static void check(boolean condition, String message)
    {
        if (!condition)
        {
            throw new AssertionError(message);
        }
    }
    public static void main(String[] args)
    {
        MainActivity activity = new MainActivity();
        activity.saveSettings(new View());
        check(activity.settings.saves == 1 && activity.restored == 1, "Successful Save must persist and return to chat");
        check(activity.backgroundService.nodeBridge.connections == 0, "Save must not override explicit Disconnect");
        activity.settings.succeeds = false;
        activity.saveSettings(new View());
        check(activity.settings.saves == 2 && activity.restored == 1, "Failed Save must leave the form open");
        check(activity.backgroundService.nodeBridge.connections == 0, "Failed Save must not connect");
        JavascriptJavaBridge bridge = new JavascriptJavaBridge();
        bridge.backgroundServiceInstance = activity.backgroundService;
        bridge.JavaExportConnect();
        activity.backgroundService.webView.drain();
        check(activity.backgroundService.nodeBridge.connections == 1, "Explicit trusted Connect must still work");
        bridge.JavaExportDisconnect();
        check(activity.backgroundService.nodeBridge.disconnections == 1, "Trusted page Disconnect must reach the native retained intent");
        activity.backgroundService.trusted = false;
        bridge.JavaExportConnect();
        bridge.JavaExportDisconnect();
        check(activity.backgroundService.nodeBridge.connections == 1, "Untrusted Connect must remain rejected");
        check(activity.backgroundService.nodeBridge.disconnections == 1, "Untrusted Disconnect must remain rejected");
        activity.backgroundService.trusted = true;
        bridge.JavaExportConnect();
        activity.backgroundService.generation++;
        activity.backgroundService.webView.drain();
        check(activity.backgroundService.nodeBridge.connections == 1, "Queued Connect cannot cross document replacement");
        bridge.JavaExportConnect();
        activity.visible = false;
        activity.backgroundService.webView.drain();
        check(activity.backgroundService.nodeBridge.connections == 1, "Queued Connect cannot open permission UI after the activity hides");
        activity.visible = true;
        activity.backgroundService.nodeBridge = null;
        bridge.JavaExportConnect();
        activity.backgroundService.webView.drain();
        bridge.JavaExportDisconnect();
        System.out.println("PASS: Save persists without Connect, failed Save retains form, trusted native Connect/Disconnect remain available and tolerate absent owner");
    }
}'''
fixture = fixture.replace("__SAVE_METHOD__", extract(activity, "public void saveSettings(View view)"))
fixture = fixture.replace("__CONNECT_METHOD__", extract(bridge, "@JavascriptInterface public void JavaExportConnect()").replace('android.webkit.WebView', 'WebView'))
fixture = fixture.replace("__DISCONNECT_METHOD__", extract(bridge, "@JavascriptInterface public void JavaExportDisconnect()"))
sources = {"com/lemonchat/SettingsActionProbe.java": fixture}
for name in ["JSONObject.java", "JSONArray.java"]:
    sources["org/json/" + name] = (FIXTURES / "identity/org/json" / name).read_text(encoding="utf-8")
compile_run(sources, "com.lemonchat.SettingsActionProbe")
