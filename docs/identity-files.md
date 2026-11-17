# Moving or backing up your identity

Suppose your computer uses a 4096-bit identity and you want the same identity on your phone. A
passphrase alone is not enough if the phone is set to 2048 bits: those two sizes produce different
keys from the same passphrase. The server would see a different identity, so the old identity's
groups and permissions would not follow it.

New `.lmn` files save the passphrase and the key size together. When you import one, the client
uses both values to recreate the identity.

## Export from the original device

Open **Export Identity** and save the identity file. The download is named after your current
username, such as `Alex.lmn`. If there is no username, it is named `identity.lmn`.

The file records the size of the key actually in use. For example, your current identity can use
4096 bits while Local settings is set to 8192 bits for the next identity you create. The export
still records 4096 because that is the size needed to reproduce the current identity. Changing the
local setting does not immediately replace the key in use. If key generation is already running,
wait for it to finish before exporting.

Keep the file private. It contains the secret needed to recreate your identity; it is not an
encrypted backup and is not a public-key file.

Android saves the file through the app. Its chat page does not display the stored passphrase;
use **Save identity file** to move or back up the identity.

## Import on the other device

Open **Import Identity**, choose the `.lmn` file, and check the displayed key size. You can also
drop the file onto that window. Selecting a file only checks its contents; it does not replace
your identity or change the key-size setting.

Choose **Import identity** to proceed. In the example above, the selected identity keeps its
4096-bit size and the client generates its key pair using the file's passphrase. Your preference
for the next identity stays separate. If the client is connected,
switching identities reconnects it. Importing the identity already in use keeps the current
connection. The server applies the groups and permissions it currently
associates with the resulting public key.

This restores the identity, rather than a snapshot of server permissions. A group removed by an
administrator is not brought back by importing an old file.

On Android, importing changes the identity saved by the app and used by its background connection.
It also survives restarting the app. Saving the file's values is the first step; the client reports
the import as complete after the matching key pair is ready. If saving or generation fails, it
reports the failure instead of claiming that a different page identity has been imported.

## Older files and older clients

Older `.lmn` files contain only a raw passphrase. The current client still reads those files, but
it shows a legacy-file notice because it cannot know the original key size. Select that size under
**Local settings → Identity → Identity key size** before confirming the import.

Older clients do not understand the new JSON file format. To move an identity to one of those
clients from a browser, copy the raw passphrase shown in **Export Identity** instead. The same window states the
current identity's key size. Set that size on the older client, then paste the complete passphrase
into **Import Identity**. A newer client also supports this manual route.

When pasting a passphrase in the current client, choose **Original identity key size** in the import
window. For the 4096-bit identity in this example, select 4096 even if the device's next-key preference
is 8192. Keep the complete passphrase, including any leading or trailing spaces and line breaks.

An older browser may have saved your passphrase without its size. On startup, the updated client
keeps that saved value and opens the import window with the passphrase already filled in. Choose its
original size to restore it, or choose a versioned `.lmn` backup that records the size. The client
does not guess from the next-key preference: a guess could connect you as a different identity.
A damaged saved record also remains unchanged while you choose a backup.

## File format

A new `.lmn` file is UTF-8 JSON with these fields:

| Field | Type | Required value |
| --- | --- | --- |
| `format` | string | `lemon-chat-identity` |
| `version` | number | `1` |
| `rsa_key_bits` | number | `2048`, `3072`, `4096`, `6144`, or `8192` |
| `passphrase` | string | The complete identity passphrase, preserved exactly |

Generated passphrases have 200 characters. Version 1 also accepts a string of at least 199
characters to preserve compatibility with identities previously entered manually. The complete
file may not exceed 65,536 bytes. Export applies the same limit, so it will not produce a file
that this importer cannot read.

Files are decoded as strict UTF-8. Invalid byte sequences are rejected rather than replaced with
different characters in the secret. Whitespace around the JSON and an initial UTF-8 byte-order mark
are accepted. Whitespace inside
the `passphrase` value is preserved because changing it would change the identity. Unknown format
markers, unsupported versions or sizes, invalid field types, and incomplete passphrases are
rejected. Malformed JSON is not retried as a raw passphrase.

Legacy raw files accept at least 199 characters within the file-size limit and preserve the text
exactly; spaces and line breaks are not trimmed. Content whose first non-whitespace character is
`{`, `[` or `"` is treated as structured JSON and must pass validation. A raw file beginning with a
byte-order mark is also ambiguous. For either unusual legacy passphrase, use **Paste a passphrase
instead** and preserve every character explicitly. Malformed structured files never silently become
a new raw identity.
