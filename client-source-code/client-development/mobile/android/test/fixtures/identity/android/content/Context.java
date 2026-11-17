package android.content;
public class Context
{
    public static final int MODE_PRIVATE = 0;
    private final SharedPreferences preferences;
    public Context(SharedPreferences preferences)
    {
        this.preferences = preferences;
    }
    public Context getApplicationContext()
    {
        return this;
    }
    public SharedPreferences getSharedPreferences(String name, int mode)
    {
        return preferences;
    }
}
