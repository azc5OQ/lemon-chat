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
 * Extracts the managed runtime files and runs Node on its own thread.
 * APK updates replace managed files and preserve other runtime data.
 */
public class NodeRuntime
{
    private static final String TAG = "lemonchat-node";

    // the directory inside assets/ that holds bundle.js, android-main.js and node_modules
    private static final String NODE_PROJECT_ASSET_DIR = "nodejs-project";
    private static final String NODE_ENTRY_SCRIPT = "android-main.js";

    private static boolean nativeLibrariesLoaded = false;
    private static Thread nodeThread = null;

    /**
     * Runs the embedded Node entry point on the calling runtime thread.
     * The native call blocks until the Node event loop exits.
     *
     * @param arguments Node argument vector, including the executable name
     * @return the native Node exit code
     */
    public native int nativeStartNode(String[] arguments);

    /**
     * Loads Node and its JNI adapter once for this process.
     *
     * @throws UnsatisfiedLinkError if the device ABI has no usable native library
     */
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
     * Extracts assets and starts Node once per process.
     * Reports startup failures through {@link AndroidRuntimeHost}.
     *
     * @param context application context used for assets and private files
     * @param bridgePort loopback TCP port owned by the Java bridge
     * @param bridgeToken capability written to the private bridge configuration
     */
    public static synchronized void start(Context context, int bridgePort, String bridgeToken)
    {
        if (nodeThread != null)
        {
            Log.i(TAG, "node startup already attempted in this process");
            return;
        }

        final File projectDirectory = new File(context.getFilesDir(), NODE_PROJECT_ASSET_DIR);

        try
        {
            copyProjectFromAssetsIfNeeded(context, projectDirectory);

            // Refresh this process's endpoint after managed runtime code has been extracted.
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
            AndroidRuntimeHost.runtimeFailed("Could not prepare the client runtime. Force stop Lemon Chat in Android settings, then reopen it.");
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
                    AndroidRuntimeHost.runtimeFailed("The client runtime stopped. Force stop Lemon Chat in Android settings, then reopen it.");
                }
                catch (UnsatisfiedLinkError missingLibrary)
                {
                    // the usual cause is an abi with no libnode.so in jniLibs
                    Log.e(TAG, "native library missing for this abi, node not started", missingLibrary);
                    AndroidRuntimeHost.runtimeFailed("The client runtime is unavailable on this device.");
                }
                catch (Throwable nodeDied)
                {
                    Log.e(TAG, "node thread died", nodeDied);
                    AndroidRuntimeHost.runtimeFailed("The client runtime stopped. Force stop Lemon Chat in Android settings, then reopen it.");
                }
            }
        }, "lemonchat-node");

        nodeThread.start();
    }

    /**
     * Updates managed runtime files when the APK stamp changes while preserving unrelated data.
     * The new stamp is written only after extraction completes.
     *
     * @param context application context supplying assets and the APK path
     * @param projectDirectory private directory containing extracted runtime files
     * @throws IOException if extraction, manifest validation or file replacement fails
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

            Log.i(TAG, "updating managed runtime code");
        }

        java.util.Set<String> currentFiles = new java.util.TreeSet<>();
        collectAssetFiles(context.getAssets(), NODE_PROJECT_ASSET_DIR, "", currentFiles);
        File manifest = new File(projectDirectory, ".managed-assets");
        java.util.Set<String> previousFiles = new java.util.HashSet<>();
        if (manifest.isFile())
        {
            if (manifest.length() > 256 * 1024)
            {
                throw new IOException("invalid managed asset manifest");
            }
            previousFiles.addAll(java.nio.file.Files.readAllLines(manifest.toPath(), java.nio.charset.StandardCharsets.UTF_8));
        }
        else
        {
            java.util.Collections.addAll(previousFiles, "bundle.js", "load-bundle.js", "android-main.js",
                "android-bridge-client.js", "loopback-ui-server.js", "mini-ws.js");
        }
        String root = projectDirectory.getCanonicalPath() + File.separator;
        for (String previous : previousFiles)
        {
            File target = new File(projectDirectory, previous);
            if (!target.getCanonicalPath().startsWith(root))
            {
                throw new IOException("invalid managed asset path");
            }
            if (!currentFiles.contains(previous) && target.isFile() && !target.delete())
            {
                throw new IOException("could not remove obsolete runtime module");
            }
        }
        copyAssetDirectory(context.getAssets(), NODE_PROJECT_ASSET_DIR, projectDirectory);
        java.nio.file.Files.write(manifest.toPath(), currentFiles, java.nio.charset.StandardCharsets.UTF_8);

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

    /**
     * Collects relative file paths for the managed runtime manifest.
     *
     * @param assets APK asset manager
     * @param assetPath asset file or directory to visit
     * @param relative corresponding path relative to the runtime root
     * @param files destination set receiving managed file names
     * @throws IOException if the asset directory cannot be listed
     */
    private static void collectAssetFiles(AssetManager assets, String assetPath, String relative, java.util.Set<String> files) throws IOException
    {
        String[] entries = assets.list(assetPath);
        if (entries == null || entries.length == 0)
        {
            files.add(relative);
            return;
        }
        for (String entry : entries)
        {
            collectAssetFiles(assets, assetPath + "/" + entry, relative.isEmpty() ? entry : relative + "/" + entry, files);
        }
    }

    /**
     * Reads the APK modification time used to detect an installed update.
     *
     * @param context application context supplying the APK path
     * @return the modification time, or zero if it is unavailable
     */
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

    /**
     * Copies an asset tree into the private runtime directory.
     *
     * @param assets APK asset manager
     * @param assetPath source asset file or directory
     * @param destination corresponding destination path
     * @throws IOException if directories cannot be created or a file cannot be copied
     */
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

    /**
     * Copies one runtime asset and closes both streams on success or failure.
     *
     * @param assets APK asset manager
     * @param assetPath source file within the APK assets
     * @param destination output file to replace
     * @throws IOException if opening, copying or closing a stream fails
     */
    private static void copyAssetFile(AssetManager assets, String assetPath, File destination) throws IOException
    {
        File parent = destination.getParentFile();

        if (parent != null && !parent.exists() && !parent.mkdirs())
        {
            throw new IOException("could not create " + parent.getAbsolutePath());
        }

        try (InputStream input = assets.open(assetPath);
             OutputStream output = new FileOutputStream(destination))
        {
            byte[] buffer = new byte[16 * 1024];
            int read;

            while ((read = input.read(buffer)) != -1)
            {
                output.write(buffer, 0, read);
            }
        }
    }

    /**
     * Reads the bounded APK stamp from a small file.
     *
     * @param file stamp file to read
     * @return at most 64 bytes decoded as UTF-8, or an empty string for an empty file
     * @throws IOException if the file cannot be read
     */
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

}
