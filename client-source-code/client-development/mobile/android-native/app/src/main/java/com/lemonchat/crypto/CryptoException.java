package com.lemonchat.crypto;

/**
 * reports rejected crypto input without exposing secret values
 */
public final class CryptoException extends RuntimeException
{
    public CryptoException(String message)
    {
        super(message);
    }
}
