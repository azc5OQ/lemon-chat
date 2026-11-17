#include <jni.h>
#include <rtc/rtc.hpp>
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace
{
struct Event
{
    unsigned char type;
    std::vector<unsigned char> value;
    std::string detail;
};

struct Peer
{
    std::mutex mutex;
    std::shared_ptr<rtc::PeerConnection> connection;
    std::shared_ptr<rtc::DataChannel> audio;
    std::deque<Event> events;
    size_t queuedBytes = 0;
    std::atomic<bool> closed{false};
    bool overflow = false;

    void push(unsigned char type, const unsigned char *value, size_t size, const std::string &detail = "")
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (closed)
        {
            return;
        }
        if (size > 65536 || detail.size() > 4096 || events.size() >= 128 || queuedBytes + size + detail.size() > 524288)
        {
            overflow = true;
            return;
        }
        Event event{type, {}, detail};
        if (size != 0)
        {
            event.value.assign(value, value + size);
        }
        queuedBytes += size + detail.size();
        events.push_back(std::move(event));
    }

    void push(unsigned char type, const std::string &value = "", const std::string &detail = "")
    {
        push(type, reinterpret_cast<const unsigned char *>(value.data()), value.size(), detail);
    }
};

std::mutex registryMutex;
std::map<jlong, std::shared_ptr<Peer>> registry;
jlong nextHandle = 1;

std::shared_ptr<Peer> findPeer(jlong handle)
{
    std::lock_guard<std::mutex> lock(registryMutex);
    auto found = registry.find(handle);
    return found == registry.end() ? nullptr : found->second;
}

std::string text(JNIEnv *env, jstring value, size_t limit)
{
    if (value == nullptr || env->GetStringUTFLength(value) > static_cast<jsize>(limit))
    {
        throw std::invalid_argument("invalid signaling text");
    }
    const char *bytes = env->GetStringUTFChars(value, nullptr);
    if (bytes == nullptr)
    {
        throw std::runtime_error("signaling allocation failed");
    }
    std::string result(bytes);
    env->ReleaseStringUTFChars(value, bytes);
    return result;
}

void setChannel(const std::weak_ptr<Peer> &weak, std::shared_ptr<rtc::DataChannel> channel)
{
    auto peer = weak.lock();
    if (!peer || peer->closed || channel->label() != "testQQQ")
    {
        channel->close();
        return;
    }
    bool duplicate = false;
    {
        std::lock_guard<std::mutex> lock(peer->mutex);
        if (peer->audio != nullptr)
        {
            duplicate = true;
        }
        else
        {
            peer->audio = channel;
        }
    }
    if (duplicate)
    {
        channel->close();
        return;
    }
    channel->onOpen([weak]()
    {
        if (auto active = weak.lock())
        {
            active->push(3);
        }
    });
    channel->onClosed([weak]()
    {
        if (auto active = weak.lock())
        {
            active->push(5);
        }
    });
    channel->onError([weak](std::string)
    {
        if (auto active = weak.lock())
        {
            active->push(5);
        }
    });
    channel->onMessage([weak](rtc::binary bytes)
    {
        if (auto active = weak.lock())
        {
            if (bytes.size() <= 3869)
            {
                active->push(4, reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size());
            }
        }
    }, [weak](std::string)
    {
        if (auto active = weak.lock())
        {
            active->push(5);
        }
    });
    if (channel->isOpen())
    {
        peer->push(3);
    }
}
}

extern "C"
{
/**
 * @brief creates one bounded server data channel peer
 *
 * @param JNIEnv* env -> JNI environment
 * @param jclass type -> transport class
 * @param jstring host -> selected chat server host
 * @param jint port -> announced STUN port or zero
 *
 * @return jlong -> owned handle or zero
 */
JNIEXPORT jlong JNICALL Java_com_lemonchat_audio_WebRtcTransport_nativeCreate(JNIEnv *env, jclass type, jstring host, jint port)
{
    (void)type;
    try
    {
        if (port < 0 || port > 65535)
        {
            return 0;
        }
        auto peer = std::make_shared<Peer>();
        rtc::Configuration configuration;
        configuration.maxMessageSize = 3869;
        configuration.disableAutoNegotiation = true;
        if (port != 0)
        {
            std::string hostname = text(env, host, 255);
            if (hostname.empty())
            {
                return 0;
            }
            configuration.iceServers.emplace_back(hostname, static_cast<uint16_t>(port));
        }
        peer->connection = std::make_shared<rtc::PeerConnection>(configuration);
        std::weak_ptr<Peer> weak = peer;
        peer->connection->onLocalDescription([weak](rtc::Description description)
        {
            if (auto active = weak.lock())
            {
                active->push(1, std::string(description), description.typeString());
            }
        });
        peer->connection->onLocalCandidate([weak](rtc::Candidate candidate)
        {
            if (auto active = weak.lock())
            {
                active->push(2, candidate.candidate(), candidate.mid());
            }
        });
        peer->connection->onDataChannel([weak](std::shared_ptr<rtc::DataChannel> channel)
        {
            setChannel(weak, channel);
        });
        peer->connection->onStateChange([weak](rtc::PeerConnection::State state)
        {
            if (state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Closed || state == rtc::PeerConnection::State::Disconnected)
            {
                if (auto active = weak.lock())
                {
                    active->push(5);
                }
            }
        });
        std::lock_guard<std::mutex> lock(registryMutex);
        if (registry.size() >= 8 || nextHandle == INT64_MAX)
        {
            return 0;
        }
        jlong handle = nextHandle++;
        registry.emplace(handle, peer);
        return handle;
    }
    catch (...)
    {
        return 0;
    }
}

/**
 * @brief applies an authenticated server offer
 *
 * @param JNIEnv* env -> JNI environment
 * @param jclass type -> transport class
 * @param jlong handle -> current peer
 * @param jstring offer -> complete SDP offer
 *
 * @return jboolean -> whether the offer was accepted
 */
JNIEXPORT jboolean JNICALL Java_com_lemonchat_audio_WebRtcTransport_nativeOffer(JNIEnv *env, jclass type, jlong handle, jstring offer)
{
    (void)type;
    try
    {
        auto peer = findPeer(handle);
        if (!peer || peer->closed)
        {
            return JNI_FALSE;
        }
        peer->connection->setRemoteDescription(rtc::Description(text(env, offer, 65536), "offer"));
        peer->connection->setLocalDescription(rtc::Description::Type::Answer);
        return JNI_TRUE;
    }
    catch (...)
    {
        return JNI_FALSE;
    }
}

/**
 * @brief adds one authenticated server ICE candidate
 *
 * @param JNIEnv* env -> JNI environment
 * @param jclass type -> transport class
 * @param jlong handle -> current peer
 * @param jstring value -> candidate text
 * @param jstring mid -> media section
 *
 * @return jboolean -> whether the candidate was accepted
 */
JNIEXPORT jboolean JNICALL Java_com_lemonchat_audio_WebRtcTransport_nativeCandidate(JNIEnv *env, jclass type, jlong handle, jstring value, jstring mid)
{
    (void)type;
    try
    {
        auto peer = findPeer(handle);
        if (!peer || peer->closed)
        {
            return JNI_FALSE;
        }
        peer->connection->addRemoteCandidate(rtc::Candidate(text(env, value, 4096), text(env, mid, 128)));
        return JNI_TRUE;
    }
    catch (...)
    {
        return JNI_FALSE;
    }
}

/**
 * @brief takes one event without invoking Java from native worker threads
 *
 * @param JNIEnv* env -> JNI environment
 * @param jclass type -> transport class
 * @param jlong handle -> current peer
 *
 * @return jobjectArray -> event type and bounded payloads or null
 */
JNIEXPORT jobjectArray JNICALL Java_com_lemonchat_audio_WebRtcTransport_nativePoll(JNIEnv *env, jclass type, jlong handle)
{
    (void)type;
    try
    {
        auto peer = findPeer(handle);
        if (!peer || peer->closed)
        {
            return nullptr;
        }
        Event event;
        {
            std::lock_guard<std::mutex> lock(peer->mutex);
            if (peer->overflow)
            {
                peer->overflow = false;
                peer->events.clear();
                peer->queuedBytes = 0;
                event = Event{5, {}, {}};
            }
            else if (!peer->events.empty())
            {
                event = std::move(peer->events.front());
                peer->events.pop_front();
                peer->queuedBytes -= event.value.size() + event.detail.size();
            }
            else
            {
                return nullptr;
            }
        }
        jclass arrayClass = env->FindClass("[B");
        if (arrayClass == nullptr)
        {
            return nullptr;
        }
        jobjectArray result = env->NewObjectArray(3, arrayClass, nullptr);
        env->DeleteLocalRef(arrayClass);
        if (result == nullptr)
        {
            return nullptr;
        }
        const unsigned char *values[] = {&event.type, event.value.data(), reinterpret_cast<const unsigned char *>(event.detail.data())};
        size_t sizes[] = {1, event.value.size(), event.detail.size()};
        for (int index = 0; index < 3; index++)
        {
            jbyteArray bytes = env->NewByteArray(static_cast<jsize>(sizes[index]));
            if (bytes == nullptr)
            {
                return nullptr;
            }
            if (sizes[index] != 0)
            {
                env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(sizes[index]), reinterpret_cast<const jbyte *>(values[index]));
            }
            env->SetObjectArrayElement(result, index, bytes);
            env->DeleteLocalRef(bytes);
        }
        return result;
    }
    catch (...)
    {
        return nullptr;
    }
}

/**
 * @brief sends one bounded encrypted media frame
 *
 * @param JNIEnv* env -> JNI environment
 * @param jclass type -> transport class
 * @param jlong handle -> current peer
 * @param jbyteArray frame -> authenticated media bytes
 *
 * @return jboolean -> whether the channel accepted the frame
 */
JNIEXPORT jboolean JNICALL Java_com_lemonchat_audio_WebRtcTransport_nativeSend(JNIEnv *env, jclass type, jlong handle, jbyteArray frame)
{
    (void)type;
    try
    {
        auto peer = findPeer(handle);
        if (!peer || peer->closed || frame == nullptr || env->GetArrayLength(frame) > 3869)
        {
            return JNI_FALSE;
        }
        std::shared_ptr<rtc::DataChannel> audio;
        {
            std::lock_guard<std::mutex> lock(peer->mutex);
            audio = peer->audio;
        }
        if (!audio || !audio->isOpen() || audio->bufferedAmount() > 32768)
        {
            return JNI_FALSE;
        }
        rtc::binary bytes(env->GetArrayLength(frame));
        if (!bytes.empty())
        {
            env->GetByteArrayRegion(frame, 0, static_cast<jsize>(bytes.size()), reinterpret_cast<jbyte *>(bytes.data()));
        }
        if (env->ExceptionCheck())
        {
            return JNI_FALSE;
        }
        audio->send(std::move(bytes));
        return JNI_TRUE;
    }
    catch (...)
    {
        return JNI_FALSE;
    }
}

/**
 * @brief retires a handle before releasing its native callbacks
 *
 * @param JNIEnv* env -> JNI environment
 * @param jclass type -> transport class
 * @param jlong handle -> peer to close
 *
 * @return void
 */
JNIEXPORT void JNICALL Java_com_lemonchat_audio_WebRtcTransport_nativeClose(JNIEnv *env, jclass type, jlong handle)
{
    (void)env;
    (void)type;
    try
    {
        std::shared_ptr<Peer> peer;
        {
            std::lock_guard<std::mutex> lock(registryMutex);
            auto found = registry.find(handle);
            if (found == registry.end())
            {
                return;
            }
            peer = found->second;
            registry.erase(found);
        }
        peer->closed = true;
        peer->connection->resetCallbacks();
        peer->connection->close();
    }
    catch (...)
    {
    }
}
}
