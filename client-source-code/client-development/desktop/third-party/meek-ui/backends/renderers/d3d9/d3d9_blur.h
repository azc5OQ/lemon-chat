// Shared by the standalone renderer and the embedded adapter. These default-
// pool targets must be released before Reset, just like the layer target pool.
#include "shaders/generated/blur.frag.h"
static struct {
    IDirect3DPixelShader9* shader;
    IDirect3DTexture9* textures[2];
    IDirect3DSurface9* surfaces[2];
    int w;
    int h;
} _d3d9_blur;

static void _d3d9_renderer_internal__release_blur(void)
{
    for (int i = 0; i < 2; i++) {
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_blur.surfaces[i]);
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_blur.textures[i]);
    }
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_blur.shader);
}

static boole _d3d9_renderer_internal__prepare_blur(int w, int h)
{
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;
    HRESULT hr = S_OK;
    if (!_d3d9_renderer_internal__ensure_layer_pipeline()) return FALSE;
    if (!_d3d9_blur.shader) {
        ID3DBlob* blob = NULL_POINTER;
        if (!_d3d9_renderer_internal__compile_shader((char*)_d3d9_blur_ps, "ps_main", "ps_3_0", &blob)) return FALSE;
        hr = dev->lpVtbl->CreatePixelShader(dev, (DWORD*)blob->lpVtbl->GetBufferPointer(blob), &_d3d9_blur.shader);
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(blob);
        if (FAILED(hr)) return FALSE;
    }
    if (!_d3d9_blur.textures[1] || _d3d9_blur.w < w || _d3d9_blur.h < h) {
        w = w > _d3d9_blur.w ? w : _d3d9_blur.w;
        h = h > _d3d9_blur.h ? h : _d3d9_blur.h;
        for (int i = 0; i < 2; i++) {
            _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_blur.surfaces[i]);
            _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_blur.textures[i]);
            hr = dev->lpVtbl->CreateTexture(dev, w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &_d3d9_blur.textures[i], NULL_POINTER);
            if (SUCCEEDED(hr)) hr = _d3d9_blur.textures[i]->lpVtbl->GetSurfaceLevel(_d3d9_blur.textures[i], 0, &_d3d9_blur.surfaces[i]);
            if (FAILED(hr)) { _d3d9_renderer_internal__release_blur(); return FALSE; }
        }
        _d3d9_blur.w = w;
        _d3d9_blur.h = h;
    }
    return TRUE;
}

GUI_API boole renderer_backend__blur_layer(int x, int y, int w, int h, int radius)
{
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;
    if (!dev || _d3d9_renderer_internal__g.device_lost || x < 0 || y < 0 || w <= 0 || h <= 0 || radius < 1 || radius > 256 || w > _d3d9_renderer_internal__g.viewport_w - x || h > _d3d9_renderer_internal__g.viewport_h - y) return FALSE;
    _d3d9_renderer_internal__flush_batches();
    IDirect3DSurface9* targets[4] = {0};
    IDirect3DSurface9* depth = NULL_POINTER;
    D3DSURFACE_DESC desc;
    D3DCAPS9 caps;
    DWORD target_count = 1;
    boole ok = FALSE;
    if (FAILED(dev->lpVtbl->GetRenderTarget(dev, 0, &targets[0]))) return FALSE;
    targets[0]->lpVtbl->GetDesc(targets[0], &desc);
    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE || (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8) || (UINT)x >= desc.Width || (UINT)y >= desc.Height || (UINT)w > desc.Width-(UINT)x || (UINT)h > desc.Height-(UINT)y) goto cleanup;
    if (FAILED(dev->lpVtbl->GetDeviceCaps(dev, &caps)) || !_d3d9_renderer_internal__prepare_blur(w, h)) goto cleanup;
    RECT region = {x, y, x+w, y+h};
    RECT scratch_region = {0, 0, w, h};
    // This copies color surfaces only. Do not end/restart an adopting host's
    // scene; unsupported driver copies simply keep the CPU reference path.
    if (FAILED(dev->lpVtbl->StretchRect(dev, targets[0], &region, _d3d9_blur.surfaces[0], &scratch_region, D3DTEXF_NONE))) goto cleanup;

    D3DVIEWPORT9 viewport;
    DWORD scissor_enabled;
    DWORD blend_enabled;
    dev->lpVtbl->GetViewport(dev, &viewport);
    dev->lpVtbl->GetRenderState(dev, D3DRS_SCISSORTESTENABLE, &scissor_enabled);
    dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend_enabled);
    dev->lpVtbl->GetDepthStencilSurface(dev, &depth);
    target_count = caps.NumSimultaneousRTs < 4 ? caps.NumSimultaneousRTs : 4;
    for (DWORD i = 1; i < target_count; i++) {
        dev->lpVtbl->GetRenderTarget(dev, i, &targets[i]);
        dev->lpVtbl->SetRenderTarget(dev, i, NULL_POINTER);
    }
    dev->lpVtbl->SetDepthStencilSurface(dev, NULL_POINTER);
    dev->lpVtbl->SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
    D3DVIEWPORT9 blur_viewport = {0, 0, (DWORD)w, (DWORD)h, 0, 1};
    float vp[4] = {(float)w, (float)h, 0, 0};
    float verts[] = {0,0,1,0,0,1, (float)w,0,1,1,0,1, 0,(float)h,1,0,1,1, (float)w,0,1,1,0,1, (float)w,(float)h,1,1,1,1, 0,(float)h,1,0,1,1};
    for (int i = 0; i < 6; i++) { verts[i*6+3] *= (float)w/_d3d9_blur.w; verts[i*6+4] *= (float)h/_d3d9_blur.h; }
    float limits[4] = {0.5f/_d3d9_blur.w, 0.5f/_d3d9_blur.h, (w-0.5f)/_d3d9_blur.w, (h-0.5f)/_d3d9_blur.h};
    dev->lpVtbl->SetPixelShaderConstantF(dev, 1, limits, 1);
    dev->lpVtbl->SetVertexDeclaration(dev, _d3d9_renderer_internal__g.layer_decl);
    dev->lpVtbl->SetVertexShader(dev, _d3d9_renderer_internal__g.layer_vs);
    dev->lpVtbl->SetVertexShaderConstantF(dev, 0, vp, 1);
    dev->lpVtbl->SetPixelShader(dev, _d3d9_blur.shader);
    dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    HRESULT hr = S_OK;
    for (int pass = 0; pass < 6 && SUCCEEDED(hr); pass++) {
        int source = pass & 1;
        hr = dev->lpVtbl->SetRenderTarget(dev, 0, _d3d9_blur.surfaces[1-source]);
        dev->lpVtbl->SetViewport(dev, &blur_viewport);
        float parameters[4] = {1.0f/_d3d9_blur.w, 1.0f/_d3d9_blur.h, (float)radius, (float)source};
        dev->lpVtbl->SetPixelShaderConstantF(dev, 0, parameters, 1);
        dev->lpVtbl->SetTexture(dev, 0, (IDirect3DBaseTexture9*)_d3d9_blur.textures[source]);
        if (SUCCEEDED(hr)) hr = dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, verts, 6*sizeof(float));
        dev->lpVtbl->SetTexture(dev, 0, NULL_POINTER);
    }
    for (DWORD i = 0; i < target_count; i++) dev->lpVtbl->SetRenderTarget(dev, i, targets[i]);
    dev->lpVtbl->SetDepthStencilSurface(dev, depth);
    dev->lpVtbl->SetViewport(dev, &viewport);
    vp[0] = (float)_d3d9_renderer_internal__g.viewport_w;
    vp[1] = (float)_d3d9_renderer_internal__g.viewport_h;
    dev->lpVtbl->SetVertexShaderConstantF(dev, 0, vp, 1);
    dev->lpVtbl->SetRenderState(dev, D3DRS_SCISSORTESTENABLE, scissor_enabled);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, blend_enabled);
    _d3d9_renderer_internal__apply_scissor_top();
    if (SUCCEEDED(hr)) hr = dev->lpVtbl->StretchRect(dev, _d3d9_blur.surfaces[0], &scratch_region, targets[0], &region, D3DTEXF_NONE);
    ok = (boole)SUCCEEDED(hr);
cleanup:
    for (DWORD i = 0; i < target_count; i++) _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(targets[i]);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(depth);
    return ok;
}
