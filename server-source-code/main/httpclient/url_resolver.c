// This file starts URL preview lookups when a client posts a website link in
// chat. The feature must be enabled in the server settings. It arranges for
// the page's title, description and picture to be downloaded and sent back
// to the client, which displays them below the message.
//
// Four background threads fetch pages while normal chat messages continue to
// be handled. A small queue limits how many lookups can wait, and a cache keeps
// recent results so repeated links do not need to be downloaded again.
//
// The administrator can enable "show link previews" in the general server
// settings, or set allow_link_previews to true in server_settings.json. It is
// off by default. Clients send a resolve_url message containing a URL and a
// request_id. The server replies with url_resolved and the same request_id,
// using the normal metadata encryption between that client and this server.
// The reply's status is ok, disabled, or unavailable. Successful replies can
// contain a title, description, site name and image bytes for the preview card.
//
// The server learns the requested URL, including when it came from a private
// message or a password-protected channel. The chat message itself keeps its
// existing encryption. The result goes only to the client that requested it.
// No website lookup holds the chat server's client lock while waiting.
//
// There can be 32 waiting requests, and one client can submit a request every
// 500 ms. The cache holds 16 results: successful lookups stay for ten minutes,
// and failed lookups stay for one minute. A queued result is discarded if its
// client disconnected or replaced the connection while the lookup was running.
#include "../definitions.h"
#include "../../third-party/dave-g-json/cJSON.h"
#include "url_resolver.h"
#include "httpclient.h"
#include "url_preview.h"
#include "../base.h"
#include "../server_message.h"
#include "../clib/clib_rwlock.h"
#include <stdlib.h>
#include <string.h>

#define URL_RESOLVER_WORKERS 4
#define URL_RESOLVER_QUEUE_LIMIT 32
#define URL_RESOLVER_CACHE_SIZE 16

typedef struct url_resolver_job_t
{
    struct url_resolver_job_t* next;
    uint64 client_id;
    ws_cli_conn_t* socket;
    crypto_key_t session_authentication;
    char request_id[65];
    char url[2049];
} url_resolver_job_t;

typedef struct url_resolver_cache_t
{
    char url[2049];
    uint64 expires;
    cJSON* result;
} url_resolver_cache_t;

static pthread_mutex_t g_url_resolver_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_url_resolver_changed = PTHREAD_COND_INITIALIZER;
static pthread_once_t g_url_resolver_once = PTHREAD_ONCE_INIT;
static url_resolver_job_t* g_url_resolver_head = NULL_POINTER;
static url_resolver_job_t* g_url_resolver_tail = NULL_POINTER;
static unsigned int g_url_resolver_queued = 0;
static unsigned int g_url_resolver_workers = 0;
static url_resolver_cache_t g_url_resolver_cache[URL_RESOLVER_CACHE_SIZE];
static unsigned int g_url_resolver_cache_next = 0;

// declarations
static boole _url_resolver_internal__same_session(const url_resolver_job_t* job);
static cJSON* _url_resolver_internal__result(const char* url);
static void* _url_resolver_internal__worker(void* unused);
static void _url_resolver_internal__start(void);

/**
 * @brief Checks whether the client that requested a URL preview still has the same connection and keys.
 *
 * The client array can reuse a disconnected client's slot. Checking its socket and keys
 * prevents a finished preview from reaching another person or a replacement connection.
 *
 * @param const url_resolver_job_t* job -> the queued request and the connection details saved with it
 *
 * @attention The caller holds the clients read lock while checking the client slot.
 *
 * @return boole -> TRUE when the original authenticated session still exists, otherwise FALSE
 */
static boole _url_resolver_internal__same_session(const url_resolver_job_t* job)
{
    if (job->client_id >= g_server_settings.max_client_count)
    {
        return FALSE;
    }
    client_t* client = &g_clients_array[job->client_id];
    return client->is_existing && client->is_authenticated && client->session_keys.is_ready
        && client->p_ws_connection == job->socket
        && memcmp(&client->session_keys.authentication, &job->session_authentication, sizeof(job->session_authentication)) == 0;
}

/**
 * @brief Gets a URL preview from the cache, or downloads the page and saves a copy for later requests.
 *
 * @param const char* url -> the website URL to look up, limited to 2048 characters by the request handler
 *
 * @return cJSON* -> a new result object for the caller to delete, or NULL if allocation fails
 */
static cJSON* _url_resolver_internal__result(const char* url)
{
    uint64 now = base__get_timestamp_ms();
    pthread_mutex_lock(&g_url_resolver_mutex);
    for (unsigned int index = 0; index < URL_RESOLVER_CACHE_SIZE; index++)
    {
        url_resolver_cache_t* entry = &g_url_resolver_cache[index];
        if (entry->result != NULL_POINTER && entry->expires > now && strcmp(entry->url, url) == 0)
        {
            cJSON* result = cJSON_Duplicate(entry->result, TRUE);
            pthread_mutex_unlock(&g_url_resolver_mutex);
            return result;
        }
    }
    pthread_mutex_unlock(&g_url_resolver_mutex);

    url_preview_t preview;
    int succeeded = url_preview__resolve(url, &preview);
    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", succeeded ? "ok" : "unavailable");
    cJSON_AddStringToObject(result, "url", url);
    if (succeeded)
    {
        cJSON_AddStringToObject(result, "resolved_url", preview.url);
        cJSON_AddStringToObject(result, "title", preview.title);
        cJSON_AddStringToObject(result, "description", preview.description);
        cJSON_AddStringToObject(result, "site_name", preview.site_name);
        if (preview.image_data != NULL_POINTER)
        {
            cJSON_AddStringToObject(result, "image", preview.image_data);
        }
    }
    url_preview__free(&preview);

    pthread_mutex_lock(&g_url_resolver_mutex);
    url_resolver_cache_t* entry = &g_url_resolver_cache[g_url_resolver_cache_next++ % URL_RESOLVER_CACHE_SIZE];
    cJSON_Delete(entry->result);
    strcpy(entry->url, url);
    entry->expires = base__get_timestamp_ms() + (succeeded ? 600000 : 60000);
    entry->result = cJSON_Duplicate(result, TRUE);
    pthread_mutex_unlock(&g_url_resolver_mutex);
    return result;
}

/**
 * @brief Waits for URL preview requests and sends each result only to the session that requested it.
 *
 * Website downloads run without the clients lock. The connection is checked before
 * downloading and again before replying because the client may disconnect meanwhile.
 *
 * @param void* unused -> unused thread argument, passed as NULL_POINTER when the worker starts
 *
 * @return void* -> this worker runs until the server exits and does not normally return
 */
static void* _url_resolver_internal__worker(void* unused)
{
    (void)unused;
    for (;;)
    {
        pthread_mutex_lock(&g_url_resolver_mutex);
        while (g_url_resolver_head == NULL_POINTER)
        {
            pthread_cond_wait(&g_url_resolver_changed, &g_url_resolver_mutex);
        }
        url_resolver_job_t* job = g_url_resolver_head;
        g_url_resolver_head = job->next;
        if (g_url_resolver_head == NULL_POINTER)
        {
            g_url_resolver_tail = NULL_POINTER;
        }
        g_url_resolver_queued--;
        pthread_mutex_unlock(&g_url_resolver_mutex);

        clib__read_lock(&g_clients_global_rwlock_guard);
        boole active = _url_resolver_internal__same_session(job) && g_server_settings.allow_link_previews;
        clib__unlock(&g_clients_global_rwlock_guard);
        cJSON* result = active ? _url_resolver_internal__result(job->url) : NULL_POINTER;

        clib__read_lock(&g_clients_global_rwlock_guard);
        if (_url_resolver_internal__same_session(job))
        {
            server_msg__send_url_resolved_to_single_client(&g_clients_array[job->client_id], job->request_id, job->url,
                g_server_settings.allow_link_previews ? result : NULL_POINTER);
        }
        clib__unlock(&g_clients_global_rwlock_guard);
        cJSON_Delete(result);
        memset(&job->session_authentication, 0, sizeof(job->session_authentication));
        free(job);
    }
    return NULL_POINTER;
}

/**
 * @brief Starts the background threads that handle queued URL preview requests.
 *
 * Called once through pthread_once. The worker count records how many threads
 * started successfully, so requests can be refused when no worker is available.
 *
 * @return void
 */
static void _url_resolver_internal__start(void)
{
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0)
    {
        return;
    }
    if (pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED) == 0)
    {
        for (unsigned int index = 0; index < URL_RESOLVER_WORKERS; index++)
        {
            pthread_t thread;
            if (pthread_create(&thread, &attributes, _url_resolver_internal__worker, NULL_POINTER) == 0)
            {
                g_url_resolver_workers++;
            }
        }
    }
    pthread_attr_destroy(&attributes);
}

/**
 * @brief Checks a client URL preview request and queues a website lookup when the server allows previews.
 *
 * The job copies its request and session details; workers reply only to that session.
 * Invalid requests are ignored; disabled or busy lookups return an empty preview.
 * The caller must not hold the clients lock.
 *
 * @param cJSON* json_root -> the parsed message containing the URL and request ID; the caller retains ownership
 * @param uint64 client_id -> the client slot that sent the request
 * @param ws_cli_conn_t* socket -> the original WebSocket connection, checked before accepting the request
 *
 * @return void
 */
void url_resolver__request(cJSON* json_root, uint64 client_id, ws_cli_conn_t* socket)
{
    cJSON* message = cJSON_GetObjectItemCaseSensitive(json_root, "message");
    cJSON* url = cJSON_GetObjectItemCaseSensitive(message, "url");
    cJSON* request_id = cJSON_GetObjectItemCaseSensitive(message, "request_id");
    httpclient_url_t parsed;
    if (!cJSON_IsString(url) || !cJSON_IsString(request_id)
        || strlen(url->valuestring) > 2048 || !request_id->valuestring[0] || strlen(request_id->valuestring) > 64
        || httpclient__parse_url(url->valuestring, &parsed) != HTTPCLIENT_OK)
    {
        return;
    }
    clib__write_lock(&g_clients_global_rwlock_guard);
    if (client_id >= g_server_settings.max_client_count || !g_clients_array[client_id].is_existing
        || !g_clients_array[client_id].is_authenticated || !g_clients_array[client_id].session_keys.is_ready
        || g_clients_array[client_id].p_ws_connection != socket)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
    client_t* client = &g_clients_array[client_id];
    uint64 now = base__get_timestamp_ms();
    if (!g_server_settings.allow_link_previews)
    {
        server_msg__send_url_resolved_to_single_client(client, request_id->valuestring, url->valuestring, NULL_POINTER);
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
    if (now - client->timestamp_last_url_lookup < 500)
    {
        server_msg__send_url_resolved_to_single_client(client, request_id->valuestring, url->valuestring, NULL_POINTER);
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
    client->timestamp_last_url_lookup = now;
    url_resolver_job_t* job = calloc(1, sizeof(*job));
    if (job != NULL_POINTER)
    {
        job->client_id = client_id;
        job->socket = client->p_ws_connection;
        job->session_authentication = client->session_keys.authentication;
        strcpy(job->url, url->valuestring);
        strcpy(job->request_id, request_id->valuestring);
    }
    else
    {
        server_msg__send_url_resolved_to_single_client(client, request_id->valuestring, url->valuestring, NULL_POINTER);
    }
    clib__unlock(&g_clients_global_rwlock_guard);
    if (job == NULL_POINTER)
    {
        return;
    }
    pthread_once(&g_url_resolver_once, _url_resolver_internal__start);
    pthread_mutex_lock(&g_url_resolver_mutex);
    if (g_url_resolver_workers == 0 || g_url_resolver_queued >= URL_RESOLVER_QUEUE_LIMIT)
    {
        pthread_mutex_unlock(&g_url_resolver_mutex);
        clib__read_lock(&g_clients_global_rwlock_guard);
        if (_url_resolver_internal__same_session(job))
        {
            server_msg__send_url_resolved_to_single_client(&g_clients_array[job->client_id], job->request_id, job->url, NULL_POINTER);
        }
        clib__unlock(&g_clients_global_rwlock_guard);
        memset(&job->session_authentication, 0, sizeof(job->session_authentication));
        free(job);
        return;
    }
    if (g_url_resolver_tail == NULL_POINTER)
    {
        g_url_resolver_head = job;
    }
    else
    {
        g_url_resolver_tail->next = job;
    }
    g_url_resolver_tail = job;
    g_url_resolver_queued++;
    pthread_cond_signal(&g_url_resolver_changed);
    pthread_mutex_unlock(&g_url_resolver_mutex);
}
