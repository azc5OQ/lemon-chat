"""Publication must succeed before native Downloads can report success."""
from test_support import JAVA, FIXTURES, extract, compile_run

source = (JAVA / 'JavascriptJavaBridge.java').read_text(encoding='utf-8')
fixture = (FIXTURES / 'publication.java.in').read_text(encoding='utf-8')
fixture = fixture.replace('__SAVE_METHOD__', extract(source, 'private boolean saveToMediaStoreDownloads('))
compile_run({'PublicationRegression.java': fixture}, 'PublicationRegression')

