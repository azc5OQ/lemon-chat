// Draws bounded channel display data inside the selected D3D11 game.
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include "MinHook.h"
#include "meek_ui.h"
#include "parser_style.h"
#include "parser_xml.h"
#include "host_services.h"
#include "embedded_d3d11.h"
#include "overlay_ipc.h"
#include "ui/member_view.h"
#include "ui/ui_voice.h"
#include "clib.h"

typedef HRESULT(STDMETHODCALLTYPE *desktop_present_fn)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *desktop_resize_fn)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static HMODULE g_module;
static LONG g_started;
static LONG g_drawn_frames;
static CRITICAL_SECTION g_render_lock;
static desktop_present_fn g_present;
static desktop_resize_fn g_resize;
static IDXGISwapChain *g_chain;
static ID3D11Device *g_device;
static ID3D11DeviceContext *g_context;
static DWORD g_render_thread;
static boole g_ready;
static boole g_failed;
static boole g_resizing;
static desktop_overlay_ipc_t *g_ipc;
static desktop_overlay_snapshot_t g_snapshot;
static desktop_overlay_snapshot_t g_displayed;
static UINT g_viewport_height;
static char g_directory[32768];

// declarations
static void _overlay_d3d11_internal__log(meek_log_level_e level, const char *file, int line, const char *message);
static boole _overlay_d3d11_internal__scene(void);
static void _overlay_d3d11_internal__draw(IDXGISwapChain *chain);
static HRESULT STDMETHODCALLTYPE _overlay_d3d11_internal__present(IDXGISwapChain *chain, UINT interval, UINT flags);
static HRESULT STDMETHODCALLTYPE _overlay_d3d11_internal__resize(IDXGISwapChain *chain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags);

/**
 * @brief forwards overlay warnings and errors to the Windows debugger output.
 *
 * @param meek_log_level_e level -> library log level
 * @param const char* file -> library source filename
 * @param int line -> source line number
 * @param const char* message -> message text
 *
 * @return void
 */
static void _overlay_d3d11_internal__log(meek_log_level_e level, const char *file, int line, const char *message)
{
    (void)file;
    (void)line;
    if (level < MEEK_LOG__WARN)
    {
        return;
    }
    OutputDebugStringA("Lemon overlay: ");
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
}

/**
 * @brief loads the overlay font, style and scene from beside the overlay DLL.
 *
 * @return boole -> 1 when the scene and required controls are ready; 0 otherwise
 */
static boole _overlay_d3d11_internal__scene(void)
{
    char path[32768] = { 0 };
    gui_node_t *root = NULL;

    clib__null_memory(&path, sizeof(path));
    if (!font__init() || clib__string_format(path, sizeof(path), "%sfonts/SpaceGrotesk-Regular.ttf", g_directory) < 0 || !font__register_from_file("SpaceGrotesk-Regular", path))
    {
        return 0;
    }
    clib__string_format(path, sizeof(path), "%sassets/overlay.style", g_directory);
    if (!parser_style__load_styles(path))
    {
        return 0;
    }
    clib__string_format(path, sizeof(path), "%sassets/overlay.ui", g_directory);
    root = parser_xml__load_ui(path);
    if (!root)
    {
        return 0;
    }
    scene__set_root(root);
    scene__set_scale(1.0f);
    return scene__find_by_id("overlay-members") && scene__find_by_id("overlay-channel");
}

/**
 * @brief draws fresh, enabled display data and restores the game render state afterward.
 *
 * @param IDXGISwapChain* chain -> game or fixture swap chain
 *
 * @return void
 */
static void _overlay_d3d11_internal__draw(IDXGISwapChain *chain)
{
    DXGI_SWAP_CHAIN_DESC description = { 0 };
    ID3D11Texture2D *buffer = NULL;
    D3D11_TEXTURE2D_DESC texture = { 0 };
    meek_host_services_t services = { 0 };
    gui_node_t *title = NULL;
    gui_node_t *caption = NULL;
    desktop_overlay_snapshot_t visible = { 0 };
    uint maximum = 0;
    boole speaking = 0;
    uint index = 0;
    gui_node_t *members = NULL;
    gui_node_t *panel = NULL;
    float list_height = 0;
    char caption_text[64] = { 0 };
    uint omitted = 0;

    desktop_overlay_ipc__read(g_ipc, &g_snapshot);
    if (g_ready && g_chain == chain && g_render_thread == GetCurrentThreadId() && !desktop_overlay_data__visible(&g_snapshot, GetTickCount64()))
    {
        desktop_ui_voice__shutdown();
    }
    if (!desktop_overlay_data__visible(&g_snapshot, GetTickCount64()) || g_failed || g_resizing || (g_chain && (g_chain != chain || g_render_thread != GetCurrentThreadId())))
    {
        return;
    }
    clib__null_memory(&description, sizeof(description));
    if (FAILED(IDXGISwapChain_GetDesc(chain, &description)) || !description.OutputWindow || IsIconic(description.OutputWindow))
    {
        return;
    }
    buffer = NULL;
    clib__null_memory(&texture, sizeof(texture));
    if (FAILED(IDXGISwapChain_GetBuffer(chain, 0, &IID_ID3D11Texture2D, (void **)&buffer)))
    {
        return;
    }
    ID3D11Texture2D_GetDesc(buffer, &texture);
    ID3D11Texture2D_Release(buffer);
    if (!texture.Width || !texture.Height)
    {
        return;
    }
    if (!g_chain)
    {
        if (FAILED(IDXGISwapChain_GetDevice(chain, &IID_ID3D11Device, (void **)&g_device)))
        {
            return;
        }
        ID3D11Device_GetImmediateContext(g_device, &g_context);
        g_chain = chain;
        IDXGISwapChain_AddRef(g_chain);
        g_render_thread = GetCurrentThreadId();
        clib__null_memory(&services, sizeof(services));
        services.write_log = _overlay_d3d11_internal__log;
        host_services__set(&services);
        memory_manager__init();
        widget_registry__bootstrap_builtins();
        desktop_ui_voice__register();
        if (!embedded_d3d11__attach(g_device, g_context, chain))
        {
            g_failed = 1;
            return;
        }
    }
    if (!embedded_d3d11__begin_frame(texture.Width, texture.Height))
    {
        return;
    }
    if (!g_ready)
    {
        g_ready = _overlay_d3d11_internal__scene();
        if (!g_ready)
        {
            g_failed = 1;
            embedded_d3d11__end_frame();
            return;
        }
    }
    if (g_displayed.revision != g_snapshot.revision || g_displayed.member_count != g_snapshot.member_count || g_displayed.preview != g_snapshot.preview || g_displayed.total_member_count != g_snapshot.total_member_count || clib__memcmp(g_displayed.members, g_snapshot.members, sizeof(g_snapshot.members)) != 0 || clib__strcmp(g_displayed.channel, g_snapshot.channel) != 0 || g_viewport_height != texture.Height)
    {
        title = scene__find_by_id("overlay-channel");
        caption = scene__find_by_id("overlay-caption");
        scene__set_text(title, g_snapshot.channel, clib__strlen(g_snapshot.channel));
        visible = g_snapshot;
        maximum = texture.Height > 240 ? (texture.Height / 3 - 64) / 24 : 1;
        if (maximum > 8)
        {
            maximum = 8;
        }
        if (!maximum)
        {
            maximum = 1;
        }
        if (visible.member_count > maximum)
        {
            visible.member_count = 0;
            // Keep speakers visible when the channel is taller than the viewport.
            for (speaking = 1; speaking >= 0; speaking--)
            {
                for (index = 0; index < g_snapshot.member_count && visible.member_count < maximum; index++)
                {
                    if (((g_snapshot.members[index].flags & DESKTOP_OVERLAY_SPEAKING) != 0) == speaking)
                    {
                        visible.members[visible.member_count++] = g_snapshot.members[index];
                    }
                }
            }
        }
        members = scene__find_by_id("overlay-members");
        panel = scene__find_by_id("overlay-panel");
        omitted = g_snapshot.total_member_count - visible.member_count;
        if (caption)
        {
            if (omitted)
            {
                clib__string_format(caption_text, sizeof(caption_text), "+%u more%s", omitted, g_snapshot.preview ? " / preview" : "");
            }
            else if (g_snapshot.preview)
            {
                clib__string_copy(caption_text, sizeof(caption_text), "Preview");
            }
            scene__set_text(caption, caption_text, clib__strlen(caption_text));
            clib__string_copy(caption->klass, sizeof(caption->klass), caption_text[0] ? "" : "hidden");
        }
        desktop_member_view__update(members, &visible);
        list_height = visible.member_count ? (float)visible.member_count * 24.0f : 24.0f;
        members->style[GUI_NODE_STATE__DEFAULT].size_h = list_height;
        members->style[GUI_NODE_STATE__DEFAULT].size_h_explicit = TRUE;
        if (panel)
        {
            panel->style[GUI_NODE_STATE__DEFAULT].size_h = list_height + 42.0f + (caption_text[0] ? 20.0f : 0.0f);
            panel->style[GUI_NODE_STATE__DEFAULT].size_h_explicit = TRUE;
        }
        g_displayed = g_snapshot;
        g_viewport_height = texture.Height;
    }
    scene__begin_frame_time((int64)GetTickCount64());
    scene__resolve_styles();
    animator__tick();
    scene__layout(texture.Width, texture.Height);
    scene__emit_draws();
    embedded_d3d11__end_frame();
    InterlockedIncrement(&g_drawn_frames);
}

/**
 * @brief draws the overlay when its render lock is available, then forwards Present.
 *
 * @param IDXGISwapChain* chain -> game or fixture swap chain
 * @param UINT interval -> presentation sync interval
 * @param UINT flags -> flags passed by the game
 *
 * @return HRESULT STDMETHODCALLTYPE -> the original swap-chain Present result
 */
static HRESULT STDMETHODCALLTYPE _overlay_d3d11_internal__present(IDXGISwapChain *chain, UINT interval, UINT flags)
{
    if (!(flags & DXGI_PRESENT_TEST) && TryEnterCriticalSection(&g_render_lock))
    {
        _overlay_d3d11_internal__draw(chain);
        LeaveCriticalSection(&g_render_lock);
    }
    return g_present(chain, interval, flags);
}

/**
 * @brief releases the overlay back-buffer view before forwarding a game resize.
 *
 * @param IDXGISwapChain* chain -> game or fixture swap chain
 * @param UINT count -> requested back-buffer count
 * @param UINT width -> requested buffer width
 * @param UINT height -> requested buffer height
 * @param DXGI_FORMAT format -> requested buffer format
 * @param UINT flags -> flags passed by the game
 *
 * @return HRESULT STDMETHODCALLTYPE -> the original ResizeBuffers result
 */
static HRESULT STDMETHODCALLTYPE _overlay_d3d11_internal__resize(IDXGISwapChain *chain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
    boole owned = 0;
    HRESULT result = 0;

    EnterCriticalSection(&g_render_lock);
    owned = g_chain == chain;
    if (owned)
    {
        g_resizing = 1;
        embedded_d3d11__release_render_target();
    }
    LeaveCriticalSection(&g_render_lock);
    result = g_resize(chain, count, width, height, format, flags);
    if (owned)
    {
        EnterCriticalSection(&g_render_lock);
        g_resizing = 0;
        LeaveCriticalSection(&g_render_lock);
    }
    return result;
}

// Called explicitly after LoadLibrary completes; DllMain never creates hooks or threads.
/**
 * @brief installs D3D11 hooks after LoadLibrary has completed, outside DllMain.
 *
 * @param void* ignored -> unused remote-thread argument
 *
 * @return BOOL WINAPI -> 1 when hooks are ready; 0 if startup failed or no enabled snapshot exists
 */
__declspec(dllexport) DWORD WINAPI lemon_overlay_start(void *ignored)
{
    LONG previous = 0;
    wchar_t wide_path[32768] = { 0 };
    DWORD length = 0;
    HWND window = NULL;
    IDXGISwapChain *chain = NULL;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    DXGI_SWAP_CHAIN_DESC description = { 0 };
    HRESULT result = 0;
    boole success = 0;
    void *present = NULL;
    void *resize = NULL;
    HMODULE pinned = NULL;

    (void)ignored;
    previous = InterlockedCompareExchange(&g_started, 1, 0);
    if (previous)
    {
        return previous == 2;
    }
    g_ipc = desktop_overlay_ipc__open(GetCurrentProcessId(), 0);
    if (!g_ipc || !desktop_overlay_ipc__read(g_ipc, &g_snapshot) || !desktop_overlay_data__visible(&g_snapshot, GetTickCount64()))
    {
        desktop_overlay_ipc__close(g_ipc);
        g_ipc = NULL;
        InterlockedExchange(&g_started, 0);
        return 0;
    }
    clib__null_memory(&wide_path, sizeof(wide_path));
    length = GetModuleFileNameW(g_module, wide_path, 32768);
    if (!length || length >= 32768)
    {
        InterlockedExchange(&g_started, -1);
        return 0;
    }
    while (length && wide_path[length - 1] != L'\\')
    {
        length--;
    }
    wide_path[length] = 0;
    if (!WideCharToMultiByte(CP_UTF8, 0, wide_path, -1, g_directory, sizeof(g_directory), NULL, NULL))
    {
        InterlockedExchange(&g_started, -1);
        return 0;
    }
    InitializeCriticalSection(&g_render_lock);
    window = CreateWindowExW(0, L"STATIC", L"Lemon overlay renderer discovery", WS_OVERLAPPED, 0, 0, 64, 64, NULL, NULL, g_module, NULL);
    chain = NULL;
    device = NULL;
    context = NULL;
    clib__null_memory(&description, sizeof(description));
    description.BufferDesc.Width = 64;
    description.BufferDesc.Height = 64;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 1;
    description.OutputWindow = window;
    description.Windowed = TRUE;
    result = E_FAIL;
    if (window)
    {
        result = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &description, &chain, &device, NULL, &context);
        if (FAILED(result))
        {
            result = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &description, &chain, &device, NULL, &context);
        }
    }
    success = 0;
    if (SUCCEEDED(result) && MH_Initialize() == MH_OK)
    {
        present = (void *)chain->lpVtbl->Present;
        resize = (void *)chain->lpVtbl->ResizeBuffers;
        if (MH_CreateHook(present, _overlay_d3d11_internal__present, (void **)&g_present) == MH_OK && MH_CreateHook(resize, _overlay_d3d11_internal__resize, (void **)&g_resize) == MH_OK && MH_EnableHook(resize) == MH_OK && MH_EnableHook(present) == MH_OK)
        {
            pinned = NULL;
            success = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)lemon_overlay_start, &pinned) != 0;
        }
        if (!success)
        {
            MH_DisableHook(present);
            MH_DisableHook(resize);
            MH_Uninitialize();
        }
    }
    if (context)
    {
        ID3D11DeviceContext_Release(context);
    }
    if (chain)
    {
        IDXGISwapChain_Release(chain);
    }
    if (device)
    {
        ID3D11Device_Release(device);
    }
    if (window)
    {
        DestroyWindow(window);
    }
    InterlockedExchange(&g_started, success ? 2 : -1);
    return success;
}

/**
 * @brief reads the number of overlay frames drawn for the bounded rendering check.
 *
 * @return BOOL WINAPI -> the frame count
 */
__declspec(dllexport) DWORD WINAPI lemon_overlay_frames(void)
{
    return (DWORD)InterlockedCompareExchange(&g_drawn_frames, 0, 0);
}

/**
 * @brief records the module handle without initializing rendering or creating threads under the loader lock.
 *
 * @param HINSTANCE instance -> overlay module instance
 * @param DWORD reason -> Windows DLL notification
 * @param LPVOID reserved -> unused Windows loader data
 *
 * @return BOOL WINAPI -> TRUE
 */
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
