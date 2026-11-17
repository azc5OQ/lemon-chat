package com.lemonchat;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;

/** Checks byte limits before decoding bridge JSON. */
final class BridgeProtocol
{
	static final int MAX_LINE_BYTES = 1024 * 1024;
	static final int MAX_PENDING_BYTES = 2 * MAX_LINE_BYTES;

	/**
	 * Reads one bridge line as strict UTF8.
	 * @param input stream to read
	 * @param limit byte limit excluding the newline
	 * @return line without its newline, or {@code null} at clean EOF
	 * @throws IOException on invalid UTF8, an oversized or incomplete line, or a read failure
	 */
	static String readLine(InputStream input, int limit) throws IOException
	{
		ByteArrayOutputStream bytes = new ByteArrayOutputStream(Math.min(limit, 4096));
		int value;
		while ((value = input.read()) != -1)
		{
			if (value == '\n')
			{
				return StandardCharsets.UTF_8.newDecoder()
					.onMalformedInput(CodingErrorAction.REPORT)
					.onUnmappableCharacter(CodingErrorAction.REPORT)
					.decode(ByteBuffer.wrap(bytes.toByteArray())).toString();
			}
			if (bytes.size() >= limit)
			{
				throw new IOException("bridge line too large");
			}
			bytes.write(value);
		}
		if (bytes.size() != 0)
		{
			throw new IOException("incomplete bridge line");
		}
		return null;
	}

	/**
	 * Escapes lone UTF16 surrogates without changing valid pairs.
	 * @param json serialized JSON
	 * @return JSON safe for UTF8 transport
	 */
	static String escapeSurrogates(String json)
	{
		StringBuilder safe = new StringBuilder(json.length());
		for (int i = 0; i < json.length(); i++)
		{
			char value = json.charAt(i);
			if (Character.isHighSurrogate(value) && i + 1 < json.length() && Character.isLowSurrogate(json.charAt(i + 1)))
			{
				safe.append(value);
				safe.append(json.charAt(++i));
			}
			else if (Character.isSurrogate(value))
			{
				safe.append(String.format(java.util.Locale.ROOT, "\\u%04x", (int)value));
			}
			else
			{
				safe.append(value);
			}
		}
		return safe.toString();
	}
}
