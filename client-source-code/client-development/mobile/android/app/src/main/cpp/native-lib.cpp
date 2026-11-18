// starts node on the service runtime thread and sends its output to logcat

#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <errno.h>

#include "node.h"

#define LOG_TAG "lemonchat-node"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static int g_stdout_pipe[2] = { -1, -1 };
static int g_stderr_pipe[2] = { -1, -1 };
static pthread_t g_log_thread;
static bool g_logging_started = false;

static void *log_pump(void *)
{
    char buffer[1024];
    pollfd streams[2] = { { g_stdout_pipe[0], POLLIN, 0 }, { g_stderr_pipe[0], POLLIN, 0 } };

    while (streams[0].fd >= 0 || streams[1].fd >= 0)
    {
        int ready = poll(streams, 2, -1);
        if (ready < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
        for (int i = 0; i < 2; i++)
        {
            if (streams[i].fd < 0 || !(streams[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
            {
                continue;
            }
            ssize_t count = read(streams[i].fd, buffer, sizeof(buffer) - 1);
            if (count > 0)
            {
                if (buffer[count - 1] == '\n')
                {
                    count--;
                }
                buffer[count] = 0;
                __android_log_print(i == 0 ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, LOG_TAG, "%s", buffer);
            }
            else if (count == 0 || errno != EINTR)
            {
                close(streams[i].fd);
                streams[i].fd = -1;
            }
        }
    }
    for (int i = 0; i < 2; i++)
    {
        if (streams[i].fd >= 0)
        {
            close(streams[i].fd);
        }
    }
    return nullptr;
}

static bool start_logging_if_needed()
{
    if (g_logging_started)
    {
        return true;
    }

    // line buffering, so a console.log shows up when it happens rather than when the buffer fills
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    int original_out = dup(STDOUT_FILENO);
    int original_error = dup(STDERR_FILENO);
    bool ready = false;
    if (original_out >= 0 && original_error >= 0 && pipe(g_stdout_pipe) == 0 && pipe(g_stderr_pipe) == 0)
    {
        if (dup2(g_stdout_pipe[1], STDOUT_FILENO) >= 0 && dup2(g_stderr_pipe[1], STDERR_FILENO) >= 0)
        {
            ready = pthread_create(&g_log_thread, nullptr, log_pump, nullptr) == 0;
        }
    }
    if (!ready)
    {
        if (original_out >= 0)
        {
            if (dup2(original_out, STDOUT_FILENO) < 0)
            {
                LOGE("could not restore stdout after log setup failure");
            }
        }
        if (original_error >= 0)
        {
            if (dup2(original_error, STDERR_FILENO) < 0)
            {
                LOGE("could not restore stderr after log setup failure");
            }
        }
    }
    for (int i = 0; i < 2; i++)
    {
        if (g_stdout_pipe[i] >= 0 && (!ready || i == 1))
        {
            close(g_stdout_pipe[i]);
            g_stdout_pipe[i] = -1;
        }
        if (g_stderr_pipe[i] >= 0 && (!ready || i == 1))
        {
            close(g_stderr_pipe[i]);
            g_stderr_pipe[i] = -1;
        }
    }
    if (original_out >= 0)
    {
        close(original_out);
    }
    if (original_error >= 0)
    {
        close(original_error);
    }
    if (ready)
    {
        pthread_detach(g_log_thread);
        g_logging_started = true;
    }
    return ready;
}

extern "C" JNIEXPORT jint JNICALL Java_com_lemonchat_NodeRuntime_nativeStartNode(JNIEnv *env, jobject /* this */, jobjectArray arguments)
{
    if (arguments == nullptr || !start_logging_if_needed())
    {
        return -1;
    }

    jsize argument_count = env->GetArrayLength(arguments);

    // node::Start wants a plain argv, and it may write to it, so give it writable copies that
    // outlive the call
    char **argv = static_cast<char **>(calloc(static_cast<size_t>(argument_count) + 1, sizeof(char *)));

    if (argv == nullptr)
    {
        LOGE("out of memory building argv");
        return -1;
    }

    for (jsize i = 0; i < argument_count; i++)
    {
        jstring argument = static_cast<jstring>(env->GetObjectArrayElement(arguments, i));
        const char *utf8 = argument != nullptr ? env->GetStringUTFChars(argument, nullptr) : nullptr;
        if (utf8 != nullptr)
        {
            argv[i] = strdup(utf8);
            env->ReleaseStringUTFChars(argument, utf8);
        }
        if (argument != nullptr)
        {
            env->DeleteLocalRef(argument);
        }
        if (argv[i] == nullptr || env->ExceptionCheck())
        {
            for (jsize allocated = 0; allocated <= i; allocated++)
            {
                free(argv[allocated]);
            }
            free(argv);
            return -1;
        }
    }

    LOGI("starting node with %d argument(s)", static_cast<int>(argument_count));

    // blocks until the event loop ends. the caller is on its own thread
    int exit_code = node::Start(static_cast<int>(argument_count), argv);

    LOGI("node exited with code %d", exit_code);

    for (jsize i = 0; i < argument_count; i++)
    {
        free(argv[i]);
    }
    free(argv);

    return exit_code;
}
