// This file starts the bundled stunnel program and renews its certificate when configured.
#include "stunnel.h"
#include <string.h>
#include "util.h"
#include "definitions.h"
#include "base.h"
#include "first_time_setup.h"
#include "clib/clib_memory.h"
#include "clib/clib_string.h"
#ifdef WIN32
#include <windows.h>
static HANDLE g_stunnel_job = NULL_POINTER;
#endif
#ifndef WIN32
#include <unistd.h> // fork, execl, access, readlink, _exit
#include <dirent.h> // opendir / readdir (Let's Encrypt cert detection)
#include <signal.h>
#include <sys/wait.h> // waitpid (certbot)
#include <sys/stat.h> // stat (certificate age for the days-of-validity-left estimate)
#ifdef __linux__
#include <sys/prctl.h> // PR_SET_PDEATHSIG (stunnel dies with the server); Linux-only, absent on macOS/BSD
#endif
#endif

static int g_stunnel_pid = 0;

// declarations
#ifdef WIN32
static boole _stunnel_internal__is_plain_domain(const char* domain);
static boole _stunnel_internal__run_and_wait(char* command_line, DWORD timeout_ms, DWORD* out_exit_code);
static void _stunnel_internal__terminate_stale_stunnel(const char* pid_path);
#endif

#ifdef WIN32
#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif

/**
 * @brief a domain name made of letters, digits, dots and dashes only: the one thing that may be placed on a command line built for another program
 *
 * @param const char* domain -> the value from the settings
 *
 * @return boole TRUE when every character is allowed and the string is not empty
 */
static boole _stunnel_internal__is_plain_domain(const char* domain)
{
    uint64 i = 0;
    char c = 0;

    if (domain == NULL_POINTER || domain[0] == 0)
    {
        return FALSE;
    }

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
 * @param DWORD timeout_ms -> after this long the program is terminated
 * @param DWORD* out_exit_code -> receives the exit code (1 when terminated)
 *
 * @return boole TRUE when the program could be started
 */
static boole _stunnel_internal__run_and_wait(char* command_line, DWORD timeout_ms, DWORD* out_exit_code)
{
    STARTUPINFOA startup_info;
    PROCESS_INFORMATION process_info;

    *out_exit_code = 1;
    ZeroMemory(&startup_info, sizeof(startup_info));
    startup_info.cb = sizeof(startup_info);
    ZeroMemory(&process_info, sizeof(process_info));

    if (CreateProcessA(NULL_POINTER, command_line, NULL_POINTER, NULL_POINTER, FALSE, 0, NULL_POINTER, NULL_POINTER, &startup_info, &process_info) == 0)
    {
        return FALSE;
    }

    if (WaitForSingleObject(process_info.hProcess, timeout_ms) == WAIT_TIMEOUT)
    {
        TerminateProcess(process_info.hProcess, 1);
        WaitForSingleObject(process_info.hProcess, 5000);
    }

    GetExitCodeProcess(process_info.hProcess, out_exit_code);
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);

    return TRUE;
}

/**
 * @brief terminates the stunnel a previous run of this server started, when it is still running
 *        the previous run left its child's pid and creation time in stunnel.pid; only a process that
 *        still has exactly that creation time is the same process (a pid alone gets reused by windows)
 *
 * @param const char* pid_path -> the stunnel.pid file next to the executable
 *
 * @return void
 */
static void _stunnel_internal__terminate_stale_stunnel(const char* pid_path)
{
    FILE* pid_file = 0;
    unsigned long old_pid = 0;
    unsigned long saved_creation_high = 0;
    unsigned long saved_creation_low = 0;
    HANDLE old_process = NULL_POINTER;
    FILETIME creation_time;
    FILETIME exit_time;
    FILETIME kernel_time;
    FILETIME user_time;

    pid_file = fopen(pid_path, "r");
    if (pid_file == NULL_POINTER)
    {
        return;
    }
    if (fscanf(pid_file, "%lu %lu %lu", &old_pid, &saved_creation_high, &saved_creation_low) != 3)
    {
        old_pid = 0;
    }
    fclose(pid_file);

    if (old_pid == 0)
    {
        return;
    }

    old_process = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, old_pid);
    if (old_process == NULL_POINTER)
    {
        return;
    }

    if (GetProcessTimes(old_process, &creation_time, &exit_time, &kernel_time, &user_time) != 0 && creation_time.dwHighDateTime == saved_creation_high && creation_time.dwLowDateTime == saved_creation_low)
    {
        printf("%s%lu%s", "terminating the stunnel left by a previous run (pid ", old_pid, ")\n");
        TerminateProcess(old_process, 1);
        WaitForSingleObject(old_process, 3000);
    }

    CloseHandle(old_process);
}
#endif

/**
 * @brief runs "certbot renew" for the configured domain before anything binds port 80, so the same
 *        standalone challenge that issued the certificate can also renew it. certbot checks the
 *        expiry itself and only acts when the certificate has under 30 days of validity left, so on
 *        most startups this returns in about a second without contacting Let's Encrypt at all.
 *
 * @attention failure is never fatal - the server keeps running with the existing certificate
 *
 * @return void
 */
void stunnel__renew_certificate_if_due(void)
{
#ifndef WIN32
    pid_t certbot_pid = 0;
    int certbot_status = 0;

    // only certbot-managed certificates can be renewed by certbot; manually entered cert paths
    // outside /etc/letsencrypt/live/ are the operator's own responsibility
    if (g_server_settings.is_stunnel_enabled == FALSE || g_server_settings.stunnel_domain[0] == 0 || strncmp(g_server_settings.stunnel_cert_fullchain, "/etc/letsencrypt/live/", 22) != 0)
    {
        return;
    }

    printf("checking whether the https/wss certificate needs renewal (certbot renew)...\n");

    certbot_pid = fork();
    if (certbot_pid == 0)
    {
        // exec certbot directly (no shell) so the stored domain cannot inject commands
        execlp("certbot", "certbot", "renew", "--cert-name", g_server_settings.stunnel_domain, "--non-interactive", (char*)NULL_POINTER);
        _exit(127); // exec failed -> certbot is not installed / not on PATH
    }
    else if (certbot_pid > 0)
    {
        waitpid(certbot_pid, &certbot_status, 0);
    }

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
    char certbot_path[MAX_PATH];
    char command_line[MAX_PATH + 600];
    DWORD found_length = 0;
    DWORD exit_code = 0;

    if (g_server_settings.is_stunnel_enabled == FALSE || g_server_settings.stunnel_domain[0] == 0 || (strstr(g_server_settings.stunnel_cert_fullchain, "\\live\\") == NULL_POINTER && strstr(g_server_settings.stunnel_cert_fullchain, "/live/") == NULL_POINTER))
    {
        return;
    }

    if (_stunnel_internal__is_plain_domain(g_server_settings.stunnel_domain) == FALSE)
    {
        printf("%s %s\n", g_mark_warn, "stunnel_domain holds characters other than letters, digits, dots and dashes - skipping the renewal check");
        return;
    }

    clib__null_memory(certbot_path, sizeof(certbot_path));
    found_length = SearchPathA(NULL_POINTER, "certbot", ".exe", (DWORD)sizeof(certbot_path), certbot_path, NULL_POINTER);
    if (found_length == 0 || found_length >= sizeof(certbot_path))
    {
        printf("%s %s\n", g_mark_warn, "certbot is not installed (or not on PATH) - skipping the renewal check. install it with: pip install certbot");
        return;
    }

    printf("checking whether the https/wss certificate needs renewal (certbot renew)...\n");

    clib__null_memory(command_line, sizeof(command_line));
    snprintf(command_line, sizeof(command_line), "\"%s\" renew --cert-name %s --non-interactive", certbot_path, g_server_settings.stunnel_domain);

    if (_stunnel_internal__run_and_wait(command_line, 180000, &exit_code) == FALSE)
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
 * @brief if wss is enabled, writes stunnel.conf next to the executable and launches
 *        the bundled stunnel so the server is reachable over wss as well as ws
 *
 * @attention the launched child is set to end with this server (PR_SET_PDEATHSIG)
 *
 * @return void
 */
void stunnel__launch(void)
{
#ifndef WIN32
    char exe_dir[600];
    char conf_path[700];
    char stunnel_path[700];
    char kill_old_stunnel_command[800];
    FILE* conf_file = 0;
    pid_t stunnel_pid = 0;

    clib__null_memory(exe_dir, sizeof(exe_dir));
    clib__null_memory(conf_path, sizeof(conf_path));
    clib__null_memory(stunnel_path, sizeof(stunnel_path));
    clib__null_memory(kill_old_stunnel_command, sizeof(kill_old_stunnel_command));

    if (g_server_settings.is_stunnel_enabled != TRUE)
    {
        return;
    }

    util__get_executable_directory(exe_dir, sizeof(exe_dir));
    if (exe_dir[0] == 0)
    {
        snprintf(exe_dir, sizeof(exe_dir), ".");
    }
    snprintf(conf_path, sizeof(conf_path), "%s/stunnel.conf", exe_dir);
    snprintf(stunnel_path, sizeof(stunnel_path), "%s/stunnel", exe_dir);

    conf_file = fopen(conf_path, "w");
    if (conf_file == NULL_POINTER)
    {
        printf("%s%s%s", "could not write ", conf_path, "; stunnel not started\n");
        return;
    }
    fprintf(conf_file, "[chatserver]\n");
    fprintf(conf_file, "accept = 0.0.0.0:%lld\n", g_server_settings.wss_port);
    fprintf(conf_file, "connect = 127.0.0.1:%lld\n", g_server_settings.websocket_port);
    fprintf(conf_file, "cert = %s\n", g_server_settings.stunnel_cert_fullchain);
    fprintf(conf_file, "key = %s\n", g_server_settings.stunnel_cert_privkey);
    // the patched-stunnel option key is still "xforwardedfor"; it injects the renamed X-Stunnel-Client-IP header
    fprintf(conf_file, "# inject the real client IP as the X-Stunnel-Client-IP header\n");
    fprintf(conf_file, "xforwardedfor = yes\n");

    if (g_server_settings.is_https_enabled == TRUE && g_server_settings.is_client_http_enabled == TRUE)
    {
        fprintf(conf_file, "\n[https]\n");
        fprintf(conf_file, "accept = 0.0.0.0:%lld\n", g_server_settings.https_port);
        fprintf(conf_file, "connect = 127.0.0.1:%lld\n", g_server_settings.http_port);
        fprintf(conf_file, "cert = %s\n", g_server_settings.stunnel_cert_fullchain);
        fprintf(conf_file, "key = %s\n", g_server_settings.stunnel_cert_privkey);
        fprintf(conf_file, "xforwardedfor = yes\n");
    }
    fclose(conf_file);

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

    // a previous run's stunnel may still hold the wss/https ports (e.g. the server was SIGKILLed so
    // PR_SET_PDEATHSIG never fired); kill the one launched with this same conf, then let the ports free
    snprintf(kill_old_stunnel_command, sizeof(kill_old_stunnel_command), "pkill -f '%s' 2>/dev/null", conf_path);
    system(kill_old_stunnel_command);
    usleep(300000);

    stunnel_pid = fork();
    if (stunnel_pid == 0)
    {
#ifdef __linux__
        // end with the parent server; Linux-only. on macOS/BSD the stale-stunnel pkill above
        // handles a leftover from a hard kill
        prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
        execl(stunnel_path, stunnel_path, conf_path, (char*)NULL_POINTER);
        _exit(127); // exec failed
    }
    else if (stunnel_pid > 0)
    {
        g_stunnel_pid = (int)stunnel_pid;
        printf("%s%d%s%lld%s%lld%s%s%s", "launched stunnel (pid ", g_stunnel_pid, "): wss 0.0.0.0:", g_server_settings.wss_port, " -> ws 127.0.0.1:", g_server_settings.websocket_port, " [domain ", g_server_settings.stunnel_domain, "]\n");
    }
    else
    {
        printf("%s", "fork() failed; stunnel not started\n");
    }
#else
    // windows: the console build tstunnel.exe is preferred (no tray icon, no window), the gui build
    // stunnel.exe otherwise. the child is placed in a job object that kills it when this process
    // ends, however it ends; stunnel.pid lets the next start remove a leftover from a hard kill
    char exe_dir[600];
    char conf_path[700];
    char log_path[700];
    char pid_path[700];
    char stunnel_path[700];
    char command_line[1500];
    FILE* conf_file = 0;
    FILE* pid_file = 0;
    STARTUPINFOA startup_info;
    PROCESS_INFORMATION process_info;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits;
    FILETIME creation_time;
    FILETIME exit_time;
    FILETIME kernel_time;
    FILETIME user_time;
    boole is_in_job = FALSE;

    if (g_server_settings.is_stunnel_enabled != TRUE)
    {
        return;
    }

    clib__null_memory(exe_dir, sizeof(exe_dir));
    util__get_executable_directory(exe_dir, sizeof(exe_dir));
    if (exe_dir[0] == 0)
    {
        snprintf(exe_dir, sizeof(exe_dir), ".");
    }
    snprintf(conf_path, sizeof(conf_path), "%s\\stunnel.conf", exe_dir);
    snprintf(log_path, sizeof(log_path), "%s\\stunnel.log", exe_dir);
    snprintf(pid_path, sizeof(pid_path), "%s\\stunnel.pid", exe_dir);
    snprintf(stunnel_path, sizeof(stunnel_path), "%s\\tstunnel.exe", exe_dir);
    if (access(stunnel_path, 0) != 0)
    {
        snprintf(stunnel_path, sizeof(stunnel_path), "%s\\stunnel.exe", exe_dir);
    }

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

    conf_file = fopen(conf_path, "w");
    if (conf_file == NULL_POINTER)
    {
        printf("%s%s%s", "could not write ", conf_path, "; stunnel not started\n");
        return;
    }
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

    if (g_server_settings.is_https_enabled == TRUE && g_server_settings.is_client_http_enabled == TRUE)
    {
        fprintf(conf_file, "\n[https]\n");
        fprintf(conf_file, "accept = 0.0.0.0:%lld\n", g_server_settings.https_port);
        fprintf(conf_file, "connect = 127.0.0.1:%lld\n", g_server_settings.http_port);
        fprintf(conf_file, "cert = %s\n", g_server_settings.stunnel_cert_fullchain);
        fprintf(conf_file, "key = %s\n", g_server_settings.stunnel_cert_privkey);
        fprintf(conf_file, "xforwardedfor = yes\n");
    }
    fclose(conf_file);

    _stunnel_internal__terminate_stale_stunnel(pid_path);

    snprintf(command_line, sizeof(command_line), "\"%s\" \"%s\"", stunnel_path, conf_path);
    ZeroMemory(&startup_info, sizeof(startup_info));
    startup_info.cb = sizeof(startup_info);
    ZeroMemory(&process_info, sizeof(process_info));

    if (CreateProcessA(NULL_POINTER, command_line, NULL_POINTER, NULL_POINTER, FALSE, CREATE_NO_WINDOW, NULL_POINTER, exe_dir, &startup_info, &process_info) == 0)
    {
        printf("%s%lu%s", "CreateProcess for stunnel failed (error ", (unsigned long)GetLastError(), "); stunnel not started\n");
        return;
    }

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
    if (g_stunnel_job != NULL_POINTER && AssignProcessToJobObject(g_stunnel_job, process_info.hProcess) != 0)
    {
        is_in_job = TRUE;
    }

    g_stunnel_pid = (int)process_info.dwProcessId;

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

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);

    printf("%s%d%s%lld%s%lld%s%s%s", "launched stunnel (pid ", g_stunnel_pid, "): wss 0.0.0.0:", g_server_settings.wss_port, " -> ws 127.0.0.1:", g_server_settings.websocket_port, " [domain ", g_server_settings.stunnel_domain, "]\n");
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
    return g_stunnel_pid > 0;
}
