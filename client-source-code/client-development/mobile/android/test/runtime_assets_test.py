"""Exercise production runtime extraction with real files and a small AssetManager fixture."""
from test_support import JAVA, compile_run

sources = {
    'com/lemonchat/NodeRuntime.java': (JAVA / 'NodeRuntime.java').read_text(encoding='utf-8'),
    'android/util/Log.java': '''package android.util;
public class Log
{
    public static int i(String tag, String text)
    {
        return 0;
    }
    public static int e(String tag, String text)
    {
        return 0;
    }
    public static int e(String tag, String text, Throwable error)
    {
        return 0;
    }
}''',
    'android/content/res/AssetManager.java': '''package android.content.res;
import java.io.*;
public class AssetManager
{
    public final File root;
    public int opened;
    public int closed;
    public final java.util.List<InputStream> openedStreams = new java.util.ArrayList<>();
    public AssetManager(File root)
    {
        this.root = root;
    }
    public String[] list(String path)
    {
        String[] entries = new File(this.root, path).list();
        return entries == null ? new String[0] : entries;
    }
    public InputStream open(String path) throws IOException
    {
        InputStream input = new FileInputStream(new File(this.root, path));
        this.opened++;
        InputStream wrapped = new FilterInputStream(input)
        {
            @Override public void close() throws IOException
            {
                AssetManager.this.closed++;
                super.close();
            }
        };
        this.openedStreams.add(wrapped);
        return wrapped;
    }
    public void closeAll() throws IOException
    {
        for (InputStream input : this.openedStreams)
        {
            input.close();
        }
    }
}''',
    'android/content/Context.java': '''package android.content;
import java.io.File;
import android.content.res.AssetManager;
public class Context
{
    public final File files;
    public final File apk;
    public final AssetManager assets;
    public Context(File files, File apk, AssetManager assets)
    {
        this.files = files;
        this.apk = apk;
        this.assets = assets;
    }
    public File getFilesDir()
    {
        return this.files;
    }
    public String getPackageCodePath()
    {
        return this.apk.getAbsolutePath();
    }
    public AssetManager getAssets()
    {
        return this.assets;
    }
}''',
    'com/lemonchat/AndroidRuntimeHost.java': '''package com.lemonchat;
final class AndroidRuntimeHost
{
    static void runtimeFailed(String reason)
    {
        throw new AssertionError("The extraction tests must not start Node");
    }
}''',
    'com/lemonchat/RuntimeAssetsRegression.java': '''package com.lemonchat;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.lang.reflect.*;
import android.content.Context;
import android.content.res.AssetManager;
public class RuntimeAssetsRegression
{
    static void check(boolean value, String reason)
    {
        if (!value)
        {
            throw new AssertionError(reason);
        }
    }
    static void write(Path path, String text) throws IOException
    {
        Files.createDirectories(path.getParent());
        Files.write(path, text.getBytes(StandardCharsets.UTF_8));
    }
    static String read(Path path) throws IOException
    {
        return new String(Files.readAllBytes(path), StandardCharsets.UTF_8);
    }
    public static void main(String[] args) throws Exception
    {
        Path temporary = Files.createTempDirectory("lemon-runtime-assets-");
        AssetManager assets = null;
        try
        {
            Path assetsPath = temporary.resolve("assets");
            Path source = assetsPath.resolve("nodejs-project/bundle.js");
            write(source, "new bundle");
            write(assetsPath.resolve("nodejs-project/android-main.js"), "entry");
            assets = new AssetManager(assetsPath.toFile());
            Method copyFile = NodeRuntime.class.getDeclaredMethod("copyAssetFile", AssetManager.class, String.class, File.class);
            copyFile.setAccessible(true);
            Path unavailable = temporary.resolve("directory-in-place-of-file");
            Files.createDirectory(unavailable);
            try
            {
                copyFile.invoke(null, assets, "nodejs-project/bundle.js", unavailable.toFile());
                throw new AssertionError("Opening an output directory must fail");
            }
            catch (InvocationTargetException expected)
            {
                check(expected.getCause() instanceof IOException, "Unexpected extraction failure");
            }
            check(assets.opened == assets.closed, "Failed destination open leaked its already-open asset stream");

            Path project = temporary.resolve("files/nodejs-project");
            Path apk = temporary.resolve("app.apk");
            write(apk, "apk");
            write(project.resolve("mini-ws.js"), "obsolete managed code");
            write(project.resolve("private-runtime-data"), "preserve");
            Context context = new Context(project.getParent().toFile(), apk.toFile(), assets);
            Method extract = NodeRuntime.class.getDeclaredMethod("copyProjectFromAssetsIfNeeded", Context.class, File.class);
            extract.setAccessible(true);
            extract.invoke(null, context, project.toFile());
            check(read(project.resolve("bundle.js")).equals("new bundle"), "Current runtime code not extracted");
            check(!Files.exists(project.resolve("mini-ws.js")), "Obsolete managed runtime code survived migration");
            check(read(project.resolve("private-runtime-data")).equals("preserve"), "Extraction deleted unrelated runtime data");
            int opens = assets.opened;
            extract.invoke(null, context, project.toFile());
            check(assets.opened == opens, "Unchanged APK unexpectedly rewrote runtime files");
            check(assets.opened == assets.closed, "Successful extraction leaked asset streams");
            System.out.println("PASS: failed destination open releases its asset stream; current extraction removes obsolete code and preserves runtime data");
        }
        finally
        {
            if (assets != null)
            {
                assets.closeAll();
            }
            try (java.util.stream.Stream<Path> paths = Files.walk(temporary))
            {
                for (Path path : (Iterable<Path>)paths.sorted(java.util.Comparator.reverseOrder())::iterator)
                {
                    Files.delete(path);
                }
            }
        }
    }
}'''
}

compile_run(sources, 'com.lemonchat.RuntimeAssetsRegression')
