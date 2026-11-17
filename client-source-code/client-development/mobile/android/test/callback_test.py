"""A destroyed/paused/replaced UI must not receive queued native actions."""
from test_support import JAVA, FIXTURES, extract, compile_run

activity = (JAVA / 'MainActivity.java').read_text(encoding='utf-8')
bridge = (JAVA / 'JavascriptJavaBridge.java').read_text(encoding='utf-8')
service = (JAVA / 'BackgroundService.java').read_text(encoding='utf-8')
fixture = (FIXTURES / 'callback.java.in').read_text(encoding='utf-8')
methods = '\n'.join(extract(activity, signature) for signature in [
    '@Override protected void onDestroy()',
    '@Override protected void onStop()',
    '@Override protected void onPause()',
    'private void handleOnStopOnDestroyOnPause()',
    'private void reportUiVisibility(boolean isVisible)'])
fixture = fixture.replace('__ACTIVITY_METHODS__', methods).replace('__BRIDGE_METHOD__',
    extract(bridge, '@JavascriptInterface public void JavaExportGoToSettings()'))
fixture = fixture.replace('android.webkit.WebView', 'WebView')
fixture = fixture.replace('__TASK_REMOVED__', extract(service, '@Override public void onTaskRemoved(Intent rootIntent)'))
compile_run({'com/lemonchat/NativeCallbackProbe.java': fixture}, 'com.lemonchat.NativeCallbackProbe')

