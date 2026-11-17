"""A completed wizard waits for its resumed activity and bound service."""
from test_support import JAVA, extract, compile_run

source = (JAVA / 'MainActivity.java').read_text(encoding='utf-8')
methods = '\n'.join(extract(source, signature) for signature in [
    '@Override protected void onActivityResult(int requestCode, int resultCode, Intent data)',
    'public void runConnectionSetup(View view)',
    'private void connectAfterWizard()',
    'private void offerFirstConnection()'])
fixture = r'''package com.lemonchat;
class Intent
{
}
class View
{
}
class ActivityBase
{
    static final int RESULT_OK = -1;
    protected void onActivityResult(int requestCode, int resultCode, Intent data)
    {
    }
}
class ConnectionWizardActivity
{
    static final int ADVANCED_SETTINGS = 1;
}
class NodeBridge
{
    int connects;
    void sendConnect()
    {
        connects++;
    }
}
class BackgroundService
{
    NodeBridge nodeBridge = new NodeBridge();
    int fileResults;
    void completeFileChooser(Object activity, int requestCode, int resultCode, Intent data)
    {
        fileResults++;
    }
}
class Settings
{
    String host = "";
    String failure = "";
    String getInitializationError()
    {
        return failure;
    }
    String getIpAddress()
    {
        return host;
    }
}
class Permissions
{
    boolean prepareConnection()
    {
        return true;
    }
}
public class WizardChecks extends ActivityBase
{
    private static final int CONNECTION_WIZARD = 208;
    boolean connectionWizardOpen;
    boolean pendingWizardConnect;
    boolean pendingAdvancedSettings;
    boolean firstConnectionOffered;
    boolean resumed;
    boolean destroyed;
    int restores;
    int wizardShows;
    BackgroundService backgroundService;
    Settings settings = new Settings();
    Permissions permissions = new Permissions();
    void restoreWebViewToForeground()
    {
        restores++;
    }
    void showConnectionWizard()
    {
        firstConnectionOffered = true;
        wizardShows++;
    }
__METHODS__
    static void check(boolean value, String message)
    {
        if (!value)
        {
            throw new AssertionError(message);
        }
    }
    public static void main(String[] args)
    {
        WizardChecks app = new WizardChecks();
        app.onActivityResult(CONNECTION_WIZARD, RESULT_OK, null);
        check(app.pendingWizardConnect && app.restores == 0, "save completion waits for service binding");
        app.backgroundService = new BackgroundService();
        app.connectAfterWizard();
        check(app.backgroundService.nodeBridge.connects == 0, "paused activity cannot attach the chat view");
        app.resumed = true;
        app.connectAfterWizard();
        check(!app.pendingWizardConnect && app.backgroundService.nodeBridge.connects == 1 && app.restores == 1, "resumed owner delivers the final Connect once");
        app.connectAfterWizard();
        check(app.backgroundService.nodeBridge.connects == 1, "repeated binding does not reconnect twice");
        app.onActivityResult(CONNECTION_WIZARD, 0, null);
        check(!app.pendingWizardConnect && app.backgroundService.nodeBridge.connects == 1, "Cancel does not connect");
        app.onActivityResult(CONNECTION_WIZARD, ConnectionWizardActivity.ADVANCED_SETTINGS, null);
        check(app.pendingAdvancedSettings && app.restores == 1, "advanced form waits until resume finishes");
        app.onActivityResult(99, RESULT_OK, new Intent());
        check(app.backgroundService.fileResults == 1, "unrelated file results retain their owner");
        app.settings.failure = "legacy_identity_conflict";
        app.offerFirstConnection();
        check(app.wizardShows == 0, "identity recovery stays ahead of connection setup");
        app.settings.failure = "";
        app.settings.host = "saved.example";
        app.offerFirstConnection();
        check(app.wizardShows == 0, "saved connections skip first setup");
        app.settings.host = "";
        app.offerFirstConnection();
        app.offerFirstConnection();
        check(app.wizardShows == 1, "first setup is offered once per flow");
        WizardChecks existing = new WizardChecks();
        existing.settings.host = "saved.example";
        existing.backgroundService = new BackgroundService();
        existing.runConnectionSetup(new View());
        check(existing.wizardShows == 1 && existing.firstConnectionOffered, "Run setup again explicitly opens the walkthrough for an existing connection");
        check(existing.backgroundService.nodeBridge.connects == 0 && existing.settings.host.equals("saved.example"), "opening the optional walkthrough does not connect or replace saved settings");
        existing.onActivityResult(CONNECTION_WIZARD, 0, null);
        check(!existing.pendingWizardConnect && existing.backgroundService.nodeBridge.connects == 0, "cancelling the optional walkthrough keeps the existing connection");
        System.out.println("PASS: 13 wizard lifecycle checks for deferred Connect, cancellation, advanced form, file callbacks and optional setup");
    }
}
'''
compile_run({'com/lemonchat/WizardChecks.java': fixture.replace('__METHODS__', methods)}, 'com.lemonchat.WizardChecks')
