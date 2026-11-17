package com.lemonchat.storage;

import android.content.Context;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import com.lemonchat.crypto.Identity;
import com.lemonchat.crypto.IdentityFile;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.util.Base64;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/** keeps the identity secret encrypted with the app's Android Keystore key */
public final class IdentityVault
{
    private static final String KEY_ALIAS = "lemon-native-identity";
    private final android.content.SharedPreferences preferences;

    public IdentityVault(Context context)
    {
        this(context, "identity-vault");
    }

    IdentityVault(Context context, String preferenceName)
    {
        preferences = context.getSharedPreferences(preferenceName, Context.MODE_PRIVATE);
    }

    /**
     * restores the saved identity or creates the first one
     *
     * @return the identity kept by this installation
     * @throws Exception when existing storage cannot be read
     */
    public Identity load() throws Exception
    {
        return load(true, 2048);
    }

    /**
     * uses the selected key size only when no identity has been saved
     *
     * @param keyBits key size for the first identity
     * @return saved or newly created identity
     * @throws Exception when creation or storage fails
     */
    public Identity load(int keyBits) throws Exception
    {
        return load(true, keyBits);
    }

    /**
     * restores an existing identity without creating a replacement
     *
     * @return saved identity
     * @throws Exception when the identity is missing or unreadable
     */
    public Identity loadExisting() throws Exception
    {
        return load(false, 2048);
    }

    private Identity load(boolean create, int keyBits) throws Exception
    {
        String saved = preferences.getString("identity", null);
        if (saved == null)
        {
            if (!create)
            {
                throw new java.io.IOException("The saved identity is missing. Import its backup to continue.");
            }
            Identity identity = Identity.create(keyBits);
            save(identity);
            return identity;
        }
        byte[] bytes = open(saved, "identity");
        try
        {
            return IdentityFile.parse(bytes, 2048);
        }
        finally
        {
            java.util.Arrays.fill(bytes, (byte)0);
        }
    }

    /**
     * saves a validated identity before it is used for a connection
     *
     * @param identity identity to keep
     * @throws Exception when encryption or persistence fails
     */
    public void save(Identity identity) throws Exception
    {
        byte[] bytes = IdentityFile.export(identity);
        String encrypted;
        try
        {
            encrypted = seal(bytes, "identity");
        }
        finally
        {
            java.util.Arrays.fill(bytes, (byte)0);
        }
        if (!preferences.edit().putString("identity", encrypted).commit())
        {
            throw new java.io.IOException("The identity could not be saved");
        }
    }

    /**
     * encrypts optional server keys separately from public connection settings
     *
     * @param value server key configuration
     * @throws Exception when the value cannot be saved
     */
    public void saveServerKeys(String value) throws Exception
    {
        byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
        String encrypted;
        try
        {
            encrypted = seal(bytes, "server-keys");
        }
        finally
        {
            java.util.Arrays.fill(bytes, (byte)0);
        }
        if (!preferences.edit().putString("server-keys", encrypted).commit())
        {
            throw new java.io.IOException("The server keys could not be saved");
        }
    }

    /**
     * restores the optional server key configuration
     *
     * @return saved keys or an empty string
     * @throws Exception when saved keys cannot be decrypted
     */
    public String loadServerKeys() throws Exception
    {
        return loadServerKeys(false);
    }

    /**
     * restores the saved server key list without assuming an empty configuration
     *
     * @return the exact saved server key list
     * @throws Exception when the configuration is missing or unreadable
     */
    public String loadExistingServerKeys() throws Exception
    {
        return loadServerKeys(true);
    }

    private String loadServerKeys(boolean required) throws Exception
    {
        String saved = preferences.getString("server-keys", null);
        if (saved == null)
        {
            if (required)
            {
                throw new java.io.IOException("The saved server encryption keys are missing");
            }
            return "";
        }
        byte[] bytes = open(saved, "server-keys");
        try
        {
            return new String(bytes, StandardCharsets.UTF_8);
        }
        finally
        {
            java.util.Arrays.fill(bytes, (byte)0);
        }
    }

    private SecretKey key() throws Exception
    {
        KeyStore store = KeyStore.getInstance("AndroidKeyStore");
        store.load(null);
        if (!store.containsAlias(KEY_ALIAS))
        {
            KeyGenerator generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
            generator.init(new KeyGenParameterSpec.Builder(KEY_ALIAS, KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT).setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).setKeySize(256).build());
            generator.generateKey();
        }
        return (SecretKey) store.getKey(KEY_ALIAS, null);
    }

    /**
     * encrypts a record and binds it to its storage purpose
     *
     * @param value record bytes
     * @param purpose record type and ownership
     * @return encoded IV and authenticated ciphertext
     * @throws Exception when encryption fails
     */
    String seal(byte[] value, String purpose) throws Exception
    {
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE, key());
        cipher.updateAAD(purpose.getBytes(StandardCharsets.UTF_8));
        return Base64.getEncoder().encodeToString(cipher.getIV()) + ":" + Base64.getEncoder().encodeToString(cipher.doFinal(value));
    }

    /**
     * opens a record only for its original storage purpose
     *
     * @param value encoded IV and ciphertext
     * @param purpose expected record type and ownership
     * @return authenticated record bytes
     * @throws Exception when the record cannot be authenticated
     */
    byte[] open(String value, String purpose) throws Exception
    {
        String[] parts = value.split(":", -1);
        if (parts.length != 2)
        {
            throw new java.io.IOException("The saved identity is damaged");
        }
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.DECRYPT_MODE, key(), new GCMParameterSpec(128, Base64.getDecoder().decode(parts[0])));
        cipher.updateAAD(purpose.getBytes(StandardCharsets.UTF_8));
        return cipher.doFinal(Base64.getDecoder().decode(parts[1]));
    }

    /**
     * encrypts bounded binary content without Base64 copies
     *
     * @param value content to encrypt
     * @param purpose record ownership and position
     * @return version, IV and authenticated ciphertext
     * @throws Exception when encryption fails
     */
    byte[] sealBytes(byte[] value, String purpose) throws Exception
    {
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE, key());
        cipher.updateAAD(purpose.getBytes(StandardCharsets.UTF_8));
        byte[] iv = cipher.getIV();
        if (iv.length != 12)
        {
            throw new java.io.IOException("Unexpected encryption IV size");
        }
        byte[] encrypted = cipher.doFinal(value);
        byte[] result = new byte[13 + encrypted.length];
        result[0] = 1;
        System.arraycopy(iv, 0, result, 1, iv.length);
        System.arraycopy(encrypted, 0, result, 13, encrypted.length);
        return result;
    }

    /**
     * authenticates a binary record before returning any content
     *
     * @param value encrypted binary record
     * @param purpose expected ownership and position
     * @return authenticated plaintext
     * @throws Exception when the record is damaged or belongs elsewhere
     */
    byte[] openBytes(byte[] value, String purpose) throws Exception
    {
        if (value.length < 29 || value[0] != 1)
        {
            throw new java.io.IOException("The saved attachment is damaged");
        }
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.DECRYPT_MODE, key(), new GCMParameterSpec(128, value, 1, 12));
        cipher.updateAAD(purpose.getBytes(StandardCharsets.UTF_8));
        return cipher.doFinal(value, 13, value.length - 13);
    }
}
