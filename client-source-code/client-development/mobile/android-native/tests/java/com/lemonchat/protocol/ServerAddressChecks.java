package com.lemonchat.protocol;

/** checks address and port editing without changing saved history scopes */
public final class ServerAddressChecks
{
    private static int passed;

    public static void main(String[] arguments)
    {
        ServerAddress fresh = ServerAddress.fromEndpoint("");
        check(fresh.address.isEmpty() && fresh.port == 1111, "new form uses the ordinary server port");
        check(fresh.endpoint("192.168.1.10", 3000).equals("ws://192.168.1.10:3000/"), "IPv4 address needs no protocol prefix");
        check(fresh.endpoint("chat.example.org", 3000).equals("ws://chat.example.org:3000/"), "hostname needs no protocol prefix");
        check(fresh.endpoint("::1", 3000).equals("ws://[::1]:3000/"), "bare IPv6 receives URL brackets");
        check(fresh.endpoint("[2001:db8::1]", 65535).equals("ws://[2001:db8::1]:65535/"), "bracketed IPv6 remains supported");
        check(ServerAddress.parsePort(" 3000 ") == 3000, "port field accepts surrounding spaces");
        check(ServerAddress.parsePort("1") == 1 && ServerAddress.parsePort("65535") == 65535, "both port boundaries accepted");
        String original = "wss://Chat.Example.org/chat%2Fsocket?key=a%2Fb";
        ServerAddress secure = ServerAddress.fromEndpoint(original);
        check(secure.address.equals("Chat.Example.org") && secure.port == 443, "saved secure endpoint fills separate fields");
        check(secure.endpoint("chat.example.org", 443).equals(original), "unchanged endpoint retains exact history scope");
        check(secure.endpoint("other.example.org", 8443).equals("wss://other.example.org:8443/chat%2Fsocket?key=a%2Fb"), "editing host retains escaped request path");
        ServerAddress ordinary = ServerAddress.fromEndpoint("ws://localhost");
        check(ordinary.port == 80 && ordinary.endpoint("localhost", 80).equals("ws://localhost"), "saved ordinary default port remains unchanged");
        ServerAddress ipv6 = ServerAddress.fromEndpoint("wss://[::1]:3000/");
        check(ipv6.address.equals("::1") && ipv6.endpoint("::1", 3000).equals("wss://[::1]:3000/"), "saved IPv6 scope remains unchanged");
        for (String invalid : new String[]{"", "0", "65536", "-1", "+3000", "30x0", "300000", "3.5"})
        {
            rejects(() -> ServerAddress.parsePort(invalid), "invalid port refused: " + invalid);
        }
        for (String invalid : new String[]{"", "ws://host", "host:3000", "host/path", "host?query", "user@host", "host#fragment", "bad host", "bad\\host", "[invalid]", "999.999.999.999"})
        {
            rejects(() -> fresh.endpoint(invalid, 3000), "invalid address refused: " + invalid);
        }
        for (String invalid : new String[]{"https://example.org/", "wss://user:password@example.org/", "wss://example.org/#fragment", "ws://example.org:65536/"})
        {
            rejects(() -> ServerAddress.fromEndpoint(invalid), "invalid saved endpoint refused");
        }
        System.out.println("PASS " + passed + " address and port checks");
    }

    private static void check(boolean result, String description)
    {
        if (!result)
        {
            throw new AssertionError(description);
        }
        passed++;
    }

    private static void rejects(Runnable action, String description)
    {
        try
        {
            action.run();
        }
        catch (IllegalArgumentException expected)
        {
            passed++;
            return;
        }
        throw new AssertionError(description);
    }
}
