# Desktop memory ownership

## When a file is sent

Suppose you choose a picture and press Send, then disconnect while it is being
prepared. The GUI must not free bytes that the decoder is still reading. It also
must not let that old work appear in a new connection.

The GUI queues a bounded copy of the path and destination. The session reserves
one attachment record, including its connection generation and the recipient's
client lifetime. The attachment worker takes exclusive ownership of that record's
payload pointers under the manager mutex. The manager's copies become NULL.

Disconnect marks unfinished records cancelled. The worker can finish its current
bounded operation, but it checks the generation and cancellation state before
returning the result. An obsolete result is erased and freed. Destruction joins
the worker before releasing its manager, mutex or private crypto context.

Without that ownership handoff, cancellation could free a file underneath the
decoder. Without the generation check, an old completion could overwrite a record
belonging to a new connection.

## Allocation ledger

The ledger is explicit ownership and capacity in each subsystem, rather than a
second allocator or changes to the server's clib. The application initializes
owned storage before use. Cleanup uses `desktop_memory__erase` (volatile byte
writes) or Mbed TLS zeroization where ordinary clearing could be optimized away.

| Storage | Owner and bound | Release point |
| --- | --- | --- |
| Session and its snapshots/command buffers | Session object; fixed-size arrays | After transport, audio and attachment workers stop; erase the object before free |
| Network message | Queue entry carries byte count; allocation includes one terminator | Consumption, queue reset or transport shutdown; erase count plus terminator |
| Transport receive buffer | Transport worker; `DESKTOP_MESSAGE_LIMIT + 1` | Worker exit, including failure; erase full capacity |
| Audio object and speaker array | Audio worker; fixed PCM rings and sixteen speaker slots | Stop devices, destroy codecs, erase rings/slots, then free |
| Device enumeration context/catalog | Enumeration worker; bounded returned choices | Uninitialize native context, erase application-owned context/catalog, then free |
| Thread, mutex, hotkey, overlay controller and IPC wrappers | Their create/destroy pair; fixed object size | Join/close native resources first; erase wrapper before free |
| Crypto scratch | Calling crypto function; explicit capacities | One cleanup path erases temporary keys, plaintext and encoded buffers |
| Crypto strings | Returned to caller; terminated text, initialized allocation | `desktop_crypto__free_text`; plaintext decryptors validate padding before returning |
| Owned crypto/chat/settings JSON trees | Calling operation | `desktop_crypto__free_json` erases owned strings and nodes; borrowed constant/reference nodes retain their owner |
| Settings file buffer | Load operation; `DESKTOP_SETTINGS_FILE_CAPACITY` | Erase full capacity on success and failure |
| Attachment original bytes | Record or its worker-local copy; `bytes_capacity` | Cancellation, failed work, eviction, reconnect or shutdown |
| Attachment ciphertext | Record or worker-local copy; `wire_capacity` | Decode, completed upload, cancellation, failure or shutdown |
| Attachment preview pixels | Record or worker; width × height × four bytes | Same record cleanup; temporary full decoded pixels are erased after preview conversion |
| GUI upload pixels | GUI owns an independent copy | Erase and free immediately after synchronous texture upload |
| Image texture | GUI cache; sixteen entries and 32 MiB | Record eviction/reconnect or UI shutdown, while the renderer still exists |
| Tree icon pixels | Session; 1000 fixed 32x32 RGBA slots, about 4 MiB | Reconnect or icon replacement clears slots; session destruction erases the complete object |
| SVG decoder scratch | Per-thread ledger; 2048 allocations, 8 MiB combined maximum | Every allocation is wiped at release; cleanup also drains vendor early-return leftovers |
| Tree textures | GUI cache; at most 1000 server icons and 676 country-code slots, each 32x32 | Server catalog replacement clears server textures; shutdown releases both caches |
| Private-message read counts | GUI; one entry per connected peer, bounded by 1024 | Snapshot reconciliation drops disconnected peers; a new connection clears the table |
| macOS resolved executable path | OS allocation returned by `realpath` | Copy into bounded application path, erase used path bytes, then free |

The attachment manager retains at most sixteen records, 64 MiB of original bytes
and preview pixels, and two incoming ciphertext transfers whose combined size is
at most 64 MiB. It prepares one outgoing transfer at a time. File and image limits
are checked before their main allocations; image dimensions are checked before
decoding. Encryption, parsing and decoding need additional bounded scratch space;
64 MiB is a retained-cache limit, not a whole-process memory ceiling.

Server icon data is limited to 64 KiB after base64 decoding. Bitmap dimensions
are checked against 256 pixels per side before decoding. SVG dimensions, path
counts and coordinates are checked before rasterization into a fixed 32-pixel
square. NanoSVG parses inline geometry without scripts, network fetches or
external resources. GUI rendering only reads prepared textures; the session
worker decodes server icons and gives the GUI independent pixel copies under a
mutex. Country flags come from the packaged assets and are cached on first use.

## Library boundaries

Application code owns library handles and releases them with the corresponding
library API. Library-internal allocations belong to Mbed TLS, Opus, miniaudio,
libfvad, cJSON, Meek UI, the image decoders or the OS. Their source has not been
rewritten to use the application's clib or allocation rules.

For example, releasing an OpenGL texture ends the application's ownership, but
the driver does not promise immediate physical erasure of GPU memory. The same
limitation applies to opaque library scratch, OS socket buffers, allocator
metadata and disk or swap copies. Application cleanup is not a claim that every
byte held by those dependencies is securely erased.

## Checks

Local tests exercise encrypted channel/direct pictures and files, binary data
containing zero bytes, size and image-dimension rejection, incomplete transfers,
offer acceptance, cancellation, saving without overwrite and worker shutdown.
Independent Node crypto code checks the existing browser wire format. No user
server or audio device is needed.

`LEMON_DESKTOP_SANITIZERS=ON` instruments application targets and local checks with
AddressSanitizer and UndefinedBehaviorSanitizer on supported GCC/Clang builds.
Leak detection checks the exercised paths. Vendored library targets retain their
normal build configuration. Passing tests does not prove all C execution paths
memory-safe, particularly unexercised failures, GUI drivers and platform adapters.

When adding an allocation, record its owner and capacity, initialize it, handle
failure before publishing it, and provide cleanup for success, cancellation and
failure. A borrowed pointer must not outlive its owner or cross a worker boundary
without an explicit handoff or copy.
