#include "definitions.h"
#include "util.h"
#include "metadata_crypto.h"

// This file asks the server owner how to host and configure Lemon Chat.
// It saves the answers when no valid server_settings.json exists yet.

#include "clib/clib_string.h"
#include "clib/clib_memory.h"
#include "../third-party/dave-g-json/cJSON.h"
#include "base.h"
#include "first_time_setup.h"
#include "core/server_settings.h"
#include "core/audio_channel.h"
#include "../third-party/rxi-log/log.h"
#include <string.h>

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h> // GetFullPathNameA, CreateProcessA (starting the tunnel setup)
#endif

#ifndef WIN32
#include <unistd.h> // fork, execlp, access, _exit, R_OK
#include <dirent.h> // opendir / readdir (Let's Encrypt cert detection)
#include <sys/wait.h> // waitpid (certbot, tunnel setup)
#include <termios.h> // tcgetattr / tcsetattr (hiding the admin password while it is typed)
#endif

// the tunnel setup of selfhosting-tools/wireguard, as a path from the working directory (the repository's buildresult)
#ifdef WIN32
static const char* g_tunnel_setup_script_path = "..\\selfhosting-tools\\wireguard\\run_setup.bat";
#else
static const char* g_tunnel_setup_script_path = "../selfhosting-tools/wireguard/setup.sh";
#endif

typedef enum
{
    HOSTING_DIRECT = 1,
    HOSTING_FUNNEL,
    HOSTING_CLOUDFLARE,
    HOSTING_NGROK,
    HOSTING_WIREGUARD,
    HOSTING_OTHER_PROXY
} first_time_setup_hosting_t;

static const char* g_hosting_names[] =
{
    "Direct hosting",
    "Tailscale Funnel",
    "Cloudflare Tunnel",
    "ngrok",
    "WireGuard through a VPS",
    "Another HTTPS/WSS proxy"
};

// declarations
static first_time_setup_hosting_t _first_time_setup_internal__ask_hosting(void);
static int64 _first_time_setup_internal__ask_port(const char* question, int64 default_port);
static boole _first_time_setup_internal__prompt_network_settings(first_time_setup_hosting_t hosting);
static boole _first_time_setup_internal__ask_yes_no(const char* question, boole default_answer);
static boole _first_time_setup_internal__ask_yes_no_with_note(const char* question, const char* note, boole default_answer);
static void _first_time_setup_internal__ask_admin_password(char* out_plaintext, uint64 out_capacity);
static int64 _first_time_setup_internal__scan_client_themes(char* client_html_path, char out_themes[][32], int64 max_themes);
static void _first_time_setup_internal__ask_certificate_paths(void);
static void _first_time_setup_internal__prompt_stunnel_setup(first_time_setup_hosting_t hosting);
static boole _first_time_setup_internal__is_tunnel_setup_available(void);
static void _first_time_setup_internal__run_tunnel_setup(void);
static void _first_time_setup_internal__reload_tunnel_settings(void);
static void _first_time_setup_internal__ask_wireguard_details(char* input, uint64 input_size);

/**
 * @brief asks one yes/no question and returns the answer
 *
 *        every question in the wizard goes through here so they all behave the same way. the
 *        prompt ends in (Y/n) or (y/N) to show which way a bare Enter goes, and "y", "yes", "n"
 *        and "no" are accepted in any capitalisation. anything else repeats the question instead
 *        of being silently counted as a no, which is what the old per-question "y" test did.
 *
 * @param const char* question -> the question text, without the trailing (y/n)
 * @param boole default_answer -> TRUE or FALSE; what a blank line means
 *
 * @return boole -> TRUE or FALSE
 */
static boole _first_time_setup_internal__ask_yes_no(const char* question, boole default_answer)
{
    return _first_time_setup_internal__ask_yes_no_with_note(question, NULL_POINTER, default_answer);
}

/**
 * @brief asks one yes/no question with a note printed under it, and returns the answer
 *
 *        layout: the question on its own line, the note under it, then the (Y/n) or (y/N) hint
 *        where the answer is typed. the note is printed on the first round only. without a note
 *        the question and the hint share one line.
 *
 * @param const char* question -> the question text, without the trailing (y/n)
 * @param const char* note -> lines printed under the question, each indented by four spaces and ended by a newline; 0 for none
 * @param boole default_answer -> TRUE or FALSE; what a blank line means
 *
 * @return boole -> TRUE or FALSE
 */
static boole _first_time_setup_internal__ask_yes_no_with_note(const char* question, const char* note, boole default_answer)
{
    char input[256];
    uint64 i = 0;
    boole is_note_printed = FALSE;

    while (TRUE)
    {
        clib__null_memory(input, sizeof(input));

        if (note != NULL_POINTER && is_note_printed == FALSE)
        {
            printf("%s %s\n%s%s %s", g_mark_ask, question, note, "   ", (default_answer == TRUE) ? "(Y/n): " : "(y/N): ");
            is_note_printed = TRUE;
        }
        else
        {
            printf("%s %s %s", g_mark_ask, question, (default_answer == TRUE) ? "(Y/n): " : "(y/N): ");
        }

        if (fgets(input, sizeof(input), stdin) == NULL_POINTER)
        {
            return default_answer;
        }

        clib__sanitize_stdin(input);

        // lowercase in place, so "Y", "Yes" and "YES" are one answer and not three
        for (i = 0; input[i] != 0; i++)
        {
            if ((input[i] >= 'A') && (input[i] <= 'Z'))
            {
                input[i] = (char)(input[i] + 32);
            }
        }

        if (input[0] == 0)
        {
            return default_answer;
        }
        if ((clib__is_string_equal(input, "y") == TRUE) || (clib__is_string_equal(input, "yes") == TRUE))
        {
            return TRUE;
        }
        if ((clib__is_string_equal(input, "n") == TRUE) || (clib__is_string_equal(input, "no") == TRUE))
        {
            return FALSE;
        }

        printf("%s %s\n", g_mark_warn, "Enter y or n, or press Enter to use the default.");
    }
}

/**
 * @brief Asks for the admin password twice and checks that both entries match.
 *
 * @param char* out_plaintext -> receives the accepted password
 * @param uint64 out_capacity -> size of out_plaintext in bytes
 *
 * @return void
 */
static void _first_time_setup_internal__ask_admin_password(char* out_plaintext, uint64 out_capacity)
{
    char first[256];
    char second[256];
#ifndef WIN32
    struct termios terminal_before;
    struct termios terminal_quiet;
    boole echo_is_off = FALSE;
#endif

    while (TRUE)
    {
        clib__null_memory(first, sizeof(first));
        clib__null_memory(second, sizeof(second));

#ifndef WIN32
        echo_is_off = FALSE;

        if (tcgetattr(STDIN_FILENO, &terminal_before) == 0)
        {
            terminal_quiet = terminal_before;
            terminal_quiet.c_lflag = terminal_quiet.c_lflag & ~((tcflag_t)ECHO);

            if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &terminal_quiet) == 0)
            {
                echo_is_off = TRUE;
            }
        }
#endif

        printf("%s %s", g_mark_ask, "Set the server admin password: ");
        fgets(first, sizeof(first), stdin);
        clib__sanitize_stdin(first);
        printf("\n");

        printf("%s %s", g_mark_ask, "Enter the admin password again: ");
        fgets(second, sizeof(second), stdin);
        clib__sanitize_stdin(second);
        printf("\n");

#ifndef WIN32
        if (echo_is_off == TRUE)
        {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &terminal_before);
        }
#endif

        if (first[0] == 0)
        {
            printf("%s %s\n", g_mark_warn, "The admin password cannot be empty.");
            continue;
        }

        if (clib__is_string_equal(first, second) == FALSE)
        {
            printf("%s %s\n", g_mark_warn, "The passwords do not match. Enter them again.");
            continue;
        }

        clib__copy_memory(first, out_plaintext, clib__utf8_string_length(first), out_capacity - 1);
        clib__null_memory(first, sizeof(first));
        clib__null_memory(second, sizeof(second));
        return;
    }
}

/**
 * @brief scans the served client.html for the selectable theme names written into it, so
 *        first-time setup can offer them as a default
 *
 *        reads the whole file into memory, blanks out every <!-- --> region first so a commented-out
 *        theme entry is never offered, then walks the remaining text collecting each
 *        choose-theme-item' data-name="X" value, skipping duplicates and names of 31 characters or
 *        more. yields nothing (0) when the file cannot be opened, is empty, or the buffer cannot be
 *        allocated.
 *
 * @param char* client_html_path -> path of the client.html to scan
 * @param char out_themes[][32] -> receives the theme names found, one null-terminated name per row
 * @param int64 max_themes -> number of rows available in out_themes; scanning stops collecting past it
 *
 * @return int64 -> how many theme names were written into out_themes
 */
static int64 _first_time_setup_internal__scan_client_themes(char* client_html_path, char out_themes[][32], int64 max_themes)
{
    FILE* file = NULL_POINTER;
    int64 file_size = 0;
    char* buffer = NULL_POINTER;
    uint64 bytes_read = 0;
    int64 count = 0;
    char* cursor = NULL_POINTER;
    char* found = NULL_POINTER;
    char* value_start = NULL_POINTER;
    char* value_end = NULL_POINTER;
    int64 length = 0;
    int64 j = 0;
    boole duplicate = FALSE;
    char* comment_open = NULL_POINTER;
    char* comment_close = NULL_POINTER;
    char* blank_p = NULL_POINTER;
    const char* marker = "choose-theme-item' data-name=\"";

    file = fopen(client_html_path, "rb");
    if (file == NULL_POINTER)
    {
        return 0;
    }

    fseek(file, 0, SEEK_END);
    file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (file_size <= 0)
    {
        fclose(file);
        return 0;
    }

    buffer = (char*)malloc(file_size + 1);
    if (buffer == NULL_POINTER)
    {
        fclose(file);
        return 0;
    }
    bytes_read = fread(buffer, 1, file_size, file);
    buffer[bytes_read] = 0;
    fclose(file);

    // blank out HTML comments first, so a commented-out theme entry is never offered
    cursor = buffer;
    for (;;)
    {
        comment_open = strstr(cursor, "<!--");
        if (comment_open == NULL_POINTER)
        {
            break;
        }
        comment_close = strstr(comment_open, "-->");
        if (comment_close == NULL_POINTER)
        {
            break;
        }
        for (blank_p = comment_open; blank_p < (comment_close + 3); blank_p++)
        {
            *blank_p = ' ';
        }
        cursor = comment_close + 3;
    }

    cursor = buffer;
    for (;;)
    {
        found = strstr(cursor, marker);
        if (found == NULL_POINTER)
        {
            break;
        }

        value_start = found + strlen(marker);
        value_end = strchr(value_start, '"');
        if (value_end == NULL_POINTER)
        {
            break;
        }

        length = (int64)(value_end - value_start);
        if ((length > 0) && (length < 31) && (count < max_themes))
        {
            duplicate = FALSE;
            for (j = 0; j < count; j++)
            {
                if ((strncmp(out_themes[j], value_start, length) == 0) && (out_themes[j][length] == 0))
                {
                    duplicate = TRUE;
                    break;
                }
            }

            if (duplicate == FALSE)
            {
                clib__null_memory(out_themes[count], 32);
                clib__copy_memory(value_start, out_themes[count], length, 31);
                count++;
            }
        }

        cursor = value_end;
    }

    free(buffer);
    return count;
}

/**
 * @brief Asks for existing certificate files without requiring a domain name.
 */
static void _first_time_setup_internal__ask_certificate_paths(void)
{
    clib__null_memory(g_server_settings.stunnel_domain, sizeof(g_server_settings.stunnel_domain));
    clib__null_memory(g_server_settings.stunnel_cert_fullchain, sizeof(g_server_settings.stunnel_cert_fullchain));
    clib__null_memory(g_server_settings.stunnel_cert_privkey, sizeof(g_server_settings.stunnel_cert_privkey));

    printf("%s %s\n", g_mark_info, "Use PEM files. The certificate must match the address clients connect to and be trusted by their browsers.");
    printf("%s %s\n", g_mark_info, "Renew these certificates separately. Lemon Chat will not request or renew them for this option.");
    printf("%s %s", g_mark_ask, "Path to the certificate chain file (fullchain.pem), without quotes: ");
    if (fgets(g_server_settings.stunnel_cert_fullchain, sizeof(g_server_settings.stunnel_cert_fullchain), stdin) == NULL_POINTER)
    {
        g_server_settings.is_stunnel_enabled = FALSE;
        return;
    }
    clib__sanitize_stdin(g_server_settings.stunnel_cert_fullchain);
    printf("%s %s", g_mark_ask, "Path to the private key file (privkey.pem), without quotes: ");
    if (fgets(g_server_settings.stunnel_cert_privkey, sizeof(g_server_settings.stunnel_cert_privkey), stdin) == NULL_POINTER)
    {
        g_server_settings.is_stunnel_enabled = FALSE;
        return;
    }
    clib__sanitize_stdin(g_server_settings.stunnel_cert_privkey);

    if (access(g_server_settings.stunnel_cert_fullchain, R_OK) != 0 || access(g_server_settings.stunnel_cert_privkey, R_OK) != 0)
    {
        printf("%s %s\n", g_mark_warn, "The certificate or private key file could not be read. Check the paths and file permissions. WSS remains disabled.");
        g_server_settings.is_stunnel_enabled = FALSE;
    }
}

/**
 * @brief Sets up WSS for the chosen hosting method.
 * @param first_time_setup_hosting_t hosting -> the selected hosting method
 */
static void _first_time_setup_internal__prompt_stunnel_setup(first_time_setup_hosting_t hosting)
{
    char input[600];
    int64 default_port = 1112;
#ifndef WIN32
    char found_domains[32][256];
    char domain[256];
    DIR* cert_dir = 0;
    struct dirent* entry = 0;
    uint64 found_count = 0;
    uint64 i = 0;
    int64 selection = 0;
    pid_t certbot_pid = 0;
    int status = 0; // waitpid writes an int through &status
    char probe[700];

    clib__null_memory(input, sizeof(input));
    clib__null_memory(found_domains, sizeof(found_domains));
    clib__null_memory(domain, sizeof(domain));

#else
    char probe_fullchain[600];
    char probe_privkey[600];

    clib__null_memory(input, sizeof(input));
#endif

    g_server_settings.is_stunnel_enabled = FALSE;
    g_server_settings.is_external_wss_enabled = (hosting == HOSTING_FUNNEL || hosting == HOSTING_CLOUDFLARE || hosting == HOSTING_NGROK || hosting == HOSTING_OTHER_PROXY);
    if (g_server_settings.is_external_wss_enabled == FALSE)
    {
        if (_first_time_setup_internal__ask_yes_no("Use stunnel to accept secure WebSocket (WSS) connections?", FALSE) == FALSE)
        {
            g_server_settings.wss_port = 0;
            return;
        }
        g_server_settings.is_stunnel_enabled = TRUE;
    }
    else
    {
        default_port = (hosting == HOSTING_FUNNEL) ? 8443 : 443;
    }

    while (TRUE)
    {
        g_server_settings.wss_port = _first_time_setup_internal__ask_port(
            (g_server_settings.is_external_wss_enabled == TRUE) ? "Public WSS port" : "WSS port", default_port);
        if (hosting == HOSTING_FUNNEL && g_server_settings.wss_port != 8443 && g_server_settings.wss_port != 10000)
        {
            printf("%s %s\n", g_mark_warn, "Choose 8443 or 10000 for WSS; this setup uses public port 443 for the HTTPS page.");
            continue;
        }
        if (g_server_settings.is_external_wss_enabled == FALSE && g_server_settings.wss_port == g_server_settings.websocket_port)
        {
            printf("%s %s\n", g_mark_warn, "That port is already used by plain WebSocket. Choose another.");
            continue;
        }
        break;
    }

    if (g_server_settings.is_external_wss_enabled == TRUE)
    {
        printf("%s %s%lld%s%lld%s\n", g_mark_info, "Forward public WSS port ", g_server_settings.wss_port, " to local WebSocket port ", g_server_settings.websocket_port, ".");
        return;
    }

    if (_first_time_setup_internal__ask_yes_no("Use certificate and private key files you already have?", TRUE) == TRUE)
    {
        _first_time_setup_internal__ask_certificate_paths();
        return;
    }

#ifndef WIN32
    // detect existing Let's Encrypt certificates
    cert_dir = opendir("/etc/letsencrypt/live");
    if (cert_dir != NULL_POINTER)
    {
        while ((entry = readdir(cert_dir)) != NULL_POINTER && found_count < 32)
        {
            clib__null_memory(probe, sizeof(probe));
            if (entry->d_name[0] == '.')
            {
                continue;
            }
            snprintf(probe, sizeof(probe), "/etc/letsencrypt/live/%s/fullchain.pem", entry->d_name);
            if (access(probe, R_OK) == 0)
            {
                snprintf(probe, sizeof(probe), "/etc/letsencrypt/live/%s/privkey.pem", entry->d_name);
                if (access(probe, R_OK) == 0)
                {
                    snprintf(found_domains[found_count], 256, "%s", entry->d_name);
                    found_count++;
                }
            }
        }
        closedir(cert_dir);
    }

    if (found_count > 0)
    {
        printf("%s", "Found existing Let's Encrypt certificate(s) under /etc/letsencrypt/live/:\n");
        for (i = 0; i < found_count; i++)
        {
            printf("%s%llu%s%s%s", "  [", (i + 1), "] ", found_domains[i], "\n");
        }
        printf("%s", "Certificate number, or 'm' to enter file paths: ");
        fgets(input, sizeof(input), stdin);
        clib__sanitize_stdin(input);
        selection = atoi(input);
        if (selection >= 1 && (uint64)selection <= found_count)
        {
            snprintf(g_server_settings.stunnel_domain, 256, "%s", found_domains[selection - 1]);
            snprintf(g_server_settings.stunnel_cert_fullchain, 512, "/etc/letsencrypt/live/%s/fullchain.pem", found_domains[selection - 1]);
            snprintf(g_server_settings.stunnel_cert_privkey, 512, "/etc/letsencrypt/live/%s/privkey.pem", found_domains[selection - 1]);
            return;
        }
    }
    else
    {
        printf("%s %s\n", g_mark_info, "No certificate found under /etc/letsencrypt/live/.");
        if (_first_time_setup_internal__ask_yes_no_with_note("Request a Let's Encrypt certificate using Certbot?",
                                                             "    Your domain's DNS A record must point to this server's public IPv4 address.\n"
                                                             "    TCP port 80 must be reachable from the internet and not used by another program.\n",
                                                             FALSE) == FALSE)
        {
            printf("%s", "No certificate was selected. WSS remains disabled.\n");
            g_server_settings.is_stunnel_enabled = FALSE;
            return;
        }

        printf("%s", "Domain name for the certificate (for example chat.example.com): ");
        if (fgets(domain, sizeof(domain), stdin) == NULL_POINTER)
        {
            g_server_settings.is_stunnel_enabled = FALSE;
            return;
        }
        clib__sanitize_stdin(domain);
        if (domain[0] == 0)
        {
            g_server_settings.is_stunnel_enabled = FALSE;
            return;
        }

        printf("%s", "Running Certbot to request the certificate.\n");
        printf("%s", "Certbot temporarily uses TCP port 80 to verify the domain.\n");

        status = 0;
        certbot_pid = fork();
        if (certbot_pid == 0)
        {
            // exec certbot directly (no shell) so the domain cannot inject commands
            execlp("certbot", "certbot", "certonly", "--standalone", "-d", domain, (char*)NULL_POINTER);
            _exit(127); // exec failed -> certbot is not installed / not on PATH
        }
        else if (certbot_pid > 0)
        {
            if (waitpid(certbot_pid, &status, 0) != certbot_pid)
            {
                printf("%s %s\n", g_mark_warn, "Could not wait for certbot; wss stays off.");
                g_server_settings.is_stunnel_enabled = FALSE;
                return;
            }
        }
        else
        {
            printf("%s %s\n", g_mark_warn, "Could not start certbot; wss stays off.");
            g_server_settings.is_stunnel_enabled = FALSE;
            return;
        }

        // we do NOT auto-install certbot (too distro-specific); detect its absence and instruct
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
        {
            printf("%s", "certbot is not installed (or not on PATH). Install it, then re-run this setup:\n");
            printf("%s", "  Debian/Ubuntu : apt install certbot\n");
            printf("%s", "  Fedora/RHEL   : dnf install certbot\n");
            printf("%s", "  Arch          : pacman -S certbot\n");
            printf("%s", "  Gentoo        : emerge app-crypt/certbot\n");
            printf("%s", "  other         : snap install --classic certbot   (then put it on PATH)\n");
            g_server_settings.is_stunnel_enabled = FALSE;
            return;
        }
        if (WIFEXITED(status) == 0 || WEXITSTATUS(status) != 0)
        {
            printf("%s %s\n", g_mark_warn, "Certbot failed; wss stays off. See its output above.");
            g_server_settings.is_stunnel_enabled = FALSE;
            return;
        }

        snprintf(g_server_settings.stunnel_domain, 256, "%s", domain);
        snprintf(g_server_settings.stunnel_cert_fullchain, 512, "/etc/letsencrypt/live/%s/fullchain.pem", domain);
        snprintf(g_server_settings.stunnel_cert_privkey, 512, "/etc/letsencrypt/live/%s/privkey.pem", domain);

        // only enable wss if certbot actually produced the cert + key
        if (access(g_server_settings.stunnel_cert_privkey, R_OK) != 0 || access(g_server_settings.stunnel_cert_fullchain, R_OK) != 0)
        {
            printf("%s", "certbot did not produce a certificate (see its output above - usually the domain\n");
            printf("%s", "not pointing here yet, or port 80 blocked/in use). wss stays off; fix and re-run.\n");
            g_server_settings.is_stunnel_enabled = FALSE;
        }
        return;
    }

    _first_time_setup_internal__ask_certificate_paths();
#else
    printf("%s %s\n", g_mark_info, "Enter a domain to find an existing Certbot certificate under C:\\Certbot\\live\\.");
    printf("%s %s\n", g_mark_info, "Leave it blank to enter certificate files instead. This setup does not request a Windows certificate.");

    while (TRUE)
    {
        clib__null_memory(g_server_settings.stunnel_domain, sizeof(g_server_settings.stunnel_domain));
        printf("%s %s", g_mark_ask, "Domain name to look up in C:\\Certbot\\live (Enter to provide file paths instead): ");
        if (fgets(g_server_settings.stunnel_domain, sizeof(g_server_settings.stunnel_domain), stdin) == NULL_POINTER)
        {
            break;
        }
        clib__sanitize_stdin(g_server_settings.stunnel_domain);
        if (g_server_settings.stunnel_domain[0] == 0)
        {
            _first_time_setup_internal__ask_certificate_paths();
            return;
        }
        break;
    }

    // certbot's default location on windows
    clib__null_memory(probe_fullchain, sizeof(probe_fullchain));
    clib__null_memory(probe_privkey, sizeof(probe_privkey));
    snprintf(probe_fullchain, sizeof(probe_fullchain), "C:\\Certbot\\live\\%s\\fullchain.pem", g_server_settings.stunnel_domain);
    snprintf(probe_privkey, sizeof(probe_privkey), "C:\\Certbot\\live\\%s\\privkey.pem", g_server_settings.stunnel_domain);

    if (access(probe_fullchain, R_OK) == 0 && access(probe_privkey, R_OK) == 0)
    {
        snprintf(g_server_settings.stunnel_cert_fullchain, 512, "%s", probe_fullchain);
        snprintf(g_server_settings.stunnel_cert_privkey, 512, "%s", probe_privkey);
        printf("%s %s%s\n", g_mark_ok, "Found the Certbot certificate under C:\\Certbot\\live\\", g_server_settings.stunnel_domain);
        return;
    }

    printf("%s %s\n", g_mark_info, "No certificate found for that domain; enter existing certificate files instead.");
    _first_time_setup_internal__ask_certificate_paths();
#endif
}

/**
 * @brief Asks how clients will reach the server before asking for network settings.
 * @return first_time_setup_hosting_t -> the chosen hosting method
 */
static first_time_setup_hosting_t _first_time_setup_internal__ask_hosting(void)
{
    char input[64];
    char* end = NULL_POINTER;
    int64 choice = HOSTING_DIRECT;

    printf("    1) Host directly (LAN or VPS)\n");
    printf("    2) Tailscale Funnel\n");
    printf("    3) Cloudflare Tunnel\n");
    printf("    4) ngrok\n");
    printf("    5) WireGuard through a VPS (forwards TCP and UDP to this server)\n");
    printf("    6) Another HTTPS/WSS proxy\n");
    while (TRUE)
    {
        printf("%s %s", g_mark_ask, "How do you want to host this server? [1]: ");
        if (fgets(input, sizeof(input), stdin) == NULL_POINTER)
        {
            return HOSTING_DIRECT;
        }
        clib__sanitize_stdin(input);
        if (input[0] == 0)
        {
            return HOSTING_DIRECT;
        }
        choice = strtol(input, &end, 10);
        if (end != input && *end == 0 && choice >= HOSTING_DIRECT && choice <= HOSTING_OTHER_PROXY)
        {
            return (first_time_setup_hosting_t)choice;
        }
        printf("%s %s\n", g_mark_warn, "Enter a number from 1 to 6.");
    }
}

/**
 * @brief Asks for a port and repeats the question if the answer is not a valid port number.
 * @param const char* question -> the name of the port being configured
 * @param int64 default_port -> the port used when Enter is pressed
 * @return int64 -> a port from 1 to 65535
 */
static int64 _first_time_setup_internal__ask_port(const char* question, int64 default_port)
{
    char input[64];
    char* end = NULL_POINTER;
    int64 port = 0;

    while (TRUE)
    {
        printf("%s %s [%lld]: ", g_mark_ask, question, default_port);
        if (fgets(input, sizeof(input), stdin) == NULL_POINTER)
        {
            return default_port;
        }
        clib__sanitize_stdin(input);
        if (input[0] == 0)
        {
            return default_port;
        }
        port = strtol(input, &end, 10);
        if (end != input && *end == 0 && port >= 1 && port <= 65535)
        {
            return port;
        }
        printf("%s %s\n", g_mark_warn, "Enter a whole port number from 1 to 65535.");
    }
}

/**
 * @brief Asks only the network questions needed for the chosen hosting method.
 * @param first_time_setup_hosting_t hosting -> the selected hosting method
 * @return boole -> TRUE when the WireGuard setup should run after saving the settings
 */
static boole _first_time_setup_internal__prompt_network_settings(first_time_setup_hosting_t hosting)
{
    char input[256];
    char client_themes[16][32];
    char theme_scan_path[600];
    uint64 i = 0;
    int64 client_theme_count = 0;
    int64 theme_choice = 0;
    boole is_tunnel_setup_requested = FALSE;
    boole is_same_public_hostname = TRUE;
    boole is_http_tunnel = (hosting == HOSTING_FUNNEL || hosting == HOSTING_CLOUDFLARE || hosting == HOSTING_NGROK);

    printf("%s Hosting: %s\n", g_mark_ok, g_hosting_names[hosting - 1]);
    if (is_http_tunnel == TRUE || hosting == HOSTING_OTHER_PROXY)
    {
        printf("%s %s\n", g_mark_info, "Configure the forwarding program separately; see selfhosting-tools/README.md.");
    }
    g_server_settings.websocket_port = _first_time_setup_internal__ask_port("WebSocket port", 1111);

    _first_time_setup_internal__prompt_stunnel_setup(hosting);

    if (hosting == HOSTING_FUNNEL)
    {
        printf("%s %s\n", g_mark_info, "The built-in HTTP server will serve client.html. The tunnel provides HTTPS.");
    }
    if (hosting == HOSTING_FUNNEL || _first_time_setup_internal__ask_yes_no("Use the built-in HTTP server to serve client.html to users?", is_http_tunnel) == TRUE)
    {
        g_server_settings.is_client_http_enabled = TRUE;

        while (TRUE)
        {
            g_server_settings.http_port = _first_time_setup_internal__ask_port("HTTP port", 80);
            if (g_server_settings.http_port == g_server_settings.websocket_port || (g_server_settings.is_stunnel_enabled == TRUE && g_server_settings.http_port == g_server_settings.wss_port))
            {
                printf("%s %s%lld%s\n", g_mark_warn, "port ", g_server_settings.http_port, " is already used (ws/wss) - pick another");
                continue;
            }
            break;
        }

        printf("%s %s%lld\n", g_mark_info, "HTTP port: ", g_server_settings.http_port);
        if (g_server_settings.is_external_wss_enabled == TRUE)
        {
            printf("%s %s%lld%s\n", g_mark_info, "Forward the public HTTPS page to local HTTP port ", g_server_settings.http_port, ".");
        }

        if (g_server_settings.is_stunnel_enabled == TRUE)
        {
            if (_first_time_setup_internal__ask_yes_no("Use stunnel to serve client.html over HTTPS?", FALSE) == TRUE)
            {
                g_server_settings.is_https_enabled = TRUE;
                while (TRUE)
                {
                    g_server_settings.https_port = _first_time_setup_internal__ask_port("HTTPS port", 443);
                    if (g_server_settings.https_port == g_server_settings.wss_port || g_server_settings.https_port == g_server_settings.http_port || g_server_settings.https_port == g_server_settings.websocket_port)
                    {
                        printf("%s %s%lld%s\n", g_mark_warn, "port ", g_server_settings.https_port, " is already used (ws/wss/http) - pick another");
                        continue;
                    }
                    break;
                }
                printf("%s %s%lld%s%lld%s\n", g_mark_ok, "HTTPS: serving the page over TLS on port ", g_server_settings.https_port, " (stunnel -> http ", g_server_settings.http_port, ")");
            }
        }

        // Automatic connection uses the page's hostname for the chat connection too.
        if (g_server_settings.is_external_wss_enabled == TRUE && hosting != HOSTING_FUNNEL)
        {
            is_same_public_hostname = _first_time_setup_internal__ask_yes_no("Will client.html and the WSS connection use the same public domain name?", FALSE);
        }
        if (is_same_public_hostname == TRUE && _first_time_setup_internal__ask_yes_no_with_note("Include the server connection details in client.html so it connects automatically?",
                                                                                              "    This includes the connection passwords. Anyone who opens the page receives them.\n",
                                                                                              FALSE) == TRUE)
        {
            g_server_settings.is_client_config_embedded = TRUE;
            printf("%s %s\n", g_mark_ok, "Automatic connection: enabled.");
        }
        else
        {
            printf("%s %s\n", g_mark_info, "Automatic connection: disabled. Users enter the server connection details themselves.");
        }

        // offer a default theme written into the served client.html, scanned from the page itself
        clib__null_memory(theme_scan_path, sizeof(theme_scan_path));
        if (g_server_settings.http_webroot[0] != 0)
        {
            snprintf(theme_scan_path, sizeof(theme_scan_path), "%s/client.html", g_server_settings.http_webroot);
        }
        else
        {
            snprintf(theme_scan_path, sizeof(theme_scan_path), "client.html");
        }

        client_theme_count = _first_time_setup_internal__scan_client_themes(theme_scan_path, client_themes, 16);
        if (client_theme_count > 0)
        {
            printf("%s %s\n", g_mark_info, "Themes available in client.html:");
            for (i = 0; i < (uint64)client_theme_count; i++)
            {
                printf("      %llu) %s\n", (unsigned long long)(i + 1), client_themes[i]);
            }

            clib__null_memory(input, sizeof(input));
            printf("%s %s", g_mark_ask, "Default theme number (Enter to keep the client's default): ");
            fgets(input, sizeof(input), stdin);
            clib__sanitize_stdin(input);
            theme_choice = atoi(input);
            if ((theme_choice >= 1) && (theme_choice <= client_theme_count))
            {
                clib__copy_memory(client_themes[theme_choice - 1], &g_server_settings.default_theme[0], clib__utf8_string_length(client_themes[theme_choice - 1]), 31);
                printf("%s %s%s\n", g_mark_ok, "default theme: ", g_server_settings.default_theme);
            }
        }
        else
        {
            printf("%s %s\n", g_mark_info, "No themes found in client.html. Clients will use their own default theme.");
        }

        if (_first_time_setup_internal__ask_yes_no_with_note("Save each user's identity in their browser?",
                                                             "    The browser reuses the same identity after the page is reloaded or reopened.\n",
                                                             FALSE) == TRUE)
        {
            g_server_settings.is_identity_localstorage_enabled = TRUE;
            printf("%s %s\n", g_mark_ok, "Saving browser identities: enabled.");
        }
    }
    clib__null_memory(input, sizeof(input));

    if (is_http_tunnel == TRUE)
    {
        g_server_settings.is_audio_over_websocket_allowed = TRUE;
        g_server_settings.is_audio_over_webrtc_allowed = FALSE;
        g_server_settings.is_same_ip_address_allowed = TRUE;
        printf("%s %s\n", g_mark_info, "Audio/video connections: WebSocket only for this hosting option.");
        printf("%s %s\n", g_mark_info, "Multiple clients from the same IP address: allowed.");
    }
    else
    {
        for (;;)
        {
            clib__null_memory(input, sizeof(input));
            printf("%s %s", g_mark_ask, "Connection type for audio and video: 1 = WebSocket, 2 = WebRTC DataChannel, 3 = both (clients choose) [3]: ");

            if (fgets(input, sizeof(input), stdin) == NULL_POINTER)
            {
                break;
            }
            clib__sanitize_stdin(input);

            if (input[0] == 0 || clib__is_string_equal(input, "3") == TRUE)
            {
                break; // both stay on, the default
            }
            if (clib__is_string_equal(input, "1") == TRUE)
            {
                g_server_settings.is_audio_over_webrtc_allowed = FALSE;
                break;
            }
            if (clib__is_string_equal(input, "2") == TRUE)
            {
                g_server_settings.is_audio_over_websocket_allowed = FALSE;
                break;
            }
            printf("%s %s\n", g_mark_warn, "Enter 1, 2 or 3.");
        }
        printf("%s %s%s\n", g_mark_ok, "audio/video connections: ", audio_channel__policy_value());
        clib__null_memory(input, sizeof(input));

        if (_first_time_setup_internal__ask_yes_no_with_note("Limit connections to one client per IP address?", "    For localhost testing with multiple clients, answer no.\n", FALSE) == TRUE)
        {
            g_server_settings.is_same_ip_address_allowed = FALSE;
            printf("%s %s\n", g_mark_ok, "Multiple clients from the same IP address: blocked.");
        }
    }

    if (hosting == HOSTING_WIREGUARD)
    {
        // the tunnel setup asks for the router itself and writes the address and the range, so
        // the two detail questions are asked only when it does not run
        if (_first_time_setup_internal__is_tunnel_setup_available() == TRUE)
        {
            printf("\n");
            is_tunnel_setup_requested = _first_time_setup_internal__ask_yes_no_with_note("Run the WireGuard setup script after saving these settings?",
                                                                                         "    Configures the VPS over SSH and installs WireGuard on this machine.\n"
                                                                                         "    Answer no if it is already configured; enter its address and UDP ports below.\n",
                                                                                         TRUE);
        }
        else
        {
            printf("%s %s\n", g_mark_info, "The WireGuard script is not available here. Enter the settings of your existing VPS below, or run selfhosting-tools/wireguard separately.");
        }

        if (is_tunnel_setup_requested == FALSE)
        {
            _first_time_setup_internal__ask_wireguard_details(input, sizeof(input));
        }
    }
    clib__null_memory(input, sizeof(input));

    return is_tunnel_setup_requested;
}

/**
 * @brief Asks for hosting, access and chat settings, then saves the answers.
 * @param char plaintext_keys[][256] -> storage for up to 100 connection passwords
 */
void first_time_setup__run(char plaintext_keys[][256])
{
    char input[256];
    char* end = NULL_POINTER;
    uint64 i = 0;
    long slots = 0;
    int64 requested_key_count = 0;
    boole is_tunnel_setup_requested = FALSE;
    first_time_setup_hosting_t hosting = HOSTING_DIRECT;

    printf("\n%s %s\n", g_mark_info, "First-time setup. Answers will be saved to server_settings.json.");
    printf("%s %s\n", "   ", "Press Enter to use the default: (Y/n) means yes; (y/N) means no.");

    printf("\n%s %s\n\n", g_mark_info, "1/4  Hosting and network connections.");
    hosting = _first_time_setup_internal__ask_hosting();
    is_tunnel_setup_requested = _first_time_setup_internal__prompt_network_settings(hosting);

    printf("\n%s %s\n\n", g_mark_info, "2/4  Passwords and server slots.");

    if (_first_time_setup_internal__ask_yes_no_with_note("Require connection passwords to join this server?",
                                                         "    Clients need all configured passwords, entered in the same order.\n",
                                                         FALSE) == TRUE)
    {
        clib__null_memory(input, sizeof(input));
        printf("%s %s", g_mark_ask, "Number of connection passwords required (1-100): ");
        fgets(input, sizeof(input), stdin);
        clib__sanitize_stdin(input);

        // clamp into range with a signed temporary: at least 1 (the operator already opted in) and
        // never past the 100-slot keys / plaintext_keys arrays. doing the comparison signed avoids a
        // negative atoi result wrapping to a huge uint64
        requested_key_count = atoi(input);
        if (requested_key_count < 1)
        {
            requested_key_count = 1;
        }
        if (requested_key_count > 100)
        {
            requested_key_count = 100;
        }
        g_server_settings.keys_count = requested_key_count;

        for (i = 0; i < g_server_settings.keys_count; i++)
        {
            clib__null_memory(input, sizeof(input));
            printf("%s%s%llu%s", g_mark_ask, " Connection password ", i + 1, ": ");
            fgets(input, sizeof(input), stdin);
            clib__sanitize_stdin(input);

            clib__copy_memory(input, &plaintext_keys[i][0], clib__utf8_string_length(input), 255);

            // Turn the entered password into an AES key. Store its type with
            // its bytes so the encryption code knows which method to use.
            if (!metadata_crypto__key_from_password(&g_server_settings.keys[i], CRYPTO_KEY_AES_256_CTR, input))
            {
                fprintf(stderr, "Failed to derive a configured encryption key.\n");
                exit(EXIT_FAILURE);
            }
        }

        if (g_server_settings.keys_count == 1)
        {
            printf("%s %s\n", g_mark_ok, "Connection password: set.");
        }
        else
        {
            printf("%s %llu %s\n", g_mark_ok, g_server_settings.keys_count, "connection passwords set; clients need all of them.");
        }
    }
    else
    {
        g_server_settings.keys_count = 0;
        printf("%s %s\n", g_mark_info, "No connection password is required. Connections still use encryption.");
    }

    clib__null_memory(input, sizeof(input));

    /* This setting is read only at startup, before any client storage exists. */
    g_server_settings.max_client_count = DEFAULT_SERVER_SLOTS;
    g_server_settings.max_channel_count = MAX_CHANNELS;
    for (;;)
    {
        printf("%s Maximum connected clients (1-%d) [%d]: ", g_mark_ask, MAX_SERVER_SLOTS, DEFAULT_SERVER_SLOTS);
        clib__null_memory(input, sizeof(input));
        if (fgets(input, sizeof(input), stdin) == NULL_POINTER)
        {
            break;
        }
        clib__sanitize_stdin(input);
        if (input[0] == 0)
        {
            break;
        }
        slots = strtol(input, &end, 10);
        if (end != input && *end == 0 && slots >= 1 && slots <= MAX_SERVER_SLOTS)
        {
            g_server_settings.max_client_count = (uint64)slots;
            break;
        }
        printf("%s Enter a whole number from 1 to %d.\n", g_mark_warn, MAX_SERVER_SLOTS);
    }

    _first_time_setup_internal__ask_admin_password(input, sizeof(input));
    util__hash_password_to_base64(input, &g_server_settings.admin_password[0], ADMIN_PASSWORD_MAX_LENGTH);
    g_server_settings.is_admin_password_initial = TRUE;
    // keep the plaintext only for this run's startup summary (shown once, then cleared)
    clib__copy_memory(input, &g_first_run_admin_password[0], clib__utf8_string_length(input), ADMIN_PASSWORD_MAX_LENGTH - 1);
    clib__null_memory(input, sizeof(input));

    printf("\n%s %s\n\n", g_mark_info, "3/4  Voice and server behaviour.");

    if (_first_time_setup_internal__ask_yes_no("Enable voice chat?", TRUE) == FALSE)
    {
        g_server_settings.is_voice_chat_active = FALSE;
        printf("%s %s\n", g_mark_off, "voice chat: off");
    }

#ifndef WIN32
    if (_first_time_setup_internal__ask_yes_no_with_note("Let start_server.sh restart the server after a crash?",
                                                         "    Applies the next time you launch through start_server.sh. Failures are recorded in crashes.txt.\n",
                                                         FALSE) == TRUE)
    {
        g_server_settings.is_restart_on_crash_enabled = TRUE;
        printf("%s %s\n", g_mark_ok, "Automatic restart through start_server.sh: enabled.");
    }
#endif

    printf("\n%s %s\n\n", g_mark_info, "4/4  Client profiles and chat features.");

    if (_first_time_setup_internal__ask_yes_no_with_note("Save client identities on the server?",
                                                         "    Clients keep their assigned tags when they reconnect with the same identity.\n",
                                                         TRUE) == FALSE)
    {
        g_server_settings.is_identity_storage_enabled = FALSE;
        printf("%s %s\n", g_mark_off, "identities: off");
        printf("%s %s\n", "   ", "Saved display names, the registered-client list, last-seen times and offline messages require saved identities. These options will be skipped.");
    }

    if (_first_time_setup_internal__ask_yes_no_with_note("Allow idle clients?", "    Useful for the Android app to wait for incoming calls in the background.\n", TRUE) == FALSE)
    {
        g_server_settings.is_idle_mode_allowed = FALSE;
        printf("%s %s\n", g_mark_off, "idle clients: off");
    }

    if (_first_time_setup_internal__ask_yes_no("Show country flags next to clients?", FALSE) == TRUE)
    {
        g_server_settings.is_display_country_flags_active = TRUE;
        printf("%s %s\n", g_mark_ok, "country flags: on");
    }

    if (_first_time_setup_internal__ask_yes_no("Allow users to upload avatar images?", FALSE) == TRUE)
    {
        g_server_settings.is_avatar_upload_allowed = TRUE;
        printf("%s %s\n", g_mark_ok, "avatars: on");

        printf("%s %s%lld%s\n", g_mark_ok, "max avatar size: ", (long long)(g_server_settings.avatar_max_size_bytes / 1024), " KB");
        printf("%s %s\n", g_mark_info, "You can change this limit in the server settings tab.");
    }
    clib__null_memory(input, sizeof(input));

    // aliases are attached to identities - without them there is nothing persistent to attach the name to
    if (g_server_settings.is_identity_storage_enabled == TRUE)
    {
        if (_first_time_setup_internal__ask_yes_no("Allow admins to assign saved display names to client identities?", FALSE) == TRUE)
        {
            g_server_settings.is_alias_registration_allowed = TRUE;
            printf("%s %s\n", g_mark_ok, "alias registrations: on");
        }
        else
        {
            printf("%s %s\n", "   ", "The registered-client list, last-seen times and offline messages require saved display names. These options will be skipped.");
        }

        // the stored-clients list is keyed and labelled by alias, so it is only useful with aliases on
        if (g_server_settings.is_alias_registration_allowed == TRUE)
        {
            if (_first_time_setup_internal__ask_yes_no("Allow users to view the registered-client list, including offline clients?", FALSE) == TRUE)
            {
                g_server_settings.is_stored_client_list_allowed = TRUE;
                printf("%s %s\n", g_mark_ok, "stored clients list: on");
            }
            else
            {
                printf("%s %s\n", "   ", "Last-seen times and offline messages require the registered-client list. These options will be skipped.");
            }

            // last seen only makes sense next to that list - it labels the offline entries
            if (g_server_settings.is_stored_client_list_allowed == TRUE)
            {
                if (_first_time_setup_internal__ask_yes_no("Show when registered clients were last online?", FALSE) == TRUE)
                {
                    g_server_settings.is_last_seen_enabled = TRUE;
                    printf("%s %s\n", g_mark_ok, "last seen: on");
                }

                if (_first_time_setup_internal__ask_yes_no_with_note("Allow users to send messages to registered clients who are offline?",
                                                                     "    The server keeps encrypted messages in RAM until the recipient reconnects.\n"
                                                                     "    Pending messages are lost when the server stops.\n"
                                                                     "    This option cannot be changed in the server settings tab.\n",
                                                                     FALSE) == TRUE)
                {
                    g_server_settings.is_offline_messaging_allowed = TRUE;
                    printf("%s %s\n", g_mark_ok, "offline messages: on");
                }
            }
        }
    }

    if (_first_time_setup_internal__ask_yes_no_with_note("Show when a user is typing a chat message?",
                                                         "    Shows a typing indicator. The unfinished message is not sent.\n",
                                                         FALSE) == TRUE)
    {
        g_server_settings.is_typing_indicator_allowed = TRUE;
        printf("%s %s\n", g_mark_ok, "typing indicator: on");
    }

    if (_first_time_setup_internal__ask_yes_no_with_note("Allow users to send files in chat?",
                                                         "    Controls file attachments, such as documents and archives. Pictures have a separate setting.\n"
                                                         "    Files are encrypted by the clients and relayed through server RAM, without being saved to disk.\n",
                                                         FALSE) == TRUE)
    {
        g_server_settings.is_file_upload_allowed = TRUE;
        printf("%s %s\n", g_mark_ok, "file uploads: on");

        printf("%s %s%lld%s\n", g_mark_ok, "max file size: ", (long long)(g_server_settings.file_upload_max_size_bytes / (1024 * 1024)), " MB");
        printf("%s %s\n", g_mark_info, "You can change this limit in the server settings tab.");
    }
    clib__null_memory(input, sizeof(input));

    // read the answers back before writing them: the file is only redone by deleting it, so a
    // wrong answer noticed here saves the operator that round trip
    printf("\n%s %s\n", g_mark_info, "Setup summary:");
    printf("      %s%s\n", "hosting .............. ", g_hosting_names[hosting - 1]);
    printf("      %s%lld\n", "websocket port ....... ", g_server_settings.websocket_port);
    if (g_server_settings.is_external_wss_enabled == TRUE)
    {
        printf("      %s%lld%s\n", "public wss port ...... ", g_server_settings.wss_port, " (provided by another program)");
    }
    printf("      %s%s\n", "connect password ..... ", (g_server_settings.keys_count > 0) ? "set" : "none");
    printf("      %s%s\n", "voice chat ........... ", (g_server_settings.is_voice_chat_active == TRUE) ? "on" : "off");
    printf("      %s%s\n", "audio connections .... ", audio_channel__policy_value());
    printf("      %s%s\n", "same-IP clients ...... ", (g_server_settings.is_same_ip_address_allowed == TRUE) ? "allowed" : "blocked");
    printf("      %s%lld%s%s\n", "min rsa key size ..... ", g_server_settings.minimum_rsa_key_bits, " bits", (g_server_settings.is_minimum_rsa_key_bits_announced == TRUE) ? " (announced to clients)" : " (not announced)");
#ifndef WIN32
    printf("      %s%s\n", "auto-restart ......... ", (g_server_settings.is_restart_on_crash_enabled == TRUE) ? "on" : "off");
#endif
    printf("      %s%s\n", "identities ........... ", (g_server_settings.is_identity_storage_enabled == TRUE) ? "on" : "off");
    printf("      %s%s\n", "idle clients ......... ", (g_server_settings.is_idle_mode_allowed == TRUE) ? "on" : "off");
    printf("      %s%s\n", "country flags ........ ", (g_server_settings.is_display_country_flags_active == TRUE) ? "on" : "off");
    printf("      %s%s\n", "avatars .............. ", (g_server_settings.is_avatar_upload_allowed == TRUE) ? "on" : "off");
    printf("      %s%s\n", "display names ........ ", (g_server_settings.is_alias_registration_allowed == TRUE) ? "on" : "off");
    printf("      %s%s\n", "people list .......... ", (g_server_settings.is_stored_client_list_allowed == TRUE) ? "on" : "off");
    printf("      %s%s\n", "last seen ............ ", (g_server_settings.is_last_seen_enabled == TRUE) ? "on" : "off");
    printf("      %s%s\n", "offline messages ..... ", (g_server_settings.is_offline_messaging_allowed == TRUE) ? "on" : "off");
    printf("      %s%s\n", "typing indicator ..... ", (g_server_settings.is_typing_indicator_allowed == TRUE) ? "on" : "off");
    printf("      %s%s\n", "client renames ....... ", (g_server_settings.is_client_rename_allowed == TRUE) ? "allowed" : "admin only");
    if (g_server_settings.is_file_upload_allowed == TRUE)
    {
        printf("      %s%s%lld%s\n", "file uploads ......... ", "on (max ", (long long)(g_server_settings.file_upload_max_size_bytes / (1024 * 1024)), " MB)");
    }
    else
    {
        printf("      %s%s\n", "file uploads ......... ", "off");
    }
    printf("      %s%s\n", "browser access ....... ", (g_server_settings.is_client_http_enabled == TRUE) ? "on" : "off");
    if (is_tunnel_setup_requested == TRUE)
    {
        printf("      %s%s\n\n", "wireguard mode ....... ", "set by the tunnel setup, which starts after the settings are saved");
    }
    else if (g_server_settings.is_wireguard_mode_active == TRUE)
    {
        printf("      %s%s%s%lld%s%lld\n\n", "wireguard mode ....... on, public address ", g_server_settings.webrtc_public_address, ", udp ", (long long)g_server_settings.webrtc_port_range_begin, "-", (long long)g_server_settings.webrtc_port_range_end);
    }
    else
    {
        printf("      %s%s\n\n", "wireguard mode ....... ", "off");
    }

    settings__save_initial(plaintext_keys, g_server_settings.keys_count);

    // after the save: the tunnel setup reads the ports from server_settings.json and writes its values into it
    if (is_tunnel_setup_requested == TRUE)
    {
        _first_time_setup_internal__run_tunnel_setup();
    }
}

/**
 * @brief tells whether the wizard can start the tunnel setup of selfhosting-tools/wireguard
 *
 *        two conditions: the setup script is found from the working directory, and the working
 *        directory is the repository's buildresult directory. the scripts update
 *        buildresult/server_settings.json and this server reads the file in its working directory,
 *        so both must be the same file.
 *
 * @return boole -> TRUE when the tunnel setup can be started from here
 */
static boole _first_time_setup_internal__is_tunnel_setup_available(void)
{
    FILE* script_file = 0;
    boole is_same_directory = FALSE;

#ifdef WIN32
    char working_directory[1024];
    char settings_directory[1024];
    DWORD working_directory_length = 0;
    DWORD settings_directory_length = 0;
#else
    char* working_directory = 0;
    char* settings_directory = 0;
#endif

    script_file = fopen(g_tunnel_setup_script_path, "rb");

    if (script_file == NULL_POINTER)
    {
        DBG_FIRST_TIME_SETUP log_info("%s %s", "tunnel setup not offered: script not found at", g_tunnel_setup_script_path);
        return FALSE;
    }

    fclose(script_file);

#ifdef WIN32
    clib__null_memory(working_directory, sizeof(working_directory));
    clib__null_memory(settings_directory, sizeof(settings_directory));

    working_directory_length = GetFullPathNameA(".", sizeof(working_directory), working_directory, NULL_POINTER);
    settings_directory_length = GetFullPathNameA("..\\buildresult", sizeof(settings_directory), settings_directory, NULL_POINTER);

    if (working_directory_length > 0 && working_directory_length < sizeof(working_directory) && settings_directory_length > 0 && settings_directory_length < sizeof(settings_directory) && lstrcmpiA(working_directory, settings_directory) == 0)
    {
        is_same_directory = TRUE;
    }
#else
    working_directory = realpath(".", NULL_POINTER);
    settings_directory = realpath("../buildresult", NULL_POINTER);

    if (working_directory != NULL_POINTER && settings_directory != NULL_POINTER && strcmp(working_directory, settings_directory) == 0)
    {
        is_same_directory = TRUE;
    }

    free(working_directory);
    free(settings_directory);
#endif

    DBG_FIRST_TIME_SETUP log_info("%s %s", "tunnel setup available:", is_same_directory == TRUE ? "yes" : "no, the server is not running from buildresult");

    return is_same_directory;
}

/**
 * @brief starts the tunnel setup, waits until it ended, then reads the values it wrote
 *
 *        Windows: run_setup.bat opens in its own console window. it restarts itself with
 *        administrator rights, so its end cannot be observed and the wizard waits for Enter.
 *        elsewhere: setup.sh runs in this terminal and the wizard waits for its exit.
 *
 * @attention server_settings.json must be written before: the tunnel setup reads the ports from it
 *
 * @return void
 */
static void _first_time_setup_internal__run_tunnel_setup(void)
{
#ifdef WIN32
    char script_path[1024];
    char command_line[1100];
    char input[64];
    DWORD script_path_length = 0;
    STARTUPINFOA startup_info;
    PROCESS_INFORMATION process_info;

    clib__null_memory(script_path, sizeof(script_path));
    clib__null_memory(command_line, sizeof(command_line));
    clib__null_memory(input, sizeof(input));
    clib__null_memory(&startup_info, sizeof(startup_info));
    clib__null_memory(&process_info, sizeof(process_info));
    startup_info.cb = sizeof(startup_info);

    script_path_length = GetFullPathNameA(g_tunnel_setup_script_path, sizeof(script_path), script_path, NULL_POINTER);

    if (script_path_length == 0 || script_path_length >= sizeof(script_path))
    {
        printf("%s %s\n", g_mark_warn, "The WireGuard setup script path could not be resolved. WireGuard remains disabled.");
        return;
    }

    // cmd.exe removes the outer pair of quotes; the inner pair keeps a path with spaces in one piece
    snprintf(command_line, sizeof(command_line), "cmd.exe /c \"\"%s\"\"", script_path);

    if (CreateProcessA(NULL_POINTER, command_line, NULL_POINTER, NULL_POINTER, FALSE, CREATE_NEW_CONSOLE, NULL_POINTER, NULL_POINTER, &startup_info, &process_info) == 0)
    {
        printf("%s %s%s\n", g_mark_warn, "The WireGuard setup script could not be started. Run it manually: ", script_path);
        return;
    }

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);

    printf("\n%s %s\n", g_mark_info, "The WireGuard setup script runs in a separate window and asks for administrator rights.");
    printf("%s %s", g_mark_ask, "Press Enter after the WireGuard setup script has finished: ");
    fgets(input, (int)sizeof(input), stdin);
#else
    pid_t setup_pid = 0;
    int status = 0;

    printf("\n%s %s\n", g_mark_info, "The WireGuard setup script is running in this terminal. Server setup will continue when it finishes.");
    fflush(stdout);

    setup_pid = fork();

    if (setup_pid == 0)
    {
        // started through POSIX sh, so the script needs no executable bit
        execlp("sh", "sh", g_tunnel_setup_script_path, (char*)NULL_POINTER);
        _exit(127); // exec failed -> sh is not installed / not on PATH
    }
    else if (setup_pid > 0)
    {
        waitpid(setup_pid, &status, 0);
    }
    else
    {
        printf("%s %s\n", g_mark_warn, "fork() failed; the tunnel setup was not started, wireguard mode stays off");
        return;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
    {
        printf("%s %s\n", g_mark_warn, "sh was not found; the tunnel setup was not started, wireguard mode stays off");
        return;
    }
#endif

    _first_time_setup_internal__reload_tunnel_settings();
}

/**
 * @brief reads the values the tunnel setup wrote into server_settings.json into g_server_settings:
 *        the wireguard mode fields and the same-IP rule
 *
 * @return void
 */
static void _first_time_setup_internal__reload_tunnel_settings(void)
{
    FILE* settings_file = 0;
    int64 file_length = 0;
    uint64 bytes_read = 0;
    char* file_buffer = NULL_POINTER;
    cJSON* json_root = NULL_POINTER;
    cJSON* json_field = NULL_POINTER;

    settings_file = fopen("server_settings.json", "rb");

    if (settings_file != NULL_POINTER)
    {
        fseek(settings_file, 0, SEEK_END);
        file_length = ftell(settings_file);
        fseek(settings_file, 0, SEEK_SET);

        if (file_length > 0)
        {
            file_buffer = (char*)malloc(file_length + 1);

            if (file_buffer != NULL_POINTER)
            {
                bytes_read = fread(file_buffer, 1, file_length, settings_file);
                file_buffer[bytes_read] = 0;
            }
        }

        fclose(settings_file);
    }

    if (file_buffer != NULL_POINTER)
    {
        json_root = cJSON_Parse(file_buffer);
        free(file_buffer);
    }

    if (json_root == NULL_POINTER)
    {
        printf("%s %s\n", g_mark_warn, "Could not read server_settings.json after WireGuard setup. WireGuard remains disabled.");
        return;
    }

    settings__read_wireguard_fields(json_root);

    json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_same_ip_address_allowed");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_same_ip_address_allowed = cJSON_IsTrue(json_field);
    }

    cJSON_Delete(json_root);

    if (g_server_settings.is_wireguard_mode_active == TRUE)
    {
        printf("%s %s%s%s%lld%s%lld\n", g_mark_ok, "wireguard mode: on, public address ", g_server_settings.webrtc_public_address, ", udp ", (long long)g_server_settings.webrtc_port_range_begin, "-", (long long)g_server_settings.webrtc_port_range_end);
    }
    else
    {
        printf("%s %s\n", g_mark_warn, "WireGuard remains disabled because its settings were not saved. Run the script from selfhosting-tools/wireguard, then restart the server.");
    }
}

/**
 * @brief the two answers behind the WireGuard question of the wizard: the router's public IPv4 address, and the UDP port range the router forwards for voice and video
 *        both are asked until they parse; an empty range takes the default. the mode is switched on
 *        only when both answers are usable
 *
 * @param char* input -> the wizard's input buffer
 * @param uint64 input_size -> its size
 *
 * @return void
 */
static void _first_time_setup_internal__ask_wireguard_details(char* input, uint64 input_size)
{
    unsigned int octets[4];
    long long range_begin = 0;
    long long range_end = 0;
    char trailing = 0;

    for (;;)
    {
        clib__null_memory(input, input_size);
        printf("%s %s", g_mark_ask, "Public IPv4 address of the VPS that forwards connections: ");
        if (fgets(input, (int)input_size, stdin) == NULL_POINTER)
        {
            return;
        }
        clib__sanitize_stdin(input);

        if (sscanf(input, "%u.%u.%u.%u%c", &octets[0], &octets[1], &octets[2], &octets[3], &trailing) == 4 && octets[0] <= 255 && octets[1] <= 255 && octets[2] <= 255 && octets[3] <= 255 && clib__utf8_string_length(input) < sizeof(g_server_settings.webrtc_public_address))
        {
            clib__null_memory(g_server_settings.webrtc_public_address, sizeof(g_server_settings.webrtc_public_address));
            clib__copy_memory(input, g_server_settings.webrtc_public_address, clib__utf8_string_length(input), sizeof(g_server_settings.webrtc_public_address) - 1);
            break;
        }

        printf("%s %s\n", g_mark_warn, "Enter an IPv4 address, for example 203.0.113.10.");
    }

    for (;;)
    {
        clib__null_memory(input, input_size);
        printf("%s %s%lld-%lld%s", g_mark_ask, "UDP port range forwarded by the VPS for WebRTC (BEGIN-END) [", (long long)g_server_settings.webrtc_port_range_begin, (long long)g_server_settings.webrtc_port_range_end, "]: ");
        if (fgets(input, (int)input_size, stdin) == NULL_POINTER)
        {
            return;
        }
        clib__sanitize_stdin(input);

        if (input[0] == 0)
        {
            break; // the default shown in the prompt stays
        }

        if (sscanf(input, "%lld-%lld%c", &range_begin, &range_end, &trailing) == 2 && range_begin >= 1024 && range_begin <= 65535 && range_end >= range_begin && range_end <= 65535)
        {
            g_server_settings.webrtc_port_range_begin = range_begin;
            g_server_settings.webrtc_port_range_end = range_end;
            break;
        }

        printf("%s %s\n", g_mark_warn, "Enter BEGIN-END using ports from 1024 to 65535. END must not be smaller than BEGIN.");
    }

    g_server_settings.is_wireguard_mode_active = TRUE;
    printf("%s %s%s%s%lld%s%lld\n", g_mark_ok, "wireguard mode: on, public address ", g_server_settings.webrtc_public_address, ", udp ", (long long)g_server_settings.webrtc_port_range_begin, "-", (long long)g_server_settings.webrtc_port_range_end);
    printf("%s %s\n", "   ", "The VPS must forward the selected TCP ports, UDP port 3478 and this UDP range to this machine. See selfhosting-tools/wireguard/README.md.");
}
