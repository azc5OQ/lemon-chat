"""Reused call screens and stale native actions must respect the current capability."""
import re
import xml.etree.ElementTree as ET
from test_support import JAVA, FIXTURES, extract, compile_run

service = (JAVA / 'BackgroundService.java').read_text(encoding='utf-8')
fixture = (FIXTURES / 'call.java.in').read_text(encoding='utf-8')
fixture = fixture.replace('__CURRENT_CALL__', extract(service, 'public synchronized boolean isCurrentCall('))
fixture = fixture.replace('__CALL_ACTION__', extract(service, 'public synchronized void handleCallAction('))
fixture = fixture.replace('__CLEAR_CALL__', extract(service, 'public synchronized void clearIncomingCall('))
production = (JAVA / 'IncomingCallActivity.java').read_text(encoding='utf-8')
production = re.sub(r'^import .*;\s*$', '', production, flags=re.M)
production = production.replace('public class IncomingCallActivity', 'class IncomingCallActivity', 1)
production = production.replace('android.view.ViewGroup', 'ViewGroup').replace('android.R.id.content', 'R.id.content')
manifest = ET.parse(JAVA.parent.parent.parent / 'AndroidManifest.xml')
namespace = '{http://schemas.android.com/apk/res/android}'
application = manifest.getroot().find('application')
for kind, name in [('service', '.BackgroundService'), ('activity', '.IncomingCallActivity')]:
    component = next(item for item in application.findall(kind) if item.get(namespace + 'name') == name)
    assert component.get(namespace + 'exported') == 'false', name + ' must reject external component launches'
compile_run({'CallRegression.java': production + '\n' + fixture}, 'com.lemonchat.Probe')

