package com.lemonchat;

import android.content.Context;
import android.content.res.AssetManager;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Owns the embedded node runtime (nodejs-mobile's prebuilt libnode.so).
 *
 * Node cannot run a script straight out of assets - assets live inside the apk and have no real
 * filesystem path - so the whole nodejs-project directory is copied into filesDir on first start,
 * and re-copied whenever the apk is newer than the copy.
 *
 * node::Start() blocks for the lifetime of the event loop, so it runs on its own thread. Everything
 * node prints lands in logcat under the tag "lemonchat-node".
 */
public class NodeRuntime
{
    private static final String TAG = "lemonchat-node";

    // the directory inside assets/ that holds bundle.js, android-main.js and node_modules
    private static final String NODE_PROJECT_ASSET_DIR = "nodejs-project";
    private static final String NODE_ENTRY_SCRIPT = "android-main.js";

    private static boolean nativeLibrariesLoaded = false;
    private static Thread nodeThread = null;

    /** implemented in native-lib.cpp; blocks until the node event loop exits */
    public native int nativeStartNode(String[] arguments);

    private static synchronized void loadNativeLibraries()
    {
        if (nativeLibrariesLoaded)
        {
            return;
        }

        // node first: liblemonchatnode.so links against it
        System.loadLibrary("node");
        System.loadLibrary("lemonchatnode");

        nativeLibrariesLoaded = true;
    }

    /**
     * Starts node on its own thread. Safe to call more than once - a second call while node is
     * already running does nothing. bridgePort/bridgeToken land in bridge.json, which is how
     * android-bridge-client.js finds the java side.
     */
    public static synchronized void start(Context context, int bridgePort, String bridgeToken)
    {
        if (nodeThread != null && nodeThread.isAlive())
        {
            Log.i(TAG, "node already running, ignoring start request");
            return;
        }

        final File projectDirectory = new File(context.getFilesDir(), NODE_PROJECT_ASSET_DIR);

        try
        {
            copyProjectFromAssetsIfNeeded(context, projectDirectory);

            // after the unpack - a stale-apk unpack deletes the whole directory first
            OutputStream bridgeOutput = new FileOutputStream(new File(projectDirectory, "bridge.json"));
            try
            {
                String bridgeJson = "{\"port\":" + bridgePort + ",\"token\":\"" + bridgeToken + "\"}";
                bridgeOutput.write(bridgeJson.getBytes("UTF-8"));
            }
            finally
            {
                bridgeOutput.close();
            }
        }
        catch (IOException copyFailed)
        {
            Log.e(TAG, "could not unpack the node project, node not started", copyFailed);
            return;
        }

        final String entryScriptPath = new File(projectDirectory, NODE_ENTRY_SCRIPT).getAbsolutePath();

        nodeThread = new Thread(new Runnable()
        {
            @Override
            public void run()
            {
                try
                {
                    loadNativeLibraries();

                    Log.i(TAG, "starting node with " + entryScriptPath);

                    // argv[0] is conventionally the binary name; node itself only reads from argv[1] on
                    int exitCode = new NodeRuntime().nativeStartNode(new String[] { "node", entryScriptPath });

                    // do NOT add a restart loop: node::Start works once per process (v8 cannot be
                    // re-initialised). the js side (android_host__dispatch_safely, uncaughtException guard) exists
                    // to make sure this line is never reached.
                    Log.e(TAG, "node event loop EXITED (code " + exitCode + ") - runtime is dead until the app process restarts");
                }
                catch (UnsatisfiedLinkError missingLibrary)
                {
                    // the usual cause is an abi with no libnode.so in jniLibs
                    Log.e(TAG, "native library missing for this abi, node not started", missingLibrary);
                }
                catch (Throwable nodeDied)
                {
                    Log.e(TAG, "node thread died", nodeDied);
                }
            }
        }, "lemonchat-node");

        nodeThread.start();
    }

    /**
     * Copies assets/nodejs-project into filesDir. Uses a stamp file holding the apk's build time so
     * an upgraded apk replaces a stale copy instead of silently running the previous bundle - which
     * would be a genuinely baffling bug to chase.
     */
    private static void copyProjectFromAssetsIfNeeded(Context context, File projectDirectory) throws IOException
    {
        final File stampFile = new File(projectDirectory, ".apk-stamp");
        final String currentStamp = String.valueOf(apkLastModified(context));

        if (stampFile.exists())
        {
            String existingStamp = readSmallFile(stampFile);

            if (currentStamp.equals(existingStamp))
            {
                Log.i(TAG, "node project already unpacked and current");
                return;
            }

            Log.i(TAG, "apk is newer than the unpacked node project, replacing it");
            deleteRecursively(projectDirectory);
        }

        copyAssetDirectory(context.getAssets(), NODE_PROJECT_ASSET_DIR, projectDirectory);

        OutputStream stampOutput = new FileOutputStream(stampFile);
        try
        {
            stampOutput.write(currentStamp.getBytes("UTF-8"));
        }
        finally
        {
            stampOutput.close();
        }

        Log.i(TAG, "node project unpacked to " + projectDirectory.getAbsolutePath());
    }

    private static long apkLastModified(Context context)
    {
        try
        {
            return new File(context.getPackageCodePath()).lastModified();
        }
        catch (Throwable unavailable)
        {
            return 0L;
        }
    }

    private static void copyAssetDirectory(AssetManager assets, String assetPath, File destination) throws IOException
    {
        String[] entries = assets.list(assetPath);

        if (entries == null || entries.length == 0)
        {
            copyAssetFile(assets, assetPath, destination);
            return;
        }

        if (!destination.exists() && !destination.mkdirs())
        {
            throw new IOException("could not create " + destination.getAbsolutePath());
        }

        for (String entry : entries)
        {
            copyAssetDirectory(assets, assetPath + "/" + entry, new File(destination, entry));
        }
    }

    private static void copyAssetFile(AssetManager assets, String assetPath, File destination) throws IOException
    {
        File parent = destination.getParentFile();

        if (parent != null && !parent.exists() && !parent.mkdirs())
        {
            throw new IOException("could not create " + parent.getAbsolutePath());
        }

        InputStream input = assets.open(assetPath);
        OutputStream output = new FileOutputStream(destination);

        try
        {
            byte[] buffer = new byte[16 * 1024];
            int read;

            while ((read = input.read(buffer)) != -1)
            {
                output.write(buffer, 0, read);
            }
        }
        finally
        {
            try { output.close(); } catch (IOException ignored) { }
            try { input.close(); } catch (IOException ignored) { }
        }
    }

    private static String readSmallFile(File file) throws IOException
    {
        InputStream input = new java.io.FileInputStream(file);

        try
        {
            byte[] buffer = new byte[64];
            int read = input.read(buffer);

            return (read > 0) ? new String(buffer, 0, read, "UTF-8") : "";
        }
        finally
        {
            try { input.close(); } catch (IOException ignored) { }
        }
    }

    private static void deleteRecursively(File target)
    {
        File[] children = target.listFiles();

        if (children != null)
        {
            for (File child : children)
            {
                deleteRecursively(child);
            }
        }

        if (!target.delete())
        {
            Log.w(TAG, "could not delete " + target.getAbsolutePath());
        }
    }
}
