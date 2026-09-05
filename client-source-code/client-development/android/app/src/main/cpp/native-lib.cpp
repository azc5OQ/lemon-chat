// Starts the embedded node runtime (nodejs-mobile's prebuilt libnode.so) on a thread owned by the
// foreground service.
//
// node::Start() BLOCKS until the node event loop exits, so the java side must call this from its own
// thread and treat a return as "node died".
//
// Node 18 has no console/stdout of its own on android - anything written to fd 1/2 goes nowhere - so
// a small pump thread copies both into logcat under the tag "lemonchat-node". Without it, every
// console.log from the client bundle is invisible, which makes the first bring-up impossible to debug.

#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>

#include "node.h"

#define LOG_TAG "lemonchat-node"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static int g_stdout_pipe[2];
static int g_stderr_pipe[2];
static pthread_t g_log_thread;
static bool g_logging_started = false;

static void* log_pump(void*)
{
    char buffer[1024];

    while (true)
    {
        ssize_t count = read(g_stdout_pipe[0], buffer, sizeof(buffer) - 1);

        if (count > 0)
        {
            // node writes its own newlines; strip a trailing one so logcat does not double-space
            if (buffer[count - 1] == '\n') { count--; }
            buffer[count] = 0;
            LOGI("%s", buffer);
            continue;
        }

        count = read(g_stderr_pipe[0], buffer, sizeof(buffer) - 1);

        if (count > 0)
        {
            if (buffer[count - 1] == '\n') { count--; }
            buffer[count] = 0;
            LOGE("%s", buffer);
            continue;
        }

        if (count <= 0)
        {
            break;
        }
    }

    return nullptr;
}

static void start_logging_if_needed()
{
    if (g_logging_started)
    {
        return;
    }

    g_logging_started = true;

    // line buffering, so a console.log shows up when it happens rather than when the buffer fills
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    if (pipe(g_stdout_pipe) != 0 || pipe(g_stderr_pipe) != 0)
    {
        LOGE("could not create log pipes, node output will be invisible");
        return;
    }

    dup2(g_stdout_pipe[1], STDOUT_FILENO);
    dup2(g_stderr_pipe[1], STDERR_FILENO);

    if (pthread_create(&g_log_thread, nullptr, log_pump, nullptr) != 0)
    {
        LOGE("could not start log pump thread");
        return;
    }

    pthread_detach(g_log_thread);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_lemonchat_NodeRuntime_nativeStartNode(JNIEnv* env, jobject /* this */, jobjectArray arguments)
{
    start_logging_if_needed();

    jsize argument_count = env->GetArrayLength(arguments);

    // node::Start wants a plain argv, and it may write to it, so give it writable copies that
    // outlive the call
    char** argv = static_cast<char**>(calloc(static_cast<size_t>(argument_count) + 1, sizeof(char*)));

    if (argv == nullptr)
    {
        LOGE("out of memory building argv");
        return -1;
    }

    for (jsize i = 0; i < argument_count; i++)
    {
        jstring argument = static_cast<jstring>(env->GetObjectArrayElement(arguments, i));
        const char* utf8 = env->GetStringUTFChars(argument, nullptr);

        argv[i] = strdup(utf8);

        env->ReleaseStringUTFChars(argument, utf8);
        env->DeleteLocalRef(argument);
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
