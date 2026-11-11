#include "../util.h"
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
// settings, or set is_link_preview_allowed to true in server_settings.json. It is
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
#include "../clib/clib.h"
#include "../../third-party/dave-g-json/cJSON.h"
#include "url_resolver.h"
#include "httpclient.h"
#include "url_preview.h"
#include "../base.h"
#include "../msg/server/server_msg.h"
#include "../clib/clib_rwlock.h"
#include <stdlib.h>
#include <string.h>

#define URL_RESOLVER_WORKERS 4
#define URL_RESOLVER_QUEUE_LIMIT 32
#define URL_RESOLVER_CACHE_SIZE 16

typedef struct url_resolver_job_t
{
    struct url_resolver_job_t *next;
    uint64 client_id;
    ws_cli_conn_t *socket;
    crypto_key_t session_authentication;
    char request_id[65];
    char url[2049];
} url_resolver_job_t;

typedef struct url_resolver_cache_t
{
    char url[2049];
    uint64 expires;
    cJSON *result;
} url_resolver_cache_t;

static pthread_mutex_t g_url_resolver_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_url_resolver_changed = PTHREAD_COND_INITIALIZER;
static pthread_once_t g_url_resolver_once = PTHREAD_ONCE_INIT;
static url_resolver_job_t *g_url_resolver_head = NULL_POINTER;
static url_resolver_job_t *g_url_resolver_tail = NULL_POINTER;
static uint64 g_url_resolver_queued = 0;
static uint64 g_url_resolver_workers = 0;
static url_resolver_cache_t g_url_resolver_cache[URL_RESOLVER_CACHE_SIZE];
static uint64 g_url_resolver_cache_next = 0;

// declarations
static boole _url_resolver_internal__same_session(const url_resolver_job_t *job);
static cJSON *_url_resolver_internal__result(const char *url);
static void *_url_resolver_internal__worker(void *unused);
static void _url_resolver_internal__start(void);

/**
 * @brief Checks whether the client that requested a URL preview still has the same connection and
 *        keys.
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
static boole _url_resolver_internal__same_session(const url_resolver_job_t *job)
{
    client_t *client = NULL_POINTER;

    // check the client id
    if (job->client_id >= g_server_settings.max_client_count)
    {
        return FALSE;
    }

    // compare socket and keys
    client = &g_clients_array[job->client_id];
    return client->is_existing && client->is_authenticated && client->session_keys.is_ready && client->p_ws_connection == job->socket && clib__memcmp(&client->session_keys.authentication, &job->session_authentication, sizeof(job->session_authentication)) == 0;
}

/**
 * @brief Gets a URL preview from the cache, or downloads the page and saves a copy for later
 *        requests.
 *
 * @param const char* url -> the website URL to look up, limited to 2048 characters by the request handler
 *
 * @return cJSON* -> a new result object for the caller to delete, or NULL if allocation fails
 */
static cJSON *_url_resolver_internal__result(const char *url)
{
    uint64 index = 0;
    url_resolver_cache_t *cached_entry = NULL_POINTER;
    cJSON *cached_result = NULL_POINTER;
    url_preview_t preview = { 0 };
    boole succeeded = FALSE;
    cJSON *result = NULL_POINTER;
    url_resolver_cache_t *entry = NULL_POINTER;
    uint64 now = util__get_timestamp_ms();

    // check cached preview
    pthread_mutex_lock(&g_url_resolver_mutex);
    for (index = 0; index < URL_RESOLVER_CACHE_SIZE; index++)
    {
        cached_entry = &g_url_resolver_cache[index];
        if (cached_entry->result != NULL_POINTER && cached_entry->expires > now && clib__strcmp(cached_entry->url, url) == 0)
        {
            cached_result = cJSON_Duplicate(cached_entry->result, TRUE);
            pthread_mutex_unlock(&g_url_resolver_mutex);
            return cached_result;
        }
    }
    pthread_mutex_unlock(&g_url_resolver_mutex);

    // download missing preview
    succeeded = url_preview__resolve(url, &preview);

    // build preview response
    result = cJSON_CreateObject();
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

    // free the preview
    url_preview__free(&preview);

    // lock the cache
    pthread_mutex_lock(&g_url_resolver_mutex);

    // cache preview result
    entry = &g_url_resolver_cache[g_url_resolver_cache_next++ % URL_RESOLVER_CACHE_SIZE];
    cJSON_Delete(entry->result);
    clib__string_copy(entry->url, sizeof(entry->url), url);
    entry->expires = util__get_timestamp_ms() + (succeeded ? 600000 : 60000);
    entry->result = cJSON_Duplicate(result, TRUE);
    pthread_mutex_unlock(&g_url_resolver_mutex);
    return result;
}

/**
 * @brief Waits for URL preview requests and sends each result only to the session that requested
 *        it.
 *
 * Website downloads run without the clients lock. The connection is checked before
 * downloading and again before replying because the client may disconnect meanwhile.
 *
 * @param void* unused -> unused thread argument, passed as NULL_POINTER when the worker starts
 *
 * @return void* -> this worker runs until the server exits and does not normally return
 */
static void *_url_resolver_internal__worker(void *unused)
{
    url_resolver_job_t *job = NULL_POINTER;
    boole active = FALSE;
    cJSON *result = NULL_POINTER;

    (void)unused;

    // repeat for every request
    for (;;)
    {
        // lock the queue
        pthread_mutex_lock(&g_url_resolver_mutex);

        // wait for work
        while (g_url_resolver_head == NULL_POINTER)
        {
            pthread_cond_wait(&g_url_resolver_changed, &g_url_resolver_mutex);
        }

        // take queued request
        job = g_url_resolver_head;
        g_url_resolver_head = job->next;
        if (g_url_resolver_head == NULL_POINTER)
        {
            g_url_resolver_tail = NULL_POINTER;
        }
        g_url_resolver_queued--;
        pthread_mutex_unlock(&g_url_resolver_mutex);

        // lock the clients
        clib__read_lock(&g_clients_global_rwlock_guard);

        // check requesting session
        active = _url_resolver_internal__same_session(job) && g_server_settings.is_link_preview_allowed;
        clib__unlock(&g_clients_global_rwlock_guard);

        // get the preview
        result = active ? _url_resolver_internal__result(job->url) : NULL_POINTER;

        // lock the clients
        clib__read_lock(&g_clients_global_rwlock_guard);

        // recheck before replying
        if (_url_resolver_internal__same_session(job) != FALSE)
        {
            server_msg__chat__send_url_resolved_to_single_client(&g_clients_array[job->client_id], job->request_id, job->url, g_server_settings.is_link_preview_allowed ? result : NULL_POINTER);
        }
        clib__unlock(&g_clients_global_rwlock_guard);

        // release job memory
        cJSON_Delete(result);
        clib__memset(&job->session_authentication, 0, sizeof(job->session_authentication));
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
    uint64 index = 0;
    pthread_t thread = 0;
    pthread_attr_t attributes;

    // create thread attributes
    if (pthread_attr_init(&attributes) != 0)
    {
        return;
    }

    // start detached workers
    if (pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED) == 0)
    {
        for (index = 0; index < URL_RESOLVER_WORKERS; index++)
        {
            if (pthread_create(&thread, &attributes, _url_resolver_internal__worker, NULL_POINTER) == 0)
            {
                g_url_resolver_workers++;
            }
        }
    }

    // free thread attributes
    pthread_attr_destroy(&attributes);
}

/**
 * @brief Queues a page lookup and sends its result to the connection that requested it.
 * @param cstring url -> checked HTTP or HTTPS URL, no more than 2048 characters
 * @param cstring request_id -> checked request ID, from 1 to 64 characters
 * @param uint64 client_id -> client requesting the preview
 * @param ws_cli_conn_t* socket -> connection that sent the request
 * @attention The caller must not hold the clients lock. The queue copies the strings.
 */
void url_resolver__queue_lookup(cstring url, cstring request_id, uint64 client_id, ws_cli_conn_t *socket)
{
    client_t *client = NULL_POINTER;
    uint64 now = 0;
    url_resolver_job_t *job = NULL_POINTER;

    // validate requesting session
    clib__write_lock(&g_clients_global_rwlock_guard);
    if (client_id >= g_server_settings.max_client_count || !g_clients_array[client_id].is_existing || !g_clients_array[client_id].is_authenticated || !g_clients_array[client_id].session_keys.is_ready || g_clients_array[client_id].p_ws_connection != socket)
    {
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
    client = &g_clients_array[client_id];
    now = util__get_timestamp_ms();

    // previews are disabled
    if (!g_server_settings.is_link_preview_allowed)
    {
        server_msg__chat__send_url_resolved_to_single_client(client, request_id, url, NULL_POINTER);
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }

    // limit request frequency
    if (now - client->timestamp_last_url_lookup < 500)
    {
        server_msg__chat__send_url_resolved_to_single_client(client, request_id, url, NULL_POINTER);
        clib__unlock(&g_clients_global_rwlock_guard);
        return;
    }
    client->timestamp_last_url_lookup = now;

    // copy request details
    job = calloc(1, sizeof(*job));
    if (job != NULL_POINTER)
    {
        job->client_id = client_id;
        job->socket = client->p_ws_connection;
        job->session_authentication = client->session_keys.authentication;
        clib__string_copy(job->url, sizeof(job->url), url);
        clib__string_copy(job->request_id, sizeof(job->request_id), request_id);
    }
    else
    {
        server_msg__chat__send_url_resolved_to_single_client(client, request_id, url, NULL_POINTER);
    }

    // unlock the clients
    clib__unlock(&g_clients_global_rwlock_guard);

    // allocation failed
    if (job == NULL_POINTER)
    {
        return;
    }

    // start workers once
    pthread_once(&g_url_resolver_once, _url_resolver_internal__start);

    // lock the queue
    pthread_mutex_lock(&g_url_resolver_mutex);

    // check queue capacity
    if (g_url_resolver_workers == 0 || g_url_resolver_queued >= URL_RESOLVER_QUEUE_LIMIT)
    {
        pthread_mutex_unlock(&g_url_resolver_mutex);

        // answer without preview
        clib__read_lock(&g_clients_global_rwlock_guard);
        if (_url_resolver_internal__same_session(job) != FALSE)
        {
            server_msg__chat__send_url_resolved_to_single_client(&g_clients_array[job->client_id], job->request_id, job->url, NULL_POINTER);
        }
        clib__unlock(&g_clients_global_rwlock_guard);

        // free the job
        clib__memset(&job->session_authentication, 0, sizeof(job->session_authentication));
        free(job);
        return;
    }

    // append queued request
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

    // wake waiting worker
    pthread_cond_signal(&g_url_resolver_changed);
    pthread_mutex_unlock(&g_url_resolver_mutex);
}
