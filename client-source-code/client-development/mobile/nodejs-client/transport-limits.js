// bound control, socket and retained delivery buffers
// allow complete encrypted file frames within the socket limits
const MiB = 1024 * 1024;

module.exports = Object.freeze({
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
