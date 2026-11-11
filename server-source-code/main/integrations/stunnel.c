// This file starts the bundled stunnel program and renews its certificate when configured.
#include "stunnel.h"
#include <string.h>
#include "../util.h"
#include "../definitions.h"
#include "../base.h"
#include "../first_time_setup.h"
#include "../clib/clib.h"
#ifdef WIN32
#include <windows.h>
static HANDLE g_stunnel_job = NULL_POINTER;
#endif
#ifndef WIN32
#include <unistd.h> // fork, execl, access, readlink, _exit
#include <dirent.h> // opendir / readdir (Let's Encrypt cert detection)
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/wait.h> // waitpid (certbot)
#include <sys/stat.h> // stat (certificate age for the days-of-validity-left estimate)
#ifdef __linux__
#include <sys/prctl.h> // PR_SET_PDEATHSIG (stunnel dies with the server); Linux-only, absent on macOS/BSD
#endif
#endif

static int64 g_stunnel_pid = 0;

#ifndef WIN32
static int g_stunnel_owner_fd = -1; // Native pipe descriptor.
static pthread_mutex_t g_stunnel_owner_mutex = PTHREAD_MUTEX_INITIALIZER;
static boole g_stunnel_exit_registered = FALSE;

// Closing this pipe also works on non-Linux Unix: the supervisor sees EOF even
// after a hard server exit and remains alive long enough to stop and reap stunnel.
static void _stunnel_internal__release_owner(void)
{
    // close the owner pipe
    pthread_mutex_lock(&g_stunnel_owner_mutex);
    if (g_stunnel_owner_fd >= 0)
    {
        close(g_stunnel_owner_fd);
        g_stunnel_owner_fd = -1;
    }
    pthread_mutex_unlock(&g_stunnel_owner_mutex);
}

static pid_t _stunnel_internal__wait(pid_t child, int options)
{
    pid_t result = 0;

    // wait, repeat when interrupted
    do
    {
        result = waitpid(child, NULL_POINTER, options);
    } while (result < 0 && errno == EINTR);
    return result;
}

static void *_stunnel_internal__reap_supervisor(void *argument)
{
    pid_t child = (pid_t)(nint)argument;

    // wait for the supervisor
    _stunnel_internal__wait(child, 0);

    // reset pid and pipe
    pthread_mutex_lock(&g_stunnel_owner_mutex);
    if (g_stunnel_pid == child)
    {
        if (g_stunnel_owner_fd >= 0)
        {
            close(g_stunnel_owner_fd);
            g_stunnel_owner_fd = -1;
        }
        g_stunnel_pid = 0;
    }
    pthread_mutex_unlock(&g_stunnel_owner_mutex);
    return NULL_POINTER;
}

static boole _stunnel_internal__make_pipe(int descriptors[2])
{
    // create the pipe
    if (pipe(descriptors) != 0)
    {
        return FALSE;
    }

    // set the close-on-exec flag
    if (fcntl(descriptors[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(descriptors[1], F_SETFD, FD_CLOEXEC) < 0)
    {
        close(descriptors[0]);
        close(descriptors[1]);
        return FALSE;
    }
    return TRUE;
}

static int64 _stunnel_internal__read_status(int descriptor, int64 *status)
{
    int64 result = 0;

    // read, repeat when interrupted
    do
    {
        result = read(descriptor, status, sizeof(*status));
    } while (result < 0 && errno == EINTR);
    return result;
}

// Runs only in the supervisor process. No application locks are used after fork.
static void _stunnel_internal__supervise(const char *executable, const char *configuration, int owner, int started)
{
    int executed[2] = { -1, -1 }; // pipe writes native descriptors
    int64 error = 0;
    pid_t child = -1;
    pid_t supervisor = getpid();
    int64 received = 0;
    struct pollfd owner_poll = { 0 };
    struct timespec pause = { 0, 25000000 };
    uint64 attempt = 0;
    int64 ready = 0;
    boole success = FALSE;

    // a dead parent must not kill the supervisor while it reports startup
    signal(SIGPIPE, SIG_IGN);

    // create the exec pipe
    success = _stunnel_internal__make_pipe(executed);
    if (success == FALSE)
    {
        error = errno;
        (void)write(started, &error, sizeof(error));
        _exit(1);
    }

    // start the child process
    child = fork();
    if (child == 0)
    {
        // close unused descriptors
        close(executed[0]);
        close(owner);
        close(started);
#ifdef __linux__
        // also stop the foreground child if its supervisor itself disappears
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 || getppid() != supervisor)
        {
            error = ECHILD;
            (void)write(executed[1], &error, sizeof(error));
            _exit(127);
        }
#else
        (void)supervisor;
#endif

        // run the stunnel program
        execl(executable, executable, configuration, (char *)NULL_POINTER);
        error = errno;
        (void)write(executed[1], &error, sizeof(error));
        _exit(127);
    }

    // close the write end
    close(executed[1]);

    // report a failed fork
    if (child < 0)
    {
        error = errno;
        close(executed[0]);
        (void)write(started, &error, sizeof(error));
        _exit(1);
    }

    // check the exec result
    received = _stunnel_internal__read_status(executed[0], &error);
    close(executed[0]);
    if (received != 0)
    {
        error = error != 0 ? error : ECHILD;
        (void)write(started, &error, sizeof(error));
        kill(child, SIGKILL);
        _stunnel_internal__wait(child, 0);
        _exit(1);
    }

    // check the child process
    if (_stunnel_internal__wait(child, WNOHANG) != 0)
    {
        error = ECHILD;
        (void)write(started, &error, sizeof(error));
        _exit(1);
    }

    // report the successful start
    error = 0;
    (void)write(started, &error, sizeof(error));
    close(started);

    // wait until owner closes
    owner_poll.fd = owner;
    owner_poll.events = POLLIN;
    owner_poll.revents = 0;
    for (;;)
    {
        // stop when child ended
        if (_stunnel_internal__wait(child, WNOHANG) != 0)
        {
            close(owner);
            _exit(0);
        }

        // poll the owner pipe
        ready = poll(&owner_poll, 1, 250);
        if (ready > 0 || (ready < 0 && errno != EINTR))
        {
            break;
        }
    }

    // send SIGTERM to child
    close(owner);
    kill(child, SIGTERM);

    // wait for the child
    for (attempt = 0; attempt < 20; attempt++)
    {
        if (_stunnel_internal__wait(child, WNOHANG) != 0)
        {
            _exit(0);
        }
        nanosleep(&pause, NULL_POINTER);
    }

    // send SIGKILL to child
    kill(child, SIGKILL);
    _stunnel_internal__wait(child, 0);
    _exit(0);
}

static boole _stunnel_internal__launch_owned(const char *executable, const char *configuration)
{
    boole success = FALSE;
    int owner[2] = { -1, -1 }; // pipe writes native descriptors
    int started[2] = { -1, -1 }; // pipe writes native descriptors
    int64 status = 0;
    pid_t supervisor = 0;
    int64 received = 0;

    // lock the stunnel state
    pthread_mutex_lock(&g_stunnel_owner_mutex);

    // stunnel runs already
    if (g_stunnel_pid > 0)
    {
        pthread_mutex_unlock(&g_stunnel_owner_mutex);
        return TRUE;
    }

    // register the exit function
    if (g_stunnel_exit_registered == FALSE)
    {
        if (atexit(_stunnel_internal__release_owner) != 0)
        {
            pthread_mutex_unlock(&g_stunnel_owner_mutex);
            return FALSE;
        }
        g_stunnel_exit_registered = TRUE;
    }

    // create the owner pipe
    success = _stunnel_internal__make_pipe(owner);
    if (success == FALSE)
    {
        pthread_mutex_unlock(&g_stunnel_owner_mutex);
        return FALSE;
    }

    // create the start pipe
    success = _stunnel_internal__make_pipe(started);
    if (success == FALSE)
    {
        close(owner[0]);
        close(owner[1]);
        pthread_mutex_unlock(&g_stunnel_owner_mutex);
        return FALSE;
    }

    // start the supervisor process
    supervisor = fork();
    if (supervisor == 0)
    {
        close(owner[1]);
        close(started[0]);
        _stunnel_internal__supervise(executable, configuration, owner[0], started[1]);
        _exit(1);
    }

    // close the unused ends
    close(owner[0]);
    close(started[1]);

    // check the fork result
    if (supervisor < 0)
    {
        close(owner[1]);
        close(started[0]);
        pthread_mutex_unlock(&g_stunnel_owner_mutex);
        return FALSE;
    }

    // read the start status
    received = _stunnel_internal__read_status(started[0], &status);
    close(started[0]);
    if (received != sizeof(status) || status != 0)
    {
        close(owner[1]);
        _stunnel_internal__wait(supervisor, 0);
        pthread_mutex_unlock(&g_stunnel_owner_mutex);
        return FALSE;
    }

    // store pid and pipe
    g_stunnel_pid = supervisor;
    g_stunnel_owner_fd = owner[1];

    // start the waiting thread
    success = util__start_detached_thread(_stunnel_internal__reap_supervisor, (void *)(nint)supervisor);
    if (success == FALSE)
    {
        close(g_stunnel_owner_fd);
        g_stunnel_owner_fd = -1;
        _stunnel_internal__wait(supervisor, 0);
        g_stunnel_pid = 0;
        pthread_mutex_unlock(&g_stunnel_owner_mutex);
        return FALSE;
    }

    // unlock the stunnel state
    pthread_mutex_unlock(&g_stunnel_owner_mutex);
    return TRUE;
}
#endif

// declarations
#ifdef WIN32
static boole _stunnel_internal__is_plain_domain(const char *domain);
static boole _stunnel_internal__run_and_wait(char *command_line, uint64 timeout_ms, uint64 *out_exit_code);
static void _stunnel_internal__terminate_stale_stunnel(const char *pid_path);
#endif

#ifdef WIN32
#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif

/**
 * @brief a domain name made of letters, digits, dots and dashes only: the one thing that may be
 *        placed on a command line built for another program
 *
 * @param const char* domain -> the value from the settings
 *
 * @return boole TRUE when every character is allowed and the string is not empty
 */
static boole _stunnel_internal__is_plain_domain(const char *domain)
{
    uint64 i = 0;
    char c = 0;

    // refuse an empty domain
    if (domain == NULL_POINTER || domain[0] == 0)
    {
        return FALSE;
    }

    // check every character
    for (i = 0; domain[i] != 0; i++)
    {
        c = domain[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-')
        {
            continue;
        }
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief starts a program with the given command line, inheriting this console, and waits for it
 *
 * @param char* command_line -> the full command line (mutable, as CreateProcess wants it)
 * @param uint64 timeout_ms -> finite timeout below INFINITE milliseconds
 * @param uint64* out_exit_code -> receives the exit code (1 when terminated)
 *
 * @return boole TRUE when the program could be started
 */
static boole _stunnel_internal__run_and_wait(char *command_line, uint64 timeout_ms, uint64 *out_exit_code)
{
    STARTUPINFOA startup_info = { 0 };
    PROCESS_INFORMATION process_info = { 0 };
    DWORD exit_code = 1; // GetExitCodeProcess writes a DWORD

    // set default exit code
    *out_exit_code = 1;

    // check the timeout
    if (timeout_ms >= INFINITE)
    {
        return FALSE;
    }

    // prepare the start info
    ZeroMemory(&startup_info, sizeof(startup_info));
    startup_info.cb = sizeof(startup_info);
    ZeroMemory(&process_info, sizeof(process_info));

    // start the program
    if (CreateProcessA(NULL_POINTER, command_line, NULL_POINTER, NULL_POINTER, FALSE, 0, NULL_POINTER, NULL_POINTER, &startup_info, &process_info) == 0)
    {
        return FALSE;
    }

    // wait, stop after timeout
    if (WaitForSingleObject(process_info.hProcess, (DWORD)timeout_ms) == WAIT_TIMEOUT)
    {
        TerminateProcess(process_info.hProcess, 1);
        WaitForSingleObject(process_info.hProcess, 5000);
    }

    // read the exit code
    GetExitCodeProcess(process_info.hProcess, &exit_code);
    *out_exit_code = exit_code;

    // close the handles
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);

    return TRUE;
}

/**
 * @brief terminates the stunnel a previous run of this server started, when it is still running the
 *        previous run left its child's pid and creation time in stunnel.pid; only a process that
 *        still has exactly that creation time is the same process (a pid alone gets reused by
 *        windows)
 *
 * @param const char* pid_path -> the stunnel.pid file next to the executable
 *
 * @return void
 */
static void _stunnel_internal__terminate_stale_stunnel(const char *pid_path)
{
    FILE *pid_file = NULL_POINTER;
    uint64 old_pid = 0;
    uint64 saved_creation_high = 0;
    uint64 saved_creation_low = 0;
    HANDLE old_process = NULL_POINTER;
    FILETIME creation_time = { 0 };
    FILETIME exit_time = { 0 };
    FILETIME kernel_time = { 0 };
    FILETIME user_time = { 0 };

    // open the pid file
    pid_file = fopen(pid_path, "r");
    if (pid_file == NULL_POINTER)
    {
        return;
    }

    // read pid and time
    if (fscanf(pid_file, "%llu %llu %llu", &old_pid, &saved_creation_high, &saved_creation_low) != 3)
    {
        old_pid = 0;
    }
    fclose(pid_file);

    // check the saved values
    if (old_pid == 0 || old_pid > MAXDWORD || saved_creation_high > MAXDWORD || saved_creation_low > MAXDWORD)
    {
        return;
    }

    // open the old process
    old_process = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)old_pid);
    if (old_process == NULL_POINTER)
    {
        return;
    }

    // stop the matching process
    if (GetProcessTimes(old_process, &creation_time, &exit_time, &kernel_time, &user_time) != 0 && creation_time.dwHighDateTime == saved_creation_high && creation_time.dwLowDateTime == saved_creation_low)
    {
        printf("%s%llu%s", "terminating the stunnel left by a previous run (pid ", old_pid, ")\n");
        TerminateProcess(old_process, 1);
        WaitForSingleObject(old_process, 3000);
    }

    // close the handle
    CloseHandle(old_process);
}
#endif

/**
 * @brief Runs Certbot renewal for the configured domain before the listeners start.
 *
 * Certbot decides whether renewal is needed. Running it before the listeners start
 * leaves port 80 available for certificate validation.
 *
 * @attention failure is never fatal - the server keeps running with the existing certificate
 *
 * @return void
 */
void stunnel__renew_certificate_if_due(void)
{
#ifndef WIN32
    pid_t certbot_pid = 0;
    int certbot_status = 0; // waitpid writes an int

    // only certbot-managed certificates can be renewed by certbot; manually entered cert paths
    // outside /etc/letsencrypt/live/ are the operator's own responsibility
    if (g_server_settings.is_stunnel_enabled == FALSE || g_server_settings.stunnel_domain[0] == 0 || clib__strncmp(g_server_settings.stunnel_cert_fullchain, "/etc/letsencrypt/live/", 22) != 0)
    {
        return;
    }

    // report the renewal check
    printf("checking whether the https/wss certificate needs renewal (certbot renew)...\n");

    // run certbot and wait
    certbot_pid = fork();
    if (certbot_pid == 0)
    {
        // exec certbot directly (no shell) so the stored domain cannot inject commands
        execlp("certbot", "certbot", "renew", "--cert-name", g_server_settings.stunnel_domain, "--non-interactive", (char *)NULL_POINTER);
        _exit(127); // exec failed -> certbot is not installed / not on PATH
    }
    else if (certbot_pid > 0)
    {
        waitpid(certbot_pid, &certbot_status, 0);
    }

    // report certbot failures
    if (WIFEXITED(certbot_status) && WEXITSTATUS(certbot_status) == 127)
    {
        printf("%s %s\n", g_mark_warn, "certbot is not installed (or not on PATH) - skipping the renewal check");
    }
    else if (WIFEXITED(certbot_status) == 0 || WEXITSTATUS(certbot_status) != 0)
    {
        printf("%s %s\n", g_mark_warn, "certbot renew failed (see its output above) - continuing with the existing certificate");
    }
#else
    // windows: certbot is found on PATH (installed with pip, or the certbot installer) and run the same
    // way. only a certificate under a certbot "live" directory (C:\Certbot\live\<domain>\) is its to renew
    char certbot_path[MAX_PATH] = { 0 };
    char command_line[MAX_PATH + 600] = { 0 };
    uint64 found_length = 0;
    uint64 exit_code = 0;
    boole success = FALSE;

    // check the stunnel settings
    if (g_server_settings.is_stunnel_enabled == FALSE || g_server_settings.stunnel_domain[0] == 0 || (clib__strstr(g_server_settings.stunnel_cert_fullchain, "\\live\\") == NULL_POINTER && clib__strstr(g_server_settings.stunnel_cert_fullchain, "/live/") == NULL_POINTER))
    {
        return;
    }

    // check the domain characters
    success = _stunnel_internal__is_plain_domain(g_server_settings.stunnel_domain);
    if (success == FALSE)
    {
        printf("%s %s\n", g_mark_warn, "stunnel_domain holds characters other than letters, digits, dots and dashes - skipping the renewal check");
        return;
    }

    // find the certbot program
    clib__null_memory(certbot_path, sizeof(certbot_path));
    found_length = SearchPathA(NULL_POINTER, "certbot", ".exe", (DWORD)sizeof(certbot_path), certbot_path, NULL_POINTER);
    if (found_length == 0 || found_length >= sizeof(certbot_path))
    {
        printf("%s %s\n", g_mark_warn, "certbot is not installed (or not on PATH) - skipping the renewal check. install it with: pip install certbot");
        return;
    }

    // report the renewal check
    printf("checking whether the https/wss certificate needs renewal (certbot renew)...\n");

    // build the command line
    clib__null_memory(command_line, sizeof(command_line));
    clib__string_format(command_line, sizeof(command_line), "\"%s\" renew --cert-name %s --non-interactive", certbot_path, g_server_settings.stunnel_domain);

    // run certbot, report failures
    success = _stunnel_internal__run_and_wait(command_line, 180000, &exit_code);
    if (success == FALSE)
    {
        printf("%s %s\n", g_mark_warn, "certbot could not be started - skipping the renewal check");
    }
    else if (exit_code != 0)
    {
        printf("%s %s\n", g_mark_warn, "certbot renew failed (see its output above) - continuing with the existing certificate");
    }
#endif
}

/**
 * @brief if wss is enabled, writes stunnel.conf next to the executable and launches the bundled
 *        stunnel so the server is reachable over wss as well as ws
 *
 * @attention On Unix a supervisor owns the foreground child and watches this server's lifetime.
 *
 * @return void
 */
void stunnel__launch(void)
{
#ifndef WIN32
    char exe_dir[600] = { 0 };
    char conf_path[700] = { 0 };
    char stunnel_path[700] = { 0 };
    FILE *conf_file = NULL_POINTER;
    boole success = FALSE;

    // clear the path buffers
    clib__null_memory(exe_dir, sizeof(exe_dir));
    clib__null_memory(conf_path, sizeof(conf_path));
    clib__null_memory(stunnel_path, sizeof(stunnel_path));

    // stunnel is not enabled
    if (g_server_settings.is_stunnel_enabled != TRUE)
    {
        return;
    }

    // build the file paths
    util__get_executable_directory(exe_dir, sizeof(exe_dir));
    if (exe_dir[0] == 0)
    {
        clib__string_format(exe_dir, sizeof(exe_dir), ".");
    }
    clib__string_format(conf_path, sizeof(conf_path), "%s/stunnel.conf", exe_dir);
    clib__string_format(stunnel_path, sizeof(stunnel_path), "%s/stunnel", exe_dir);

    // open the configuration file
    conf_file = fopen(conf_path, "w");
    if (conf_file == NULL_POINTER)
    {
        printf("%s%s%s", "could not write ", conf_path, "; stunnel not started\n");
        return;
    }

    // keep the process under its supervisor instead of daemonizing into a new PID
    fprintf(conf_file, "foreground = quiet\n\n");
    fprintf(conf_file, "[chatserver]\n");
    fprintf(conf_file, "accept = 0.0.0.0:%lld\n", g_server_settings.wss_port);
    fprintf(conf_file, "connect = 127.0.0.1:%lld\n", g_server_settings.websocket_port);
    fprintf(conf_file, "cert = %s\n", g_server_settings.stunnel_cert_fullchain);
    fprintf(conf_file, "key = %s\n", g_server_settings.stunnel_cert_privkey);

    // the patched-stunnel option key is still "xforwardedfor"; it injects the renamed X-Stunnel-Client-IP header
    fprintf(conf_file, "# inject the real client IP as the X-Stunnel-Client-IP header\n");
    fprintf(conf_file, "xforwardedfor = yes\n");

    // add the https section
    if (g_server_settings.is_https_enabled == TRUE)
    {
        fprintf(conf_file, "\n[https]\n");
        fprintf(conf_file, "accept = 0.0.0.0:%lld\n", g_server_settings.https_port);
        fprintf(conf_file, "connect = 127.0.0.1:%lld\n", g_server_settings.http_port);
        fprintf(conf_file, "cert = %s\n", g_server_settings.stunnel_cert_fullchain);
        fprintf(conf_file, "key = %s\n", g_server_settings.stunnel_cert_privkey);
        fprintf(conf_file, "xforwardedfor = yes\n");
    }

    // close the file
    fclose(conf_file);

    // check the stunnel program
    if (access(stunnel_path, X_OK) != 0)
    {
        printf("%s%s%s", "stunnel binary not found / not executable at ", stunnel_path, "; not started\n");
        return;
    }

    // never launch stunnel without a usable cert + key, or it just crash-loops on startup
    if (access(g_server_settings.stunnel_cert_privkey, R_OK) != 0 || access(g_server_settings.stunnel_cert_fullchain, R_OK) != 0)
    {
        printf("%s%s%s", "wss disabled: TLS certificate not found at ", g_server_settings.stunnel_cert_privkey, "\n");
        printf("%s", "get one (certbot) and restart to enable wss; ws + http keep running normally.\n");
        g_server_settings.is_stunnel_enabled = FALSE;
        return;
    }

    // start stunnel, report result
    success = _stunnel_internal__launch_owned(stunnel_path, conf_path);
    if (success == TRUE)
    {
        printf("%s%lld%s%lld%s%s%s", "launched supervised stunnel: wss 0.0.0.0:", g_server_settings.wss_port, " -> ws 127.0.0.1:", g_server_settings.websocket_port, " [domain ", g_server_settings.stunnel_domain, "]\n");
    }
    else
    {
        printf("%s", "stunnel process could not be started and supervised\n");
    }

#else
    // windows: the console build tstunnel.exe is preferred (no tray icon, no window), the gui build
    // stunnel.exe otherwise. the child is placed in a job object that kills it when this process
    // ends, however it ends; stunnel.pid lets the next start remove a leftover from a hard kill
    char exe_dir[600] = { 0 };
    char conf_path[700] = { 0 };
    char log_path[700] = { 0 };
    char pid_path[700] = { 0 };
    char stunnel_path[700] = { 0 };
    char command_line[1500] = { 0 };
    FILE *conf_file = NULL_POINTER;
    FILE *pid_file = NULL_POINTER;
    STARTUPINFOA startup_info;
    PROCESS_INFORMATION process_info;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits;
    FILETIME creation_time;
    FILETIME exit_time;
    FILETIME kernel_time;
    FILETIME user_time;
    boole is_in_job = FALSE;

    // stunnel is not enabled
    if (g_server_settings.is_stunnel_enabled != TRUE)
    {
        return;
    }

    // build the file paths
    clib__null_memory(exe_dir, sizeof(exe_dir));
    util__get_executable_directory(exe_dir, sizeof(exe_dir));
    if (exe_dir[0] == 0)
    {
        clib__string_format(exe_dir, sizeof(exe_dir), ".");
    }
    clib__string_format(conf_path, sizeof(conf_path), "%s\\stunnel.conf", exe_dir);
    clib__string_format(log_path, sizeof(log_path), "%s\\stunnel.log", exe_dir);
    clib__string_format(pid_path, sizeof(pid_path), "%s\\stunnel.pid", exe_dir);

    // choose the stunnel program
    clib__string_format(stunnel_path, sizeof(stunnel_path), "%s\\tstunnel.exe", exe_dir);
    if (access(stunnel_path, 0) != 0)
    {
        clib__string_format(stunnel_path, sizeof(stunnel_path), "%s\\stunnel.exe", exe_dir);
    }

    // check the program exists
    if (access(stunnel_path, 0) != 0)
    {
        printf("%s%s%s", "stunnel binary not found at ", stunnel_path, "; not started (the windows build script produces it when Perl is installed)\n");
        return;
    }

    // never launch stunnel without a usable cert + key, or it just crash-loops on startup
    if (access(g_server_settings.stunnel_cert_privkey, R_OK) != 0 || access(g_server_settings.stunnel_cert_fullchain, R_OK) != 0)
    {
        printf("%s%s%s", "wss disabled: TLS certificate not found at ", g_server_settings.stunnel_cert_privkey, "\n");
        printf("%s", "get one (certbot certonly --standalone, or win-acme) and restart to enable wss; ws + http keep running normally.\n");
        g_server_settings.is_stunnel_enabled = FALSE;
        return;
    }

    // open the configuration file
    conf_file = fopen(conf_path, "w");
    if (conf_file == NULL_POINTER)
    {
        printf("%s%s%s", "could not write ", conf_path, "; stunnel not started\n");
        return;
    }

    // write the wss section
    fprintf(conf_file, "; written by the chat server at every start\n");
    fprintf(conf_file, "output = %s\n\n", log_path);
    fprintf(conf_file, "[chatserver]\n");
    fprintf(conf_file, "accept = 0.0.0.0:%lld\n", g_server_settings.wss_port);
    fprintf(conf_file, "connect = 127.0.0.1:%lld\n", g_server_settings.websocket_port);
    fprintf(conf_file, "cert = %s\n", g_server_settings.stunnel_cert_fullchain);
    fprintf(conf_file, "key = %s\n", g_server_settings.stunnel_cert_privkey);

    // the patched-stunnel option key is still "xforwardedfor"; it injects the renamed X-Stunnel-Client-IP header
    fprintf(conf_file, "; inject the real client IP as the X-Stunnel-Client-IP header\n");
    fprintf(conf_file, "xforwardedfor = yes\n");

    // add the https section
    if (g_server_settings.is_https_enabled == TRUE)
    {
        fprintf(conf_file, "\n[https]\n");
        fprintf(conf_file, "accept = 0.0.0.0:%lld\n", g_server_settings.https_port);
        fprintf(conf_file, "connect = 127.0.0.1:%lld\n", g_server_settings.http_port);
        fprintf(conf_file, "cert = %s\n", g_server_settings.stunnel_cert_fullchain);
        fprintf(conf_file, "key = %s\n", g_server_settings.stunnel_cert_privkey);
        fprintf(conf_file, "xforwardedfor = yes\n");
    }

    // close the file
    fclose(conf_file);

    // stop the previous stunnel
    _stunnel_internal__terminate_stale_stunnel(pid_path);

    // prepare the process start
    clib__string_format(command_line, sizeof(command_line), "\"%s\" \"%s\"", stunnel_path, conf_path);
    ZeroMemory(&startup_info, sizeof(startup_info));
    startup_info.cb = sizeof(startup_info);
    ZeroMemory(&process_info, sizeof(process_info));

    // start the stunnel process
    if (CreateProcessA(NULL_POINTER, command_line, NULL_POINTER, NULL_POINTER, FALSE, CREATE_NO_WINDOW, NULL_POINTER, exe_dir, &startup_info, &process_info) == 0)
    {
        printf("%s%lu%s", "CreateProcess for stunnel failed (error ", (unsigned long)GetLastError(), "); stunnel not started\n");
        return;
    }

    // create the job object
    if (g_stunnel_job == NULL_POINTER)
    {
        g_stunnel_job = CreateJobObjectA(NULL_POINTER, NULL_POINTER);
        if (g_stunnel_job != NULL_POINTER)
        {
            ZeroMemory(&job_limits, sizeof(job_limits));
            job_limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(g_stunnel_job, JobObjectExtendedLimitInformation, &job_limits, sizeof(job_limits));
        }
    }

    // assign process to job
    if (g_stunnel_job != NULL_POINTER && AssignProcessToJobObject(g_stunnel_job, process_info.hProcess) != 0)
    {
        is_in_job = TRUE;
    }

    // store the pid
    g_stunnel_pid = process_info.dwProcessId;

    // pid + creation time, so the next start can tell this very process from a reused pid
    pid_file = fopen(pid_path, "w");
    if (pid_file != NULL_POINTER)
    {
        if (GetProcessTimes(process_info.hProcess, &creation_time, &exit_time, &kernel_time, &user_time) != 0)
        {
            fprintf(pid_file, "%lu %lu %lu\n", (unsigned long)process_info.dwProcessId, (unsigned long)creation_time.dwHighDateTime, (unsigned long)creation_time.dwLowDateTime);
        }
        fclose(pid_file);
    }

    // close the handles
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);

    // report the start
    printf("%s%lld%s%lld%s%lld%s%s%s", "launched stunnel (pid ", g_stunnel_pid, "): wss 0.0.0.0:", g_server_settings.wss_port, " -> ws 127.0.0.1:", g_server_settings.websocket_port, " [domain ", g_server_settings.stunnel_domain, "]\n");
    if (is_in_job == FALSE)
    {
        printf("%s", "note: stunnel could not be tied to this process; if this server is killed, stunnel keeps running until the next start removes it\n");
    }
#endif
}

/**
 * @brief Reports whether the bundled stunnel process was started.
 * @return boole -> TRUE after stunnel was launched successfully
 */
boole stunnel__was_started(void)
{
    boole started = FALSE;

#ifndef WIN32
    pthread_mutex_lock(&g_stunnel_owner_mutex);
#endif
    started = g_stunnel_pid > 0;
#ifndef WIN32
    pthread_mutex_unlock(&g_stunnel_owner_mutex);
#endif
    return started;
}
