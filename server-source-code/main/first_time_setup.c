#include "definitions.h"

// first-run interactive configuration wizard, split out of main.c (see first_time_setup.h). runs only
// when there is no valid server_settings.json. the settings-LOAD path and the push-config-to-client step
// stay in main.c; this file owns every operator prompt and the initial write of server_settings.json.

#include "clib/clib_string.h"
#include "clib/clib_memory.h"
#include "../third-party/dave-g-json/cJSON.h"
#include "base.h"
#include "../third-party/ITH-sha/sha256.h"
#include "first_time_setup.h"
#include <string.h>

#ifndef WIN32
#include <unistd.h>    // fork, execlp, access, _exit, R_OK
#include <dirent.h>    // opendir / readdir (Let's Encrypt cert detection)
#include <sys/wait.h>  // waitpid (certbot)
#include <termios.h>   // tcgetattr / tcsetattr (hiding the admin password while it is typed)
#endif

static void _first_time_setup_internal__save_server_settings(char plaintext_keys[][256], uint64 keys_count);
static int64 _first_time_setup_internal__scan_client_themes(char* client_html_path, char out_themes[][32], int64 max_themes);
static void _first_time_setup_internal__prompt_stunnel_setup(void);
static boole _first_time_setup_internal__ask_yes_no(const char* question, boole default_answer);
static void _first_time_setup_internal__ask_admin_password(char* out_plaintext, uint64 out_capacity);

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
    char input[256];
    uint64 i = 0;

    while (TRUE)
    {
        clib__null_memory(input, sizeof(input));
        printf("%s %s %s", g_mark_ask, question, (default_answer == TRUE) ? "(Y/n): " : "(y/N): ");

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

        printf("%s %s\n", g_mark_warn, "please answer y or n, or press Enter for the default");
    }
}

/**
 * @brief reads the admin password twice, with the terminal echo turned off
 *
 *        a mistyped admin password is only recoverable by deleting server_settings.json and
 *        redoing the whole setup, so it is entered twice and an empty one is refused. on windows
 *        there is no termios, and the password is read the old way and echoes as it is typed.
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

        printf("%s %s", g_mark_ask, "Admin password (max 50 chars, not shown as you type): ");
        fgets(first, sizeof(first), stdin);
        clib__sanitize_stdin(first);
        printf("\n");

        printf("%s %s", g_mark_ask, "Repeat the admin password: ");
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
            printf("%s %s\n", g_mark_warn, "the admin password cannot be empty");
            continue;
        }

        if (clib__is_string_equal(first, second) == FALSE)
        {
            printf("%s %s\n", g_mark_warn, "the two entries did not match, try again");
            continue;
        }

        clib__copy_memory(first, out_plaintext, clib__utf8_string_length(first), out_capacity - 1);
        clib__null_memory(first, sizeof(first));
        clib__null_memory(second, sizeof(second));
        return;
    }
}

/**
 * @brief writes the current settings back to server_settings.json so the next start
 *        is non-interactive
 *
 *        serializes every g_server_settings field with cJSON, appends the plaintext keys as a
 *        "keys" array, and writes the result through base__write_file_atomically. a failed write
 *        is only reported on stdout - setup still continues, the answers are just not persisted.
 *
 * @param char plaintext_keys[][256] -> the channel keys as entered, one per row (g_server_settings keeps
 *        only their hashes, but the file stores plaintext keys, which are re-hashed on load)
 * @param uint64 keys_count -> number of valid entries in plaintext_keys
 *
 * @return void
 */
static void _first_time_setup_internal__save_server_settings(char plaintext_keys[][256], uint64 keys_count)
{
    cJSON* json_root = 0;
    cJSON* json_keys = 0;
    char* json_text = 0;
    uint64 i = 0;

    json_root = cJSON_CreateObject();
    if (json_root == NULL_POINTER)
    {
        return;
    }

    cJSON_AddNumberToObject(json_root, "websocket_port", g_server_settings.websocket_port);
    cJSON_AddStringToObject(json_root, "admin_password", &g_server_settings.admin_password[0]);
    cJSON_AddItemToObject(json_root, "admin_password_is_initial", cJSON_CreateBool(g_server_settings.admin_password_is_initial == TRUE));

    json_keys = cJSON_CreateArray();
    cJSON_AddItemToObject(json_root, "keys", json_keys);
    for (i = 0; i < keys_count; i++)
    {
        cJSON_AddItemToArray(json_keys, cJSON_CreateString(&plaintext_keys[i][0]));
    }

    cJSON_AddItemToObject(json_root, "is_voice_chat_active", cJSON_CreateBool(g_server_settings.is_voice_chat_active == TRUE));
    cJSON_AddItemToObject(json_root, "is_music_bot_audio_active", cJSON_CreateBool(g_server_settings.is_music_bot_audio_active == TRUE));
    cJSON_AddItemToObject(json_root, "is_same_ip_address_allowed", cJSON_CreateBool(g_server_settings.is_same_ip_address_allowed == TRUE));
    cJSON_AddNumberToObject(json_root, "minimum_rsa_key_bits", (double)g_server_settings.minimum_rsa_key_bits);
    cJSON_AddItemToObject(json_root, "announce_minimum_rsa_key_bits", cJSON_CreateBool(g_server_settings.announce_minimum_rsa_key_bits == TRUE));
    cJSON_AddItemToObject(json_root, "is_display_country_flags_active", cJSON_CreateBool(g_server_settings.is_display_country_flags_active == TRUE));
    cJSON_AddItemToObject(json_root, "is_hide_clients_in_password_protected_channels_active", cJSON_CreateBool(g_server_settings.is_hide_clients_in_password_protected_channels_active == TRUE));
    cJSON_AddItemToObject(json_root, "is_temp_channel_creation_allowed", cJSON_CreateBool(g_server_settings.is_temp_channel_creation_allowed == TRUE));
    cJSON_AddItemToObject(json_root, "is_idle_mode_allowed", cJSON_CreateBool(g_server_settings.is_idle_mode_allowed == TRUE));
    cJSON_AddItemToObject(json_root, "hide_admin_country_flag", cJSON_CreateBool(g_server_settings.hide_admin_country_flag == TRUE));
    cJSON_AddItemToObject(json_root, "is_fast_reconnect_allowed", cJSON_CreateBool(g_server_settings.is_fast_reconnect_allowed == TRUE));
    cJSON_AddItemToObject(json_root, "is_identity_takeover_allowed", cJSON_CreateBool(g_server_settings.is_identity_takeover_allowed == TRUE));
    cJSON_AddItemToObject(json_root, "is_websocket_ping_active", cJSON_CreateBool(g_server_settings.is_websocket_ping_active == TRUE));
    cJSON_AddItemToObject(json_root, "restart_on_crash", cJSON_CreateBool(g_server_settings.restart_on_crash == TRUE));

    cJSON_AddItemToObject(json_root, "use_stunnel", cJSON_CreateBool(g_server_settings.use_stunnel == TRUE));
    cJSON_AddNumberToObject(json_root, "wss_port", g_server_settings.wss_port);
    cJSON_AddStringToObject(json_root, "stunnel_domain", &g_server_settings.stunnel_domain[0]);
    cJSON_AddStringToObject(json_root, "stunnel_cert_fullchain", &g_server_settings.stunnel_cert_fullchain[0]);
    cJSON_AddStringToObject(json_root, "stunnel_cert_privkey", &g_server_settings.stunnel_cert_privkey[0]);
    cJSON_AddStringToObject(json_root, "client_html_dest", &g_server_settings.client_html_dest[0]);

    cJSON_AddItemToObject(json_root, "serve_client_http", cJSON_CreateBool(g_server_settings.serve_client_http == TRUE));
    cJSON_AddNumberToObject(json_root, "http_port", g_server_settings.http_port);
    cJSON_AddStringToObject(json_root, "http_webroot", &g_server_settings.http_webroot[0]);
    cJSON_AddItemToObject(json_root, "serve_https", cJSON_CreateBool(g_server_settings.serve_https == TRUE));
    cJSON_AddNumberToObject(json_root, "https_port", g_server_settings.https_port);
    cJSON_AddStringToObject(json_root, "default_theme", &g_server_settings.default_theme[0]);
    cJSON_AddItemToObject(json_root, "embed_client_config", cJSON_CreateBool(g_server_settings.embed_client_config == TRUE));
    cJSON_AddItemToObject(json_root, "persist_identity_in_localstorage", cJSON_CreateBool(g_server_settings.persist_identity_in_localstorage == TRUE));
    cJSON_AddItemToObject(json_root, "allow_avatars", cJSON_CreateBool(g_server_settings.allow_avatars == TRUE));
    cJSON_AddNumberToObject(json_root, "avatar_max_size_bytes", (double)g_server_settings.avatar_max_size_bytes);
    cJSON_AddItemToObject(json_root, "allow_alias_registrations", cJSON_CreateBool(g_server_settings.allow_alias_registrations == TRUE));
    cJSON_AddItemToObject(json_root, "allow_stored_clients_list", cJSON_CreateBool(g_server_settings.allow_stored_clients_list == TRUE));
    cJSON_AddItemToObject(json_root, "allow_last_seen", cJSON_CreateBool(g_server_settings.allow_last_seen == TRUE));
    cJSON_AddItemToObject(json_root, "allow_offline_messages", cJSON_CreateBool(g_server_settings.allow_offline_messages == TRUE));
    cJSON_AddItemToObject(json_root, "allow_typing_indicator", cJSON_CreateBool(g_server_settings.allow_typing_indicator == TRUE));
    cJSON_AddItemToObject(json_root, "allow_client_renames", cJSON_CreateBool(g_server_settings.allow_client_renames == TRUE));
    cJSON_AddItemToObject(json_root, "is_sending_text_to_idle_clients_allowed", cJSON_CreateBool(g_server_settings.is_sending_text_to_idle_clients_allowed == TRUE));
    cJSON_AddItemToObject(json_root, "allow_private_messages", cJSON_CreateBool(g_server_settings.allow_private_messages == TRUE));
    cJSON_AddItemToObject(json_root, "allow_file_uploads", cJSON_CreateBool(g_server_settings.allow_file_uploads == TRUE));
    cJSON_AddNumberToObject(json_root, "file_upload_max_size_bytes", (double)g_server_settings.file_upload_max_size_bytes);
    cJSON_AddNumberToObject(json_root, "chat_picture_max_size_bytes", (double)g_server_settings.chat_picture_max_size_bytes);
    // pictures are assumed wanted: on by default and never a setup question, only a settings-tab toggle
    cJSON_AddItemToObject(json_root, "allow_chat_pictures", cJSON_CreateBool(g_server_settings.allow_chat_pictures == TRUE));
    // country blocking is deliberately NOT a setup question; the admin sets it later in the settings tab
    cJSON_AddItemToObject(json_root, "is_country_blocking_active", cJSON_CreateBool(g_server_settings.is_country_blocking_active == TRUE));
    cJSON_AddItemToObject(json_root, "blocked_countries", cJSON_CreateArray());
    // the admin log toggles are not setup questions either; all off until the admin turns them on
    cJSON_AddItemToObject(json_root, "log_client_joins", cJSON_CreateBool(g_server_settings.log_client_joins == TRUE));
    cJSON_AddItemToObject(json_root, "log_username_changes", cJSON_CreateBool(g_server_settings.log_username_changes == TRUE));
    cJSON_AddItemToObject(json_root, "log_tag_changes", cJSON_CreateBool(g_server_settings.log_tag_changes == TRUE));
    cJSON_AddItemToObject(json_root, "log_server_settings_updates", cJSON_CreateBool(g_server_settings.log_server_settings_updates == TRUE));
    cJSON_AddItemToObject(json_root, "log_kicks_and_bans", cJSON_CreateBool(g_server_settings.log_kicks_and_bans == TRUE));
    cJSON_AddItemToObject(json_root, "log_client_disconnects", cJSON_CreateBool(g_server_settings.log_client_disconnects == TRUE));
    cJSON_AddItemToObject(json_root, "log_failed_attempts", cJSON_CreateBool(g_server_settings.log_failed_attempts == TRUE));
    cJSON_AddNumberToObject(json_root, "admin_log_max_size_bytes", (double)g_server_settings.admin_log_max_size_bytes);
    cJSON_AddNumberToObject(json_root, "admin_log_retention_days", (double)g_server_settings.admin_log_retention_days);

    json_text = cJSON_Print(json_root);
    if (json_text != NULL_POINTER)
    {
        if (base__write_file_atomically("server_settings.json", json_text) == TRUE)
        {
            printf("%s %s\n", g_mark_ok, "settings saved to server_settings.json (next start skips these prompts)");
        }
        else
        {
            printf("%s %s\n", g_mark_warn, "could not write server_settings.json (settings not persisted)");
        }
        cJSON_free(json_text);
    }

    cJSON_Delete(json_root);
}

/**
 * @brief scans the served client.html for the selectable theme names baked into it, so
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

    buffer = (char* )malloc(file_size + 1);
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
        if (comment_open == NULL_POINTER) { break; }
        comment_close = strstr(comment_open, "-->");
        if (comment_close == NULL_POINTER) { break; }
        for (blank_p = comment_open; blank_p < (comment_close + 3); blank_p++) { *blank_p = ' '; }
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
 * @brief asks whether to front the server with the bundled stunnel for wss, then picks or
 *        obtains the TLS certificate for it
 *
 *        reached only in the interactive setup path; the JSON path fills these fields directly.
 *        after the wss port, it lists any Let's Encrypt certificates found under
 *        /etc/letsencrypt/live/ to choose from, or falls back to manual cert paths. with none
 *        found it explains the DNS 'A' record and port 80 requirements and runs certbot certonly
 *        --standalone itself (exec'd without a shell so the domain cannot inject commands).
 *        use_stunnel is turned back off whenever certbot is missing or produced no cert/key.
 *        the whole body is compiled out on WIN32.
 *
 * @return void
 */
static void _first_time_setup_internal__prompt_stunnel_setup(void)
{
#ifndef WIN32
    char input[600];
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

    printf("%s", "Enable HTTPS (wss) via the bundled stunnel, for use on a live website? (y/n) ");
    fgets(input, sizeof(input), stdin);
    clib__sanitize_stdin(input);
    if (input[0] != 'y' && input[0] != 'Y')
    {
        g_server_settings.use_stunnel = FALSE;
        return;
    }
    g_server_settings.use_stunnel = TRUE;
    if (g_server_settings.wss_port == 0)
    {
        g_server_settings.wss_port = 1112;
    }

    while (TRUE)
    {
        clib__null_memory(input, sizeof(input));
        printf("%s %s%lld%s", g_mark_ask, "wss (secure websocket) port [default ", g_server_settings.wss_port, "]: ");
        if (fgets(input, sizeof(input), stdin) == NULL_POINTER)
        {
            break;
        }
        clib__sanitize_stdin(input);
        if (input[0] != 0)
        {
            g_server_settings.wss_port = strtol(input, 0, 10);
        }
        if (g_server_settings.wss_port <= 0 || g_server_settings.wss_port > 65535)
        {
            printf("%s %s\n", g_mark_warn, "invalid port (must be 1-65535)");
            g_server_settings.wss_port = 1112;
            continue;
        }
        if (g_server_settings.wss_port == g_server_settings.websocket_port)
        {
            printf("%s %s%lld%s\n", g_mark_warn, "port ", g_server_settings.wss_port, " is the plain ws port - pick another");
            continue;
        }
        break;
    }

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
                snprintf(found_domains[found_count], 256, "%s", entry->d_name);
                found_count++;
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
        printf("%s", "Pick a number to use it, or 'm' for manual cert paths: ");
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
        printf("%s", "You decided to make lemon-chat accessible as an https secured website, but no existing\n");
        printf("%s", "certificate was found under /etc/letsencrypt/live/.\n\n");
        printf("%s", "To get this working, you need a valid https certificate, and therefore a domain.\n");
        printf("%s", "If you already have a domain:\n");
        printf("%s", "  1. open the DNS control panel at the website where you bought the domain\n");
        printf("%s", "  2. add an 'A' record for your domain pointing to this server's public IP address\n");
        printf("%s", "     (the same IP address you use to connect to / SSH into this server)\n");
        printf("%s", "  3. make sure port 80 is reachable from the internet (open in your firewall;\n");
        printf("%s", "     if your VPS provider has its own cloud firewall, open it there too)\n");
        printf("%s", "lemon-chat's server will then run certbot, which starts its own temporary http server\n");
        printf("%s", "on port 80, and Let's Encrypt connects to http://<your-domain>/ to verify you own it.\n");
        printf("%s", "If the DNS record already points to this server's IP address, the cert will get created\n");
        printf("%s", "and the chat will be accessible over https.\n");
        printf("%s", "(a freshly added DNS record can take some minutes to start resolving)\n\n");
        printf("%s", "Do you already have a domain with its DNS record set to this server? (y/n) ");
        fgets(input, sizeof(input), stdin);
        clib__sanitize_stdin(input);
        if (input[0] != 'y' && input[0] != 'Y')
        {
            printf("%s", "Set up the DNS 'A' record first, then re-run this setup. wss stays off for now.\n");
            g_server_settings.use_stunnel = FALSE;
            return;
        }

        printf("%s", "Domain name (e.g. chat.example.com): ");
        fgets(domain, sizeof(domain), stdin);
        clib__sanitize_stdin(domain);

        printf("%s", "running: certbot certonly --standalone -d <your-domain>\n");
        printf("%s", "(certbot serves port 80 itself to answer the challenge; nothing else may use it now)\n");

        status = 0;
        certbot_pid = fork();
        if (certbot_pid == 0)
        {
            // exec certbot directly (no shell) so the domain cannot inject commands
            execlp("certbot", "certbot", "certonly", "--standalone", "-d", domain, (char* )NULL_POINTER);
            _exit(127); // exec failed -> certbot is not installed / not on PATH
        }
        else if (certbot_pid > 0)
        {
            waitpid(certbot_pid, &status, 0);
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
            g_server_settings.use_stunnel = FALSE;
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
            g_server_settings.use_stunnel = FALSE;
        }
        return;
    }

    // manual entry
    printf("%s", "Domain name: ");
    fgets(g_server_settings.stunnel_domain, sizeof(g_server_settings.stunnel_domain), stdin);
    clib__sanitize_stdin(g_server_settings.stunnel_domain);
    printf("%s", "Path to fullchain.pem: ");
    fgets(g_server_settings.stunnel_cert_fullchain, sizeof(g_server_settings.stunnel_cert_fullchain), stdin);
    clib__sanitize_stdin(g_server_settings.stunnel_cert_fullchain);
    printf("%s", "Path to privkey.pem: ");
    fgets(g_server_settings.stunnel_cert_privkey, sizeof(g_server_settings.stunnel_cert_privkey), stdin);
    clib__sanitize_stdin(g_server_settings.stunnel_cert_privkey);
#endif
}

/**
 * @brief runs the interactive first-run setup wizard: every operator prompt, then writes
 *        server_settings.json. see first_time_setup.h.
 *
 *        asks in order for the websocket port, the optional extra metadata keys (1-100, hashed into
 *        g_server_settings.keys), the admin password, the feature toggles (voice, same-IP, country
 *        flags, idle, auto-restart, identities, avatars and - only while identities are on - aliases,
 *        the stored clients list, last seen and offline messages), then stunnel/wss and the built-in
 *        HTTP(S) server, including a default theme picked from the themes found in client.html.
 *        client counts are not asked for; they are fixed to MAX_CLIENTS / MAX_CHANNELS.
 *
 * @param char plaintext_keys[][256] -> receives the entered metadata keys as plaintext, one per row;
 *        must have at least 100 rows, and is handed straight to the settings writer
 *
 * @return void
 */
void first_time_setup__run(char plaintext_keys[][256])
{
    char input[256];
    uint64 i = 0;
    char client_themes[16][32];
    int64 client_theme_count = 0;
    int64 theme_choice = 0;
    char theme_scan_path[600];
    ITH_SHA256_CTX ctx;
    int64 requested_key_count = 0;

    printf("\n%s %s\n", g_mark_info, "First-time setup (answers are saved to server_settings.json; delete that file to redo)");
    printf("%s %s\n", "   ", "each question shows its default in brackets - (Y/n) takes yes on Enter, (y/N) takes no.");

    printf("\n%s %s\n\n", g_mark_info, "1/4  Server basics - the port and the two passwords.");

    printf("%s %s", g_mark_ask, "WebSocket port: ");
    fgets(input, sizeof(input), stdin);
    clib__sanitize_stdin(input);
    g_server_settings.websocket_port = strtol(input, 0, 10);
    clib__null_memory(input, sizeof(input));

    // one name for this thing throughout: "connect password". the prompt, the confirmation and
    // the else branch used to call it three different things
    printf("%s %s\n", "   ", "a connect password is one shared secret everybody needs to get in. it also encrypts");
    printf("%s %s\n", "   ", "traffic on top of the per-client encryption. leave it off for an open server.");

    if (_first_time_setup_internal__ask_yes_no("Set a connect password?", FALSE) == TRUE)
    {
        clib__null_memory(input, sizeof(input));
        printf("%s %s", g_mark_ask, "How many connect passwords? Any one of them lets a client in (1-100): ");
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
            printf("%s%s%llu%s", g_mark_ask, " connect password ", i + 1, ": ");
            fgets(input, sizeof(input), stdin);
            clib__sanitize_stdin(input);

            clib__copy_memory(input, &plaintext_keys[i][0], clib__utf8_string_length(input), 255);

            ith_sha256_init(&ctx);
            ith_sha256_update(&ctx, (unsigned char* )input, strlen(input));
            ith_sha256_final(&ctx, g_server_settings.keys[i].key_value);
        }

        if (g_server_settings.keys_count == 1)
        {
            printf("%s %s\n", g_mark_ok, "connect password: set");
        }
        else
        {
            printf("%s %llu %s\n", g_mark_ok, g_server_settings.keys_count, "connect passwords set");
        }
    }
    else
    {
        g_server_settings.keys_count = 0;
        printf("%s %s\n", g_mark_info, "no connect password: anybody who knows the address can join (traffic is still encrypted per client)");
    }

    clib__null_memory(input, sizeof(input));

    // clib__null_memory(input, sizeof(input));
    // printf("%s", "max allowed number of clients {from 1 to 499} : ");
    // fgets(input, sizeof(input), stdin);
    // clib__sanitize_stdin(input);

    g_server_settings.max_client_count = MAX_CLIENTS;
    g_server_settings.max_channel_count = MAX_CHANNELS;

    // g_server_settings.max_client_count = atoi(input);
    // if(g_server_settings.max_client_count > 499)
    // {
    // printf("SETUP FAIL");
    // return;
    // }

    // clib__null_memory(input, sizeof(input));
    // printf("%s", "max allowed number of channels {from 1 to 99} : ");
    // fgets(input, sizeof(input), stdin);
    // clib__sanitize_stdin(input);
    // g_server_settings.max_channel_count = atoi(input);

    // if(g_server_settings.max_client_count > 99)
    // {
    // printf("SETUP FAIL");
    // return;
    // }

    _first_time_setup_internal__ask_admin_password(input, sizeof(input));
    base__hash_password_to_base64(input, &g_server_settings.admin_password[0], ADMIN_PASSWORD_MAX_LENGTH);
    g_server_settings.admin_password_is_initial = TRUE;
    // keep the plaintext only for this run's startup summary (shown once, then wiped)
    clib__copy_memory(input, &g_first_run_admin_password[0], clib__utf8_string_length(input), ADMIN_PASSWORD_MAX_LENGTH - 1);
    clib__null_memory(input, sizeof(input));

    printf("\n%s %s\n\n", g_mark_info, "2/4  Moderation and uptime.");

    // from here on every question is phrased so yes means on. the wizard used to mix "Disable x?"
    // in among the "Allow x?" ones, so a straight run of y answers switched features OFF
    if (_first_time_setup_internal__ask_yes_no("Enable voice chat?", TRUE) == FALSE)
    {
        g_server_settings.is_voice_chat_active = FALSE;
        printf("%s %s\n", g_mark_off, "voice chat: off");
    }

    if (_first_time_setup_internal__ask_yes_no("Allow more than one client to connect from the same IP address?", TRUE) == FALSE)
    {
        g_server_settings.is_same_ip_address_allowed = FALSE;
        printf("%s %s\n", g_mark_ok, "same-IP clients: blocked");
    }

    if (_first_time_setup_internal__ask_yes_no("Restart the server automatically if it crashes?", FALSE) == TRUE)
    {
        g_server_settings.restart_on_crash = TRUE;
        printf("%s %s\n", g_mark_ok, "auto-restart: on (relaunches on crash; times logged to crashes.txt)");
    }

    // the questions below all serve the same thing: they are what makes the app feel like a normal
    // messenger (faces, names, people you can see and write to while they are away, "is typing").
    // identities leads the group because the four questions after it are only asked when it is on -
    // it used to be asked earlier, so turning it off silently swallowed features promised right here
    printf("\n%s %s\n", g_mark_info, "3/4  App comfort - what makes the phone app pleasant to use.");
    printf("%s %s\n", "   ", "avatars, display names, staying connected in your pocket, seeing people who are");
    printf("%s %s\n", "   ", "offline, writing to them, and \"x is typing ...\".");
    printf("%s %s\n", "   ", "yes to all of them gives the best app experience. none of them is required.");
    printf("%s %s\n", "   ", "the first question is the one most of the others depend on.");
    printf("%s %s\n\n", "   ", "all of these are changeable later in server settings, except where a question says otherwise.");

    if (_first_time_setup_internal__ask_yes_no("Remember identities, so each user keeps their tags across reconnects?", TRUE) == FALSE)
    {
        g_server_settings.are_identities_enabled = FALSE;
        printf("%s %s\n", g_mark_off, "identities: off");
        printf("%s %s\n", "   ", "display names, the people list, last seen and offline messages all need identities - skipping those");
    }

    if (_first_time_setup_internal__ask_yes_no("Allow idle clients, so the phone app can hold its connection in the background?", TRUE) == FALSE)
    {
        g_server_settings.is_idle_mode_allowed = FALSE;
        printf("%s %s\n", g_mark_off, "idle clients: off");
    }

    if (_first_time_setup_internal__ask_yes_no("Show country flags next to clients?", FALSE) == TRUE)
    {
        g_server_settings.is_display_country_flags_active = TRUE;
        printf("%s %s\n", g_mark_ok, "country flags: on");
    }

    if (_first_time_setup_internal__ask_yes_no("Allow users to set an image avatar, kept with their identity?", FALSE) == TRUE)
    {
        g_server_settings.allow_avatars = TRUE;
        printf("%s %s\n", g_mark_ok, "avatars: on");

        clib__null_memory(input, sizeof(input));
        printf("%s %s", g_mark_ask, "Max avatar image size in KB (blank = 50, capped at 90): ");
        fgets(input, sizeof(input), stdin);
        clib__sanitize_stdin(input);
        g_server_settings.avatar_max_size_bytes = (int64)strtol(input, 0, 10) * 1024;
        if (g_server_settings.avatar_max_size_bytes < 1024)
        {
            g_server_settings.avatar_max_size_bytes = 51200;
        }
        else if (g_server_settings.avatar_max_size_bytes > 92160)
        {
            g_server_settings.avatar_max_size_bytes = 92160;
        }
        printf("%s %s%lld%s\n", g_mark_ok, "max avatar size: ", (long long)(g_server_settings.avatar_max_size_bytes / 1024), " KB");
    }
    clib__null_memory(input, sizeof(input));

    // aliases ride on identities - without them there is nothing persistent to attach the name to
    if (g_server_settings.are_identities_enabled == TRUE)
    {

        if (_first_time_setup_internal__ask_yes_no("Allow admins to register aliases (display names) for identities?", FALSE) == TRUE)
        {
            g_server_settings.allow_alias_registrations = TRUE;
            printf("%s %s\n", g_mark_ok, "alias registrations: on");
        }
        else
        {
            printf("%s %s\n", "   ", "the people list, last seen and offline messages are labelled by alias - skipping those");
        }

        // the stored-clients list is keyed and labelled by alias, so it is only useful with aliases on
        if (g_server_settings.allow_alias_registrations == TRUE)
        {

            if (_first_time_setup_internal__ask_yes_no("Let users list the people registered here (alias, avatar, tags), so they see them while offline?", FALSE) == TRUE)
            {
                g_server_settings.allow_stored_clients_list = TRUE;
                printf("%s %s\n", g_mark_ok, "stored clients list: on");
            }
            else
            {
                printf("%s %s\n", "   ", "last seen and offline messages both hang off that list - skipping those");
            }

            // last seen only makes sense next to that list - it labels the offline entries
            if (g_server_settings.allow_stored_clients_list == TRUE)
            {

                if (_first_time_setup_internal__ask_yes_no("Record when each identity last connected, so users see \"last seen\" on offline people?", FALSE) == TRUE)
                {
                    g_server_settings.allow_last_seen = TRUE;
                    printf("%s %s\n", g_mark_ok, "last seen: on");
                }

                // offline messages ride on that same list. this one is asked ONCE, here, and is not
                // editable later: saying yes makes the server keep each identity's raw public key
                // (a peer cannot encrypt to somebody who is not connected without it)
                printf("%s %s\n", "   ", "the server holds them in ram and delivers them on reconnect (lost if the server restarts),");
                printf("%s %s\n", "   ", "and it keeps each identity's public key so senders can encrypt to them while away.");
                printf("%s %s\n", "   ", "NOT changeable later - this is the one answer you are stuck with.");

                if (_first_time_setup_internal__ask_yes_no("Allow users to send messages to registered people who are offline?", FALSE) == TRUE)
                {
                    g_server_settings.allow_offline_messages = TRUE;
                    printf("%s %s\n", g_mark_ok, "offline messages: on");
                }
            }
        }
    }

    // typing indicator: no message content ever leaves with it, only "this person is writing to you",
    // but it does tell others when somebody is at the keyboard - so it stays a choice. editable later
    // in the server settings tab, unlike the offline-message question above
    printf("%s %s\n", "   ", "clients send a short-lived \"x is typing ...\" to the channel or person they write to.");
    printf("%s %s\n", "   ", "no message content goes with it.");

    if (_first_time_setup_internal__ask_yes_no("Show people when somebody is typing to them?", FALSE) == TRUE)
    {
        g_server_settings.allow_typing_indicator = TRUE;
        printf("%s %s\n", g_mark_ok, "typing indicator: on");
    }

    // chat files: pictures are always allowed, this opens the door for any file type. the file is
    // end-to-end encrypted like a picture and only ever passes through ram, but ram is the cost:
    // the server holds the upload plus a relay copy (~2x the file) until every receiver has it
    printf("%s %s\n", "   ", "files (documents, archives, ...) are end-to-end encrypted like pictures and relayed through");
    printf("%s %s\n", "   ", "ram only - never written to disk. while a file is being relayed the server holds about twice");
    printf("%s %s\n", "   ", "its size in ram, and big files upload slowly from phones.");

    if (_first_time_setup_internal__ask_yes_no("Allow users to send files (not just pictures) in chat?", FALSE) == TRUE)
    {
        g_server_settings.allow_file_uploads = TRUE;
        printf("%s %s\n", g_mark_ok, "file uploads: on");

        clib__null_memory(input, sizeof(input));
        printf("%s %s", g_mark_ask, "Max file size in MB (blank = 10, capped at 100; more than 20 is not recommended): ");
        fgets(input, sizeof(input), stdin);
        clib__sanitize_stdin(input);
        g_server_settings.file_upload_max_size_bytes = (int64)strtol(input, 0, 10) * 1024 * 1024;
        if (g_server_settings.file_upload_max_size_bytes < FILE_UPLOAD_MIN_SIZE_BYTES)
        {
            g_server_settings.file_upload_max_size_bytes = FILE_UPLOAD_DEFAULT_SIZE_BYTES;
        }
        else if (g_server_settings.file_upload_max_size_bytes > FILE_UPLOAD_MAX_SIZE_BYTES)
        {
            g_server_settings.file_upload_max_size_bytes = FILE_UPLOAD_MAX_SIZE_BYTES;
        }
        printf("%s %s%lld%s\n", g_mark_ok, "max file size: ", (long long)(g_server_settings.file_upload_max_size_bytes / (1024 * 1024)), " MB");

        if (g_server_settings.file_upload_max_size_bytes > 20 * 1024 * 1024)
        {
            printf("%s %s\n", g_mark_warn, "over 20 MB: each transfer can take ~2x that in ram and minutes to upload on a slow link");
        }
    }
    clib__null_memory(input, sizeof(input));

    printf("\n%s %s\n\n", g_mark_info, "4/4  Web access - optional, for joining from a browser instead of the app.");

    _first_time_setup_internal__prompt_stunnel_setup();


    if (_first_time_setup_internal__ask_yes_no("Serve the web client from this server, so people can join from a browser?", FALSE) == TRUE)
    {
        g_server_settings.serve_client_http = TRUE;

        if (g_server_settings.http_port == 0)
        {
            g_server_settings.http_port = 80;
        }
        while (TRUE)
        {
            clib__null_memory(input, sizeof(input));
            printf("%s %s", g_mark_ask, "HTTP port (80 is standard; choose another like 8080 if 80 is taken): ");
            if (fgets(input, sizeof(input), stdin) == NULL_POINTER)
            {
                break;
            }
            clib__sanitize_stdin(input);
            if (input[0] != 0)
            {
                g_server_settings.http_port = strtol(input, 0, 10);
            }
            if (g_server_settings.http_port <= 0 || g_server_settings.http_port > 65535)
            {
                printf("%s %s\n", g_mark_warn, "invalid port (must be 1-65535)");
                g_server_settings.http_port = 80;
                continue;
            }
            if (g_server_settings.http_port == g_server_settings.websocket_port || (g_server_settings.use_stunnel == TRUE && g_server_settings.http_port == g_server_settings.wss_port))
            {
                printf("%s %s%lld%s\n", g_mark_warn, "port ", g_server_settings.http_port, " is already used (ws/wss) - pick another");
                continue;
            }
            break;
        }

        printf("%s %s%lld%s\n", g_mark_info, "HTTP server: serving the client on port ", g_server_settings.http_port, " (port 80 may need admin/root)");

        if (g_server_settings.use_stunnel == TRUE)
        {
            if (_first_time_setup_internal__ask_yes_no("Also serve this page over HTTPS here (stunnel), so you don't need apache/nginx?", FALSE) == TRUE)
            {
                g_server_settings.serve_https = TRUE;
                if (g_server_settings.https_port == 0)
                {
                    g_server_settings.https_port = 443;
                }
                while (TRUE)
                {
                    clib__null_memory(input, sizeof(input));
                    printf("%s %s%lld%s", g_mark_ask, "HTTPS port [default ", g_server_settings.https_port, "]: ");
                    if (fgets(input, sizeof(input), stdin) == NULL_POINTER)
                    {
                        break;
                    }
                    clib__sanitize_stdin(input);
                    if (input[0] != 0)
                    {
                        g_server_settings.https_port = strtol(input, 0, 10);
                    }
                    if (g_server_settings.https_port <= 0 || g_server_settings.https_port > 65535)
                    {
                        printf("%s %s\n", g_mark_warn, "invalid port (must be 1-65535)");
                        g_server_settings.https_port = 443;
                        continue;
                    }
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

        if (_first_time_setup_internal__ask_yes_no("Embed connection details so the served page connects automatically?", FALSE) == TRUE)
        {
            g_server_settings.embed_client_config = TRUE;
            printf("%s %s\n", g_mark_ok, "autoconnect on: the served page carries the connection details");
        }
        else
        {
            printf("%s %s\n", g_mark_info, "autoconnect off: the served page omits the keys; users connect manually");
        }

        // offer a default theme baked into the served client.html, scanned from the page itself
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
            printf("%s %s\n", g_mark_info, "themes baked into client.html:");
            for (i = 0; i < (uint64)client_theme_count; i++)
            {
                printf("      %llu) %s\n", (unsigned long long)(i + 1), client_themes[i]);
            }

            clib__null_memory(input, sizeof(input));
            printf("%s %s", g_mark_ask, "Default theme number for served clients (blank = the client's own default): ");
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
            printf("%s %s\n", g_mark_info, "no themes detected in client.html; served clients will use their own default");
        }

        printf("%s %s\n", "   ", "browser users get a fresh random ID on every reload unless this is on, which means");
        printf("%s %s\n", "   ", "their tags, alias and avatar do not survive a refresh.");

        if (_first_time_setup_internal__ask_yes_no("Store each user's identity string in their browser (localStorage)?", FALSE) == TRUE)
        {
            g_server_settings.persist_identity_in_localstorage = TRUE;
            printf("%s %s\n", g_mark_ok, "identity string stored in browser localStorage: on");
        }
    }
    clib__null_memory(input, sizeof(input));

    // read the answers back before writing them: the file is only redone by deleting it, so a
    // wrong answer noticed here saves the operator that round trip
    printf("\n%s %s\n", g_mark_info, "Setup summary:");
    printf("      %s%lld\n", "websocket port ....... ", g_server_settings.websocket_port);
    printf("      %s%s\n", "connect password ..... ", (g_server_settings.keys_count > 0) ? "set" : "none");
    printf("      %s%s\n", "voice chat ........... ", (g_server_settings.is_voice_chat_active == TRUE) ? "on" : "off");
    printf("      %s%s\n", "same-IP clients ...... ", (g_server_settings.is_same_ip_address_allowed == TRUE) ? "allowed" : "blocked");
    printf("      %s%lld%s%s\n", "min rsa key size ..... ", g_server_settings.minimum_rsa_key_bits, " bits",
        (g_server_settings.announce_minimum_rsa_key_bits == TRUE) ? " (announced to clients)" : " (not announced)");
    printf("      %s%s\n", "auto-restart ......... ", (g_server_settings.restart_on_crash == TRUE) ? "on" : "off");
    printf("      %s%s\n", "identities ........... ", (g_server_settings.are_identities_enabled == TRUE) ? "on" : "off");
    printf("      %s%s\n", "idle clients ......... ", (g_server_settings.is_idle_mode_allowed == TRUE) ? "on" : "off");
    printf("      %s%s\n", "country flags ........ ", (g_server_settings.is_display_country_flags_active == TRUE) ? "on" : "off");
    printf("      %s%s\n", "avatars .............. ", (g_server_settings.allow_avatars == TRUE) ? "on" : "off");
    printf("      %s%s\n", "display names ........ ", (g_server_settings.allow_alias_registrations == TRUE) ? "on" : "off");
    printf("      %s%s\n", "people list .......... ", (g_server_settings.allow_stored_clients_list == TRUE) ? "on" : "off");
    printf("      %s%s\n", "last seen ............ ", (g_server_settings.allow_last_seen == TRUE) ? "on" : "off");
    printf("      %s%s\n", "offline messages ..... ", (g_server_settings.allow_offline_messages == TRUE) ? "on" : "off");
    printf("      %s%s\n", "typing indicator ..... ", (g_server_settings.allow_typing_indicator == TRUE) ? "on" : "off");
    printf("      %s%s\n", "client renames ....... ", (g_server_settings.allow_client_renames == TRUE) ? "allowed" : "admin only");
    if (g_server_settings.allow_file_uploads == TRUE)
    {
        printf("      %s%s%lld%s\n", "file uploads ......... ", "on (max ", (long long)(g_server_settings.file_upload_max_size_bytes / (1024 * 1024)), " MB)");
    }
    else
    {
        printf("      %s%s\n", "file uploads ......... ", "off");
    }
    printf("      %s%s\n\n", "browser access ....... ", (g_server_settings.serve_client_http == TRUE) ? "on" : "off");

    _first_time_setup_internal__save_server_settings(plaintext_keys, g_server_settings.keys_count);
}
