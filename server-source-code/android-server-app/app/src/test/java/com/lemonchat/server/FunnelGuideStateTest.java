package com.lemonchat.server;

import org.junit.Test;
import static org.junit.Assert.*;

public final class FunnelGuideStateTest
{
    private static final String ADDRESS = "https://lemon-chat.example.ts.net/";
    private static final String LOGIN = "https://login.tailscale.com/a/example";

    @Test
    public void openingGuideDoesNotTreatPreferenceAsRunning()
    {
        assertEquals(FunnelGuideState.Phase.NEEDS_SETUP, phase(false, false, false, false, "", ""));
        assertEquals(FunnelGuideState.Phase.STOPPED, phase(true, false, false, false, "", ""));
        assertEquals(FunnelGuideState.Phase.STOPPED, phase(true, true, false, false, "", ""));
    }

    @Test
    public void runningLocalServerRequiresSeparateStop()
    {
        assertEquals(FunnelGuideState.Phase.LOCAL_RUNNING, phase(true, false, true, true, LOGIN, ADDRESS));
        assertEquals(FunnelGuideState.Phase.LOCAL_RUNNING, phase(true, false, true, false, "", ""));
        assertEquals(FunnelGuideState.Phase.STOPPED, phase(true, false, false, false, "", ""));
    }

    @Test
    public void onlyActualListeningSessionMakesAddressReady()
    {
        assertEquals(FunnelGuideState.Phase.CONNECTING, phase(true, true, true, false, "", ADDRESS));
        assertEquals(FunnelGuideState.Phase.CONNECTING, phase(true, true, true, true, "", ""));
        assertEquals(FunnelGuideState.Phase.READY, phase(true, true, true, true, "", ADDRESS));
        assertEquals(FunnelGuideState.Phase.STOPPED, phase(true, true, false, false, LOGIN, ADDRESS));
    }

    @Test
    public void signInRequiresAnExactHttpsLoginHost()
    {
        assertTrue(FunnelGuideState.LoginAddress(LOGIN));
        assertTrue(FunnelGuideState.LoginAddress("https://login.tailscale.com:443/a/example"));
        for (String invalid : new String[] {null, "", "http://login.tailscale.com/a/x", "https://login.tailscale.com.evil.test/", "https://login.tailscale.com@evil.test/", "https://user@login.tailscale.com/", "https://login.tailscale.com:8443/", "intent://login.tailscale.com/"})
        {
            assertFalse(String.valueOf(invalid), FunnelGuideState.LoginAddress(invalid));
        }
        assertEquals(FunnelGuideState.Phase.SIGN_IN, phase(true, true, true, true, LOGIN, ""));
        assertEquals(FunnelGuideState.Phase.CONNECTING, phase(true, true, true, true, "https://evil.test/", ""));
    }

    @Test
    public void shareOnlyAcceptsPlainTailnetHttpsAddresses()
    {
        assertTrue(FunnelGuideState.PublicAddress(ADDRESS));
        assertTrue(FunnelGuideState.PublicAddress("https://lemon-chat.example.ts.net"));
        for (String invalid : new String[] {null, "", "http://lemon-chat.example.ts.net/", "https://ts.net/", "https://example.ts.net.evil.test/", "https://x@example.ts.net/", "https://example.ts.net:8443/", "https://example.ts.net/file", "https://example.ts.net/?token=private", "https://example.ts.net/#private", "https://exa mple.ts.net/"})
        {
            assertFalse(String.valueOf(invalid), FunnelGuideState.PublicAddress(invalid));
            assertNotEquals(FunnelGuideState.Phase.READY, phase(true, true, true, true, "", invalid));
        }
    }

    private FunnelGuideState.Phase phase(boolean configured, boolean enabled, boolean active, boolean hosting, String login, String address)
    {
        return FunnelGuideState.PhaseFor(configured, enabled, active, hosting, login, address);
    }
}
