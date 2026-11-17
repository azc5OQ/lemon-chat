package com.lemonchat;

import android.Manifest;
import android.app.Activity;
import android.app.ActivityManager;
import android.app.Application;
import android.app.Instrumentation;
import android.app.NotificationManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.os.Build;
import android.os.Bundle;
import android.os.SystemClock;
import android.service.notification.StatusBarNotification;
import android.webkit.WebView;
import android.widget.EditText;
import android.widget.LinearLayout;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import com.google.android.material.switchmaterial.SwitchMaterial;
import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Field;
import java.util.concurrent.Callable;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONObject;
import org.json.JSONTokener;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.*;

/** tests the real page, native settings and retained service against a disposable server */
@RunWith(AndroidJUnit4.class)
public final class LiveClientTest
{
    private Instrumentation instrumentation;
    private Context target;
    private BackgroundService service;
    private MainActivity activity;
    private File artifacts;
    private int checks;

    @Test(timeout = 300000)
    public void realClientSurvivesBackgroundAndTaskRemoval() throws Exception
    {
        instrumentation = InstrumentationRegistry.getInstrumentation();
        assertTrue("Use the production service runner", instrumentation instanceof ServiceRuntimeTestRunner);
        target = instrumentation.getTargetContext();
        assertEquals("Live checks require the isolated verification installation", "com.lemonchat.hybridverification", target.getPackageName());
        Bundle arguments = InstrumentationRegistry.getArguments();
        int port = Integer.parseInt(arguments.getString("serverPort", "29111"));
        assertTrue(port >= 1024 && port <= 65535);
        String username = "HybridPhone" + Long.toString(System.currentTimeMillis(), 36);
        String sent = "hybrid-live-" + username;
        artifacts = new File(target.getCacheDir(), "live-check-" + username);
        assertTrue(artifacts.mkdirs());
        assertTrue("Grant notification permission to the verification app before running this test", Build.VERSION.SDK_INT < 33 || target.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED);
        Application application = (Application)target.getApplicationContext();
        AtomicReference<Throwable> onboardingFailure = new AtomicReference<>();
        Application.ActivityLifecycleCallbacks lifecycle = Lifecycle(onboardingFailure);
        ChatSettings.attachContext(target);
        ChatSettings settings = ChatSettings.getInstance();
        assertTrue("Existing identity must remain readable", settings.loadPersistedState());
        String originalSettings = settings.getJsonSettings();
        application.registerActivityLifecycleCallbacks(lifecycle);
        try
        {
            Launch();
            Await(() -> Main(() -> service != null && service.nodeBridge != null && service.webView != null && service.isTrustedPage()), 45000, "the production page becomes trusted");
            assertNull("Permission onboarding isolation failed", onboardingFailure.get());
            Main(() ->
            {
                service.nodeBridge.sendDisconnect();
                activity.showSettings();
                ((EditText)activity.findViewById(R.id.ip_address)).setText("127.0.0.1");
                ((EditText)activity.findViewById(R.id.websocket_port)).setText(Integer.toString(port));
                ((EditText)activity.findViewById(R.id.default_username)).setText(username);
                ((SwitchMaterial)activity.findViewById(R.id.settings_autoconnect_enabled)).setChecked(true);
                ((LinearLayout)activity.findViewById(R.id.keys_container)).removeAllViews();
                return null;
            });
            Screenshot("01-address-and-port");
            Main(() ->
            {
                activity.findViewById(R.id.save_settings).performClick();
                return null;
            });
            JSONObject saved = new JSONObject(settings.getJsonSettings());
            Check(saved.getString("host").equals("127.0.0.1") && saved.getInt("port") == port, "native form saves plain address and separate port");
            Check(saved.getString("identity_string").equals(new JSONObject(originalSettings).getString("identity_string")), "connection changes preserve the existing identity");
            service.nodeBridge.sendConnect();
            Await(() -> Snapshot().optBoolean("authenticated"), 90000, "the visible page authenticates through the production Node connection");
            JSONObject connected = Snapshot();
            int clientId = connected.getInt("client_id");
            String sessionId = connected.getString("session_id");
            Check(clientId >= 0 && !sessionId.isEmpty(), "authenticated state includes the current client and session");
            Check(connected.getInt("members") >= 2, "the page receives the real server member list");
            Await(() -> Boolean.TRUE.equals(Script("what.read_state().current_channel_keys!==null&&what.read_state().current_channel_id>=0")), 30000, "the current channel has usable content keys");
            Check(Boolean.TRUE.equals(Script("(document.querySelector('[data-chat-context-selector-id=\"channel-'+what.read_state().current_channel_id+'\"]')?.click(),what.read_state().g_current_chat_context_id==='chat-context-channel-'+what.read_state().current_channel_id)")), "the actual current-channel tab selects the send destination");
            Screenshot("02-connected-chat");
            String submit = "(function(){var input=document.getElementById('chat-input-container-text-input');var button=document.getElementById('chat-input-container-send-chat-input');if(!input||!button){return false;}input.value=" + JSONObject.quote(sent) + ";input.dispatchEvent(new Event('input',{bubbles:true}));button.click();return true;})()";
            Check(Boolean.TRUE.equals(Script(submit)), "the actual composer submits a test message");
            String acknowledged = "(function(){return Array.from(document.querySelectorAll('[data-single-chat-message-server-message-id]')).some(function(item){return item.textContent.indexOf(" + JSONObject.quote(sent) + ")>=0 && /^\\d+$/.test(item.getAttribute('data-single-chat-message-server-message-id'));});})()";
            Await(() -> Boolean.TRUE.equals(Script(acknowledged)), 20000, "the server acknowledges the sent channel message");
            Screenshot("03-message-confirmed");
            NodeBridge bridge = service.nodeBridge;
            int loopbackPort = bridge.getLoopbackPort();
            Main(() ->
            {
                NotificationManager notifications = target.getSystemService(NotificationManager.class);
                notifications.cancel(2);
                notifications.cancel(5);
                activity.moveTaskToBack(true);
                return null;
            });
            Await(() -> Main(() -> MainActivity.instance == null), 10000, "the activity leaves the foreground");
            Check(Main(() -> BackgroundService.isRunning && service.nodeBridge == bridge && bridge.getLoopbackPort() == loopbackPort), "backgrounding retains the service and runtime endpoint");
            Progress("HYBRID_BACKGROUND_READY username=" + username);
            if (arguments.getString("incoming", "false").equals("true"))
            {
                Incoming();
            }
            Main(() ->
            {
                ActivityManager manager = target.getSystemService(ActivityManager.class);
                for (ActivityManager.AppTask task : manager.getAppTasks())
                {
                    if (task.getTaskInfo().baseIntent.getComponent() != null && target.getPackageName().equals(task.getTaskInfo().baseIntent.getComponent().getPackageName()))
                    {
                        task.finishAndRemoveTask();
                    }
                }
                return null;
            });
            Await(() -> Boolean.TRUE.equals(Field(bridge, "uiForceIdle")), 10000, "task removal reaches the service's forced idle path");
            Check(Main(() -> BackgroundService.isRunning && service.nodeBridge == bridge && bridge.getLoopbackPort() == loopbackPort), "removing the task retains the service and embedded runtime");
            Launch();
            Await(() -> Snapshot().optBoolean("authenticated"), 30000, "the page reattaches after task removal");
            JSONObject restored = Snapshot();
            Check(restored.getInt("client_id") == clientId && restored.getString("session_id").equals(sessionId), "reattachment keeps the authenticated server session");
            Screenshot("04-returned-to-chat");
        }
        finally
        {
            try
            {
                if (service != null && service.nodeBridge != null)
                {
                    service.nodeBridge.sendDisconnect();
                }
            }
            finally
            {
                try
                {
                    Check(settings.saveJsonSettings(originalSettings), "test restores the original verification settings");
                }
                finally
                {
                    application.unregisterActivityLifecycleCallbacks(lifecycle);
                    if (activity != null)
                    {
                        Main(() ->
                        {
                            if (!activity.isDestroyed())
                            {
                                activity.finishAndRemoveTask();
                            }
                            return null;
                        });
                    }
                }
            }
        }
        Progress("PASS " + checks + " hybrid live checks; screenshots=" + artifacts.getAbsolutePath());
    }

    private void Incoming() throws Exception
    {
        Await(() -> HasNotification(5), 45000, "a background private message creates a notification");
        Progress("HYBRID_CALL_READY");
        Await(() -> HasNotification(2) && Field(service.nodeBridge, "pendingCall") instanceof JSONObject, 45000, "a real call reaches the native notification and scoped capability");
        JSONObject call = new JSONObject(((JSONObject)Field(service.nodeBridge, "pendingCall")).toString());
        String callId = call.getString("call_id");
        String sessionId = call.getString("session_id");
        Check(callId.matches("[a-fA-F0-9]{32}") && !sessionId.isEmpty(), "incoming call retains the actual server capability");
        Main(() ->
        {
            service.handleCallAction(new Intent(target, BackgroundService.class).setAction(BackgroundService.ACTION_DECLINE_CALL).putExtra("call_id", callId).putExtra("session_id", sessionId));
            return null;
        });
        Await(() -> !HasNotification(2), 10000, "declining removes the incoming call notification");
        Check(Main(() -> MainActivity.instance == null), "declining does not open the main activity");
    }

    private boolean HasNotification(int identifier)
    {
        for (StatusBarNotification notification : target.getSystemService(NotificationManager.class).getActiveNotifications())
        {
            if (notification.getId() == identifier)
            {
                return true;
            }
        }
        return false;
    }

    private void Launch() throws Exception
    {
        activity = (MainActivity)instrumentation.startActivitySync(new Intent(target, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        Await(() ->
        {
            service = (BackgroundService)Main(() -> Field(activity, "backgroundService"));
            return service != null;
        }, 15000, "the activity binds its production service");
    }

    private JSONObject Snapshot() throws Exception
    {
        Object value = Script("(function(){if(!window.what||typeof what.get_ui_session_snapshot!=='function'){return {};}var s=what.get_ui_session_snapshot();return {authenticated:s.authenticated,phase:s.phase,client_id:s.local_client_id,session_id:s.session_id,members:s.clients.length};})()");
        return value instanceof JSONObject ? (JSONObject)value : new JSONObject();
    }

    private Object Script(String source) throws Exception
    {
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<String> result = new AtomicReference<>();
        Main(() ->
        {
            WebView page = service.webView;
            if (page == null)
            {
                done.countDown();
                return null;
            }
            page.evaluateJavascript(source, value ->
            {
                result.set(value);
                done.countDown();
            });
            return null;
        });
        assertTrue("WebView response timed out", done.await(5, TimeUnit.SECONDS));
        return result.get() == null ? JSONObject.NULL : new JSONTokener(result.get()).nextValue();
    }

    private void Screenshot(String name) throws Exception
    {
        Await(() -> Main(() -> MainActivity.instance == activity && activity.hasWindowFocus()), 5000, "the app owns the screen before capture");
        Bitmap image = instrumentation.getUiAutomation().takeScreenshot();
        assertNotNull("The phone did not provide a screenshot", image);
        try (FileOutputStream output = new FileOutputStream(new File(artifacts, name + ".png")))
        {
            assertTrue(image.compress(Bitmap.CompressFormat.PNG, 100, output));
        }
        finally
        {
            image.recycle();
        }
    }

    private void Await(Callable<Boolean> condition, long timeout, String description) throws Exception
    {
        long deadline = SystemClock.elapsedRealtime() + timeout;
        while (SystemClock.elapsedRealtime() < deadline)
        {
            if (condition.call())
            {
                Check(true, description);
                return;
            }
            SystemClock.sleep(150);
        }
        if (description.equals("the visible page authenticates through the production Node connection"))
        {
            DiagnoseAuthentication();
        }
        fail(description);
    }

    private void DiagnoseAuthentication()
    {
        try
        {
            JSONObject state = Snapshot();
            Object phase = service.nodeBridge == null ? "no bridge" : Field(service.nodeBridge, "lastPhaseState");
            boolean visible = Main(() -> MainActivity.instance == activity && activity.hasWindowFocus());
            Progress("AUTH_DIAGNOSTIC page_phase=" + state.optString("phase", "missing") + " authenticated=" + state.optBoolean("authenticated") + " members=" + state.optInt("members") + " native_phase=" + phase + " activity_visible=" + visible);
            if (visible)
            {
                Screenshot("authentication-timeout");
                Progress("AUTH_SCREENSHOT " + artifacts.getAbsolutePath());
            }
        }
        catch (Exception | AssertionError error)
        {
            Progress("AUTH_DIAGNOSTIC unavailable: " + error.getClass().getSimpleName());
        }
    }

    private <T> T Main(Callable<T> action) throws Exception
    {
        AtomicReference<T> result = new AtomicReference<>();
        AtomicReference<Throwable> failure = new AtomicReference<>();
        instrumentation.runOnMainSync(() ->
        {
            try
            {
                result.set(action.call());
            }
            catch (Throwable error)
            {
                failure.set(error);
            }
        });
        if (failure.get() != null)
        {
            throw new AssertionError("Main activity operation failed", failure.get());
        }
        return result.get();
    }

    private static Object Field(Object target, String name) throws Exception
    {
        synchronized (target)
        {
            Field field = target.getClass().getDeclaredField(name);
            field.setAccessible(true);
            return field.get(target);
        }
    }

    private void Check(boolean valid, String description)
    {
        assertTrue(description, valid);
        checks++;
    }

    private void Progress(String text)
    {
        Bundle report = new Bundle();
        report.putString("stream", text + "\n");
        instrumentation.sendStatus(0, report);
    }

    private Application.ActivityLifecycleCallbacks Lifecycle(AtomicReference<Throwable> failure)
    {
        return new Application.ActivityLifecycleCallbacks()
        {
            @Override
            public void onActivityCreated(Activity screen, Bundle saved)
            {
                if (screen instanceof MainActivity)
                {
                    try
                    {
                        // permission prompts are outside this connection and lifecycle check
                        Field field = MainActivity.class.getDeclaredField("hasBegunPermissionChain");
                        field.setAccessible(true);
                        field.set(screen, true);
                    }
                    catch (Throwable error)
                    {
                        failure.set(error);
                    }
                }
            }

            @Override
            public void onActivityStarted(Activity screen)
            {
            }

            @Override
            public void onActivityResumed(Activity screen)
            {
            }

            @Override
            public void onActivityPaused(Activity screen)
            {
            }

            @Override
            public void onActivityStopped(Activity screen)
            {
            }

            @Override
            public void onActivitySaveInstanceState(Activity screen, Bundle saved)
            {
            }

            @Override
            public void onActivityDestroyed(Activity screen)
            {
            }
        };
    }
}
