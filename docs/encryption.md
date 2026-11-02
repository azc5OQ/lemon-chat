# Encryption

Lemon Chat encrypts message content with AES keys. For a direct message, the client creates four random AES keys and encrypts the content with each of them in turn.

The recipient needs those same keys to read the message. The sender therefore includes a copy of them in the message envelope, encrypted for the recipient using their public key. The encrypted content and encrypted keys travel together.

The recipient uses their private key to recover the AES keys, then uses those AES keys to decrypt the content. The server forwards the envelope without opening the encrypted keys or reading the content.

## Direct messages and attachments

The direct-message envelope contains two fields:

| Field | What it contains |
| --- | --- |
| `message_keys` | The AES keys, encrypted for the recipient using their public key. |
| `message_value` | The message content, encrypted with those AES keys. |

These fields are stored together in the request's `message.value`. The recipient's ID is stored separately as `receiver_id`, so the server can read where to send the message without decrypting its content.

Each identity has an RSA public key and a matching private key. Other clients use the public key to encrypt data for that identity. The private key stays with the receiving client and is needed to recover that data.

### How the AES keys are encrypted

The current implementation uses one extra AES key when encrypting the four message keys for the recipient:

1. It encrypts the four message keys together using this extra AES key and AES-CBC.
2. It encrypts the extra AES key with the recipient's RSA public key.
3. It stores both encrypted results together in `message_keys`.

The receiver reverses this process. Their RSA private key decrypts the extra AES key. That key decrypts the four message keys. Those four keys then decrypt `message_value`, in reverse order to the encryption.

This is what encrypting the message keys for the recipient means in the current code. RSA protects the extra key used to open the four message keys; it does not encrypt the chat text directly.

Each direct message gets four new message keys. Direct pictures and files use the same approach: their envelopes contain encrypted content and the encrypted keys needed to read it.

## Channel content

A channel uses four shared AES keys so its members can read the same conversation. One client creates these keys and sends them to the others. This client is called the **maintainer**.

The server chooses the maintainer **at random from the available clients in the channel**. It announces that client together with a number called `maintainer_generation`. The maintainer then sends a separate encrypted copy of the channel keys to each active member. Idle clients and music bots are not included in these deliveries.

The channel keys are delivered inside direct messages using the process above. In this case, the content of the direct message is the set of channel keys. The direct message has its own four AES message keys, encrypted for that particular recipient.

The recipient first opens the direct message, then reads the channel keys inside it. After that, they can use the shared channel keys to decrypt channel text, pictures, files, audio and video. Ordinary channel messages use those shared keys rather than sending new keys to every member with every message.

The server forwards the key deliveries without creating or receiving readable copies of the channel keys. An admin who joins the channel receives the same keys as its other active members.

Before accepting a delivery, the client checks the sender against the announced maintainer and checks the channel and `maintainer_generation`. A delivery from an earlier maintainer must not replace the current keys. The [channel guide](channels.md#the-channel-maintainer) explains selection and replacement, including voting when keys do not arrive.

Channel keys normally change when someone joins or leaves. The server can allow larger channels to keep their keys to reduce the work of creating and delivering replacements. See [Keys in large channels](channels.md#keys-in-large-channels) for the limits and effects of this setting.

After replacement, a client keeps the older keys for up to ten minutes to read delayed content. When they expire, it also removes references kept for file downloads. Current keys stay until replaced or cleared; a full connection reset clears both current and older keys. Keeping old keys locally does not give them to a new member.

The channel password is separate. The server checks that password to allow entry; it is not used as the channel encryption key. The admin password is also separate from the conversation keys.

## Editing a message

Suppose you sent “Meet at six” and want to change it to “Meet at seven.” The updated client puts the replacement text and a reference to the original message together inside an encrypted edit body. Removing the connection encryption at the server still leaves that body encrypted.

For a direct edit, the client creates four fresh message keys and encrypts those keys for the recipient, just as it does for a new direct message. For a channel edit, it uses the current channel keys and a fresh message IV. If channel keys are unavailable, it keeps the original message and asks you to wait for the keys.

The recipient decrypts the edit before reading the replacement text. It checks that the sender is the original author and that the target belongs to the same conversation, then changes the displayed text. An admin password does not supply the recipient's private key or bypass that author check.

Older clients used a separate `edit_chat_message_request` with server-readable replacement text. The updated client never sends this legacy request and ignores the corresponding outer `chat_message_edit` response. Both participants, or all members of a channel, need updated clients for encrypted editing. Updating does not undo any disclosure from earlier legacy edits.

## Messages between a client and the server

The completed message envelope is also encrypted for its journey between the client and server. This protects the request as a whole, including the message type, recipient and the already encrypted content and keys inside it.

The sender encrypts the request with its session encryption key. If the operator has configured server connection keys, it then encrypts the result with each of those keys. The server checks the request, removes any server-key encryption, then decrypts it with the sender's session key. It can then read the delivery information, but the content and message keys remain encrypted for the recipient.

To forward it, the server encrypts the outgoing request with the configured server keys, if any, then with the receiver's session key. The receiving client removes this connection encryption before opening `message_keys` and `message_value` as described above.

This is why the keys used for the server connection do not let the server read the conversation. They open the outer request, not the recipient's encrypted copy of the message keys.

Requests such as joining a channel use this connection encryption too, but do not contain an encrypted conversation inside them. The server needs to read the request itself to carry it out. Its type, channel, recipient and similar delivery information are referred to as **metadata** in the code.

### Establishing the connection keys

The operator can configure connection passwords, but they are optional. If configured, these passwords are converted into server connection keys using **SHA-256**. The client and server use these keys for their initial setup messages.

During setup the client and server perform a **Diffie-Hellman exchange**, whether or not the operator has configured connection passwords. This gives them a shared secret for that connection. **HKDF-SHA256** derives two session keys from this secret: one for encryption and one for checking messages.

Connection messages use **AES-256-CTR** encryption and a random starting value, called an **IV**. Once the session keys are ready, the sender also supplies an **HMAC-SHA256** check. The receiver checks it before decrypting the message. HMAC detects changes to the protected data; it is not another encryption operation.

### Before the session keys are ready

The first setup messages use only the configured server keys, if any, because the session keys do not exist yet. A secure WebSocket connection, called **WSS**, also protects them with TLS. Plain WebSocket, called **WS**, has no TLS protection.

With no configured server keys, these early messages have no Lemon Chat encryption. They are readable on the network over WS, while WSS still protects them with TLS. Once setup completes, session encryption and the HMAC check apply as well.

The `rsa_key_too_weak` response is an exception during login. If the identity key is too small and the admin allows the required size to be shared, the server sends that size as readable JSON. This response has no Lemon Chat encryption, although WSS still protects it with TLS.

## Audio and video connections

User audio and video are encrypted with the channel's four shared AES keys. The client then encrypts each packet with any server keys configured by the operator, followed by its session key. These packets use WebSocket or WebRTC DataChannel according to the connection method in use.

The server checks each packet, removes the sender's session encryption and replaces it with encryption for the receiving client's session. It leaves the channel-key encryption and any server-key encryption in place. The receiver removes these protections to play the audio or video.

This order differs from incoming chat requests, where the server-key encryption is outside the session encryption. Audio and video also use packet counters and an HMAC-SHA256 check shortened to eight bytes.

The [WebRTC guide](webrtc.md) explains the connection design. Each client's connection ends at the server; clients do not connect directly to one another or exchange their IP addresses through WebRTC. WebRTC is optional, and users on WebSocket and WebRTC can communicate with each other.

Server music bots work differently because the server reads and plays the uploaded file. The server creates four keys for the bot's audio, encrypts it with those keys and any server keys configured by the operator, then encrypts it for each receiver's session. The server therefore knows the bot's original sound and audio keys. These are separate from the channel keys used for users' voices.

## TLS, WSS and DTLS

A `wss://` connection uses TLS to protect the WebSocket traffic in addition to Lemon Chat's own encryption. A `ws://` connection does not add TLS. WebRTC DataChannel uses DTLS for its connection protection.

Chat and audio or video use separate connections. If chat uses WSS and voice uses WebRTC, the voice packets have DTLS protection. The chat connection's TLS does not also apply to those voice packets. Audio or video sent over a separate WSS connection gets TLS on that connection.

TLS may end at stunnel or a tunnel service before the traffic reaches Lemon Chat. That service removes TLS while leaving Lemon Chat's encrypted request intact. Handling TLS does not by itself give the service Lemon Chat's connection or conversation keys. If it forwards the request over plain WS, that remaining part of the route has Lemon Chat's encryption without TLS.

HTTPS protects the download of the web client. It does not determine whether the client's later chat connection uses WS or WSS.

## What the server can see

The server can read requests and delivery information. It also knows connection addresses, public identities, usernames and channel membership. It forwards users' encrypted conversation content without receiving the private keys needed to open their message keys.

Some features intentionally give the server readable data. A server music bot receives and decodes an uploaded MP3 file. Link previews send the posted URL separately to the server for lookup, even though the chat message containing that URL is encrypted.

Someone observing network traffic can see addresses, ports, timing and amounts of data. Over WS, they can also see the outer fields containing encrypted data, an IV and the message check. The message type, recipient and chat content remain inside the encrypted request after login. WSS protects those outer fields with TLS too.

Clients obtain other users' public keys from the server. Lemon Chat is intended to be hosted by the people using it: the host controls the server software and knows whether they have changed the public keys it supplies.

## Implementation details

Each of the four content keys has 32 random bytes, giving a 256-bit AES key, and a 16-byte random starting value. Channel keys use the same format. Current text messages also carry a fresh random 16-byte IV, combined with each key's starting value during encryption. These starting values are not secret keys.

Some encrypted values are represented as Base64 text. Converting Base64 back into bytes does not decrypt those bytes.

The connection encryption code separates the list of keys from the operation used for each key type. Both client and server reject unsupported types. **AES-256-CTR is currently the only implemented metadata encryption algorithm**; the type field does not allow the host to select another algorithm.

### Encryption count

For reference, the content is encrypted with four conversation keys, one session key and each server key configured by the operator, if any. The count below is for content after login on one client-to-server or server-to-client connection. RSA and the extra AES operation protect the enclosed keys separately and are not counted as encryption of the content.

| Configured server keys | WS | WSS | WebRTC audio or video |
| --- | --- | --- | --- |
| 0 | 5 AES encryptions | 5 AES + TLS | 5 AES + DTLS |
| 1 | 6 AES encryptions | 6 AES + TLS | 6 AES + DTLS |
| 5 | 10 AES encryptions | 10 AES + TLS | 10 AES + DTLS |

With one server key and WSS, the total is seven: six AES encryptions and TLS. The server replaces connection encryption when forwarding, so the counts on the incoming and outgoing connections are not added together. Delivery information and requests without conversation content do not have the four content encryptions.

## Relevant source files

- [Direct message construction, receipt and channel key delivery](../client-source-code/client-development/browser/src/scripts/app/workers.js)
- [RSA public-key protection and the extra AES key](../client-source-code/client-development/browser/src/scripts/app/rsa-crypto.js)
- [Content encryption, decryption and channel key handling](../client-source-code/client-development/browser/src/scripts/app/keys.js)
- [Direct picture and file envelopes](../client-source-code/client-development/browser/src/scripts/app/chat-files.js)
- [Server forwarding of encrypted direct messages](../server-source-code/main/msg/client/client_msg_chat.c)
- [Metadata encryption and key types](../server-source-code/main/metadata_crypto.c)
- [Audio and video connection protection](../server-source-code/main/media_auth.c)
- [Client audio encryption and connection choice](../client-source-code/client-development/browser/src/scripts/app/audio-transport.js)
- [Client video encryption](../client-source-code/client-development/browser/src/scripts/app/video-worker.js)
- [WebRTC DataChannel and DTLS connection setup](../server-source-code/third-party/libdatachannel-0.24.2/src/impl/peerconnection.cpp)
- [Server music bot audio encryption](../server-source-code/main/core/audio_channel.c)
