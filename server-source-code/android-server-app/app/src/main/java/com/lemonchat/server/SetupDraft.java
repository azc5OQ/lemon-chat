package com.lemonchat.server;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import org.json.JSONArray;
import org.json.JSONObject;

/** keeps unfinished setup in memory until the user chooses to start */
final class SetupDraft
{
    static final int STEPS = 7;
    // the steps after the slot count: the server features, how the browser page is offered, the review
    static final int STEP_FEATURES = 4;
    static final int STEP_WEB_PAGE = 5;
    static final int STEP_REVIEW = 6;
    static final List<Integer> SLOT_PRESETS = Collections.unmodifiableList(Arrays.asList(8, 16, 32, 64));
    static final List<String> JOIN_CIPHERS = Collections.unmodifiableList(Arrays.asList("aes256", "chacha20", "camellia256", "twofish256", "serpent256", "trivium80"));
    static final List<String> JOIN_CIPHER_NAMES = Collections.unmodifiableList(Arrays.asList("AES-256", "ChaCha20", "Camellia-256", "Twofish-256", "Serpent-256", "Trivium-80"));
    int step;
    String admin = "";
    String confirmation = "";
    String join = "";
    String joinCipher = "aes256";
    boolean requireJoin;
    String slots = "16";
    String port = "1111";
    String transport = "auto";
    ServerFeatures features = ServerFeatures.Proposed();
    String webPage = HostingSettings.PAGE_NONE;
    String webPagePort = Integer.toString(HostingSettings.DEFAULT_PAGE_PORT);
    boolean isPageConfigEmbedded = true;
    boolean advanced;

    /**
     * checks the current step without writing settings
     *
     * @param selected step to validate
     * @return a user explanation or null when valid
     */
    String Problem(int selected)
    {
        if (selected == 1)
        {
            if (admin.trim().isEmpty())
            {
                return "Choose an admin password before continuing.";
            }
            if (admin.indexOf('\0') >= 0)
            {
                return "The admin password contains an unsupported character. Enter it again.";
            }
            if (admin.getBytes(StandardCharsets.UTF_8).length > 50)
            {
                return "That admin password is too long. Choose a shorter one.";
            }
            if (!admin.equals(confirmation))
            {
                return "The two admin passwords do not match.";
            }
        }
        if (selected == 2 && requireJoin)
        {
            if (!JOIN_CIPHERS.contains(joinCipher))
            {
                return "Choose a supported password type.";
            }
            if (join.isEmpty())
            {
                return "Enter a join password or turn off the password requirement.";
            }
            if (!join.equals(join.trim()))
            {
                return "Remove spaces at the beginning or end of the join password.";
            }
            if (join.indexOf('\0') >= 0)
            {
                return "The join password contains an unsupported character. Enter it again.";
            }
            if (join.getBytes(StandardCharsets.UTF_8).length > 255)
            {
                return "That join password is too long. Choose a shorter one.";
            }
        }
        if (selected == 3 && !InRange(slots, 1, 100000))
        {
            return "Enter a whole number of people from 1 to 100,000.";
        }
        if (selected == STEP_WEB_PAGE)
        {
            if (!webPage.equals(HostingSettings.PAGE_NONE) && !webPage.equals(HostingSettings.PAGE_HTTP) && !webPage.equals(HostingSettings.PAGE_FUNNEL))
            {
                return "Choose how other devices get the web page.";
            }
            if (webPage.equals(HostingSettings.PAGE_HTTP) && !InRange(webPagePort, 1024, 65535))
            {
                return "Enter a web page port from 1024 to 65535.";
            }
            if (webPage.equals(HostingSettings.PAGE_HTTP) && InRange(port, 1024, 65535) && Integer.parseInt(webPagePort) == Integer.parseInt(port))
            {
                return "The web page port must differ from the chat port " + port + ".";
            }
        }
        if (selected == STEP_REVIEW)
        {
            if (!InRange(port, 1024, 65535))
            {
                return "Enter a port from 1024 to 65535 in Advanced.";
            }
            if (!transport.equals("webrtc") && !transport.equals("auto") && !transport.equals("websocket") && !transport.equals("disabled"))
            {
                return "Choose an audio connection mode in Advanced.";
            }
        }
        return null;
    }

    JSONObject Settings() throws Exception
    {
        for (int selected = 1; selected < STEPS; selected++)
        {
            String problem = Problem(selected);
            if (problem != null)
            {
                throw new IllegalArgumentException(problem);
            }
        }
        JSONObject settings = ServerSettings.Update(new JSONObject(), Integer.parseInt(port), Integer.parseInt(slots), admin, requireJoin ? join : "", features.IsEnabled("is_video_streaming_active"), transport);
        features.WriteTo(settings);
        if (requireJoin)
        {
            settings.put("keys", new JSONArray().put(new JSONObject().put("type", joinCipher).put("password", join)));
        }
        return settings;
    }

    SetupDraft Copy()
    {
        SetupDraft copy = new SetupDraft();
        copy.step = step;
        copy.admin = admin;
        copy.confirmation = confirmation;
        copy.join = join;
        copy.joinCipher = joinCipher;
        copy.requireJoin = requireJoin;
        copy.slots = slots;
        copy.port = port;
        copy.transport = transport;
        copy.features = features.Copy();
        copy.webPage = webPage;
        copy.webPagePort = webPagePort;
        copy.isPageConfigEmbedded = isPageConfigEmbedded;
        copy.advanced = advanced;
        return copy;
    }

    void ClearPasswords()
    {
        admin = "";
        confirmation = "";
        join = "";
    }

    private static boolean InRange(String value, int minimum, int maximum)
    {
        try
        {
            return value.matches("[0-9]{1,6}") && Integer.parseInt(value) >= minimum && Integer.parseInt(value) <= maximum;
        }
        catch (NumberFormatException error)
        {
            return false;
        }
    }
}
