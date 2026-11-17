"""Optional prompts follow Connect and never block it after a refusal."""
from test_support import JAVA, extract, compile_run

source = (JAVA / 'Permissions.java').read_text(encoding='utf-8')
methods = '\n'.join(extract(source, signature) for signature in [
    'public void beginCheck()', 'public boolean prepareConnection()',
    'public void resumeCheck()', 'public void restoreState(', 'public void saveState('])
fixture = r'''package com.lemonchat;
class Bundle
{
    java.util.Map<String, Boolean> values = new java.util.HashMap<>();
    boolean getBoolean(String key)
    {
        return values.getOrDefault(key, false);
    }
    void putBoolean(String key, boolean value)
    {
        values.put(key, value);
    }
}
class Build
{
    static class VERSION
    {
        static int SDK_INT = 35;
    }
}
class Context
{
    static final String POWER_SERVICE = "power";
}
class Manifest
{
    static class permission
    {
        static final String POST_NOTIFICATIONS = "notifications";
    }
}
class PowerManager
{
    boolean exempt;
    boolean isIgnoringBatteryOptimizations(String packageName)
    {
        return exempt;
    }
}
class NotificationManagerCompat
{
    MainActivity owner;
    static NotificationManagerCompat from(MainActivity owner)
    {
        NotificationManagerCompat manager = new NotificationManagerCompat();
        manager.owner = owner;
        return manager;
    }
    boolean areNotificationsEnabled()
    {
        return owner.notifications;
    }
}
class MainActivity
{
    final PowerManager power = new PowerManager();
    boolean notifications;
    boolean visible = true;
    int setup;
    int continuations;
    String getPackageName()
    {
        return "com.lemonchat";
    }
    Object getSystemService(String key)
    {
        return power;
    }
    boolean isMediaActivityVisible()
    {
        return visible;
    }
    void promptForAppModeIfNeeded()
    {
        setup++;
    }
    void resumePendingConnection()
    {
        continuations++;
    }
}
class Launcher
{
    int requests;
    boolean fail;
    void launch(String permission)
    {
        requests++;
        if (fail)
        {
            throw new IllegalStateException("test refusal");
        }
    }
}
class Permissions
{
    MainActivity context;
    Launcher notificationResult = new Launcher();
    boolean notificationRequested;
    boolean batteryRequested;
    boolean requestPending;
    int batteryPrompts;
    Permissions(MainActivity context)
    {
        this.context = context;
    }
    void offerBackgroundDelivery()
    {
        batteryPrompts++;
    }
__METHODS__
}
public class PermissionChecks
{
    static int checks;
    static void check(boolean value, String message)
    {
        if (!value)
        {
            throw new AssertionError(message);
        }
        checks++;
    }
    public static void main(String[] args)
    {
        MainActivity owner = new MainActivity();
        Permissions permissions = new Permissions(owner);
        permissions.beginCheck();
        check(owner.setup == 1 && permissions.notificationResult.requests == 0 && permissions.batteryPrompts == 0, "first launch offers setup without permission prompts");
        owner.visible = false;
        check(!permissions.prepareConnection() && permissions.notificationResult.requests == 0, "hidden activity cannot launch permission UI");
        owner.visible = true;
        check(!permissions.prepareConnection() && permissions.notificationResult.requests == 1, "Connect requests notification permission");
        check(!permissions.prepareConnection() && permissions.notificationResult.requests == 1, "repeated Connect does not stack a prompt");
        permissions.resumeCheck();
        check(owner.continuations == 1, "refusal resumes the original action");
        check(!permissions.prepareConnection() && permissions.batteryPrompts == 1, "notification refusal advances to optional battery explanation");
        permissions.resumeCheck();
        check(permissions.prepareConnection(), "battery refusal permits connection");
        check(permissions.prepareConnection() && permissions.batteryPrompts == 1 && permissions.notificationResult.requests == 1, "later connection retries do not repeat refusals");
        Bundle state = new Bundle();
        permissions.saveState(state);
        Permissions rotated = new Permissions(owner);
        rotated.restoreState(state);
        check(rotated.prepareConnection() && rotated.batteryPrompts == 0 && rotated.notificationResult.requests == 0, "rotation preserves offered decisions without secret data");
        check(state.values.size() == 2, "saved state contains only prompt decisions");
        owner.power.exempt = true;
        owner.notifications = true;
        Permissions granted = new Permissions(owner);
        check(granted.prepareConnection() && granted.notificationResult.requests == 0, "existing grants continue without prompts");
        owner.notifications = false;
        Permissions refused = new Permissions(owner);
        refused.notificationResult.fail = true;
        check(refused.prepareConnection(), "unavailable permission dialog does not strand Connect");
        Build.VERSION.SDK_INT = 32;
        Permissions old = new Permissions(owner);
        check(old.prepareConnection() && old.notificationResult.requests == 0, "pre33 does not request an unavailable permission");
        System.out.println("PASS: " + checks + " contextual permission and rotation checks");
    }
}
'''
compile_run({'com/lemonchat/PermissionChecks.java': fixture.replace('__METHODS__', methods)}, 'com.lemonchat.PermissionChecks')
