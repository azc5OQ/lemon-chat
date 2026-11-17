package com.lemonchat.server;

import java.net.URI;

/** derives guide actions from the current hosting session */
final class FunnelGuideState
{
    enum Phase
    {
        NEEDS_SETUP, LOCAL_RUNNING, STOPPED, CONNECTING, SIGN_IN, READY
    }

    private FunnelGuideState()
    {
    }

    /**
     * selects the next action without changing hosting state
     *
     * @param configured whether a saved server exists
     * @param enabled whether public hosting was selected
     * @param active whether the service is starting or running
     * @param hosting whether the local server is listening
     * @param login current helper sign in address
     * @param address current helper public address
     * @return phase supported by the observed service state
     */
    static Phase PhaseFor(boolean configured, boolean enabled, boolean active, boolean hosting, String login, String address)
    {
        if (!configured)
        {
            return Phase.NEEDS_SETUP;
        }
        if (active && !enabled)
        {
            return Phase.LOCAL_RUNNING;
        }
        if (!active)
        {
            return Phase.STOPPED;
        }
        if (hosting && PublicAddress(address))
        {
            return Phase.READY;
        }
        if (LoginAddress(login))
        {
            return Phase.SIGN_IN;
        }
        return Phase.CONNECTING;
    }

    /**
     * checks the helper address before opening the browser
     *
     * @param value helper sign in address
     * @return whether it belongs to the Tailscale login service
     */
    static boolean LoginAddress(String value)
    {
        URI uri = Https(value);
        return uri != null && "login.tailscale.com".equalsIgnoreCase(uri.getHost());
    }

    /**
     * checks the address emitted after both public listeners are ready
     *
     * @param value helper public address
     * @return whether it is a plain HTTPS tailnet address
     */
    static boolean PublicAddress(String value)
    {
        URI uri = Https(value);
        if (uri == null || !uri.getHost().toLowerCase(java.util.Locale.ROOT).endsWith(".ts.net"))
        {
            return false;
        }
        String path = uri.getRawPath();
        return (path.isEmpty() || "/".equals(path)) && uri.getRawQuery() == null && uri.getRawFragment() == null;
    }

    private static URI Https(String value)
    {
        if (value == null || value.length() > 4096)
        {
            return null;
        }
        try
        {
            URI uri = new URI(value);
            return "https".equalsIgnoreCase(uri.getScheme()) && uri.getHost() != null && uri.getRawUserInfo() == null && (uri.getPort() == -1 || uri.getPort() == 443) ? uri : null;
        }
        catch (Exception error)
        {
            return null;
        }
    }
}
