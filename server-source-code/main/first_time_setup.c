#include "definitions.h"
#include "crypto/ciphers.h"
#include "util.h"
#include "crypto/crypto_metadata.h"

// This file asks the server owner how to host and configure Lemon Chat.
// It saves the answers when no valid server_settings.json exists yet.

#include "clib/clib.h"
#include "../third-party/dave-g-json/cJSON.h"
#include "base.h"
#include "first_time_setup.h"
#include "integrations/tailscale.h"
#include "core/server_settings.h"
#include "core/audio_channel.h"
#include "../third-party/rxi-log/log.h"
#include <string.h>
#include <ctype.h>

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
static const char *g_tunnel_setup_script_path = "..\\selfhosting-tools\\wireguard\\run_setup.bat";
#else
static const char *g_tunnel_setup_script_path = "../selfhosting-tools/wireguard/setup.sh";
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

static const char *g_hosting_names[] = { "Direct hosting", "Tailscale Funnel", "Cloudflare Tunnel", "ngrok", "WireGuard through a VPS", "Another HTTPS/WSS proxy" };

// declarations
static cstring _first_time_setup_internal__get_hosting_requirement(first_time_setup_hosting_t hosting);
#ifdef WIN32
static boole _first_time_setup_internal__has_wireguard(void);
#endif
static first_time_setup_hosting_t _first_time_setup_internal__ask_hosting(void);
static int64 _first_time_setup_internal__ask_port(const char *question, int64 default_port);
static void _first_time_setup_internal__prompt_network_settings(first_time_setup_hosting_t hosting);
static boole _first_time_setup_internal__prompt_media_settings(first_time_setup_hosting_t hosting);
static boole _first_time_setup_internal__ask_yes_no(const char *question, boole default_answer);
static boole _first_time_setup_internal__ask_yes_no_with_note(const char *question, const char *note, boole default_answer);
static void _first_time_setup_internal__ask_admin_password(char *out_plaintext, uint64 out_capacity);
static boole _first_time_setup_internal__theme_attribute(const char *tag, const char *end, const char *name, char *out, uint64 capacity);
static uint64 _first_time_setup_internal__scan_client_themes(const char *client_html_path, char out_themes[][32], uint64 max_themes, boole mobile_only);
static void _first_time_setup_internal__ask_theme(const char *path, boole mobile, char out_theme[32]);
static void _first_time_setup_internal__ask_certificate_paths(void);
static void _first_time_setup_internal__prompt_stunnel_setup(first_time_setup_hosting_t hosting);
static boole _first_time_setup_internal__is_tunnel_setup_available(void);
static void _first_time_setup_internal__run_tunnel_setup(void);
static void _first_time_setup_internal__reload_tunnel_settings(void);
static void _first_time_setup_internal__ask_wireguard_details(char *input, uint64 input_size);

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
static boole _first_time_setup_internal__ask_yes_no(const char *question, boole default_answer)
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
static boole _first_time_setup_internal__ask_yes_no_with_note(const char *question, const char *note, boole default_answer)
{
    char input[256] = { 0 };
    uint64 i = 0;
    boole is_note_printed = FALSE;

    // repeat until valid answer
    while (TRUE)
    {
        // clear the input
        clib__null_memory(input, sizeof(input));

        // print the question
        if (note != NULL_POINTER && is_note_printed == FALSE)
        {
            printf("%s %s\n%s%s %s", g_mark_ask, question, note, "   ", (default_answer == TRUE) ? "(Y/n): " : "(y/N): ");
            is_note_printed = TRUE;
        }
        else
        {
            printf("%s %s %s", g_mark_ask, question, (default_answer == TRUE) ? "(Y/n): " : "(y/N): ");
        }

        // read the answer
        if (fgets(input, sizeof(input), stdin) == NULL_POINTER)
        {
            return default_answer;
        }

        clib__string_trim_newline(input, sizeof(input));

        // lowercase in place, so "Y", "Yes" and "YES" are one answer and not three
        for (i = 0; input[i] != 0; i++)
        {
            if ((input[i] >= 'A') && (input[i] <= 'Z'))
            {
                input[i] = (char)(input[i] + 32);
            }
        }

        // return the answer
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

        // report wrong answer
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
static void _first_time_setup_internal__ask_admin_password(char *out_plaintext, uint64 out_capacity)
{
    char first[256] = { 0 };
    char second[256] = { 0 };

#ifndef WIN32
    struct termios terminal_before;
    struct termios terminal_quiet;
    boole echo_is_off = FALSE;
#endif

    // ask until passwords match
    while (TRUE)
    {
        // clear both buffers
        clib__null_memory(first, sizeof(first));
        clib__null_memory(second, sizeof(second));

#ifndef WIN32
        // turn terminal echo off
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

        // read the first password
        printf("%s %s", g_mark_ask, "Set the server admin password: ");
        fgets(first, sizeof(first), stdin);
        clib__string_trim_newline(first, sizeof(first));
        printf("\n");

        // read the second password
        printf("%s %s", g_mark_ask, "Enter the admin password again: ");
        fgets(second, sizeof(second), stdin);
        clib__string_trim_newline(second, sizeof(second));
        printf("\n");

#ifndef WIN32
        // restore the terminal
        if (echo_is_off == TRUE)
        {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &terminal_before);
        }
#endif

        // refuse empty password
        if (first[0] == 0)
        {
            printf("%s %s\n", g_mark_warn, "The admin password cannot be empty.");
            continue;
        }

        // refuse different passwords
        if (clib__is_string_equal(first, second) == FALSE)
        {
            printf("%s %s\n", g_mark_warn, "The passwords do not match. Enter them again.");
            continue;
        }

        // copy the password
        clib__string_copy(out_plaintext, out_capacity, first);
        clib__null_memory(first, sizeof(first));
        clib__null_memory(second, sizeof(second));
        return;
    }
}

/**
 * @brief Reads a file length and returns to its beginning before the caller allocates a buffer.
 * @param FILE* file -> open file to measure
 * @return int64 -> file length, or -1 when either seek fails
 */
static int64 _first_time_setup_internal__file_size(FILE *file)
{
    int64 length = -1;

#ifdef WIN32
    // read the file length
    if (_fseeki64(file, 0, SEEK_END) != 0)
    {
        return -1;
    }
    length = _ftelli64(file);
    if (_fseeki64(file, 0, SEEK_SET) != 0)
    {
        return -1;
    }
#else
    // read the file length
    if (fseeko(file, 0, SEEK_END) != 0)
    {
        return -1;
    }
    length = ftello(file);
    if (fseeko(file, 0, SEEK_SET) != 0)
    {
        return -1;
    }
#endif
    return length;
}

/**
 * @brief Reads one HTML attribute, accepting either quote style or an unquoted value.
 */
static boole _first_time_setup_internal__theme_attribute(const char *tag, const char *end, const char *name, char *out, uint64 capacity)
{
    const char *cursor = tag + 1;
    const char *attribute = NULL_POINTER;
    const char *value = NULL_POINTER;
    uint64 name_length = clib__strlen(name);
    uint64 attribute_length = 0;
    uint64 value_length = 0;
    char quote = 0;

    // skip the element name before reading its attributes
    while (cursor < end && !isspace((unsigned char)*cursor))
    {
        cursor++;
    }

    // read each attribute
    while (cursor < end)
    {
        // skip spaces and slashes
        while (cursor < end && (isspace((unsigned char)*cursor) || *cursor == '/'))
        {
            cursor++;
        }

        // read the attribute name
        attribute = cursor;
        while (cursor < end && !isspace((unsigned char)*cursor) && *cursor != '=' && *cursor != '/')
        {
            cursor++;
        }
        attribute_length = (uint64)(cursor - attribute);

        // find the equals sign
        while (cursor < end && isspace((unsigned char)*cursor))
        {
            cursor++;
        }
        if (cursor == end)
        {
            break;
        }
        if (*cursor != '=')
        {
            continue;
        }
        cursor++;
        while (cursor < end && isspace((unsigned char)*cursor))
        {
            cursor++;
        }

        // read the value
        quote = cursor < end && (*cursor == '"' || *cursor == '\'') ? *cursor++ : 0;
        value = cursor;
        while (cursor < end && (quote != 0 ? *cursor != quote : !isspace((unsigned char)*cursor)))
        {
            cursor++;
        }
        value_length = (uint64)(cursor - value);

        // check the closing quote
        if (quote != 0 && cursor == end)
        {
            return FALSE;
        }

        // copy the matching value
        if (attribute_length == name_length && clib__strncmp(attribute, name, name_length) == 0)
        {
            if (value_length >= capacity)
            {
                return FALSE;
            }
            clib__memcpy(out, value, value_length);
            out[value_length] = 0;
            return TRUE;
        }

        // skip the closing quote
        if (quote != 0)
        {
            cursor++;
        }
    }
    return FALSE;
}

/**
 * @brief Finds theme menu entries in the served page, optionally keeping only mobile themes.
 */
static uint64 _first_time_setup_internal__scan_client_themes(const char *client_html_path, char out_themes[][32], uint64 max_themes, boole mobile_only)
{
    FILE *file = NULL_POINTER;
    int64 file_size = 0;
    char *buffer = NULL_POINTER;
    char *cursor = NULL_POINTER;
    char *end = NULL_POINTER;
    char *closing = NULL_POINTER;
    char *theme_class = NULL_POINTER;
    char classes[128] = { 0 };
    char name[32] = { 0 };
    char mobile[8] = { 0 };
    char quote = 0;
    uint64 bytes_read = 0;
    uint64 count = 0;
    uint64 i = 0;
    boole duplicate = FALSE;

    // open the page file
    file = fopen(client_html_path, "rb");
    if (file == NULL_POINTER)
    {
        return 0;
    }

    // check the file size
    file_size = _first_time_setup_internal__file_size(file);
    if (file_size <= 0 || (uint64)file_size >= SIZE_MAX)
    {
        fclose(file);
        return 0;
    }

    // allocate the buffer
    buffer = (char *)malloc((uint64)file_size + 1);
    if (buffer == NULL_POINTER)
    {
        fclose(file);
        return 0;
    }

    // read the file
    bytes_read = fread(buffer, 1, file_size, file);
    buffer[bytes_read] = 0;
    fclose(file);

    // check each tag
    cursor = buffer;
    while (count < max_themes && (cursor = (char *)clib__strchr(cursor, '<')) != NULL_POINTER)
    {
        // commented-out entries are not available themes
        if (clib__strncmp(cursor, "<!--", 4) == 0)
        {
            closing = (char *)clib__strstr(cursor + 4, "-->");
            if (closing == NULL_POINTER)
            {
                break;
            }
            cursor = closing + 3;
            continue;
        }

        // a quoted '>' belongs to an attribute, not the end of the tag
        end = cursor + 1;
        quote = 0;
        while (*end != 0)
        {
            if (quote != 0)
            {
                if (*end == quote)
                {
                    quote = 0;
                }
            }
            else if (*end == '"' || *end == '\'')
            {
                quote = *end;
            }
            else if (*end == '>')
            {
                break;
            }
            end++;
        }

        // stop at unfinished tag
        if (*end == 0)
        {
            break;
        }

        // ignore markup strings inside the page's scripts and styles
        if (clib__strncmp(cursor, "<script", 7) == 0 || clib__strncmp(cursor, "<style", 6) == 0)
        {
            closing = (char *)clib__strstr(end + 1, cursor[2] == 'c' ? "</script" : "</style");
            if (closing == NULL_POINTER)
            {
                break;
            }
            cursor = closing;
            continue;
        }

        // read the tag attributes
        classes[0] = 0;
        name[0] = 0;
        mobile[0] = 0;
        if (_first_time_setup_internal__theme_attribute(cursor, end, "class", classes, sizeof(classes)) == FALSE ||
            _first_time_setup_internal__theme_attribute(cursor, end, "data-name", name, sizeof(name)) == FALSE)
        {
            cursor = end + 1;
            continue;
        }
        _first_time_setup_internal__theme_attribute(cursor, end, "data-mobile", mobile, sizeof(mobile));
        cursor = end + 1;

        // keep theme items only
        theme_class = (char *)clib__strstr(classes, "choose-theme-item");
        if (theme_class == NULL_POINTER || (theme_class != classes && !isspace((unsigned char)theme_class[-1])) ||
            (theme_class[17] != 0 && !isspace((unsigned char)theme_class[17])) ||
            (mobile_only == TRUE && clib__strcmp(mobile, "true") != 0))
        {
            continue;
        }

        // theme IDs become style element IDs; keep only simple, complete names
        for (i = 0; name[i] != 0; i++)
        {
            if (!isalnum((unsigned char)name[i]) && name[i] != '-' && name[i] != '_')
            {
                break;
            }
        }
        if (i == 0 || name[i] != 0)
        {
            continue;
        }

        // find duplicate names
        duplicate = FALSE;
        for (i = 0; i < count; i++)
        {
            if (clib__strcmp(out_themes[i], name) == 0)
            {
                duplicate = TRUE;
                break;
            }
        }

        // store the new theme
        if (duplicate == FALSE)
        {
            clib__string_copy(out_themes[count], 32, name);
            count++;
        }
    }

    // free the buffer
    free(buffer);
    return count;
}

/**
 * @brief Offers the available themes for one device type and saves the selection.
 */
static void _first_time_setup_internal__ask_theme(const char *path, boole mobile, char out_theme[32])
{
    char themes[64][32] = { 0 };
    char input[64] = { 0 };
    char *end = NULL_POINTER;
    const char *device = mobile == TRUE ? "mobile" : "desktop";
    uint64 count = _first_time_setup_internal__scan_client_themes(path, themes, 64, mobile);
    uint64 i = 0;
    int64 choice = 0;

    // no themes found
    if (count == 0)
    {
        printf("%s No %s themes found in %s. Keeping the client's default.\n", g_mark_info, device, path);
        return;
    }

    // print the themes
    printf("%s Themes available for %s browsers:\n", g_mark_info, device);
    for (i = 0; i < count; i++)
    {
        printf("      %llu) %s\n", i + 1, themes[i]);
    }

    // repeat until valid answer
    while (TRUE)
    {
        // read the answer
        printf("%s Default %s theme number (Enter to keep the client's default): ", g_mark_ask, device);
        if (fgets(input, sizeof(input), stdin) == NULL_POINTER)
        {
            return;
        }
        clib__string_trim_newline(input, sizeof(input));

        // empty answer keeps default
        if (input[0] == 0)
        {
            return;
        }

        // convert to a number
        choice = clib__strtol(input, &end, 10);
        while (isspace((unsigned char)*end))
        {
            end++;
        }

        // store the chosen theme
        if (end != input && *end == 0 && choice >= 1 && (uint64)choice <= count)
        {
            clib__string_copy(out_theme, 32, themes[choice - 1]);
            printf("%s Default %s theme: %s\n", g_mark_ok, device, out_theme);
            return;
        }

        // report wrong answer
        printf("%s Enter a theme number from 1 to %llu, or press Enter.\n", g_mark_warn, count);
    }
}

/**
 * @brief Asks for existing certificate files without requiring a domain name.
 */
static void _first_time_setup_internal__ask_certificate_paths(void)
{
    // clear certificate settings
    clib__null_memory(g_server_settings.stunnel_domain, sizeof(g_server_settings.stunnel_domain));
    clib__null_memory(g_server_settings.stunnel_cert_fullchain, sizeof(g_server_settings.stunnel_cert_fullchain));
    clib__null_memory(g_server_settings.stunnel_cert_privkey, sizeof(g_server_settings.stunnel_cert_privkey));

    // print the instructions
    printf("%s %s\n", g_mark_info, "Use PEM files. The certificate must match the address clients connect to and be trusted by their browsers.");
    printf("%s %s\n", g_mark_info, "Renew these certificates separately. Lemon Chat will not request or renew them for this option.");

    // read the certificate path
    printf("%s %s", g_mark_ask, "Path to the certificate chain file (fullchain.pem), without quotes: ");
    if (fgets(g_server_settings.stunnel_cert_fullchain, sizeof(g_server_settings.stunnel_cert_fullchain), stdin) == NULL_POINTER)
    {
        g_server_settings.is_stunnel_enabled = FALSE;
        return;
    }
    clib__string_trim_newline(g_server_settings.stunnel_cert_fullchain, sizeof(g_server_settings.stunnel_cert_fullchain));

    // read the key path
    printf("%s %s", g_mark_ask, "Path to the private key file (privkey.pem), without quotes: ");
    if (fgets(g_server_settings.stunnel_cert_privkey, sizeof(g_server_settings.stunnel_cert_privkey), stdin) == NULL_POINTER)
    {
        g_server_settings.is_stunnel_enabled = FALSE;
        return;
    }
    clib__string_trim_newline(g_server_settings.stunnel_cert_privkey, sizeof(g_server_settings.stunnel_cert_privkey));

    // check files are readable
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
    char input[600] = { 0 };
    int64 default_port = 1112;
    boole status0 = FALSE;
#ifndef WIN32
    char found_domains[32][256] = { 0 };
    char domain[256] = { 0 };
    DIR *cert_dir = NULL_POINTER;
    struct dirent *entry = NULL_POINTER;
    uint64 found_count = 0;
    uint64 i = 0;
    int64 selection = 0;
    pid_t certbot_pid = 0;
    int status = 0; // waitpid writes an int through &status
    char probe[700] = { 0 };

    // clear the buffers
    clib__null_memory(input, sizeof(input));
    clib__null_memory(found_domains, sizeof(found_domains));
    clib__null_memory(domain, sizeof(domain));

#else
    char probe_fullchain[600] = { 0 };
    char probe_privkey[600] = { 0 };

    // clear the input buffer
    clib__null_memory(input, sizeof(input));
#endif

    // set the hosting flags
    g_server_settings.is_stunnel_enabled = FALSE;
    g_server_settings.is_tailscale_funnel_enabled = (hosting == HOSTING_FUNNEL);
    g_server_settings.is_external_wss_enabled = (hosting == HOSTING_FUNNEL || hosting == HOSTING_CLOUDFLARE || hosting == HOSTING_NGROK || hosting == HOSTING_OTHER_PROXY);

    // ask about stunnel
    if (g_server_settings.is_external_wss_enabled == FALSE)
    {
        status0 = _first_time_setup_internal__ask_yes_no("Use stunnel to accept secure WebSocket (WSS) connections?", FALSE);
        if (status0 == FALSE)
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

    // read the WSS port
    while (TRUE)
    {
        g_server_settings.wss_port = _first_time_setup_internal__ask_port((g_server_settings.is_external_wss_enabled == TRUE) ? "Public WSS port" : "WSS port", default_port);

        // check the Funnel ports
        if (hosting == HOSTING_FUNNEL && g_server_settings.wss_port != 8443 && g_server_settings.wss_port != 10000)
        {
            printf("%s %s\n", g_mark_warn, "Choose 8443 or 10000 for WSS; this setup uses public port 443 for the HTTPS page.");
            continue;
        }

        // refuse a used port
        if (g_server_settings.is_external_wss_enabled == FALSE && g_server_settings.wss_port == g_server_settings.websocket_port)
        {
            printf("%s %s\n", g_mark_warn, "That port is already used by plain WebSocket. Choose another.");
            continue;
        }
        break;
    }

    // print the forwarding ports
    if (g_server_settings.is_external_wss_enabled == TRUE)
    {
        if (hosting == HOSTING_FUNNEL)
        {
            printf("%s %s%lld%s%lld%s\n", g_mark_info, "Funnel will forward public WSS port ", g_server_settings.wss_port, " to WebSocket port ", g_server_settings.websocket_port, ".");
        }
        else
        {
            printf("%s %s%lld%s%lld%s\n", g_mark_info, "Forward public WSS port ", g_server_settings.wss_port, " to local WebSocket port ", g_server_settings.websocket_port, ".");
        }
        return;
    }

    // ask for certificate files
    status0 = _first_time_setup_internal__ask_yes_no("Use certificate and private key files you already have?", TRUE);
    if (status0 == TRUE)
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
            // clear the path buffer
            clib__null_memory(probe, sizeof(probe));

            // skip hidden entries
            if (entry->d_name[0] == '.')
            {
                continue;
            }

            // check both certificate files
            clib__string_format(probe, sizeof(probe), "/etc/letsencrypt/live/%s/fullchain.pem", entry->d_name);
            if (access(probe, R_OK) == 0)
            {
                clib__string_format(probe, sizeof(probe), "/etc/letsencrypt/live/%s/privkey.pem", entry->d_name);
                if (access(probe, R_OK) == 0)
                {
                    clib__string_format(found_domains[found_count], 256, "%s", entry->d_name);
                    found_count++;
                }
            }
        }
        closedir(cert_dir);
    }

    // choose a found certificate
    if (found_count > 0)
    {
        // print the certificates
        printf("%s", "Found existing Let's Encrypt certificate(s) under /etc/letsencrypt/live/:\n");
        for (i = 0; i < found_count; i++)
        {
            printf("%s%llu%s%s%s", "  [", (i + 1), "] ", found_domains[i], "\n");
        }

        // read the choice
        printf("%s", "Certificate number, or 'm' to enter file paths: ");
        fgets(input, sizeof(input), stdin);
        clib__string_trim_newline(input, sizeof(input));
        selection = clib__atoi(input);

        // store the chosen paths
        if (selection >= 1 && (uint64)selection <= found_count)
        {
            clib__string_format(g_server_settings.stunnel_domain, 256, "%s", found_domains[selection - 1]);
            clib__string_format(g_server_settings.stunnel_cert_fullchain, 512, "/etc/letsencrypt/live/%s/fullchain.pem", found_domains[selection - 1]);
            clib__string_format(g_server_settings.stunnel_cert_privkey, 512, "/etc/letsencrypt/live/%s/privkey.pem", found_domains[selection - 1]);
            return;
        }
    }
    else
    {
        // ask about Certbot
        printf("%s %s\n", g_mark_info, "No certificate found under /etc/letsencrypt/live/.");
        status0 = _first_time_setup_internal__ask_yes_no_with_note("Request a Let's Encrypt certificate using Certbot?",
                                                                   "    Your domain's DNS A record must point to this server's public IPv4 address.\n"
                                                                   "    TCP port 80 must be reachable from the internet and not used by another program.\n",
                                                                   FALSE);
        if (status0 == FALSE)
        {
            printf("%s", "No certificate was selected. WSS remains disabled.\n");
            g_server_settings.is_stunnel_enabled = FALSE;
            return;
        }

        // read the domain name
        printf("%s", "Domain name for the certificate (for example chat.example.com): ");
        if (fgets(domain, sizeof(domain), stdin) == NULL_POINTER)
        {
            g_server_settings.is_stunnel_enabled = FALSE;
            return;
        }
        clib__string_trim_newline(domain, sizeof(domain));
        if (domain[0] == 0)
        {
            g_server_settings.is_stunnel_enabled = FALSE;
            return;
        }

        // start Certbot
        printf("%s", "Running Certbot to request the certificate.\n");
        printf("%s", "Certbot temporarily uses TCP port 80 to verify the domain.\n");

        status = 0;
        certbot_pid = fork();
        if (certbot_pid == 0)
        {
            // exec certbot directly (no shell) so the domain cannot inject commands
            execlp("certbot", "certbot", "certonly", "--standalone", "-d", domain, (char *)NULL_POINTER);
            _exit(127); // exec failed -> certbot is not installed / not on PATH
        }
        else if (certbot_pid > 0)
        {
            // wait for Certbot
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

        // report Certbot failure
        if (WIFEXITED(status) == 0 || WEXITSTATUS(status) != 0)
        {
            printf("%s %s\n", g_mark_warn, "Certbot failed; wss stays off. See its output above.");
            g_server_settings.is_stunnel_enabled = FALSE;
            return;
        }

        // store the certificate paths
        clib__string_format(g_server_settings.stunnel_domain, 256, "%s", domain);
        clib__string_format(g_server_settings.stunnel_cert_fullchain, 512, "/etc/letsencrypt/live/%s/fullchain.pem", domain);
        clib__string_format(g_server_settings.stunnel_cert_privkey, 512, "/etc/letsencrypt/live/%s/privkey.pem", domain);

        // only enable wss if certbot actually produced the cert + key
        if (access(g_server_settings.stunnel_cert_privkey, R_OK) != 0 || access(g_server_settings.stunnel_cert_fullchain, R_OK) != 0)
        {
            printf("%s", "certbot did not produce a certificate (see its output above - usually the domain\n");
            printf("%s", "not pointing here yet, or port 80 blocked/in use). wss stays off; fix and re-run.\n");
            g_server_settings.is_stunnel_enabled = FALSE;
        }
        return;
    }

    // ask for file paths
    _first_time_setup_internal__ask_certificate_paths();
#else
    // print the instructions
    printf("%s %s\n", g_mark_info, "Enter a domain to find an existing Certbot certificate under C:\\Certbot\\live\\.");
    printf("%s %s\n", g_mark_info, "Leave it blank to enter certificate files instead. This setup does not request a Windows certificate.");

    // read the domain name
    while (TRUE)
    {
        clib__null_memory(g_server_settings.stunnel_domain, sizeof(g_server_settings.stunnel_domain));
        printf("%s %s", g_mark_ask, "Domain name to look up in C:\\Certbot\\live (Enter to provide file paths instead): ");
        if (fgets(g_server_settings.stunnel_domain, sizeof(g_server_settings.stunnel_domain), stdin) == NULL_POINTER)
        {
            break;
        }
        clib__string_trim_newline(g_server_settings.stunnel_domain, sizeof(g_server_settings.stunnel_domain));

        // ask paths when empty
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
    clib__string_format(probe_fullchain, sizeof(probe_fullchain), "C:\\Certbot\\live\\%s\\fullchain.pem", g_server_settings.stunnel_domain);
    clib__string_format(probe_privkey, sizeof(probe_privkey), "C:\\Certbot\\live\\%s\\privkey.pem", g_server_settings.stunnel_domain);

    // use the found certificate
    if (access(probe_fullchain, R_OK) == 0 && access(probe_privkey, R_OK) == 0)
    {
        clib__string_format(g_server_settings.stunnel_cert_fullchain, 512, "%s", probe_fullchain);
        clib__string_format(g_server_settings.stunnel_cert_privkey, 512, "%s", probe_privkey);
        printf("%s %s%s\n", g_mark_ok, "Found the Certbot certificate under C:\\Certbot\\live\\", g_server_settings.stunnel_domain);
        return;
    }

    // ask for file paths
    printf("%s %s\n", g_mark_info, "No certificate found for that domain; enter existing certificate files instead.");
    _first_time_setup_internal__ask_certificate_paths();
#endif
}

#ifdef WIN32
/**
 * @brief Finds the WireGuard commands in PATH or their installation directory.
 * @return Whether both WireGuard commands are installed.
 */
static boole _first_time_setup_internal__has_wireguard(void)
{
    cstring folders[] = { "ProgramW6432", "ProgramFiles", "ProgramFiles(x86)" };
    char directory[2048] = { 0 };
    char command[4096] = { 0 };
    uint64 length = 0;
    uint64 attributes = 0;
    DWORD binary_type = 0; // GetBinaryTypeA writes through a DWORD pointer
    uint64 index = 0;

    // search the PATH first
    if (util__find_executable("wg", command, sizeof(command)) == TRUE && util__find_executable("wireguard", command, sizeof(command)) == TRUE)
    {
        return TRUE;
    }

    // search the program folders
    for (index = 0; index < sizeof(folders) / sizeof(folders[0]); index++)
    {
        // read the folder path
        length = GetEnvironmentVariableA(folders[index], directory, sizeof(directory));
        if (length == 0 || length >= sizeof(directory))
        {
            continue;
        }

        // check the wg program
        clib__string_format(command, sizeof(command), "%s\\WireGuard\\wg.exe", directory);
        attributes = GetFileAttributesA(command);
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || GetBinaryTypeA(command, &binary_type) == 0)
        {
            continue;
        }

        // check the wireguard program
        clib__string_format(command, sizeof(command), "%s\\WireGuard\\wireguard.exe", directory);
        attributes = GetFileAttributesA(command);
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && GetBinaryTypeA(command, &binary_type) != 0)
        {
            return TRUE;
        }
    }
    return FALSE;
}
#endif

/**
 * @brief Names a missing hosting tool before the user chooses that option.
 * @param hosting Hosting method being checked.
 * @return Missing tool name or NULL_POINTER when the option is available.
 */
static cstring _first_time_setup_internal__get_hosting_requirement(first_time_setup_hosting_t hosting)
{
    char command[4096] = { 0 };

    // find the needed tool
    switch (hosting)
    {
    case HOSTING_FUNNEL:
        return tailscale__is_available() == TRUE ? NULL_POINTER : "Tailscale";
    case HOSTING_CLOUDFLARE:
        return util__find_executable("cloudflared", command, sizeof(command)) == TRUE ? NULL_POINTER : "cloudflared";
    case HOSTING_NGROK:
        return util__find_executable("ngrok", command, sizeof(command)) == TRUE ? NULL_POINTER : "ngrok";
    case HOSTING_WIREGUARD:
#ifdef WIN32
        return _first_time_setup_internal__has_wireguard() == TRUE ? NULL_POINTER : "WireGuard";
#else
        return util__find_executable("wg", command, sizeof(command)) == TRUE ? NULL_POINTER : "wireguard-tools (wg)";
#endif
    default:
        return NULL_POINTER;
    }
}

/**
 * @brief Shows installed hosting tools and refuses unavailable choices.
 * @return The hosting method selected by the user.
 */
static first_time_setup_hosting_t _first_time_setup_internal__ask_hosting(void)
{
    char input[64] = { 0 };
    char *end = NULL_POINTER;
    cstring requirement = NULL_POINTER;
    cstring name = NULL_POINTER;
    int64 choice = HOSTING_DIRECT;
    uint64 index = 0;

    // print the hosting choices
    for (index = HOSTING_DIRECT; index <= HOSTING_OTHER_PROXY; index++)
    {
        requirement = _first_time_setup_internal__get_hosting_requirement((first_time_setup_hosting_t)index);
        name = index == HOSTING_DIRECT ? "Host directly (LAN or VPS)" : g_hosting_names[index - 1];
        printf("    %llu) %s%s\n", index, name, requirement == NULL_POINTER ? "" : " (unavailable, install first)");
    }

    // repeat until valid answer
    while (TRUE)
    {
        // read the answer
        printf("%s %s", g_mark_ask, "How do you want to host this server? [1]: ");
        if (fgets(input, sizeof(input), stdin) == NULL_POINTER)
        {
            return HOSTING_DIRECT;
        }
        clib__string_trim_newline(input, sizeof(input));

        // use default when empty
        if (input[0] == 0)
        {
            return HOSTING_DIRECT;
        }

        // check the chosen number
        choice = clib__strtol(input, &end, 10);
        if (end != input && *end == 0 && choice >= HOSTING_DIRECT && choice <= HOSTING_OTHER_PROXY)
        {
            requirement = _first_time_setup_internal__get_hosting_requirement((first_time_setup_hosting_t)choice);
            if (requirement != NULL_POINTER)
            {
                printf("%s %s is unavailable. Install %s first and make its command available to the server.\n", g_mark_warn, g_hosting_names[choice - 1], requirement);
                continue;
            }
            return (first_time_setup_hosting_t)choice;
        }

        // report wrong answer
        printf("%s %s\n", g_mark_warn, "Enter a number from 1 to 6.");
    }
}

/**
 * @brief Asks for a port and repeats the question if the answer is not a valid port number.
 * @param const char* question -> the name of the port being configured
 * @param int64 default_port -> the port used when Enter is pressed
 * @return int64 -> a port from 1 to 65535
 */
static int64 _first_time_setup_internal__ask_port(const char *question, int64 default_port)
{
    char input[64] = { 0 };
    char *end = NULL_POINTER;
    int64 port = 0;

    // repeat until valid answer
    while (TRUE)
    {
        // read the answer
        printf("%s %s [%lld]: ", g_mark_ask, question, default_port);
        if (fgets(input, sizeof(input), stdin) == NULL_POINTER)
        {
            return default_port;
        }
        clib__string_trim_newline(input, sizeof(input));

        // use default when empty
        if (input[0] == 0)
        {
            return default_port;
        }

        // check the port number
        port = clib__strtol(input, &end, 10);
        if (end != input && *end == 0 && port >= 1 && port <= 65535)
        {
            return port;
        }

        // report wrong answer
        printf("%s %s\n", g_mark_warn, "Enter a whole port number from 1 to 65535.");
    }
}

/**
 * @brief Asks only the network questions needed for the chosen hosting method.
 * @param first_time_setup_hosting_t hosting -> the selected hosting method
 * @return void
 */
static void _first_time_setup_internal__prompt_network_settings(first_time_setup_hosting_t hosting)
{
    char theme_scan_path[600] = { 0 };
    boole is_same_public_hostname = TRUE;
    boole status0 = FALSE;
    boole is_http_tunnel = (hosting == HOSTING_FUNNEL || hosting == HOSTING_CLOUDFLARE || hosting == HOSTING_NGROK);

    // print the chosen hosting
    printf("%s Hosting: %s\n", g_mark_ok, g_hosting_names[hosting - 1]);
    if (hosting == HOSTING_FUNNEL)
    {
        printf("%s %s\n", g_mark_info, "Install Tailscale and sign in. Lemon Chat configures Funnel when the server starts.");
    }
    else if (is_http_tunnel == TRUE || hosting == HOSTING_OTHER_PROXY)
    {
        printf("%s %s\n", g_mark_info, "Configure the forwarding program separately; see selfhosting-tools/README.md.");
    }

    // read the WebSocket port
    g_server_settings.websocket_port = _first_time_setup_internal__ask_port("WebSocket port", 1111);

    // ask the WSS questions
    _first_time_setup_internal__prompt_stunnel_setup(hosting);

    // ask about HTTP server
    if (hosting == HOSTING_FUNNEL)
    {
        printf("%s %s\n", g_mark_info, "The built-in HTTP server will serve client.html. The tunnel provides HTTPS.");
    }
    if (hosting == HOSTING_FUNNEL || _first_time_setup_internal__ask_yes_no("Use the built-in HTTP server to serve client.html to users?", is_http_tunnel) == TRUE)
    {
        // enable the HTTP server
        g_server_settings.is_client_http_enabled = TRUE;

        // read the HTTP port
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

        // print the HTTP port
        printf("%s %s%lld\n", g_mark_info, "HTTP port: ", g_server_settings.http_port);
        if (hosting == HOSTING_FUNNEL)
        {
            printf("%s %s%lld%s\n", g_mark_info, "Funnel will forward HTTPS port 443 to HTTP port ", g_server_settings.http_port, ".");
        }
        else if (g_server_settings.is_external_wss_enabled == TRUE)
        {
            printf("%s %s%lld%s\n", g_mark_info, "Forward the public HTTPS page to local HTTP port ", g_server_settings.http_port, ".");
        }

        // ask about HTTPS
        if (g_server_settings.is_stunnel_enabled == TRUE)
        {
            status0 = _first_time_setup_internal__ask_yes_no("Use stunnel to serve client.html over HTTPS?", FALSE);
            if (status0 == TRUE)
            {
                g_server_settings.is_https_enabled = TRUE;

                // read the HTTPS port
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

        // automatic connection uses the page's hostname for the chat connection too
        if (g_server_settings.is_external_wss_enabled == TRUE && hosting != HOSTING_FUNNEL)
        {
            is_same_public_hostname = _first_time_setup_internal__ask_yes_no("Will client.html and the WSS connection use the same public domain name?", FALSE);
        }

        // ask about automatic connection
        if (is_same_public_hostname == TRUE && _first_time_setup_internal__ask_yes_no_with_note("Include the server connection details in client.html so it connects automatically?", "    This includes the connection passwords. Anyone who opens the page receives them.\n", FALSE) == TRUE)
        {
            g_server_settings.is_client_config_embedded = TRUE;
            printf("%s %s\n", g_mark_ok, "Automatic connection: enabled.");
        }
        else
        {
            printf("%s %s\n", g_mark_info, "Automatic connection: disabled. Users enter the server connection details themselves.");
        }

        // scan the served page so custom themes are offered too
        if (g_server_settings.http_webroot[0] != 0)
        {
            clib__string_format(theme_scan_path, sizeof(theme_scan_path), "%s/client.html", g_server_settings.http_webroot);
        }
        else
        {
            clib__string_copy(theme_scan_path, sizeof(theme_scan_path), "client.html");
        }
        _first_time_setup_internal__ask_theme(theme_scan_path, FALSE, g_server_settings.default_theme);
        _first_time_setup_internal__ask_theme(theme_scan_path, TRUE, g_server_settings.default_mobile_theme);

        // ask about buttons
        status0 = _first_time_setup_internal__ask_yes_no_with_note("Show a large connect button before the page connects?",
                                                                   "    A browser plays no sound on a page until the user has clicked on it.\n"
                                                                   "    With the button, every user clicks once, so voices and sound effects can be heard.\n",
                                                                   FALSE);
        if (status0 == TRUE)
        {
            g_server_settings.is_start_page_connect_button_enabled = TRUE;
            printf("%s %s\n", g_mark_ok, "Connect button on the start page: enabled.");
        }

        // ask about import button
        status0 = _first_time_setup_internal__ask_yes_no_with_note("Show a large import identity button on the start page?",
                                                                   "    A user who has an identity from another browser or device can load it before connecting,\n"
                                                                   "    from pasted text or from an identity file.\n",
                                                                   FALSE);
        if (status0 == TRUE)
        {
            g_server_settings.is_start_page_import_identity_button_enabled = TRUE;
            printf("%s %s\n", g_mark_ok, "Import identity button on the start page: enabled.");
        }

        // ask about browser identity
        status0 = _first_time_setup_internal__ask_yes_no_with_note("Save each user's identity in their browser?",
                                                                   "    The browser reuses the same identity after the page is reloaded or reopened.\n"
                                                                   "    This is separate from saving identities on the server, asked later.\n",
                                                                   FALSE);
        if (status0 == TRUE)
        {
            g_server_settings.is_identity_localstorage_enabled = TRUE;
            printf("%s %s\n", g_mark_ok, "Saving browser identities: enabled.");
        }
    }

    // ask about blank page
    if (g_server_settings.is_client_http_enabled == FALSE)
    {
        g_server_settings.is_client_http_blank_when_disabled = _first_time_setup_internal__ask_yes_no_with_note("Keep an HTTP listener that serves a blank page?", "    No stops HTTP completely. Yes keeps the port open but does not serve client.html.\n", FALSE);
        if (g_server_settings.is_client_http_blank_when_disabled)
        {
            do
            {
                g_server_settings.http_port = _first_time_setup_internal__ask_port("HTTP port for the blank page", 80);
            } while (g_server_settings.http_port == g_server_settings.websocket_port || (g_server_settings.is_stunnel_enabled && g_server_settings.http_port == g_server_settings.wss_port));
        }
    }

    // set same ip rule
    if (is_http_tunnel == TRUE)
    {
        g_server_settings.is_same_ip_address_allowed = TRUE;
        printf("%s %s\n", g_mark_info, "Multiple clients from the same IP address: allowed.");
    }
    else
    {
        status0 = _first_time_setup_internal__ask_yes_no_with_note("Limit connections to one client per IP address?", "    For localhost testing with multiple clients, answer no.\n", FALSE);
        if (status0 == TRUE)
        {
            g_server_settings.is_same_ip_address_allowed = FALSE;
            printf("%s %s\n", g_mark_ok, "Multiple clients from the same IP address: blocked.");
        }
    }
}

/**
 * @brief Chooses media features before asking which transports they should use.
 * @param first_time_setup_hosting_t hosting -> the selected hosting method
 * @return boole -> TRUE when WireGuard setup should run after saving the settings
 */
static boole _first_time_setup_internal__prompt_media_settings(first_time_setup_hosting_t hosting)
{
    char input[256] = { 0 };
    boole is_tunnel_setup_requested = FALSE;
    boole is_http_tunnel = (hosting == HOSTING_FUNNEL || hosting == HOSTING_CLOUDFLARE || hosting == HOSTING_NGROK);

    // ask about media features
    g_server_settings.is_voice_chat_active = _first_time_setup_internal__ask_yes_no("Enable voice chat?", TRUE);
    g_server_settings.is_music_bot_audio_active = _first_time_setup_internal__ask_yes_no_with_note("Enable music bot audio?", "    Music bots can play audio even when voice chat is disabled.\n", TRUE);
    g_server_settings.is_video_streaming_active = _first_time_setup_internal__ask_yes_no("Enable video streaming and screen sharing?", FALSE);

    // choose the media transport
    if (g_server_settings.is_voice_chat_active == FALSE && g_server_settings.is_music_bot_audio_active == FALSE && g_server_settings.is_video_streaming_active == FALSE)
    {
        g_server_settings.is_audio_over_websocket_allowed = TRUE;
        g_server_settings.is_audio_over_webrtc_allowed = FALSE;
        printf("%s %s\n", g_mark_info, "Audio and video are disabled. Skipping the transport question; WebSocket is selected if media is enabled later.");
    }
    else if (is_http_tunnel == TRUE)
    {
        g_server_settings.is_audio_over_websocket_allowed = TRUE;
        g_server_settings.is_audio_over_webrtc_allowed = FALSE;
        printf("%s %s\n", g_mark_info, "Audio/video connections: WebSocket only for this hosting option.");
    }
    else
    {
        // repeat until valid answer
        for (;;)
        {
            // read the answer
            clib__null_memory(input, sizeof(input));
            printf("%s %s", g_mark_ask, "Connection type for audio and video: 1 = WebSocket, 2 = WebRTC DataChannel, 3 = both (clients choose) [3]: ");

            if (fgets(input, sizeof(input), stdin) == NULL_POINTER)
            {
                break;
            }
            clib__string_trim_newline(input, sizeof(input));

            // keep both transports
            if (input[0] == 0 || clib__is_string_equal(input, "3") == TRUE)
            {
                break; // both stay on, the default
            }

            // allow only WebSocket
            if (clib__is_string_equal(input, "1") == TRUE)
            {
                g_server_settings.is_audio_over_webrtc_allowed = FALSE;
                break;
            }

            // allow only WebRTC
            if (clib__is_string_equal(input, "2") == TRUE)
            {
                g_server_settings.is_audio_over_websocket_allowed = FALSE;
                break;
            }

            // report wrong answer
            printf("%s %s\n", g_mark_warn, "Enter 1, 2 or 3.");
        }

        // print the transport
        printf("%s %s%s\n", g_mark_ok, "audio/video connections: ", audio_channel__policy_value());
        clib__null_memory(input, sizeof(input));
    }

    // ask the WireGuard questions
    if (hosting == HOSTING_WIREGUARD)
    {
        // the tunnel setup asks for the router itself and writes the address and the range, so
        // the two detail questions are asked only when it does not run
        if (_first_time_setup_internal__is_tunnel_setup_available() == TRUE)
        {
            printf("\n");
            is_tunnel_setup_requested = _first_time_setup_internal__ask_yes_no_with_note("Run the WireGuard setup script after saving these settings?",
                                                                                         "    Configures the VPS over SSH and connects this machine through WireGuard.\n"
                                                                                         "    Answer no if it is already configured; enter its address and any required UDP ports below.\n",
                                                                                         TRUE);
        }
        else
        {
            printf("%s %s\n", g_mark_info, "The WireGuard script is not available here. Enter the settings of your existing VPS below, or run selfhosting-tools/wireguard separately.");
        }

        // ask address and ports
        if (is_tunnel_setup_requested == FALSE)
        {
            _first_time_setup_internal__ask_wireguard_details(input, sizeof(input));
        }
    }

    // clear the input buffer
    clib__null_memory(input, sizeof(input));

    return is_tunnel_setup_requested;
}

/** @brief Selects a supported cipher without silently replacing an invalid choice. */
static crypto_key_type_t _first_time_setup_internal__cipher(void)
{
    int64 type = 0;
    char input[80] = { 0 };

    // repeat until valid answer
    for (;;)
    {
        // read the answer
        printf("%s Cipher: aes256, chacha20, camellia256, twofish256, serpent256, trivium80 [aes256]: ", g_mark_ask);
        if (fgets(input, sizeof(input), stdin) == NULL_POINTER)
        {
            return CRYPTO_KEY_AES_256_CTR;
        }
        clib__string_trim_newline(input, sizeof(input));

        // use default when empty
        if (input[0] == 0)
        {
            return CRYPTO_KEY_AES_256_CTR;
        }

        // find the cipher type
        type = ciphers__type(input);
        if (type != 0)
        {
            return (crypto_key_type_t)type;
        }

        // report wrong answer
        printf("Choose one of the listed cipher names.\n");
    }
}

/**
 * @brief Asks for hosting, access and chat settings, then saves the answers.
 * @param char plaintext_keys[][256] -> storage for up to 100 connection passwords
 */
void first_time_setup__run(char plaintext_keys[][256])
{
    crypto_key_type_t cipher = 0;
    char input[256] = { 0 };
    char *end = NULL_POINTER;
    uint64 i = 0;
    int64 slots = 0;
    int64 requested_key_count = 0;
    boole is_tunnel_setup_requested = FALSE;
    first_time_setup_hosting_t hosting = HOSTING_DIRECT;
    boole status0 = FALSE;

    // print the introduction
    printf("\n%s %s\n", g_mark_info, "First-time setup. Answers will be saved to server_settings.json.");
    printf("%s %s\n", "   ", "Press Enter to use the default: (Y/n) means yes; (y/N) means no.");

    // ask about permission system
    g_server_settings.is_permission_system_enabled = _first_time_setup_internal__ask_yes_no_with_note("Enable server groups, icons and group permissions?",
                                                                                                      "    Choose no for a simple chat. The admin password still works.\n"
                                                                                                      "    To change this later, edit server_settings.json and restart the server.\n",
                                                                                                      TRUE);

    // ask hosting and network
    printf("\n%s %s\n\n", g_mark_info, "1/4  Hosting and network connections.");
    hosting = _first_time_setup_internal__ask_hosting();
    _first_time_setup_internal__prompt_network_settings(hosting);

    // print the part title
    printf("\n%s %s\n\n", g_mark_info, "2/4  Passwords and server limits.");

    // ask about connection passwords
    status0 = _first_time_setup_internal__ask_yes_no_with_note("Require connection passwords to join this server?", "    Clients need all configured passwords, entered in the same order.\n", FALSE);
    if (status0 == TRUE)
    {
        // read the password count
        clib__null_memory(input, sizeof(input));
        printf("%s %s", g_mark_ask, "Number of connection passwords required (1-100): ");
        fgets(input, sizeof(input), stdin);
        clib__string_trim_newline(input, sizeof(input));

        // clamp into range with a signed temporary: at least 1 (the operator already opted in) and
        // never past the 100-slot keys / plaintext_keys arrays. doing the comparison signed avoids a
        // negative atoi result wrapping to a huge uint64
        requested_key_count = clib__atoi(input);
        if (requested_key_count < 1)
        {
            requested_key_count = 1;
        }
        if (requested_key_count > 100)
        {
            requested_key_count = 100;
        }
        g_server_settings.keys_count = requested_key_count;

        // read every password
        for (i = 0; i < g_server_settings.keys_count; i++)
        {
            // read the next password
            clib__null_memory(input, sizeof(input));
            printf("%s%s%llu%s", g_mark_ask, " Connection password ", i + 1, ": ");
            fgets(input, sizeof(input), stdin);
            clib__string_trim_newline(input, sizeof(input));

            // copy the plain password
            clib__string_copy(plaintext_keys[i], sizeof(plaintext_keys[i]), input);

            // create the encryption key
            cipher = _first_time_setup_internal__cipher();
            if (!crypto_metadata__key_from_password(&g_server_settings.keys[i], cipher, input))
            {
                fprintf(stderr, "Failed to derive a configured encryption key.\n");
                exit(EXIT_FAILURE);
            }
        }

        // print the confirmation
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

    // clear the input buffer
    clib__null_memory(input, sizeof(input));

    // ask about cipher layer
    printf("%s Session encryption applies even when no connection passwords are configured.\n", g_mark_info);
    g_server_settings.is_random_cipher_layer_enabled = _first_time_setup_internal__ask_yes_no_with_note("Add one random cipher layer (the troll key option)?",
                                                                                                        "    Each connection tries the supported ciphers against a cryptographic proof.\n"
                                                                                                        "    Extra exchanges and layers cost CPU; they do not multiply the security rating.\n",
                                                                                                        FALSE);

    /* This setting is read only at startup, before any client storage exists. */
    g_server_settings.max_client_count = DEFAULT_SERVER_SLOTS;
    g_server_settings.max_channel_count = DEFAULT_CHANNEL_COUNT;
    g_server_settings.channel_count_limit = DEFAULT_CHANNEL_COUNT;

    // read the client limit
    for (;;)
    {
        // read the answer
        printf("%s Maximum connected clients (1-%d) [%d]: ", g_mark_ask, MAX_SERVER_SLOTS, DEFAULT_SERVER_SLOTS);
        clib__null_memory(input, sizeof(input));
        if (fgets(input, sizeof(input), stdin) == NULL_POINTER)
        {
            break;
        }
        clib__string_trim_newline(input, sizeof(input));

        // keep default when empty
        if (input[0] == 0)
        {
            break;
        }

        // check the number
        slots = clib__strtoll(input, &end, 10);
        if (end != input && *end == 0 && slots >= 1 && slots <= MAX_SERVER_SLOTS)
        {
            g_server_settings.max_client_count = (uint64)slots;
            break;
        }

        // report wrong answer
        printf("%s Enter a whole number from 1 to %d.\n", g_mark_warn, MAX_SERVER_SLOTS);
    }

    // read the channel limit
    printf("%s %s\n", g_mark_info, "The channel limit includes root, permanent channels and temporary channels.");
    for (;;)
    {
        // read the answer
        printf("%s Maximum channels [%d]: ", g_mark_ask, DEFAULT_CHANNEL_COUNT);
        clib__null_memory(input, sizeof(input));
        if (fgets(input, sizeof(input), stdin) == NULL_POINTER)
        {
            break;
        }
        clib__string_trim_newline(input, sizeof(input));

        // keep default when empty
        if (input[0] == 0)
        {
            break;
        }

        // check the number
        slots = clib__strtoll(input, &end, 10);
        if (end != input && *end == 0 && slots >= 1 && slots <= MAX_CHANNEL_COUNT)
        {
            g_server_settings.channel_count_limit = (uint64)slots;
            g_server_settings.max_channel_count = (uint64)slots;
            break;
        }

        // report wrong answer
        printf("%s Enter a whole number from 1 to %d.\n", g_mark_warn, MAX_CHANNEL_COUNT);
    }

    // set the admin password
    _first_time_setup_internal__ask_admin_password(input, sizeof(input));
    util__hash_password_to_base64(input, &g_server_settings.admin_password[0], ADMIN_PASSWORD_MAX_LENGTH);
    g_server_settings.is_admin_password_initial = TRUE;

    // keep the plaintext only for this run's startup summary (shown once, then cleared)
    clib__string_copy(g_first_run_admin_password, ADMIN_PASSWORD_MAX_LENGTH, input);
    clib__null_memory(input, sizeof(input));

    // print the part title
    printf("\n%s %s\n\n", g_mark_info, "3/4  Audio, video and server behaviour.");

    // ask the media settings
    is_tunnel_setup_requested = _first_time_setup_internal__prompt_media_settings(hosting);

#ifndef WIN32
    // ask about crash restart
    status0 = _first_time_setup_internal__ask_yes_no_with_note("Let start_server.sh restart the server after a crash?", "    Applies the next time you launch through start_server.sh. Failures are recorded in crashes.txt.\n", FALSE);
    if (status0 == TRUE)
    {
        g_server_settings.is_restart_on_crash_enabled = TRUE;
        printf("%s %s\n", g_mark_ok, "Automatic restart through start_server.sh: enabled.");
    }
#endif

    // print the part title
    printf("\n%s %s\n\n", g_mark_info, "4/4  Client profiles and chat features.");

    // ask about saved identities
    g_server_settings.is_identity_storage_enabled = _first_time_setup_internal__ask_yes_no_with_note("Save client identities on the server?", g_server_settings.is_permission_system_enabled == TRUE ? "    Saves assigned groups and profiles for clients returning with the same identity.\n" : "    Saves profiles and enables an optional saved-user list for password admins.\n", TRUE);
    g_server_settings.is_stored_client_list_allowed = FALSE;
    g_server_settings.is_last_seen_enabled = FALSE;
    g_server_settings.is_offline_messaging_allowed = FALSE;
    if (g_server_settings.is_identity_storage_enabled == FALSE)
    {
        printf("%s %s\n", g_mark_off, "identities: off");
        printf("%s %s\n", "   ", "The saved user list, last seen times and offline messages require saved identities. These options will be skipped.");
    }

    // ask about idle clients
    status0 = _first_time_setup_internal__ask_yes_no_with_note("Allow idle clients?", "    Useful for the Android app to wait for incoming calls in the background.\n", TRUE);
    if (status0 == FALSE)
    {
        g_server_settings.is_idle_mode_allowed = FALSE;
        printf("%s %s\n", g_mark_off, "idle clients: off");
    }

    // ask about country flags
    status0 = _first_time_setup_internal__ask_yes_no("Show country flags next to clients?", FALSE);
    if (status0 == TRUE)
    {
        g_server_settings.is_display_country_flags_active = TRUE;
        printf("%s %s\n", g_mark_ok, "country flags: on");
    }

    // ask about avatar uploads
    status0 = _first_time_setup_internal__ask_yes_no("Allow users to upload avatar images?", FALSE);
    if (status0 == TRUE)
    {
        g_server_settings.is_avatar_upload_allowed = TRUE;
        printf("%s %s\n", g_mark_ok, "avatars: on");

        printf("%s %s%lld%s\n", g_mark_ok, "max avatar size: ", (int64)(g_server_settings.avatar_max_size_bytes / 1024), " KB");
        printf("%s %s\n", g_mark_info, "You can change this limit in the server settings tab.");
    }
    clib__null_memory(input, sizeof(input));

    // ask about saved users
    if (g_server_settings.is_identity_storage_enabled == TRUE)
    {
        g_server_settings.is_stored_client_list_allowed = _first_time_setup_internal__ask_yes_no(g_server_settings.is_permission_system_enabled == TRUE ? "Show saved users, including offline users, to clients with permission?" : "Show saved users, including offline users, to password admins?", FALSE);
        if (g_server_settings.is_stored_client_list_allowed == TRUE)
        {
            // ask about last seen
            g_server_settings.is_last_seen_enabled = _first_time_setup_internal__ask_yes_no("Show when saved users were last online?", FALSE);

            // ask about offline messages
            g_server_settings.is_offline_messaging_allowed = _first_time_setup_internal__ask_yes_no_with_note(g_server_settings.is_permission_system_enabled == TRUE ? "Allow offline messages for users whose groups have permission?" : "Allow offline messages between password admins?",
                                                                                                              "    The server saves public keys so users can encrypt messages for offline recipients.\n"
                                                                                                              "    Encrypted messages wait in RAM until the recipient reconnects.\n"
                                                                                                              "    Pending messages are lost when the server stops.\n",
                                                                                                              FALSE);
        }
        else
        {
            printf("%s %s\n", g_mark_info, "Last seen times and offline messages require the saved-user list. These options will be skipped.");
        }
    }

    // ask about typing indicator
    status0 = _first_time_setup_internal__ask_yes_no_with_note("Show when a user is typing a chat message?", "    Shows a typing indicator. The unfinished message is not sent.\n", FALSE);
    if (status0 == TRUE)
    {
        g_server_settings.is_typing_indicator_allowed = TRUE;
        printf("%s %s\n", g_mark_ok, "typing indicator: on");
    }

    // ask about file uploads
    status0 = _first_time_setup_internal__ask_yes_no_with_note("Allow users to send files in chat?",
                                                               "    Controls file attachments, such as documents and archives. Pictures have a separate setting.\n"
                                                               "    Files are encrypted by the clients and relayed through server RAM, without being saved to disk.\n",
                                                               FALSE);
    if (status0 == TRUE)
    {
        g_server_settings.is_file_upload_allowed = TRUE;
        printf("%s %s\n", g_mark_ok, "file uploads: on");

        printf("%s %s%lld%s\n", g_mark_ok, "max file size: ", (int64)(g_server_settings.file_upload_max_size_bytes / (1024 * 1024)), " MB");
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
    printf("      %s%llu\n", "maximum channels ..... ", (uint64)g_server_settings.channel_count_limit);
    printf("      %s%s\n", "music bot audio ...... ", (g_server_settings.is_music_bot_audio_active == TRUE) ? "on" : "off");
    printf("      %s%s\n", "video streaming ...... ", (g_server_settings.is_video_streaming_active == TRUE) ? "on" : "off");
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
    printf("      %s%s\n", "people list .......... ", (g_server_settings.is_stored_client_list_allowed == TRUE) ? "on" : "off");
    printf("      %s%s\n", "last seen ............ ", (g_server_settings.is_last_seen_enabled == TRUE) ? "on" : "off");
    printf("      %s%s\n", "offline messages ..... ", (g_server_settings.is_offline_messaging_allowed == TRUE) ? "on" : "off");
    printf("      %s%s\n", "typing indicator ..... ", (g_server_settings.is_typing_indicator_allowed == TRUE) ? "on" : "off");
    printf("      %s%s\n", "client renames ....... ", (g_server_settings.is_client_rename_allowed == TRUE) ? "allowed" : "admin only");

    // print the file uploads
    if (g_server_settings.is_file_upload_allowed == TRUE)
    {
        printf("      %s%s%lld%s\n", "file uploads ......... ", "on (max ", (int64)(g_server_settings.file_upload_max_size_bytes / (1024 * 1024)), " MB)");
    }
    else
    {
        printf("      %s%s\n", "file uploads ......... ", "off");
    }
    printf("      %s%s\n", "browser access ....... ", (g_server_settings.is_client_http_enabled == TRUE) ? "on" : "off");

    // print the wireguard mode
    if (is_tunnel_setup_requested == TRUE)
    {
        printf("      %s%s\n\n", "wireguard mode ....... ", "set by the tunnel setup, which starts after the settings are saved");
    }
    else if (g_server_settings.is_wireguard_mode_active == TRUE)
    {
        printf("      %s%s%s%lld%s%lld\n\n", "wireguard mode ....... on, public address ", g_server_settings.webrtc_public_address, ", udp ", (int64)g_server_settings.webrtc_port_range_begin, "-", (int64)g_server_settings.webrtc_port_range_end);
    }
    else
    {
        printf("      %s%s\n\n", "wireguard mode ....... ", "off");
    }

    // save the settings
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
    FILE *script_file = NULL_POINTER;
    boole is_same_directory = FALSE;

#ifdef WIN32
    char working_directory[1024] = { 0 };
    char settings_directory[1024] = { 0 };
    uint64 working_directory_length = 0;
    uint64 settings_directory_length = 0;
#else
    char *working_directory = NULL_POINTER;
    char *settings_directory = NULL_POINTER;
#endif

    // check the script exists
    script_file = fopen(g_tunnel_setup_script_path, "rb");

    if (script_file == NULL_POINTER)
    {
        DBG_FIRST_TIME_SETUP log_info("%s %s", "tunnel setup not offered: script not found at", g_tunnel_setup_script_path);
        return FALSE;
    }

    fclose(script_file);

#ifdef WIN32
    // compare the two directories
    clib__null_memory(working_directory, sizeof(working_directory));
    clib__null_memory(settings_directory, sizeof(settings_directory));

    working_directory_length = GetFullPathNameA(".", sizeof(working_directory), working_directory, NULL_POINTER);
    settings_directory_length = GetFullPathNameA("..\\buildresult", sizeof(settings_directory), settings_directory, NULL_POINTER);

    if (working_directory_length > 0 && working_directory_length < sizeof(working_directory) && settings_directory_length > 0 && settings_directory_length < sizeof(settings_directory) && lstrcmpiA(working_directory, settings_directory) == 0)
    {
        is_same_directory = TRUE;
    }
#else
    // compare the two directories
    working_directory = realpath(".", NULL_POINTER);
    settings_directory = realpath("../buildresult", NULL_POINTER);

    if (working_directory != NULL_POINTER && settings_directory != NULL_POINTER && clib__strcmp(working_directory, settings_directory) == 0)
    {
        is_same_directory = TRUE;
    }

    // free the paths
    free(working_directory);
    free(settings_directory);
#endif

    // log the result
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
    char script_path[1024] = { 0 };
    char command_line[1100] = { 0 };
    char input[64] = { 0 };
    uint64 script_path_length = 0;
    STARTUPINFOA startup_info = { 0 };
    PROCESS_INFORMATION process_info = { 0 };

    // clear the buffers
    clib__null_memory(script_path, sizeof(script_path));
    clib__null_memory(command_line, sizeof(command_line));
    clib__null_memory(input, sizeof(input));
    clib__null_memory(&startup_info, sizeof(startup_info));
    clib__null_memory(&process_info, sizeof(process_info));
    startup_info.cb = sizeof(startup_info);

    // build the full path
    script_path_length = GetFullPathNameA(g_tunnel_setup_script_path, sizeof(script_path), script_path, NULL_POINTER);

    if (script_path_length == 0 || script_path_length >= sizeof(script_path))
    {
        printf("%s %s\n", g_mark_warn, "The WireGuard setup script path could not be resolved. WireGuard remains disabled.");
        return;
    }

    // cmd.exe removes the outer pair of quotes; the inner pair keeps a path with spaces in one piece
    clib__string_format(command_line, sizeof(command_line), "cmd.exe /c \"\"%s\"\"", script_path);

    // start the script
    if (CreateProcessA(NULL_POINTER, command_line, NULL_POINTER, NULL_POINTER, FALSE, CREATE_NEW_CONSOLE, NULL_POINTER, NULL_POINTER, &startup_info, &process_info) == 0)
    {
        printf("%s %s%s\n", g_mark_warn, "The WireGuard setup script could not be started. Run it manually: ", script_path);
        return;
    }

    // close the process handles
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);

    // wait for Enter
    printf("\n%s %s\n", g_mark_info, "The WireGuard setup script runs in a separate window and asks for administrator rights.");
    printf("%s %s", g_mark_ask, "Press Enter after the WireGuard setup script has finished: ");
    fgets(input, (int)sizeof(input), stdin);
#else
    pid_t setup_pid = 0;
    int status = 0; // waitpid writes through an int pointer

    // report the script start
    printf("\n%s %s\n", g_mark_info, "The WireGuard setup script is running in this terminal. Server setup will continue when it finishes.");
    fflush(stdout);

    // start script and wait
    setup_pid = fork();

    if (setup_pid == 0)
    {
        // started through POSIX sh, so the script needs no executable bit
        execlp("sh", "sh", g_tunnel_setup_script_path, (char *)NULL_POINTER);
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

    // report a missing shell
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
    {
        printf("%s %s\n", g_mark_warn, "sh was not found; the tunnel setup was not started, wireguard mode stays off");
        return;
    }
#endif

    // load the new settings
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
    FILE *settings_file = NULL_POINTER;
    int64 file_length = 0;
    uint64 bytes_read = 0;
    char *file_buffer = NULL_POINTER;
    cJSON *json_root = NULL_POINTER;
    cJSON *json_field = NULL_POINTER;

    // read the settings file
    settings_file = fopen("server_settings.json", "rb");

    if (settings_file != NULL_POINTER)
    {
        file_length = _first_time_setup_internal__file_size(settings_file);

        if (file_length > 0 && (uint64)file_length <= SERVER_SETTINGS_MAX_FILE_BYTES && (uint64)file_length < SIZE_MAX)
        {
            file_buffer = (char *)malloc((uint64)file_length + 1);

            if (file_buffer != NULL_POINTER)
            {
                bytes_read = fread(file_buffer, 1, file_length, settings_file);
                file_buffer[bytes_read] = 0;
            }
        }

        fclose(settings_file);
    }

    // read the JSON
    if (file_buffer != NULL_POINTER)
    {
        json_root = cJSON_Parse(file_buffer);
        free(file_buffer);
    }

    // report the failure
    if (json_root == NULL_POINTER)
    {
        printf("%s %s\n", g_mark_warn, "Could not read server_settings.json after WireGuard setup. WireGuard remains disabled.");
        return;
    }

    // load the WireGuard fields
    settings__read_wireguard_fields(json_root);

    // load same ip rule
    json_field = cJSON_GetObjectItemCaseSensitive(json_root, "is_same_ip_address_allowed");
    if (cJSON_IsBool(json_field))
    {
        g_server_settings.is_same_ip_address_allowed = cJSON_IsTrue(json_field);
    }

    // free the JSON
    cJSON_Delete(json_root);

    // print the WireGuard state
    if (g_server_settings.is_wireguard_mode_active == TRUE)
    {
        printf("%s %s%s%s%lld%s%lld\n", g_mark_ok, "wireguard mode: on, public address ", g_server_settings.webrtc_public_address, ", udp ", (int64)g_server_settings.webrtc_port_range_begin, "-", (int64)g_server_settings.webrtc_port_range_end);
    }
    else
    {
        printf("%s %s\n", g_mark_warn, "WireGuard remains disabled because its settings were not saved. Run the script from selfhosting-tools/wireguard, then restart the server.");
    }
}

/**
 * @brief Asks for the WireGuard router address and, only with WebRTC enabled, its forwarded UDP
 *        range. Answers are checked before enabling the mode; an empty range keeps the default.
 *
 * @param char* input -> the wizard's input buffer
 * @param uint64 input_size -> its size
 *
 * @return void
 */
static void _first_time_setup_internal__ask_wireguard_details(char *input, uint64 input_size)
{
    uint64 octets[4] = { 0 };
    int64 range_begin = 0;
    int64 range_end = 0;
    char trailing = 0;

    // check arguments
    if (input == NULL_POINTER || input_size < 2 || input_size > INT_MAX)
    {
        return;
    }

    // read the VPS address
    for (;;)
    {
        // read the answer
        clib__null_memory(input, input_size);
        printf("%s %s", g_mark_ask, "Public IPv4 address of the VPS that forwards connections: ");
        if (fgets(input, (int)input_size, stdin) == NULL_POINTER)
        {
            return;
        }
        clib__string_trim_newline(input, input_size);

        // store a valid address
        if (sscanf(input, "%llu.%llu.%llu.%llu%c", &octets[0], &octets[1], &octets[2], &octets[3], &trailing) == 4 && octets[0] <= 255 && octets[1] <= 255 && octets[2] <= 255 && octets[3] <= 255 && clib__utf8_string_length(input) < sizeof(g_server_settings.webrtc_public_address))
        {
            clib__null_memory(g_server_settings.webrtc_public_address, sizeof(g_server_settings.webrtc_public_address));
            clib__string_copy(g_server_settings.webrtc_public_address, sizeof(g_server_settings.webrtc_public_address), input);
            break;
        }

        // report wrong answer
        printf("%s %s\n", g_mark_warn, "Enter an IPv4 address, for example 203.0.113.10.");
    }

    // read the port range
    while (g_server_settings.is_audio_over_webrtc_allowed == TRUE)
    {
        // read the answer
        clib__null_memory(input, input_size);
        printf("%s %s%lld-%lld%s", g_mark_ask, "UDP port range forwarded by the VPS for WebRTC (BEGIN-END) [", (int64)g_server_settings.webrtc_port_range_begin, (int64)g_server_settings.webrtc_port_range_end, "]: ");
        if (fgets(input, (int)input_size, stdin) == NULL_POINTER)
        {
            return;
        }
        clib__string_trim_newline(input, input_size);

        // keep default when empty
        if (input[0] == 0)
        {
            break; // the default shown in the prompt stays
        }

        // store a valid range
        if (sscanf(input, "%lld-%lld%c", &range_begin, &range_end, &trailing) == 2 && range_begin >= 1024 && range_begin <= 65535 && range_end >= range_begin && range_end <= 65535)
        {
            g_server_settings.webrtc_port_range_begin = range_begin;
            g_server_settings.webrtc_port_range_end = range_end;
            break;
        }

        // report wrong answer
        printf("%s %s\n", g_mark_warn, "Enter BEGIN-END using ports from 1024 to 65535. END must not be smaller than BEGIN.");
    }

    // enable WireGuard mode
    g_server_settings.is_wireguard_mode_active = TRUE;
    printf("%s %s%s%s%lld%s%lld\n", g_mark_ok, "wireguard mode: on, public address ", g_server_settings.webrtc_public_address, ", udp ", (int64)g_server_settings.webrtc_port_range_begin, "-", (int64)g_server_settings.webrtc_port_range_end);

    // print the forwarding note
    if (g_server_settings.is_audio_over_webrtc_allowed == TRUE)
    {
        printf("%s %s\n", "   ", "The VPS must forward the selected TCP ports, UDP port 3478 and this UDP range to this machine. See selfhosting-tools/wireguard/README.md.");
    }
    else
    {
        printf("%s %s\n", "   ", "Forward the selected TCP ports to this machine. WebRTC UDP forwarding is not needed with WebSocket-only media.");
    }
}
