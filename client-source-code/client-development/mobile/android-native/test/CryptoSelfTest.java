import com.lemonchat.crypto.CryptoException;
import com.lemonchat.crypto.CryptoSession;
import com.lemonchat.crypto.Identity;
import com.lemonchat.crypto.IdentityFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.Base64;

public final class CryptoSelfTest
{
    private static int checks;

    private static void check(boolean condition, String name)
    {
        checks++;
        if (!condition)
        {
            throw new AssertionError(name);
        }
    }

    private static void rejects(Runnable action, String name)
    {
        try
        {
            action.run();
        }
        catch (IllegalArgumentException | CryptoException expected)
        {
            checks++;
            return;
        }
        throw new AssertionError(name);
    }

    private static String utf8(String text)
    {
        return new String(Base64.getDecoder().decode(text), StandardCharsets.UTF_8);
    }

    private static String utf16(String encoded)
    {
        byte[] bytes = Base64.getDecoder().decode(encoded);
        char[] chars = new char[bytes.length / 2];
        for (int index = 0; index < chars.length; index++)
        {
            chars[index] = (char)((bytes[index * 2] & 255) | ((bytes[index * 2 + 1] & 255) << 8));
        }
        return new String(chars);
    }

    private static String hex(byte[] bytes)
    {
        StringBuilder output = new StringBuilder();
        for (byte value : bytes)
        {
            output.append(String.format("%02x", value & 255));
        }
        return output.toString();
    }

    public static void main(String[] arguments) throws Exception
    {
        StringBuilder nativeOutput = new StringBuilder();
        for (String line : Files.readAllLines(Path.of(arguments[0]), StandardCharsets.UTF_8))
        {
            String[] parts = line.split("\t");
            Identity identity = new Identity(utf16(parts[2]), Integer.parseInt(parts[1]));
            check(hex(identity.seedDigest()).equals(parts[3]), parts[0] + " browser seed");
            byte[] exported = IdentityFile.export(identity);
            Identity imported = IdentityFile.parse(exported, 8192);
            check(imported.passphrase.equals(identity.passphrase) && imported.rsaKeyBits == identity.rsaKeyBits, "versioned identity roundtrip");
            byte[] bom = new byte[exported.length + 3];
            bom[0] = (byte)239;
            bom[1] = (byte)187;
            bom[2] = (byte)191;
            System.arraycopy(exported, 0, bom, 3, exported.length);
            check(IdentityFile.parse(bom, 4096).passphrase.equals(identity.passphrase), "structured BOM accepted");
            try (CryptoSession session = new CryptoSession(imported, ""))
            {
                check(session.getPublicKey().equals(parts[4]), parts[0] + " browser public key");
                String keys = utf8(parts[5]);
                String plaintext = utf8(parts[6]);
                check(session.decryptDirect(utf8(parts[7])).equals(plaintext), "browser to native direct content");
                check(session.decryptChannel(keys, utf8(parts[8])).equals(plaintext), "browser to native channel content");
                String direct = session.encryptDirect(session.getPublicKey(), plaintext);
                String channel = session.encryptChannel(keys, plaintext);
                check(session.decryptDirect(direct).equals(plaintext), "native direct roundtrip");
                check(session.decryptChannel(keys, channel).equals(plaintext), "native channel roundtrip");
                String file = "{\"name\":\"test.txt\",\"size\":5,\"mime\":\"text/plain\",\"data\":\"SGVsbG8=\"}";
                String privateFile = session.encryptDirectAttachment(session.getPublicKey(), file, true);
                check(session.decryptDirectAttachment(privateFile, true).equals(file), "authenticated direct file roundtrip");
                rejects(() -> session.decryptDirect(privateFile), "file cannot become unauthenticated direct text");
                String channelFile = session.encryptChannelAttachment(keys, file, true);
                check(session.decryptChannelAttachment(keys, channelFile, true).equals(file), "authenticated channel file roundtrip");
                rejects(() -> session.decryptChannelAttachment(keys, channel, true), "text cannot bypass file authentication");
                String picture = "data:image/png;base64,SGVsbG8=";
                String privatePicture = session.encryptDirectAttachment(session.getPublicKey(), picture, false);
                String channelPicture = session.encryptChannelAttachment(keys, picture, false);
                check(session.decryptDirectAttachment(utf8(parts[9]), true).equals(file), "browser to native authenticated private file");
                check(session.decryptChannelAttachment(keys, utf8(parts[10]), true).equals(file), "browser to native authenticated channel file");
                check(session.decryptDirectAttachment(utf8(parts[11]), false).equals(picture), "browser to native private picture");
                check(session.decryptChannelAttachment(keys, utf8(parts[12]), false).equals(picture), "browser to native channel picture");
                byte[] media = new byte[] {1, 2, 3, 4, 5, 6};
                byte[] encrypted = CryptoSession.contentMediaLayer(keys, 7, 4, media, true);
                check(!Arrays.equals(media, encrypted), "media encrypted");
                check(Arrays.equals(media, CryptoSession.contentMediaLayer(keys, 7, 4, encrypted, false)), "media layers roundtrip");
                rejects(() -> session.openMedia(new byte[32]), "invalid media rejected");
                rejects(() -> session.decryptDirect("{}"), "invalid direct envelope rejected");
                nativeOutput.append(parts[0]);
                for (String value : new String[]{direct, channel, privateFile, channelFile, privatePicture, channelPicture})
                {
                    nativeOutput.append('\t').append(Base64.getEncoder().encodeToString(value.getBytes(StandardCharsets.UTF_8)));
                }
                nativeOutput.append('\n');
            }
        }
        Identity ordinary = new Identity("x".repeat(200), 2048);
        String valid = new String(IdentityFile.export(ordinary), StandardCharsets.UTF_8);
        rejects(() -> IdentityFile.parse(new byte[] {(byte)0xc0, (byte)0xaf}, 2048), "malformed UTF8 rejected");
        rejects(() -> IdentityFile.parse(new byte[65537], 2048), "oversized file rejected");
        rejects(() -> IdentityFile.parse(valid.replace("2048", "2049").getBytes(StandardCharsets.UTF_8), 2048), "unsupported key size rejected");
        rejects(() -> IdentityFile.parse(valid.replace("\"version\":1", "\"version\":2").getBytes(StandardCharsets.UTF_8), 2048), "unsupported version rejected");
        rejects(() -> IdentityFile.parse((valid + "garbage").getBytes(StandardCharsets.UTF_8), 2048), "trailing JSON garbage rejected");
        rejects(() -> IdentityFile.parse(("{" + "x".repeat(200)).getBytes(StandardCharsets.UTF_8), 2048), "malformed JSON never becomes raw identity");
        rejects(() -> IdentityFile.parse(("\ufeff" + "x".repeat(200)).getBytes(StandardCharsets.UTF_8), 2048), "ambiguous raw BOM rejected");
        rejects(() -> IdentityFile.parse(valid.replace("\"version\":1", "\"version\":1,\"version\":1").getBytes(StandardCharsets.UTF_8), 2048), "duplicate fields rejected");
        check(IdentityFile.parse((" " + ordinary.passphrase + "\n").getBytes(StandardCharsets.UTF_8), 3072).passphrase.equals(" " + ordinary.passphrase + "\n"), "legacy whitespace preserved");
        try (CryptoSession session = new CryptoSession(ordinary, ""))
        {
            session.close();
            rejects(session::newContentKeys, "closed state rejected");
        }
        Files.writeString(Path.of(arguments[1]), nativeOutput.toString(), StandardCharsets.UTF_8);
        System.out.println("PASS: " + checks + " identity, JNI, content and rejection checks");
    }
}
