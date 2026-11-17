package com.lemonchat;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.IBinder;
import android.os.SystemClock;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assume.assumeTrue;

/** checks the real bundled client and its authenticated Java control connection */
@RunWith(AndroidJUnit4.class)
public final class ServiceRuntimeTest
{
    @Test(timeout = 90000)
    public void bundledClientPublishesItsLoopbackEndpoint() throws Exception
    {
        assumeTrue(InstrumentationRegistry.getInstrumentation() instanceof ServiceRuntimeTestRunner);
        Context target = InstrumentationRegistry.getInstrumentation().getTargetContext();
        CountDownLatch connected = new CountDownLatch(1);
        AtomicReference<BackgroundService> owner = new AtomicReference<>();
        ServiceConnection connection = new ServiceConnection()
        {
            @Override
            public void onServiceConnected(ComponentName name, IBinder binder)
            {
                if (binder instanceof BackgroundService.LocalBinder)
                {
                    owner.set(((BackgroundService.LocalBinder)binder).getService());
                }
                connected.countDown();
            }

            @Override
            public void onServiceDisconnected(ComponentName name)
            {
                owner.set(null);
            }
        };
        boolean bound = target.bindService(new Intent(target, BackgroundService.class), connection, Context.BIND_AUTO_CREATE);
        assertTrue("The production service did not bind", bound);
        try
        {
            assertTrue("The production service binding timed out", connected.await(15, TimeUnit.SECONDS));
            BackgroundService service = owner.get();
            assertNotNull("The test must run in the service process", service);
            int port = 0;
            long deadline = SystemClock.elapsedRealtime() + 60000;
            while (port == 0 && SystemClock.elapsedRealtime() < deadline)
            {
                NodeBridge bridge = service.nodeBridge;
                if (bridge != null)
                {
                    port = bridge.getLoopbackPort();
                }
                if (port == 0)
                {
                    Thread.sleep(100);
                }
            }
            assertTrue("Node did not announce its UI listener through the authenticated Java bridge", port > 0);
            android.util.Log.i("lemonchat-runtime-test", "PASS production bundle, Java control authentication and UI port publication");
        }
        finally
        {
            target.unbindService(connection);
        }
    }
}
