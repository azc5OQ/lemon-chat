package com.lemonchat;

import android.content.Context;
import android.content.res.AssetManager;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.UUID;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import static org.junit.Assume.assumeFalse;

/** executes compatibility checks inside the packaged Android runtime */
@RunWith(AndroidJUnit4.class)
public final class EmbeddedRuntimeTest
{
    @Test(timeout = 180000)
    public void currentRuntimePreservesIdentityAndTransport() throws Exception
    {
        assumeFalse(InstrumentationRegistry.getInstrumentation() instanceof ServiceRuntimeTestRunner);
        Context target = InstrumentationRegistry.getInstrumentation().getTargetContext();
        Context tests = InstrumentationRegistry.getInstrumentation().getContext();
        File directory = new File(target.getCacheDir(), "node-runtime-check-" + UUID.randomUUID());
        assertTrue(directory.mkdirs());
        try
        {
            File fixtures = new File(directory, "probe");
            File project = new File(directory, "nodejs-project");
            copyAssets(tests.getAssets(), "probe", fixtures);
            copyAssets(target.getAssets(), "nodejs-project", project);
            File result = new File(directory, "result.json");
            System.loadLibrary("node");
            System.loadLibrary("lemonchatnode");
            int code = new NodeRuntime().nativeStartNode(new String[]{"node", new File(fixtures, "runtime-probe.cjs").getAbsolutePath(), fixtures.getAbsolutePath(), project.getAbsolutePath(), result.getAbsolutePath()});
            assertTrue("Node did not write its compatibility result", result.isFile());
            JSONObject report = new JSONObject(new String(Files.readAllBytes(result.toPath()), StandardCharsets.UTF_8));
            assertFalse(report.optString("error"), report.has("error"));
            assertEquals(0, code);
            assertEquals("android", report.getString("platform"));
            String expectedArchitecture = InstrumentationRegistry.getArguments().getString("expectedArchitecture", "");
            if (!expectedArchitecture.isEmpty())
            {
                assertEquals(expectedArchitecture, report.getString("architecture"));
            }
            assertTrue(report.getInt("checks") >= 30);
            android.util.Log.i("lemonchat-runtime-test", report.toString());
        }
        finally
        {
            remove(directory);
        }
    }

    private static void copyAssets(AssetManager assets, String name, File destination) throws Exception
    {
        String[] children = assets.list(name);
        if (children != null && children.length > 0)
        {
            assertTrue(destination.isDirectory() || destination.mkdirs());
            for (String child : children)
            {
                copyAssets(assets, name + "/" + child, new File(destination, child));
            }
            return;
        }
        try (InputStream input = assets.open(name); FileOutputStream output = new FileOutputStream(destination))
        {
            byte[] buffer = new byte[16384];
            int count;
            while ((count = input.read(buffer)) >= 0)
            {
                output.write(buffer, 0, count);
            }
        }
    }

    private static void remove(File file)
    {
        File[] children = file.listFiles();
        if (children != null)
        {
            for (File child : children)
            {
                remove(child);
            }
        }
        file.delete();
    }
}
