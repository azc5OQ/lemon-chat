package com.lemonchat.crypto;

import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;

/**
 * reads and writes portable identity files without changing their passphrases
 */
public final class IdentityFile
{
    public static final int MAX_BYTES = 65536;

    private IdentityFile()
    {
    }

    /**
     * reads a versioned identity or an explicitly sized legacy file
     *
     * @param bytes complete file bytes
     * @param legacyBits original key size for a raw passphrase file
     * @return validated identity values
     */
    public static Identity parse(byte[] bytes, int legacyBits)
    {
        if (bytes == null || bytes.length == 0 || bytes.length > MAX_BYTES)
        {
            throw new IllegalArgumentException("The identity file must be between 1 and 65536 bytes");
        }
        String text;
        try
        {
            text = StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString();
        }
        catch (CharacterCodingException error)
        {
            throw new IllegalArgumentException("The identity file is not valid UTF8", error);
        }
        boolean hasBom = text.charAt(0) == '\ufeff';
        String structured = hasBom ? text.substring(1) : text;
        int first = 0;
        while (first < structured.length() && isWhitespace(structured.charAt(first)))
        {
            first++;
        }
        char initial = first < structured.length() ? structured.charAt(first) : 0;
        if (initial != '{' && initial != '[' && initial != '"')
        {
            if (hasBom)
            {
                throw new IllegalArgumentException("Paste this legacy passphrase explicitly because it begins with a byte order mark");
            }
            return new Identity(text, legacyBits);
        }
        Map<String, Object> fields = new Parser(structured).readObject();
        if (!"lemon-chat-identity".equals(fields.get("format")) || !(fields.get("version") instanceof Number) || ((Number)fields.get("version")).doubleValue() != 1)
        {
            throw new IllegalArgumentException("The identity format or version is not supported");
        }
        Object bits = fields.get("rsa_key_bits");
        Object passphrase = fields.get("passphrase");
        if (!(bits instanceof Number) || !(passphrase instanceof String) || ((Number)bits).doubleValue() != ((Number)bits).intValue())
        {
            throw new IllegalArgumentException("The identity file has invalid fields");
        }
        return new Identity((String)passphrase, ((Number)bits).intValue());
    }

    /**
     * exports the active identity and its original key size
     *
     * @param identity identity to preserve
     * @return complete UTF8 file bytes
     */
    public static byte[] export(Identity identity)
    {
        String text = "{\"format\":\"lemon-chat-identity\",\"version\":1,\"rsa_key_bits\":" + identity.rsaKeyBits + ",\"passphrase\":" + quote(identity.passphrase) + "}";
        byte[] bytes = text.getBytes(StandardCharsets.UTF_8);
        if (bytes.length > MAX_BYTES)
        {
            throw new IllegalArgumentException("The identity file exceeds 65536 bytes");
        }
        return bytes;
    }

    private static boolean isWhitespace(char value)
    {
        return Character.isWhitespace(value) || value == '\u00a0' || value == '\ufeff';
    }

    private static String quote(String text)
    {
        StringBuilder result = new StringBuilder(text.length() + 2);
        result.append('"');
        for (int index = 0; index < text.length(); index++)
        {
            char value = text.charAt(index);
            if (value == '"' || value == '\\')
            {
                result.append('\\').append(value);
            }
            else if (value < 32 || Character.isSurrogate(value))
            {
                result.append("\\u");
                String hex = Integer.toHexString(value);
                for (int padding = hex.length(); padding < 4; padding++)
                {
                    result.append('0');
                }
                result.append(hex);
            }
            else
            {
                result.append(value);
            }
        }
        return result.append('"').toString();
    }

    private static final class Parser
    {
        private final String text;
        private int position;

        Parser(String text)
        {
            this.text = text;
        }

        Map<String, Object> readObject()
        {
            Map<String, Object> fields = new HashMap<>();
            expect('{');
            space();
            if (take('}'))
            {
                finish();
                return fields;
            }
            do
            {
                String name = string();
                expect(':');
                space();
                Object value = peek() == '"' ? string() : number();
                if (fields.put(name, value) != null)
                {
                    fail();
                }
                space();
                if (take('}'))
                {
                    finish();
                    return fields;
                }
                expect(',');
            }
            while (position < text.length());
            throw new IllegalArgumentException("The identity JSON is incomplete");
        }

        private String string()
        {
            expect('"');
            StringBuilder result = new StringBuilder();
            while (position < text.length())
            {
                char value = text.charAt(position++);
                if (value == '"')
                {
                    return result.toString();
                }
                if (value < 32)
                {
                    fail();
                }
                if (value == '\\')
                {
                    if (position >= text.length())
                    {
                        fail();
                    }
                    value = text.charAt(position++);
                    switch (value)
                    {
                        case '"': case '\\': case '/': break;
                        case 'b': value = '\b'; break;
                        case 'f': value = '\f'; break;
                        case 'n': value = '\n'; break;
                        case 'r': value = '\r'; break;
                        case 't': value = '\t'; break;
                        case 'u':
                            if (position + 4 > text.length())
                            {
                                fail();
                            }
                            int decoded = 0;
                            for (int digit = 0; digit < 4; digit++)
                            {
                                char character = text.charAt(position++);
                                int hex = character >= '0' && character <= '9' ? character - '0' : character >= 'a' && character <= 'f' ? character - 'a' + 10 : character >= 'A' && character <= 'F' ? character - 'A' + 10 : -1;
                                if (hex < 0)
                                {
                                    fail();
                                }
                                decoded = decoded * 16 + hex;
                            }
                            value = (char)decoded;
                            break;
                        default: fail();
                    }
                }
                result.append(value);
            }
            throw new IllegalArgumentException("The identity JSON has an incomplete string");
        }

        private Double number()
        {
            int start = position;
            while (position < text.length() && "0123456789+-.eE".indexOf(text.charAt(position)) >= 0)
            {
                position++;
            }
            String value = text.substring(start, position);
            if (!value.matches("-?(0|[1-9][0-9]*)(\\.[0-9]+)?([eE][+-]?[0-9]+)?"))
            {
                fail();
            }
            return Double.valueOf(value);
        }

        private void expect(char value)
        {
            space();
            if (!take(value))
            {
                fail();
            }
        }

        private boolean take(char value)
        {
            if (peek() == value)
            {
                position++;
                return true;
            }
            return false;
        }

        private char peek()
        {
            return position < text.length() ? text.charAt(position) : 0;
        }

        private void space()
        {
            while (position < text.length() && " \t\r\n".indexOf(text.charAt(position)) >= 0)
            {
                position++;
            }
        }

        private void finish()
        {
            space();
            if (position != text.length())
            {
                fail();
            }
        }

        private void fail()
        {
            throw new IllegalArgumentException("The identity file contains invalid JSON");
        }
    }
}
