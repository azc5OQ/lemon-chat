package com.lemonchat.crypto;

import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.security.SecureRandom;

/**
 * holds the passphrase and key size needed to restore one identity
 */
public final class Identity
{
    public final String passphrase;
    public final int rsaKeyBits;

    /**
     * validates the values without generating the RSA key pair
     *
     * @param passphrase complete identity secret
     * @param rsaKeyBits original RSA key size
     */
    public Identity(String passphrase, int rsaKeyBits)
    {
        if (passphrase == null || passphrase.length() < 199 || passphrase.length() > 65536 || !isAllowedSize(rsaKeyBits))
        {
            throw new IllegalArgumentException("The identity passphrase or RSA key size is invalid");
        }
        this.passphrase = passphrase;
        this.rsaKeyBits = rsaKeyBits;
    }

    /**
     * creates a fresh passphrase with secure random bytes
     *
     * @param rsaKeyBits size of the identity to generate
     * @return new identity values
     */
    public static Identity create(int rsaKeyBits)
    {
        byte[] bytes = new byte[100];
        new SecureRandom().nextBytes(bytes);
        char[] hex = "0123456789abcdef".toCharArray();
        StringBuilder text = new StringBuilder(200);
        for (byte value : bytes)
        {
            text.append(hex[(value & 255) >>> 4]);
            text.append(hex[value & 15]);
        }
        java.util.Arrays.fill(bytes, (byte)0);
        return new Identity(text.toString(), rsaKeyBits);
    }

    /**
     * checks the key sizes supported by identity files
     *
     * @param bits proposed RSA key size
     * @return true when the size is supported
     */
    public static boolean isAllowedSize(int bits)
    {
        return bits == 2048 || bits == 3072 || bits == 4096 || bits == 6144 || bits == 8192;
    }

    /**
     * reproduces the browser's historical passphrase hash
     *
     * @return SHA256 digest used by the shared RSA generator
     */
    public byte[] seedDigest()
    {
        try
        {
            MessageDigest hash = MessageDigest.getInstance("SHA-256");
            String text = passphrase.replace("\r\n", "\n");
            for (int index = 0; index < text.length(); index++)
            {
                int value = text.charAt(index);
                if (value < 128)
                {
                    hash.update((byte)value);
                }
                else if (value < 2048)
                {
                    hash.update((byte)((value >>> 6) | 192));
                    hash.update((byte)((value & 63) | 128));
                }
                else
                {
                    hash.update((byte)((value >>> 12) | 224));
                    hash.update((byte)(((value >>> 6) & 63) | 128));
                    hash.update((byte)((value & 63) | 128));
                }
            }
            return hash.digest();
        }
        catch (NoSuchAlgorithmException error)
        {
            throw new IllegalStateException("SHA256 is unavailable", error);
        }
    }
}
