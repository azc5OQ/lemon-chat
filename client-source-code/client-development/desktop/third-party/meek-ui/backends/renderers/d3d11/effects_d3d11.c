// Optional Windows compute engine. All work stays on the caller's drawing
// thread and an independently owned hardware device; host state is untouched.
#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include "clib/stdlib.h"
#include "effect_acceleration.h"
#include "effects_profile.h"
#include "third_party/log.h"
#include "shaders/generated/compose.h"
#include "shaders/generated/background.h"

typedef HRESULT(WINAPI* effects_create_device_fn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                  const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef HRESULT(WINAPI* effects_compile_fn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
                                            ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

typedef struct effects_texture_t
{
    ID3D11Texture2D* texture;
    ID3D11Texture2D* staging;
    ID3D11ShaderResourceView* srv;
    ID3D11UnorderedAccessView* uav;
    int width, height;
} effects_texture_t;

// Only immutable CPU decoration content is admitted. The byte budget includes
// padded texture dimensions; dynamic sources keep the reusable upload texture.
#define EFFECTS_SOURCE_CACHE_CAP 8
#define EFFECTS_SOURCE_CACHE_BYTES (32 * 1024 * 1024)
typedef struct effects_cached_source_t
{
    effects_texture_t texture;
    uint64 version;
    uint64 used;
    int width, height;
} effects_cached_source_t;

typedef struct effects_parameters_t
{
    float inverse[3][4];
    int source[4];
    int region[4];
    float options[4];
} effects_parameters_t;

typedef struct effects_background_layer_t
{
    float tile[4];
    int info[4];
    float geometry0[4], geometry1[4];
    int extra[4];
    gui_color_t solid;
    float positions[4][4];
    gui_color_t colors[16];
} effects_background_layer_t;

typedef struct effects_background_parameters_t
{
    int region[4];
    gui_rect_t bounds;
    gui_color_t base;
    float options[4];
    float composition[4];
    effects_background_layer_t layers[EFFECT_BACKGROUND_LAYER_CAP];
} effects_background_parameters_t;

_Static_assert(sizeof(effects_background_layer_t) == 416, "HLSL background layer layout");
_Static_assert(sizeof(effects_background_parameters_t) == 6736, "HLSL background constant layout");
_Static_assert(GUI_GRADIENT_MAX_STOPS == 16 && EFFECT_BACKGROUND_LAYER_CAP == 16, "HLSL background limits");
_Static_assert(GUI_GRADIENT_KIND__LINEAR == 0 && GUI_GRADIENT_KIND__RADIAL == 1 && GUI_GRADIENT_KIND__CONIC == 2, "HLSL gradient kinds");

static HMODULE effects_runtime, effects_compiler;
static ID3D11Device* effects_device;
static ID3D11DeviceContext* effects_context;
static ID3D11ComputeShader* effects_shader;
static ID3D11Buffer* effects_constants;
static ID3D11ComputeShader* effects_background_shader;
static ID3D11Buffer* effects_background_constants;
static effects_texture_t effects_source, effects_output;
static effects_cached_source_t effects_cached_sources[EFFECTS_SOURCE_CACHE_CAP];
static uint64 effects_source_clock;
static uint64 effects_source_hits;
static int64 effects_source_bytes;
static int64 effects_source_peak;
static boole effects_failed;
static unsigned long long effects_dispatches;
static unsigned long long effects_background_dispatches;
static unsigned long long effects_fused_dispatches;

#ifdef MEEK_UI_PROFILE_GPU_CALLS
static struct
{
    long long ticks, maximum;
    unsigned long long calls;
} effects_profile_stats[EFFECTS_PROFILE_STAGE_COUNT];

long long effects_profile__now(void)
{
    LARGE_INTEGER stamp;
    QueryPerformanceCounter(&stamp);
    return stamp.QuadPart;
}

void effects_profile__step(enum effects_profile_stage stage, long long* previous)
{
    long long now = effects_profile__now();
    long long elapsed = now - *previous;
    effects_profile_stats[stage].ticks += elapsed;
    if (elapsed > effects_profile_stats[stage].maximum)
    {
        effects_profile_stats[stage].maximum = elapsed;
    }
    effects_profile_stats[stage].calls++;
    *previous = now;
}

void effects_profile__reset(void)
{
    stdlib__memset(effects_profile_stats, 0, sizeof(effects_profile_stats));
}

static void effects_profile_report(void)
{
    static const char* names[] = {"background_upload", "background_submit", "background_map", "background_copy",
                                  "compose_upload", "compose_submit", "compose_map", "compose_copy", "present",
                                  "readback_copy", "readback_map", "layer_upload"};
    LARGE_INTEGER frequency;
    int i;
    QueryPerformanceFrequency(&frequency);
    for (i = 0; i < EFFECTS_PROFILE_STAGE_COUNT; i++)
    {
        if (effects_profile_stats[i].calls)
        {
            log_info("GPU API profile %s: calls %llu total_ms %.3f maximum_ms %.3f", names[i], effects_profile_stats[i].calls,
                     effects_profile_stats[i].ticks * 1000.0 / frequency.QuadPart,
                     effects_profile_stats[i].maximum * 1000.0 / frequency.QuadPart);
        }
    }
    effects_profile__reset();
}
#endif

#define EFFECTS_RELEASE(object, interface) \
    do                                     \
    {                                      \
        if (object)                        \
        {                                  \
            interface##_Release(object);   \
            object = NULL;                 \
        }                                  \
    } while (0)

static void effects_release_texture(effects_texture_t* texture)
{
    EFFECTS_RELEASE(texture->srv, ID3D11ShaderResourceView);
    EFFECTS_RELEASE(texture->uav, ID3D11UnorderedAccessView);
    EFFECTS_RELEASE(texture->staging, ID3D11Texture2D);
    EFFECTS_RELEASE(texture->texture, ID3D11Texture2D);
    stdlib__memset(texture, 0, sizeof(*texture));
}

void effects_d3d11__shutdown(void)
{
#ifdef MEEK_UI_PROFILE_GPU_CALLS
    effects_profile_report();
#endif
    if (effects_context)
    {
        ID3D11DeviceContext_ClearState(effects_context);
    }
    for (int i = 0; i < EFFECTS_SOURCE_CACHE_CAP; i++)
    {
        effects_release_texture(&effects_cached_sources[i].texture);
    }
    stdlib__memset(effects_cached_sources, 0, sizeof(effects_cached_sources));
    if (effects_source_peak)
    {
        log_info("D3D11 immutable source cache: %llu reused uploads, peak_texture_bytes=%lld", (unsigned long long)effects_source_hits, (long long)effects_source_peak);
    }
    effects_source_clock = effects_source_hits = 0;
    effects_source_bytes = effects_source_peak = 0;
    effects_release_texture(&effects_source);
    effects_release_texture(&effects_output);
    EFFECTS_RELEASE(effects_constants, ID3D11Buffer);
    EFFECTS_RELEASE(effects_shader, ID3D11ComputeShader);
    EFFECTS_RELEASE(effects_background_constants, ID3D11Buffer);
    EFFECTS_RELEASE(effects_background_shader, ID3D11ComputeShader);
    EFFECTS_RELEASE(effects_context, ID3D11DeviceContext);
    EFFECTS_RELEASE(effects_device, ID3D11Device);
    if (effects_compiler)
    {
        FreeLibrary(effects_compiler);
    }
    if (effects_runtime)
    {
        FreeLibrary(effects_runtime);
    }
    effects_runtime = effects_compiler = NULL;
    if (effects_dispatches)
    {
        log_info("D3D11 compute effects: %llu compositions", effects_dispatches);
    }
    if (effects_background_dispatches)
    {
        log_info("D3D11 compute backgrounds: %llu paints", effects_background_dispatches);
    }
    if (effects_fused_dispatches)
    {
        log_info("D3D11 fused backgrounds: %llu compositions", effects_fused_dispatches);
    }
    effects_dispatches = 0;
    effects_background_dispatches = 0;
    effects_fused_dispatches = 0;
    effects_failed = FALSE;
}

static boole effects_fail(HRESULT result, const char* operation)
{
    log_warn("D3D11 compute effects %s failed (0x%08lx); using CPU effects", operation, (unsigned long)result);
    effects_d3d11__shutdown();
    effects_failed = TRUE;
    return FALSE;
}

static boole effects_init(void)
{
    effects_create_device_fn create;
    effects_compile_fn compile;
    D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_11_0;
    ID3DBlob* code = NULL;
    ID3DBlob* error = NULL;
    D3D11_BUFFER_DESC buffer;
    HRESULT result;
    if (effects_failed)
    {
        return FALSE;
    }
    if (effects_device)
    {
        return TRUE;
    }
    effects_runtime = LoadLibraryExW(L"d3d11.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    effects_compiler = LoadLibraryExW(L"d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!effects_runtime || !effects_compiler)
    {
        return effects_fail(HRESULT_FROM_WIN32(GetLastError()), "runtime loading");
    }
    create = (effects_create_device_fn)(void*)GetProcAddress(effects_runtime, "D3D11CreateDevice");
    compile = (effects_compile_fn)(void*)GetProcAddress(effects_compiler, "D3DCompile");
    if (!create || !compile)
    {
        return effects_fail(E_NOINTERFACE, "entry-point loading");
    }
    result = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_SINGLETHREADED,
                    &requested, 1, D3D11_SDK_VERSION, &effects_device, NULL, &effects_context);
    if (FAILED(result))
    {
        return effects_fail(result, "hardware device creation");
    }
    result = compile(meek_compose_hlsl, sizeof(meek_compose_hlsl) - 1, "compose.hlsl", NULL, NULL,
                     "main", "cs_5_0", D3DCOMPILE_IEEE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error);
    if (error)
    {
        log_warn("D3D11 compute shader: %s", (const char*)ID3D10Blob_GetBufferPointer(error));
        ID3D10Blob_Release(error);
    }
    if (FAILED(result))
    {
        return effects_fail(result, "shader compilation");
    }
    result = ID3D11Device_CreateComputeShader(effects_device, ID3D10Blob_GetBufferPointer(code),
                                              ID3D10Blob_GetBufferSize(code), NULL, &effects_shader);
    ID3D10Blob_Release(code);
    if (FAILED(result))
    {
        return effects_fail(result, "shader creation");
    }
    stdlib__memset(&buffer, 0, sizeof(buffer));
    buffer.ByteWidth = sizeof(effects_parameters_t);
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    result = ID3D11Device_CreateBuffer(effects_device, &buffer, NULL, &effects_constants);
    if (FAILED(result))
    {
        return effects_fail(result, "constant buffer creation");
    }
    log_info("%s", "D3D11 compute effects enabled on a separate hardware device");
    return TRUE;
}

static HRESULT effects_texture(effects_texture_t* texture, int width, int height, boole output)
{
    D3D11_TEXTURE2D_DESC description;
    HRESULT result;
    if (texture->texture && width <= texture->width && height <= texture->height)
    {
        return S_OK;
    }
    // Grow in bounded blocks; do not allocate a new resource for each animated size.
    width = (width + 255) & ~255;
    height = (height + 255) & ~255;
    if (width < texture->width)
    {
        width = texture->width;
    }
    if (height < texture->height)
    {
        height = texture->height;
    }
    effects_release_texture(texture);
    stdlib__memset(&description, 0, sizeof(description));
    description.Width = width;
    description.Height = height;
    description.MipLevels = description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R32_UINT;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = output ? D3D11_BIND_UNORDERED_ACCESS : D3D11_BIND_SHADER_RESOURCE;
    result = ID3D11Device_CreateTexture2D(effects_device, &description, NULL, &texture->texture);
    if (FAILED(result))
    {
        return result;
    }
    if (output)
    {
        result = ID3D11Device_CreateUnorderedAccessView(effects_device, (ID3D11Resource*)texture->texture, NULL, &texture->uav);
        if (FAILED(result))
        {
            return result;
        }
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        result = ID3D11Device_CreateTexture2D(effects_device, &description, NULL, &texture->staging);
    }
    else
    {
        result = ID3D11Device_CreateShaderResourceView(effects_device, (ID3D11Resource*)texture->texture, NULL, &texture->srv);
    }
    if (FAILED(result))
    {
        return result;
    }
    texture->width = width;
    texture->height = height;
    return S_OK;
}

/**
 * @brief retain a bounded GPU copy of immutable CPU decoration bytes
 * @param source -> versioned CPU source, still available for fallback
 * @param reused -> TRUE only when the returned texture already holds its bytes
 * @return texture -> cached resource, or NULL to use the ordinary upload path
 */
static effects_texture_t* effects_cached_source(const effect_surface_t* source, boole* reused)
{
    effects_cached_source_t* entry = NULL;
    effects_cached_source_t* oldest = NULL;
    int64 bytes = (int64)((source->w + 255) & ~255) * ((source->h + 255) & ~255) * 4;
    int i = 0;
    *reused = FALSE;
    if (!source->content_version || bytes > EFFECTS_SOURCE_CACHE_BYTES)
    {
        return NULL;
    }
    effects_source_clock++;
    for (i = 0; i < EFFECTS_SOURCE_CACHE_CAP; i++)
    {
        effects_cached_source_t* candidate = &effects_cached_sources[i];
        if (candidate->version == source->content_version && candidate->width == source->w && candidate->height == source->h)
        {
            candidate->used = effects_source_clock;
            effects_source_hits++;
            *reused = TRUE;
            return &candidate->texture;
        }
        if (!candidate->version)
        {
            entry = candidate;
        }
    }
    // Evict only enough least-recently-used resources to satisfy both bounds.
    while (!entry || effects_source_bytes + bytes > EFFECTS_SOURCE_CACHE_BYTES)
    {
        oldest = NULL;
        for (i = 0; i < EFFECTS_SOURCE_CACHE_CAP; i++)
        {
            effects_cached_source_t* candidate = &effects_cached_sources[i];
            if (candidate->version && (!oldest || candidate->used < oldest->used))
            {
                oldest = candidate;
            }
        }
        if (!oldest)
        {
            return NULL;
        }
        effects_source_bytes -= (int64)oldest->texture.width * oldest->texture.height * 4;
        effects_release_texture(&oldest->texture);
        stdlib__memset(oldest, 0, sizeof(*oldest));
        entry = oldest;
    }
    if (FAILED(effects_texture(&entry->texture, source->w, source->h, FALSE)))
    {
        effects_release_texture(&entry->texture);
        return NULL; // The optional cache must not remove the normal fallback.
    }
    entry->version = source->content_version;
    entry->used = effects_source_clock;
    entry->width = source->w;
    entry->height = source->h;
    effects_source_bytes += (int64)entry->texture.width * entry->texture.height * 4;
    if (effects_source_bytes > effects_source_peak)
    {
        effects_source_peak = effects_source_bytes;
    }
    return &entry->texture;
}

static boole effects_background_init(void)
{
    effects_compile_fn compile;
    ID3DBlob* code = NULL;
    ID3DBlob* error = NULL;
    D3D11_BUFFER_DESC buffer;
    HRESULT result;
    if (!effects_init())
    {
        return FALSE;
    }
    if (effects_background_shader)
    {
        return TRUE;
    }
    compile = (effects_compile_fn)(void*)GetProcAddress(effects_compiler, "D3DCompile");
    result = compile(meek_background_hlsl, sizeof(meek_background_hlsl) - 1, "background.hlsl", NULL, NULL,
                     "main", "cs_5_0", D3DCOMPILE_IEEE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error);
    if (error)
    {
        log_warn("D3D11 background shader: %s", (const char*)ID3D10Blob_GetBufferPointer(error));
        ID3D10Blob_Release(error);
    }
    if (FAILED(result))
    {
        EFFECTS_RELEASE(code, ID3D10Blob);
        return effects_fail(result, "background shader compilation");
    }
    result = ID3D11Device_CreateComputeShader(effects_device, ID3D10Blob_GetBufferPointer(code),
                                              ID3D10Blob_GetBufferSize(code), NULL, &effects_background_shader);
    ID3D10Blob_Release(code);
    if (FAILED(result))
    {
        return effects_fail(result, "background shader creation");
    }
    stdlib__memset(&buffer, 0, sizeof(buffer));
    buffer.ByteWidth = sizeof(effects_background_parameters_t);
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    result = ID3D11Device_CreateBuffer(effects_device, &buffer, NULL, &effects_background_constants);
    if (FAILED(result))
    {
        return effects_fail(result, "background constant buffer creation");
    }
    return TRUE;
}

boole effects_d3d11__background(effect_surface_t* destination, int left, int top, int width, int height,
                                gui_rect_t bounds, float radius, float opacity, gui_color_t base, const effect_background_layer_t* layers,
                                int count, const float* const* coordinates, int coordinate_width, int blend, boole compose)
{
    effects_background_parameters_t parameters;
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_BOX box;
    HRESULT result;
    ID3D11ShaderResourceView* no_srv = NULL;
    ID3D11UnorderedAccessView* no_uav = NULL;
    ubyte* output;
    int conics = 0, row = 0, i, stop;
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || count <= 0 ||
        count > EFFECT_BACKGROUND_LAYER_CAP || coordinate_width < width || left < destination->x || top < destination->y ||
        left - destination->x + width > destination->w || top - destination->y + height > destination->h)
    {
        return FALSE;
    }
    for (i = 0; i < count; i++)
    {
        if (layers[i].gradient.count < 0 || layers[i].gradient.count > GUI_GRADIENT_MAX_STOPS)
        {
            return FALSE;
        }
        if (layers[i].is_gradient && layers[i].gradient.kind != GUI_GRADIENT_KIND__CONIC)
        {
            // Shader division is not guaranteed to round like scalar CPU
            // division. Keep discontinuous ramps on CPU: a sub-ULP coordinate
            // difference at a hard stop otherwise changes an entire grid line.
            if (layers[i].gradient.repeating)
            {
                return FALSE;
            }
            for (stop = 1; stop < layers[i].gradient.count; stop++)
            {
                if (layers[i].gradient.pos[stop] <= layers[i].gradient.pos[stop - 1])
                {
                    return FALSE;
                }
            }
        }
        if (layers[i].is_gradient && layers[i].gradient.kind == GUI_GRADIENT_KIND__CONIC)
        {
            // First encounter and cache-budget misses stay on CPU. Never
            // approximate atan2 at hard stops, including its signed-zero seam.
            if (!coordinates[i])
            {
                return FALSE;
            }
            conics++;
        }
    }
    if (height * conics > 4096)
    {
        return FALSE;
    }
    if (!effects_background_init())
    {
        return FALSE;
    }
    EFFECTS_PROFILE_BEGIN(profile);
    if (conics)
    {
        result = effects_texture(&effects_source, width, height * conics, FALSE);
        if (FAILED(result))
        {
            return effects_fail(result, "background coordinate texture allocation");
        }
    }
    result = effects_texture(&effects_output, width, height, TRUE);
    if (FAILED(result))
    {
        return effects_fail(result, "background output texture allocation");
    }
    stdlib__memset(&parameters, 0, sizeof(parameters));
    parameters.region[0] = left;
    parameters.region[1] = top;
    parameters.region[2] = width;
    parameters.region[3] = height;
    parameters.bounds = bounds;
    parameters.base = base;
    parameters.options[0] = radius;
    parameters.options[1] = compose ? 1.0f : opacity;
    parameters.options[2] = (float)count;
    parameters.composition[0] = opacity;
    parameters.composition[1] = (float)blend;
    parameters.composition[2] = (float)compose;
    stdlib__memset(&box, 0, sizeof(box));
    box.right = width;
    box.back = 1;
    for (i = 0; i < count; i++)
    {
        const effect_background_layer_t* layer = &layers[i];
        const gradient_sampler_t* gradient = &layer->gradient;
        effects_background_layer_t* target = &parameters.layers[i];
        target->tile[0] = layer->x;
        target->tile[1] = layer->y;
        target->tile[2] = layer->w;
        target->tile[3] = layer->h;
        target->info[0] = layer->is_gradient ? (int)gradient->kind : -1;
        target->info[1] = layer->repeat_x;
        target->info[2] = layer->repeat_y;
        target->info[3] = gradient->count;
        target->geometry0[0] = gradient->cx;
        target->geometry0[1] = gradient->cy;
        target->geometry0[2] = gradient->dx;
        target->geometry0[3] = gradient->dy;
        target->geometry1[0] = gradient->line;
        target->geometry1[1] = gradient->rx;
        target->geometry1[2] = gradient->ry;
        target->geometry1[3] = gradient->angle;
        target->extra[0] = gradient->repeating;
        target->solid = layer->color;
        for (stop = 0; stop < gradient->count; stop++)
        {
            target->positions[stop / 4][stop % 4] = gradient->pos[stop];
            target->colors[stop] = gradient->color[stop];
        }
        if (target->info[0] == GUI_GRADIENT_KIND__CONIC)
        {
            target->extra[1] = row;
            box.top = row;
            box.bottom = row + height;
            ID3D11DeviceContext_UpdateSubresource(effects_context, (ID3D11Resource*)effects_source.texture, 0,
                                                  &box, coordinates[i], coordinate_width * sizeof(float), 0);
            row += height;
        }
    }
    box.top = 0;
    box.bottom = height;
    output = destination->pixels + ((int64)(top - destination->y) * destination->w + left - destination->x) * 4;
    ID3D11DeviceContext_UpdateSubresource(effects_context, (ID3D11Resource*)effects_output.texture, 0, &box, output, destination->w * 4, 0);
    ID3D11DeviceContext_UpdateSubresource(effects_context, (ID3D11Resource*)effects_background_constants, 0, NULL, &parameters, 0, 0);
    EFFECTS_PROFILE_STEP(profile, EFFECTS_PROFILE_BACKGROUND_UPLOAD);
    ID3D11DeviceContext_CSSetShader(effects_context, effects_background_shader, NULL, 0);
    ID3D11DeviceContext_CSSetConstantBuffers(effects_context, 0, 1, &effects_background_constants);
    ID3D11DeviceContext_CSSetShaderResources(effects_context, 0, 1, &effects_source.srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(effects_context, 0, 1, &effects_output.uav, NULL);
    ID3D11DeviceContext_Dispatch(effects_context, (width + 7) / 8, (height + 7) / 8, 1);
    ID3D11DeviceContext_CSSetShaderResources(effects_context, 0, 1, &no_srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(effects_context, 0, 1, &no_uav, NULL);
    ID3D11DeviceContext_CopySubresourceRegion(effects_context, (ID3D11Resource*)effects_output.staging, 0, 0, 0, 0,
                                              (ID3D11Resource*)effects_output.texture, 0, &box);
    stdlib__memset(&mapped, 0, sizeof(mapped));
    EFFECTS_PROFILE_STEP(profile, EFFECTS_PROFILE_BACKGROUND_SUBMIT);
    result = ID3D11DeviceContext_Map(effects_context, (ID3D11Resource*)effects_output.staging, 0, D3D11_MAP_READ, 0, &mapped);
    EFFECTS_PROFILE_STEP(profile, EFFECTS_PROFILE_BACKGROUND_MAP);
    if (FAILED(result))
    {
        return effects_fail(result, "background readback");
    }
    for (i = 0; i < height; i++)
    {
        stdlib__memcpy(output + (int64)i * destination->w * 4,
               (const ubyte*)mapped.pData + (int64)i * mapped.RowPitch, (size_t)width * 4);
    }
    ID3D11DeviceContext_Unmap(effects_context, (ID3D11Resource*)effects_output.staging, 0);
    effects_background_dispatches++;
    if (compose)
    {
        effects_fused_dispatches++;
    }
    EFFECTS_PROFILE_STEP(profile, EFFECTS_PROFILE_BACKGROUND_COPY);
    return TRUE;
}

boole effects_d3d11__compose(const effect_surface_t* source, effect_surface_t* destination,
                             int left, int top, int width, int height, const float* inverse, float opacity, int blend)
{
    effects_parameters_t parameters;
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_BOX box;
    HRESULT result;
    ID3D11ShaderResourceView* no_srv = NULL;
    ID3D11UnorderedAccessView* no_uav = NULL;
    ubyte* output;
    int i;
    effects_texture_t* read_source;
    boole source_reused;
    // Bounded resource allocation; larger or invalid regions retain CPU behavior.
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || source->w <= 0 || source->h <= 0 ||
        source->w > 4096 || source->h > 4096 || left < destination->x || top < destination->y ||
        left - destination->x + width > destination->w || top - destination->y + height > destination->h)
    {
        return FALSE;
    }
    if (!effects_init())
    {
        return FALSE;
    }
    EFFECTS_PROFILE_BEGIN(profile);
    read_source = effects_cached_source(source, &source_reused);
    if (!read_source)
    {
        result = effects_texture(&effects_source, source->w, source->h, FALSE);
        if (FAILED(result))
        {
            return effects_fail(result, "source texture allocation");
        }
        read_source = &effects_source;
    }
    result = effects_texture(&effects_output, width, height, TRUE);
    if (FAILED(result))
    {
        return effects_fail(result, "output texture allocation");
    }
    stdlib__memset(&parameters, 0, sizeof(parameters));
    for (i = 0; i < 9; i++)
    {
        parameters.inverse[i / 3][i % 3] = inverse[i];
    }
    parameters.source[0] = source->w;
    parameters.source[1] = source->h;
    parameters.source[2] = source->x;
    parameters.source[3] = source->y;
    parameters.region[0] = left;
    parameters.region[1] = top;
    parameters.region[2] = width;
    parameters.region[3] = height;
    parameters.options[0] = opacity;
    parameters.options[1] = (float)blend;
    parameters.options[2] = (float)(inverse[0] == 1 && inverse[1] == 0 && inverse[3] == 0 && inverse[4] == 1 &&
                                    inverse[6] == 0 && inverse[7] == 0 && inverse[8] == 1 && inverse[2] == (int)inverse[2] && inverse[5] == (int)inverse[5]);
    stdlib__memset(&box, 0, sizeof(box));
    box.right = source->w;
    box.bottom = source->h;
    box.back = 1;
    if (!source_reused)
    {
        ID3D11DeviceContext_UpdateSubresource(effects_context, (ID3D11Resource*)read_source->texture, 0, &box, source->pixels, source->w * 4, 0);
    }
    box.right = width;
    box.bottom = height;
    output = destination->pixels + ((int64)(top - destination->y) * destination->w + left - destination->x) * 4;
    ID3D11DeviceContext_UpdateSubresource(effects_context, (ID3D11Resource*)effects_output.texture, 0, &box, output, destination->w * 4, 0);
    ID3D11DeviceContext_UpdateSubresource(effects_context, (ID3D11Resource*)effects_constants, 0, NULL, &parameters, 0, 0);
    EFFECTS_PROFILE_STEP(profile, EFFECTS_PROFILE_COMPOSE_UPLOAD);
    ID3D11DeviceContext_CSSetShader(effects_context, effects_shader, NULL, 0);
    ID3D11DeviceContext_CSSetConstantBuffers(effects_context, 0, 1, &effects_constants);
    ID3D11DeviceContext_CSSetShaderResources(effects_context, 0, 1, &read_source->srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(effects_context, 0, 1, &effects_output.uav, NULL);
    ID3D11DeviceContext_Dispatch(effects_context, (width + 7) / 8, (height + 7) / 8, 1);
    ID3D11DeviceContext_CSSetShaderResources(effects_context, 0, 1, &no_srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(effects_context, 0, 1, &no_uav, NULL);
    ID3D11DeviceContext_CopySubresourceRegion(effects_context, (ID3D11Resource*)effects_output.staging, 0, 0, 0, 0,
                                              (ID3D11Resource*)effects_output.texture, 0, &box);
    stdlib__memset(&mapped, 0, sizeof(mapped));
    EFFECTS_PROFILE_STEP(profile, EFFECTS_PROFILE_COMPOSE_SUBMIT);
    result = ID3D11DeviceContext_Map(effects_context, (ID3D11Resource*)effects_output.staging, 0, D3D11_MAP_READ, 0, &mapped);
    EFFECTS_PROFILE_STEP(profile, EFFECTS_PROFILE_COMPOSE_MAP);
    if (FAILED(result))
    {
        return effects_fail(result, "readback");
    }
    for (i = 0; i < height; i++)
    {
        stdlib__memcpy(output + (int64)i * destination->w * 4, (const ubyte*)mapped.pData + (int64)i * mapped.RowPitch, (size_t)width * 4);
    }
    ID3D11DeviceContext_Unmap(effects_context, (ID3D11Resource*)effects_output.staging, 0);
    effects_dispatches++;
    EFFECTS_PROFILE_STEP(profile, EFFECTS_PROFILE_COMPOSE_COPY);
    return TRUE;
}
