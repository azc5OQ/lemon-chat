// this file holds the size and time limits of the connections inside the phone, between Node.js, the Java part of
// the app and the page in the app window. a limit stops one connection from using too much memory or waiting forever.
// the message limits are large enough for one whole encrypted file part
const MiB = 1024 * 1024;

module.exports = Object.freeze(
{
    controlLineBytes: MiB,
    controlQueueBytes: 2 * MiB,
    controlWriteTimeoutMs: 10000,
    headerBytes: 16 * 1024,
    handshakeTimeoutMs: 5000,
    tokenFrameBytes: 128,
    pendingConnections: 8,
    messageBytes: 144 * MiB,
    websocketQueueBytes: 152 * MiB,
    replayQueueBytes: 152 * MiB,
    replayQueueCount: 4096,
    requestQueueCount: 128,
    writeTimeoutMs: 15000
});
