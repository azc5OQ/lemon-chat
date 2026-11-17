package android.content;
public interface SharedPreferences
{
    String getString(String key, String fallback);
    boolean getBoolean(String key, boolean fallback);
    Editor edit();
    interface Editor
    {
        Editor putString(String key, String value);
        Editor putBoolean(String key, boolean value);
        boolean commit();
        void apply();
    }
}
