"""Exercise foreground admission using the production service methods."""
import xml.etree.ElementTree as ET
from test_support import JAVA, extract, compile_run

source = (JAVA / 'BackgroundService.java').read_text(encoding='utf-8')
methods = '\n'.join(extract(source, signature) for signature in [
    'private boolean updateForeground(', 'private boolean admitMicrophone(',
    'public void resumePageMedia(', 'private void retirePageMedia()',
    'public void completeAudioPermission(', 'public void invalidatePage()'])
fixture = r'''package com.lemonchat;
class Build
{
    static class VERSION
    {
        static int SDK_INT = 35;
    }
}
class ServiceInfo
{
    static final int FOREGROUND_SERVICE_TYPE_REMOTE_MESSAGING = 1024;
    static final int FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK = 2;
    static final int FOREGROUND_SERVICE_TYPE_MICROPHONE = 128;
}
class Manifest
{
    static class permission
    {
        static final String RECORD_AUDIO = "microphone";
    }
}
class PackageManager
{
    static final int PERMISSION_GRANTED = 0;
}
class ContextCompat
{
    static int checkSelfPermission(ForegroundChecks service, String permission)
    {
        return service.recordPermission ? 0 : -1;
    }
}
class Log
{
    static void w(String tag, String message, Throwable error)
    {
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
}
class PermissionRequest
{
    static final String RESOURCE_AUDIO_CAPTURE = "audio";
    int grants;
    int denials;
    void grant(String[] resources)
    {
        grants++;
    }
    void deny()
    {
        denials++;
    }
}
public class ForegroundChecks
{
    boolean foregroundAdmitted;
    boolean trustedPage;
    boolean recordPermission;
    boolean refuse;
    long pageGeneration = 1;
    long microphoneGeneration = -1;
    long permissionGeneration;
    Object foregroundNotification;
    MainActivity permissionOwner;
    PermissionRequest pendingPermission;
    boolean permissionPromptPending;
    int mask = -1;
    int changes;
    static int checks;
    void startForeground(int id, Object notification, int types)
    {
        if (refuse)
        {
            throw new SecurityException("test refusal");
        }
        mask = types;
        changes++;
    }
    void startForeground(int id, Object notification)
    {
        startForeground(id, notification, 0);
    }
    void abandonPendingFileChooser()
    {
    }
__METHODS__
    static void check(boolean value, String message)
    {
        if (!value)
        {
            throw new AssertionError(message);
        }
        checks++;
    }
    public static void main(String[] arguments)
    {
        ForegroundChecks service = new ForegroundChecks();
        check(service.updateForeground(false, false) && service.mask == 1024, "boot admits messaging without microphone or playback");
        MainActivity owner = new MainActivity();
        MainActivity.instance = owner;
        service.resumePageMedia(owner);
        check(service.mask == 1024, "untrusted page cannot promote media");
        service.trustedPage = true;
        service.resumePageMedia(owner);
        check(service.mask == 1026, "visible page admits playback without microphone");
        check(!service.admitMicrophone(owner), "missing recording permission refuses capture");
        service.recordPermission = true;
        owner.visible = false;
        check(!service.admitMicrophone(owner), "hidden page cannot start microphone");
        owner.visible = true;
        service.refuse = true;
        check(!service.admitMicrophone(owner) && service.microphoneGeneration == -1, "Android refusal cannot create a document grant");
        service.refuse = false;
        check(service.admitMicrophone(owner) && service.mask == 1154, "visible permitted capture promotes microphone first");
        service.retirePageMedia();
        check(service.mask == 1024, "task removal returns to messaging only");
        owner.visible = false;
        service.resumePageMedia(owner);
        check(service.mask == 1024, "hidden task cannot restore retained media permission");
        owner.visible = true;
        service.resumePageMedia(owner);
        check(service.mask == 1154, "visible retained document restores its cached WebView grant");
        service.invalidatePage();
        check(service.mask == 1024 && service.microphoneGeneration == -1, "document retirement revokes microphone eligibility");
        service.trustedPage = true;
        service.resumePageMedia(owner);
        check(service.mask == 1026, "replacement document does not inherit capture");
        PermissionRequest pending = new PermissionRequest();
        service.pendingPermission = pending;
        service.permissionOwner = owner;
        service.permissionGeneration = service.pageGeneration;
        owner.visible = false;
        service.completeAudioPermission(owner);
        check(pending.grants == 0 && service.pendingPermission == pending, "permission result waits for its visible owner");
        owner.visible = true;
        service.completeAudioPermission(owner);
        check(pending.grants == 1 && service.pendingPermission == null && service.mask == 1154, "resumed owner settles the grant after foreground admission");
        pending = new PermissionRequest();
        service.pendingPermission = pending;
        service.permissionOwner = owner;
        service.permissionGeneration = service.pageGeneration - 1;
        service.completeAudioPermission(owner);
        check(pending.grants == 0 && pending.denials == 1, "stale document result is denied");
        service.recordPermission = false;
        service.resumePageMedia(owner);
        check(service.mask == 1026, "revoked recording permission drops microphone type");
        ForegroundChecks refused = new ForegroundChecks();
        refused.refuse = true;
        check(!refused.updateForeground(false, false) && !refused.foregroundAdmitted, "failed initial admission never marks the service running");
        Build.VERSION.SDK_INT = 29;
        check(service.updateForeground(false, false) && service.mask == 0, "Android29 uses the supported baseline");
        Build.VERSION.SDK_INT = 26;
        check(service.updateForeground(false, false) && service.mask == 0, "Android26 retains the two-argument start API");
        System.out.println("PASS: " + checks + " foreground admission and document lifecycle checks");
    }
}
'''
manifest = ET.parse(JAVA.parent.parent.parent / 'AndroidManifest.xml')
android = '{http://schemas.android.com/apk/res/android}'
permissions = {item.get(android + 'name'): item for item in manifest.getroot().findall('uses-permission')}
for name in ['REMOTE_MESSAGING', 'MEDIA_PLAYBACK', 'MICROPHONE']:
    permission = permissions['android.permission.FOREGROUND_SERVICE_' + name]
    assert permission.get(android + 'maxSdkVersion') is None
component = next(item for item in manifest.getroot().find('application').findall('service') if item.get(android + 'name') == '.BackgroundService')
assert set(component.get(android + 'foregroundServiceType').split('|')) == {'remoteMessaging', 'mediaPlayback', 'microphone'}
compile_run({'com/lemonchat/ForegroundChecks.java': fixture.replace('__METHODS__', methods)}, 'com.lemonchat.ForegroundChecks')
