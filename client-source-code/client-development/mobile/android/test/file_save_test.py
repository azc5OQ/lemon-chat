"""Native file saving rejects missing data and removes failed legacy writes."""
from test_support import JAVA, FIXTURES, extract, compile_run

source = (JAVA / 'JavascriptJavaBridge.java').read_text(encoding='utf-8')
fixture = (FIXTURES / 'file-save.java.in').read_text(encoding='utf-8')
for token, signature in [
    ('__ENTRY__', '@JavascriptInterface public void JavaExportSaveFile('),
    ('__SANITIZE__', 'private static String sanitizeFileName('),
    ('__SAVE__', 'private boolean saveToAppDownloads('),
]:
    fixture = fixture.replace(token, extract(source, signature))
compile_run({
    'FileSaveRegression.java': fixture,
    'android/util/Base64.java': '''package android.util;
public class Base64
{
    public static final int DEFAULT = 0;
    public static byte[] decode(String data, int flags)
    {
        return java.util.Base64.getDecoder().decode(data);
    }
}
''',
}, 'FileSaveRegression')
