package com.lemonchat.storage;

import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.util.Base64;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/** encrypts history with a key independent of the saved identity */
public final class ArchiveVault
{
    private static final String KEY_ALIAS = "lemon-native-private-history";
    private final String alias;
    private final IdentityVault legacy;

    public ArchiveVault()
    {
        this(KEY_ALIAS, null);
    }

    ArchiveVault(String alias)
    {
        this(alias, null);
        if (!alias.startsWith("lemon-native-history-test-"))
        {
            throw new IllegalArgumentException("Invalid test history alias");
        }
    }

    private ArchiveVault(String alias, IdentityVault legacy)
    {
        this.alias = alias;
        this.legacy = legacy;
    }

    static ArchiveVault legacy(IdentityVault identity)
    {
        return new ArchiveVault(null, identity);
    }

    private SecretKey key(boolean create) throws Exception
    {
        if (legacy != null)
        {
            throw new IOException("Legacy history is read only");
        }
        KeyStore store = KeyStore.getInstance("AndroidKeyStore");
        store.load(null);
        if (!store.containsAlias(alias))
        {
            if (!create)
            {
                throw new IOException("The saved history key is unavailable");
            }
            KeyGenerator generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
            generator.init(new KeyGenParameterSpec.Builder(alias, KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT).setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).setKeySize(256).build());
            generator.generateKey();
        }
        return (SecretKey)store.getKey(alias, null);
    }

    /**
     * revokes only the dedicated history key
     *
     * @throws Exception when Android refuses key deletion
     */
    public void revoke() throws Exception
    {
        if (legacy != null)
        {
            throw new IOException("The identity key cannot be removed with history");
        }
        KeyStore store = KeyStore.getInstance("AndroidKeyStore");
        store.load(null);
        if (store.containsAlias(alias))
        {
            store.deleteEntry(alias);
        }
        if (store.containsAlias(alias))
        {
            throw new IOException("The history key could not be removed");
        }
    }

    void requireExistingKey() throws Exception
    {
        if (key(false) == null)
        {
            throw new IOException("The saved history key is unavailable");
        }
    }

    String seal(byte[] value, String purpose) throws Exception
    {
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE, key(true));
        cipher.updateAAD(purpose.getBytes(StandardCharsets.UTF_8));
        return Base64.getEncoder().encodeToString(cipher.getIV()) + ":" + Base64.getEncoder().encodeToString(cipher.doFinal(value));
    }

    byte[] open(String value, String purpose) throws Exception
    {
        if (legacy != null)
        {
            return legacy.open(value, purpose);
        }
        String[] parts = value.split(":", -1);
        if (parts.length != 2)
        {
            throw new IOException("The saved history is damaged");
        }
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.DECRYPT_MODE, key(false), new GCMParameterSpec(128, Base64.getDecoder().decode(parts[0])));
        cipher.updateAAD(purpose.getBytes(StandardCharsets.UTF_8));
        return cipher.doFinal(Base64.getDecoder().decode(parts[1]));
    }

    byte[] sealBytes(byte[] value, String purpose) throws Exception
    {
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE, key(true));
        cipher.updateAAD(purpose.getBytes(StandardCharsets.UTF_8));
        byte[] iv = cipher.getIV();
        if (iv.length != 12)
        {
            throw new IOException("Unexpected history IV size");
        }
        byte[] encrypted = cipher.doFinal(value);
        byte[] result = new byte[13 + encrypted.length];
        result[0] = 1;
        System.arraycopy(iv, 0, result, 1, iv.length);
        System.arraycopy(encrypted, 0, result, 13, encrypted.length);
        return result;
    }

    byte[] openBytes(byte[] value, String purpose) throws Exception
    {
        if (legacy != null)
        {
            return legacy.openBytes(value, purpose);
        }
        if (value.length < 29 || value[0] != 1)
        {
            throw new IOException("The saved attachment is damaged");
        }
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.DECRYPT_MODE, key(false), new GCMParameterSpec(128, value, 1, 12));
        cipher.updateAAD(purpose.getBytes(StandardCharsets.UTF_8));
        return cipher.doFinal(value, 13, value.length - 13);
    }
}
