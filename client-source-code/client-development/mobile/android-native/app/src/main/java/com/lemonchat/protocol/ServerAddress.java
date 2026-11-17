package com.lemonchat.protocol;

import java.net.URI;

/** separates connection fields without changing saved history scopes */
public final class ServerAddress
{
    public final String address;
    public final int port;
    private final String scheme;
    private final String original;
    private final String suffix;

    private ServerAddress(String address, int port, String scheme, String original, String suffix)
    {
        this.address = address;
        this.port = port;
        this.scheme = scheme;
        this.original = original;
        this.suffix = suffix;
    }

    /**
     * reads saved connection fields without changing the endpoint
     *
     * @param endpoint saved WebSocket URL or an empty value
     * @return editable address and port
     */
    public static ServerAddress fromEndpoint(String endpoint)
    {
        if (endpoint == null || endpoint.trim().isEmpty())
        {
            return new ServerAddress("", 1111, "ws", null, "/");
        }
        String original = endpoint.trim();
        URI parsed = validate(original);
        boolean secure = "wss".equals(parsed.getScheme());
        String path = parsed.getRawPath();
        String query = parsed.getRawQuery();
        String suffix = (path == null || path.isEmpty() ? "/" : path) + (query == null ? "" : "?" + query);
        return new ServerAddress(unbracket(parsed.getHost()), parsed.getPort() < 0 ? secure ? 443 : 80 : parsed.getPort(), parsed.getScheme(), original, suffix);
    }

    /**
     * builds an endpoint from the visible connection fields
     *
     * @param address hostname or IP address without a protocol or port
     * @param port explicit server port
     * @return saved address identifier retaining its request path and query
     */
    public String endpoint(String address, int port)
    {
        String host = address == null ? "" : unbracket(address.trim());
        if (host.isEmpty() || host.length() > 253 || host.contains("/") || host.contains("?") || host.contains("#") || host.contains("@") || host.contains("\\") || host.contains("[") || host.contains("]") || port < 1 || port > 65535)
        {
            throw new IllegalArgumentException("Enter an IP address or hostname without a protocol or port");
        }
        String value = scheme + "://" + (host.contains(":") ? "[" + host + "]" : host) + ":" + port + suffix;
        validate(value);
        if (original != null && this.address.equalsIgnoreCase(host) && this.port == port)
        {
            return original;
        }
        return value;
    }

    /**
     * validates the separate decimal port field
     *
     * @param value entered port
     * @return port from 1 through 65535
     */
    public static int parsePort(String value)
    {
        String digits = value == null ? "" : value.trim();
        if (!digits.matches("[0-9]{1,5}"))
        {
            throw new IllegalArgumentException("Enter a port from 1 to 65535");
        }
        int port = Integer.parseInt(digits);
        if (port < 1 || port > 65535)
        {
            throw new IllegalArgumentException("Enter a port from 1 to 65535");
        }
        return port;
    }

    private static URI validate(String value)
    {
        try
        {
            URI parsed = URI.create(value);
            if (value.length() > 2048 || (!"ws".equals(parsed.getScheme()) && !"wss".equals(parsed.getScheme())) || parsed.getHost() == null || parsed.getRawUserInfo() != null || parsed.getRawFragment() != null || parsed.getPort() == 0 || parsed.getPort() > 65535)
            {
                throw new IllegalArgumentException();
            }
            return parsed;
        }
        catch (IllegalArgumentException error)
        {
            throw new IllegalArgumentException("Check the server address and port");
        }
    }

    private static String unbracket(String value)
    {
        if (value.startsWith("[") && value.endsWith("]") && value.contains(":"))
        {
            return value.substring(1, value.length() - 1);
        }
        return value;
    }
}
