package com.lemonchat.crypto;

import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import org.json.JSONException;
import org.json.JSONObject;

/**
 * owns one connection's identity and encryption state
 */
public final class CryptoSession implements AutoCloseable
{
    static
    {
        System.loadLibrary("lemonnative");
    }

    private long handle;
    private final String publicKey;

    /**
     * generates the identity and derives configured server keys
     *
     * @param identity identity to use on this connection
     * @param serverKeys ordered server passwords using the desktop row format
     */
    public CryptoSession(Identity identity, String serverKeys)
    {
        byte[] digest = identity.seedDigest();
        try
        {
            handle = nativeCreate(digest, identity.rsaKeyBits, bytes(serverKeys == null ? "" : serverKeys));
            if (handle == 0)
            {
                throw new CryptoException("Cannot create the identity or connection keys");
            }
            publicKey = text(0, null, null);
        }
        catch (RuntimeException error)
        {
            close();
            throw error;
        }
        finally
        {
            Arrays.fill(digest, (byte)0);
        }
    }

    /**
     * returns the public modulus used by the wire protocol
     *
     * @return base64 RSA modulus
     */
    public String getPublicKey()
    {
        return publicKey;
    }

    /**
     * creates the first DH exchange for a new connection
     *
     * @return decimal public mix
     */
    public synchronized String beginDh()
    {
        return text(1, null, null);
    }

    /**
     * accepts the server policy and prepares its public key request
     *
     * @param policy server crypto policy
     * @return inner public_key_info body
     */
    public synchronized JSONObject negotiate(JSONObject policy)
    {
        try
        {
            return new JSONObject(text(2, policy.toString(), null));
        }
        catch (JSONException error)
        {
            throw new CryptoException("Invalid generated negotiation body");
        }
    }

    /**
     * verifies the exchange confirmation before installing session keys
     *
     * @param challenge complete inner challenge body
     */
    public synchronized void finishNegotiation(JSONObject challenge)
    {
        text(3, challenge.toString(), null);
    }

    /**
     * recovers the challenge using this identity's private key
     *
     * @param challenge RSA encrypted challenge
     * @return challenge response text
     */
    public synchronized String decryptChallenge(String challenge)
    {
        return text(4, challenge, null);
    }

    /**
     * protects an application message for the server
     *
     * @param applicationJson complete message wrapper
     * @return metadata envelope
     */
    public synchronized String encrypt(String applicationJson)
    {
        return text(5, applicationJson, null);
    }

    /**
     * checks and removes the server's metadata envelope
     *
     * @param wireEnvelope received envelope
     * @return application JSON
     */
    public synchronized String decrypt(String wireEnvelope)
    {
        return text(6, wireEnvelope, null);
    }

    /**
     * generates four content keys and their IVs
     *
     * @return key array JSON for the current channel
     */
    public synchronized String newContentKeys()
    {
        return text(7, null, null);
    }

    /**
     * protects channel text using a fresh message IV
     *
     * @param keys current channel key array
     * @param plaintext formatted message JSON
     * @return channel wire envelope
     */
    public synchronized String encryptChannel(String keys, String plaintext)
    {
        return text(8, keys, plaintext);
    }

    /**
     * decrypts channel text with the supplied key generation
     *
     * @param keys channel key array
     * @param envelope channel wire envelope
     * @return formatted message JSON
     */
    public synchronized String decryptChannel(String keys, String envelope)
    {
        return text(9, keys, envelope);
    }

    /**
     * protects private content for one recipient
     *
     * @param recipientPublicKey recipient's base64 modulus
     * @param plaintext inner direct content JSON
     * @return direct wire envelope
     */
    public synchronized String encryptDirect(String recipientPublicKey, String plaintext)
    {
        return text(10, recipientPublicKey, plaintext);
    }

    /**
     * opens private content with this identity's private key
     *
     * @param envelope direct wire envelope
     * @return inner direct content JSON
     */
    public synchronized String decryptDirect(String envelope)
    {
        return text(11, envelope, null);
    }

    /**
     * protects a channel picture or authenticated file
     *
     * @param keys current channel key array
     * @param plaintext picture URL or file JSON
     * @param file true for authenticated file content
     * @return attachment wire envelope
     */
    public synchronized String encryptChannelAttachment(String keys, String plaintext, boolean file)
    {
        return text(file ? 14 : 12, keys, plaintext);
    }

    /**
     * opens a channel picture or authenticated file
     *
     * @param keys captured channel key array
     * @param envelope attachment wire envelope
     * @param file true to require file authentication
     * @return decoded picture URL or file JSON
     */
    public synchronized String decryptChannelAttachment(String keys, String envelope, boolean file)
    {
        return text(file ? 15 : 13, keys, envelope);
    }

    /**
     * protects an attachment for one private recipient
     *
     * @param publicKey recipient's base64 modulus
     * @param plaintext picture URL or file JSON
     * @param file true for authenticated file content
     * @return attachment wire envelope
     */
    public synchronized String encryptDirectAttachment(String publicKey, String plaintext, boolean file)
    {
        return text(file ? 18 : 16, publicKey, plaintext);
    }

    /**
     * opens a private picture or authenticated file
     *
     * @param envelope attachment wire envelope
     * @param file true to require file authentication
     * @return picture URL or file JSON
     */
    public synchronized String decryptDirectAttachment(String envelope, boolean file)
    {
        return text(file ? 19 : 17, envelope, null);
    }

    /**
     * copies authenticated media keys for the audio worker
     *
     * @return independently owned media encryption context
     */
    public synchronized MediaCrypto createMediaCrypto()
    {
        requireOpen();
        return MediaCrypto.adopt(nativeCreateMedia(handle));
    }

    /**
     * wraps channel encrypted audio with the connection's media keys
     *
     * @param sender local client ID
     * @param counter unique frame counter
     * @param content channel encrypted audio content
     * @return authenticated version 2 frame
     */
    public synchronized byte[] sealMedia(int sender, long counter, byte[] content)
    {
        requireOpen();
        return requireResult(nativeSealMedia(handle, sender, counter, content));
    }

    /**
     * authenticates a media packet before returning its content
     *
     * @param frame complete received frame
     * @return authenticated counters, sender and channel ciphertext
     */
    public synchronized MediaPacket openMedia(byte[] frame)
    {
        requireOpen();
        byte[] result = requireResult(nativeOpenMedia(handle, frame));
        if (result.length < 23)
        {
            throw new CryptoException("Invalid media content");
        }
        ByteBuffer buffer = ByteBuffer.wrap(result);
        int sender = buffer.getInt();
        long senderCounter = buffer.getLong();
        long frameCounter = buffer.getLong();
        byte[] content = new byte[buffer.remaining()];
        buffer.get(content);
        return new MediaPacket(sender, senderCounter, frameCounter, content);
    }

    /**
     * creates the one use audio connection proof
     *
     * @param keyHex authentication key from the server
     * @param ticketHex server issued ticket
     * @param id local client ID
     * @return proof frame
     */
    public static byte[] mediaProof(String keyHex, String ticketHex, int id)
    {
        return requireResult(nativeMediaProof(bytes(keyHex), bytes(ticketHex), id));
    }

    /**
     * checks the audio server's ticket acknowledgement
     *
     * @param keyHex expected authentication key
     * @param ticketHex expected ticket
     * @param frame received acknowledgement
     * @return true when the ticket and tag match
     */
    public static boolean verifyMediaAcknowledgement(String keyHex, String ticketHex, byte[] frame)
    {
        return nativeVerifyMediaAcknowledgement(bytes(keyHex), bytes(ticketHex), frame);
    }

    /**
     * applies the channel's four inner audio layers
     *
     * @param keysJson channel key array
     * @param sender audio sender ID
     * @param counter sender's frame counter
     * @param content content to transform
     * @param encrypt true for sending
     * @return transformed copy
     */
    public static byte[] contentMediaLayer(String keysJson, int sender, long counter, byte[] content, boolean encrypt)
    {
        return requireResult(nativeContentMediaLayer(bytes(keysJson), sender, counter, content, encrypt));
    }

    /**
     * applies one channel or music bot audio layer
     *
     * @param key 32 byte AES key
     * @param iv 16 byte channel IV or null for a music bot
     * @param sender audio sender ID
     * @param counter sender's frame counter
     * @param content content to transform
     * @return transformed copy
     */
    public static byte[] aesMediaLayer(byte[] key, byte[] iv, int sender, long counter, byte[] content)
    {
        return requireResult(nativeAesMediaLayer(key, iv, sender, counter, content));
    }

    /**
     * clears native keys after all in flight operations finish
     */
    @Override
    public synchronized void close()
    {
        if (handle != 0)
        {
            nativeDestroy(handle);
            handle = 0;
        }
    }

    private String text(int operation, String first, String second)
    {
        requireOpen();
        byte[] result = requireResult(nativeText(handle, operation, bytes(first), bytes(second)));
        try
        {
            return StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(result)).toString();
        }
        catch (CharacterCodingException error)
        {
            throw new CryptoException("The decrypted content is not valid UTF8");
        }
        finally
        {
            Arrays.fill(result, (byte)0);
        }
    }

    private void requireOpen()
    {
        if (handle == 0)
        {
            throw new CryptoException("The connection's encryption state is closed");
        }
    }

    private static byte[] bytes(String text)
    {
        if (text == null)
        {
            return null;
        }
        if (text.indexOf('\0') >= 0)
        {
            throw new CryptoException("Unexpected zero byte in protocol text");
        }
        return text.getBytes(StandardCharsets.UTF_8);
    }

    private static byte[] requireResult(byte[] result)
    {
        if (result == null)
        {
            throw new CryptoException("The encrypted data or negotiation was rejected");
        }
        return result;
    }

    public static final class MediaPacket
    {
        public final int sender;
        public final long senderCounter;
        public final long frameCounter;
        public final byte[] content;

        MediaPacket(int sender, long senderCounter, long frameCounter, byte[] content)
        {
            this.sender = sender;
            this.senderCounter = senderCounter;
            this.frameCounter = frameCounter;
            this.content = content;
        }
    }

    private static native long nativeCreate(byte[] digest, int bits, byte[] serverKeys);
    private static native void nativeDestroy(long handle);
    private static native byte[] nativeText(long handle, int operation, byte[] first, byte[] second);
    private static native long nativeCreateMedia(long handle);
    private static native byte[] nativeSealMedia(long handle, int sender, long counter, byte[] content);
    private static native byte[] nativeOpenMedia(long handle, byte[] frame);
    private static native byte[] nativeMediaProof(byte[] key, byte[] ticket, int id);
    private static native boolean nativeVerifyMediaAcknowledgement(byte[] key, byte[] ticket, byte[] frame);
    private static native byte[] nativeContentMediaLayer(byte[] keys, int sender, long counter, byte[] content, boolean encrypt);
    private static native byte[] nativeAesMediaLayer(byte[] key, byte[] iv, int sender, long counter, byte[] content);
}
