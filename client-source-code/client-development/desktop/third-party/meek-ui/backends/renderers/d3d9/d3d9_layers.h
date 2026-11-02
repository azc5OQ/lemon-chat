// Private implementation included by d3d9_renderer.c. Uses the same device,
// batches and reset lifecycle for standalone windows and adopted host devices.
#include "shaders/generated/layer.vert.h"
#include "shaders/generated/layer.frag.h"

static void _d3d9_renderer_internal__release_layers(void)
{
    _d3d9_renderer_internal__release_blur();
    if (_d3d9_renderer_internal__g.layer_active) renderer_backend__end_layer();
    for (int i = 0; i < 4; i++) {
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.layer_targets[i].surface);
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.layer_targets[i].texture);
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.layer_saved_targets[i]);
    }
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.layer_saved_depth);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.layer_readback);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.layer_decl);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.layer_ps);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.layer_vs);
}

static boole _d3d9_renderer_internal__ensure_layer_pipeline(void)
{
    if (_d3d9_renderer_internal__g.layer_decl) return TRUE;
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;
    ID3DBlob *vs = NULL_POINTER, *ps = NULL_POINTER;
    if (!dev || _d3d9_renderer_internal__g.device_lost) return FALSE;
    if (!_d3d9_renderer_internal__compile_shader(_d3d9_layer_vs_source, "vs_main", "vs_3_0", &vs) ||
        !_d3d9_renderer_internal__compile_shader(_d3d9_layer_ps_source, "ps_main", "ps_3_0", &ps)) {
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(vs);
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(ps);
        return FALSE;
    }
    HRESULT hr = dev->lpVtbl->CreateVertexShader(dev, (const DWORD*)vs->lpVtbl->GetBufferPointer(vs), &_d3d9_renderer_internal__g.layer_vs);
    if (SUCCEEDED(hr)) hr = dev->lpVtbl->CreatePixelShader(dev, (const DWORD*)ps->lpVtbl->GetBufferPointer(ps), &_d3d9_renderer_internal__g.layer_ps);
    D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {0, 20, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
        D3DDECL_END()
    };
    if (SUCCEEDED(hr)) hr = dev->lpVtbl->CreateVertexDeclaration(dev, elements, &_d3d9_renderer_internal__g.layer_decl);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(vs);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(ps);
    if (FAILED(hr)) {
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.layer_decl);
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.layer_ps);
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.layer_vs);
        log_error("D3D9 layer pipeline failed: %08lX", (unsigned long)hr);
        return FALSE;
    }
    return TRUE;
}

GUI_API boole renderer_backend__supports_layers(void)
{
    // Creation failures select the existing CPU frame fallback rather than
    // advertising a pipeline which silently drops layers.
    return _d3d9_renderer_internal__ensure_layer_pipeline();
}

GUI_API void* renderer_backend__create_layer_texture(const ubyte* rgba, int width, int height)
{
    if (rgba) return _d3d9_renderer_internal__create_texture_rgba(rgba, width, height, FALSE);
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;
    IDirect3DTexture9* texture = NULL_POINTER;
    if (!dev || width <= 0 || height <= 0 || _d3d9_renderer_internal__g.device_lost) return NULL_POINTER;
    if (FAILED(dev->lpVtbl->CreateTexture(dev, width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, NULL_POINTER))) return NULL_POINTER;
    return texture;
}

GUI_API boole renderer_backend__update_layer_texture(void* texture, const ubyte* rgba, int width, int height)
{
    IDirect3DTexture9* tex = texture;
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT locked;
    if (!tex || !rgba || width <= 0 || height <= 0 || _d3d9_renderer_internal__g.device_lost) return FALSE;
    if (FAILED(tex->lpVtbl->GetLevelDesc(tex, 0, &desc)) || desc.Width != (UINT)width || desc.Height != (UINT)height || desc.Pool != D3DPOOL_MANAGED) return FALSE;
    if (FAILED(tex->lpVtbl->LockRect(tex, 0, &locked, NULL_POINTER, 0))) return FALSE;
    for (int y = 0; y < height; y++) {
        ubyte* dst = (ubyte*)locked.pBits + (int64)y * locked.Pitch;
        const ubyte* src = rgba + (int64)y * width * 4;
        for (int x = 0; x < width; x++, src += 4, dst += 4) {
            dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = src[3];
        }
    }
    tex->lpVtbl->UnlockRect(tex, 0);
    return TRUE;
}

GUI_API void renderer_backend__submit_layer(void* texture, const float* corners, float opacity, int blend_mode)
{
    IDirect3DTexture9* tex = texture;
    if (!tex || !corners || !_d3d9_renderer_internal__ensure_layer_pipeline()) return;
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;
    _d3d9_renderer_internal__flush_batches();
    float verts[6 * 6];
    static const int order[] = {0, 1, 3, 1, 2, 3};
    static const float uv[][2] = {{0,0}, {1,0}, {1,1}, {0,1}};
    for (int i = 0; i < 6; i++) {
        int k = order[i];
        verts[i*6] = corners[k*3]; verts[i*6+1] = corners[k*3+1]; verts[i*6+2] = corners[k*3+2];
        verts[i*6+3] = uv[k][0]; verts[i*6+4] = uv[k][1]; verts[i*6+5] = opacity;
    }
    D3DSURFACE_DESC td = {0};
    tex->lpVtbl->GetLevelDesc(tex, 0, &td);
    boole aligned = corners[2] == 1 && corners[5] == 1 && corners[8] == 1 && corners[11] == 1 &&
        corners[0] == (float)(int)corners[0] && corners[1] == (float)(int)corners[1] &&
        corners[3] - corners[0] == (float)td.Width && corners[7] - corners[1] == (float)td.Height &&
        corners[4] == corners[1] && corners[6] == corners[3] && corners[9] == corners[0] && corners[10] == corners[7];
    float viewport[4] = {(float)_d3d9_renderer_internal__g.viewport_w, (float)_d3d9_renderer_internal__g.viewport_h, 0, 0};
    dev->lpVtbl->SetVertexShaderConstantF(dev, 0, viewport, 1);
    dev->lpVtbl->SetVertexDeclaration(dev, _d3d9_renderer_internal__g.layer_decl);
    dev->lpVtbl->SetVertexShader(dev, _d3d9_renderer_internal__g.layer_vs);
    dev->lpVtbl->SetPixelShader(dev, _d3d9_renderer_internal__g.layer_ps);
    dev->lpVtbl->SetTexture(dev, 0, (IDirect3DBaseTexture9*)tex);
    dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_MINFILTER, aligned ? D3DTEXF_POINT : D3DTEXF_LINEAR);
    dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, aligned ? D3DTEXF_POINT : D3DTEXF_LINEAR);
    dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->lpVtbl->SetRenderState(dev, D3DRS_SRCBLEND, blend_mode == 3 ? D3DBLEND_DESTCOLOR : D3DBLEND_ONE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_DESTBLEND, blend_mode == 2 ? D3DBLEND_ZERO : (blend_mode == 1 ? D3DBLEND_INVSRCCOLOR : D3DBLEND_INVSRCALPHA));
    dev->lpVtbl->SetRenderState(dev, D3DRS_DESTBLENDALPHA, blend_mode == 2 ? D3DBLEND_ZERO : D3DBLEND_INVSRCALPHA);
    dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, verts, 6 * sizeof(float));
    dev->lpVtbl->SetTexture(dev, 0, NULL_POINTER);
    // Ordinary rects, text and images use straight-alpha input. Restore their
    // factors now; their individual draw paths restore shaders and streams.
    dev->lpVtbl->SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->lpVtbl->SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->lpVtbl->SetRenderState(dev, D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
}

GUI_API boole renderer_backend__begin_layer(int width, int height)
{
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;
    if (!dev || _d3d9_renderer_internal__g.device_lost || _d3d9_renderer_internal__g.layer_active || width <= 0 || height <= 0) return FALSE;
    D3DCAPS9 caps;
    if (FAILED(dev->lpVtbl->GetDeviceCaps(dev, &caps)) || (UINT)width > caps.MaxTextureWidth || (UINT)height > caps.MaxTextureHeight) return FALSE;
    int slot = 0;
    for (int i = 0; i < 4; i++) {
        if (_d3d9_renderer_internal__g.layer_targets[i].texture && _d3d9_renderer_internal__g.layer_targets[i].w == width && _d3d9_renderer_internal__g.layer_targets[i].h == height) { slot = i; break; }
        if (_d3d9_renderer_internal__g.layer_targets[i].used < _d3d9_renderer_internal__g.layer_targets[slot].used) slot = i;
    }
    if (!_d3d9_renderer_internal__g.layer_targets[slot].texture || _d3d9_renderer_internal__g.layer_targets[slot].w != width || _d3d9_renderer_internal__g.layer_targets[slot].h != height) {
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.layer_targets[slot].surface);
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.layer_targets[slot].texture);
        HRESULT hr = dev->lpVtbl->CreateTexture(dev, width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &_d3d9_renderer_internal__g.layer_targets[slot].texture, NULL_POINTER);
        IDirect3DTexture9* tex = _d3d9_renderer_internal__g.layer_targets[slot].texture;
        if (SUCCEEDED(hr)) hr = tex->lpVtbl->GetSurfaceLevel(tex, 0, &_d3d9_renderer_internal__g.layer_targets[slot].surface);
        if (FAILED(hr)) {
            _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.layer_targets[slot].texture);
            return FALSE;
        }
        _d3d9_renderer_internal__g.layer_targets[slot].w = width;
        _d3d9_renderer_internal__g.layer_targets[slot].h = height;
    }
    _d3d9_renderer_internal__flush_batches();
    // An adopting host can have multiple color targets and a depth surface.
    // Save all bindings; a small UI target cannot use the host's depth buffer.
    if (FAILED(dev->lpVtbl->GetRenderTarget(dev, 0, &_d3d9_renderer_internal__g.layer_saved_targets[0]))) return FALSE;
    dev->lpVtbl->GetViewport(dev, &_d3d9_renderer_internal__g.layer_saved_viewport);
    dev->lpVtbl->GetDepthStencilSurface(dev, &_d3d9_renderer_internal__g.layer_saved_depth);
    _d3d9_renderer_internal__g.layer_target_count = caps.NumSimultaneousRTs < 4 ? caps.NumSimultaneousRTs : 4;
    for (DWORD i = 1; i < _d3d9_renderer_internal__g.layer_target_count; i++) {
        dev->lpVtbl->GetRenderTarget(dev, i, &_d3d9_renderer_internal__g.layer_saved_targets[i]);
        dev->lpVtbl->SetRenderTarget(dev, i, NULL_POINTER);
    }
    dev->lpVtbl->SetDepthStencilSurface(dev, NULL_POINTER);
    _d3d9_renderer_internal__g.layer_saved_w = _d3d9_renderer_internal__g.viewport_w;
    _d3d9_renderer_internal__g.layer_saved_h = _d3d9_renderer_internal__g.viewport_h;
    _d3d9_renderer_internal__g.layer_saved_scissor_depth = _d3d9_renderer_internal__g.scissor_depth;
    stdlib__memcpy(_d3d9_renderer_internal__g.layer_saved_scissors, _d3d9_renderer_internal__g.scissor_stack, sizeof(_d3d9_renderer_internal__g.scissor_stack));
    _d3d9_renderer_internal__g.layer_active = slot + 1;
    if (FAILED(dev->lpVtbl->SetRenderTarget(dev, 0, _d3d9_renderer_internal__g.layer_targets[slot].surface))) {
        renderer_backend__end_layer();
        return FALSE;
    }
    _d3d9_renderer_internal__g.layer_targets[slot].used = ++_d3d9_renderer_internal__g.layer_counter;
    _d3d9_renderer_internal__g.viewport_w = width;
    _d3d9_renderer_internal__g.viewport_h = height;
    _d3d9_renderer_internal__g.scissor_depth = 0;
    D3DVIEWPORT9 vp = {0, 0, (DWORD)width, (DWORD)height, 0, 1};
    dev->lpVtbl->SetViewport(dev, &vp);
    float viewport[4] = {(float)width, (float)height, 0, 0};
    dev->lpVtbl->SetVertexShaderConstantF(dev, 0, viewport, 1);
    _d3d9_renderer_internal__apply_scissor_top();
    dev->lpVtbl->Clear(dev, 0, NULL_POINTER, D3DCLEAR_TARGET, 0, 1, 0);
    return TRUE;
}

GUI_API void* renderer_backend__end_layer(void)
{
    int slot = _d3d9_renderer_internal__g.layer_active - 1;
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;
    if (slot < 0 || !dev) return NULL_POINTER;
    _d3d9_renderer_internal__flush_batches();
    _d3d9_renderer_internal__g.layer_active = 0;
    for (DWORD i = 0; i < _d3d9_renderer_internal__g.layer_target_count; i++) {
        dev->lpVtbl->SetRenderTarget(dev, i, _d3d9_renderer_internal__g.layer_saved_targets[i]);
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.layer_saved_targets[i]);
    }
    dev->lpVtbl->SetDepthStencilSurface(dev, _d3d9_renderer_internal__g.layer_saved_depth);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.layer_saved_depth);
    dev->lpVtbl->SetViewport(dev, &_d3d9_renderer_internal__g.layer_saved_viewport);
    _d3d9_renderer_internal__g.viewport_w = _d3d9_renderer_internal__g.layer_saved_w;
    _d3d9_renderer_internal__g.viewport_h = _d3d9_renderer_internal__g.layer_saved_h;
    _d3d9_renderer_internal__g.scissor_depth = _d3d9_renderer_internal__g.layer_saved_scissor_depth;
    stdlib__memcpy(_d3d9_renderer_internal__g.scissor_stack, _d3d9_renderer_internal__g.layer_saved_scissors, sizeof(_d3d9_renderer_internal__g.scissor_stack));
    float viewport[4] = {(float)_d3d9_renderer_internal__g.viewport_w, (float)_d3d9_renderer_internal__g.viewport_h, 0, 0};
    dev->lpVtbl->SetVertexShaderConstantF(dev, 0, viewport, 1);
    _d3d9_renderer_internal__apply_scissor_top();
    return _d3d9_renderer_internal__g.layer_targets[slot].texture;
}

GUI_API boole renderer_backend__read_region(int x, int y, int w, int h, ubyte* out_rgba)
{
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;
    IDirect3DSurface9* target = NULL_POINTER;
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT locked;
    if (!dev || !out_rgba || x < 0 || y < 0 || w <= 0 || h <= 0 || _d3d9_renderer_internal__g.device_lost) return FALSE;
    _d3d9_renderer_internal__flush_batches();
    if (FAILED(dev->lpVtbl->GetRenderTarget(dev, 0, &target))) return FALSE;
    HRESULT hr = target->lpVtbl->GetDesc(target, &desc);
    if (FAILED(hr) || (UINT)w > desc.Width || (UINT)h > desc.Height || (UINT)x > desc.Width - (UINT)w || (UINT)y > desc.Height - (UINT)h ||
        desc.MultiSampleType != D3DMULTISAMPLE_NONE || (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8)) {
        target->lpVtbl->Release(target);
        return FALSE;
    }
    // GetRenderTargetData requires an equal-size system-memory surface. Keep
    // it between frames; LockRect restricts the CPU copy to the requested area.
    if (!_d3d9_renderer_internal__g.layer_readback || _d3d9_renderer_internal__g.layer_readback_w != desc.Width || _d3d9_renderer_internal__g.layer_readback_h != desc.Height || _d3d9_renderer_internal__g.layer_readback_format != desc.Format) {
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.layer_readback);
        hr = dev->lpVtbl->CreateOffscreenPlainSurface(dev, desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &_d3d9_renderer_internal__g.layer_readback, NULL_POINTER);
        if (FAILED(hr)) { target->lpVtbl->Release(target); return FALSE; }
        _d3d9_renderer_internal__g.layer_readback_w = desc.Width;
        _d3d9_renderer_internal__g.layer_readback_h = desc.Height;
        _d3d9_renderer_internal__g.layer_readback_format = desc.Format;
    }
    IDirect3DSurface9* staging = _d3d9_renderer_internal__g.layer_readback;
    hr = dev->lpVtbl->GetRenderTargetData(dev, target, staging);
    target->lpVtbl->Release(target);
    RECT rect = {x, y, x + w, y + h};
    if (FAILED(hr) || FAILED(staging->lpVtbl->LockRect(staging, &locked, &rect, D3DLOCK_READONLY))) return FALSE;
    for (int row = 0; row < h; row++) {
        const ubyte* src = (const ubyte*)locked.pBits + (int64)row * locked.Pitch;
        ubyte* dst = out_rgba + (int64)row * w * 4;
        for (int col = 0; col < w; col++, src += 4, dst += 4) {
            dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = desc.Format == D3DFMT_X8R8G8B8 ? 255 : src[3];
        }
    }
    staging->lpVtbl->UnlockRect(staging);
    return TRUE;
}

// Tiled gradients share tessellation with D3D11 and OpenGL 3. Local
// coordinates refer to the whole box so every tile has the same rounded clip.
GUI_API void renderer_backend__submit_tiles(gui_rect_t box, float radius, gui_rect_t tile, int columns, int rows, const gui_gradient_t* gradient, gui_color_t color, float scale, float opacity)
{
    static gradient_vertex_t verts[GRADIENT_MAX_VERTS];
    int64 count = 0;
    int64 i = 0;
    int column = 0;
    int row = 0;
    float dx = 0.0f;
    float dy = 0.0f;
    float max_r = (box.w < box.h ? box.w : box.h) * 0.5f;

    if (columns <= 0 || rows <= 0 || tile.w <= 0.0f || tile.h <= 0.0f)
    {
        return;
    }
    if (radius < 0.0f)
    {
        radius = 0.0f;
    }
    if (radius > max_r)
    {
        radius = max_r;
    }
    if (gradient != NULL_POINTER)
    {
        count = gradient__tessellate(gradient, tile, scale, opacity, verts, GRADIENT_MAX_VERTS);
    }
    else
    {
        // the two triangles of renderer__submit_rect
        color.a *= opacity;
        for (i = 0; i < 6; i++)
        {
            verts[i].x = tile.x + ((i == 1 || i == 3 || i == 4) ? tile.w : 0.0f);
            verts[i].y = tile.y + ((i == 2 || i == 4 || i == 5) ? tile.h : 0.0f);
            verts[i].c = color;
        }
        count = 6;
    }
    if (count <= 0)
    {
        return;
    }
    for (row = 0; row < rows; row++)
    {
        dy = (float)row * tile.h;
        for (column = 0; column < columns; column++)
        {
            dx = (float)column * tile.w;
            // a full batch is drawn before the next tile; the flush keeps
            // the submit order (every pending rect and glyph goes first)
            if (_d3d9_renderer_internal__g.vert_count + count > _d3d9_renderer_internal__g.vert_cap)
            {
                _d3d9_renderer_internal__flush_batches();
            }
            if (_d3d9_renderer_internal__g.vert_count + count > _d3d9_renderer_internal__g.vert_cap)
            {
                return;
            }
            for (i = 0; i < count; i++)
            {
                _d3d9_renderer_internal__push_vertex(verts[i].x + dx, verts[i].y + dy, verts[i].x + dx - box.x, verts[i].y + dy - box.y, box.w, box.h, radius, verts[i].c);
            }
        }
    }
}
