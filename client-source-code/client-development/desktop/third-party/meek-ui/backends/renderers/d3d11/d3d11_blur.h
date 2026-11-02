// Included by d3d11_renderer.c: standalone and embedded share this pipeline.
#include "shaders/generated/blur.vert.h"
#include "shaders/generated/blur.frag.h"

static void _d3d11_renderer_internal__release_blur(void)
{
    for (int i = 0; i < 2; i++) {
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.blur_srv[i]);
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.blur_rtv[i]);
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.blur_texture[i]);
    }
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.blur_constants);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.blur_vs);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.blur_ps);
}

static boole _d3d11_renderer_internal__prepare_blur(int w, int h, DXGI_FORMAT format)
{
    ID3D11Device* dev = _d3d11_renderer_internal__g.device;
    HRESULT hr = S_OK;
    if (!_d3d11_renderer_internal__g.blur_constants) {
        ID3DBlob* vs = NULL_POINTER;
        ID3DBlob* ps = NULL_POINTER;
        boole compiled = _d3d11_renderer_internal__compile_shader((char*)_d3d11_blur_vs, "vs_main", "vs_5_0", &vs) && _d3d11_renderer_internal__compile_shader((char*)_d3d11_blur_ps, "ps_main", "ps_5_0", &ps);
        if (compiled) hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs), NULL_POINTER, &_d3d11_renderer_internal__g.blur_vs);
        if (compiled && SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps), NULL_POINTER, &_d3d11_renderer_internal__g.blur_ps);
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs);
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(ps);
        D3D11_BUFFER_DESC bd = {0};
        bd.ByteWidth = 16;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (compiled && SUCCEEDED(hr)) hr = ID3D11Device_CreateBuffer(dev, &bd, NULL_POINTER, &_d3d11_renderer_internal__g.blur_constants);
        if (!compiled || FAILED(hr)) { _d3d11_renderer_internal__release_blur(); return FALSE; }
    }
    if (!_d3d11_renderer_internal__g.blur_texture[1] || _d3d11_renderer_internal__g.blur_w < w || _d3d11_renderer_internal__g.blur_h < h || _d3d11_renderer_internal__g.blur_format != format) {
        w = w > _d3d11_renderer_internal__g.blur_w ? w : _d3d11_renderer_internal__g.blur_w;
        h = h > _d3d11_renderer_internal__g.blur_h ? h : _d3d11_renderer_internal__g.blur_h;
        for (int i = 0; i < 2; i++) {
            _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.blur_srv[i]);
            _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.blur_rtv[i]);
            _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.blur_texture[i]);
            D3D11_TEXTURE2D_DESC td = {0};
            td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = format; td.SampleDesc.Count = 1;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            hr = ID3D11Device_CreateTexture2D(dev, &td, NULL_POINTER, &_d3d11_renderer_internal__g.blur_texture[i]);
            if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource*)_d3d11_renderer_internal__g.blur_texture[i], NULL_POINTER, &_d3d11_renderer_internal__g.blur_rtv[i]);
            if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource*)_d3d11_renderer_internal__g.blur_texture[i], NULL_POINTER, &_d3d11_renderer_internal__g.blur_srv[i]);
            if (FAILED(hr)) { _d3d11_renderer_internal__release_blur(); return FALSE; }
        }
        _d3d11_renderer_internal__g.blur_w = w;
        _d3d11_renderer_internal__g.blur_h = h;
        _d3d11_renderer_internal__g.blur_format = format;
    }
    return TRUE;
}

GUI_API boole renderer_backend__blur_layer(int x, int y, int w, int h, int radius)
{
    ID3D11DeviceContext* ctx = _d3d11_renderer_internal__g.context;
    if (!ctx || x < 0 || y < 0 || w <= 0 || h <= 0 || radius < 1 || radius > 256 || w > _d3d11_renderer_internal__g.viewport_w - x || h > _d3d11_renderer_internal__g.viewport_h - y) return FALSE;
    _d3d11_renderer_internal__flush_batches();
    ID3D11RenderTargetView* saved_targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {0};
    ID3D11DepthStencilView* saved_depth = NULL_POINTER;
    ID3D11Resource* resource = NULL_POINTER;
    ID3D11Texture2D* target = NULL_POINTER;
    boole ok = FALSE;
    ID3D11DeviceContext_OMGetRenderTargets(ctx, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved_targets, &saved_depth);
    if (!saved_targets[0]) goto cleanup;
    D3D11_RENDER_TARGET_VIEW_DESC view;
    ID3D11RenderTargetView_GetDesc(saved_targets[0], &view);
    // Preserve byte-domain color and avoid silently changing HDR/sRGB semantics.
    if (view.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || view.Texture2D.MipSlice != 0 || (view.Format != DXGI_FORMAT_R8G8B8A8_UNORM && view.Format != DXGI_FORMAT_B8G8R8A8_UNORM)) goto cleanup;
    ID3D11RenderTargetView_GetResource(saved_targets[0], &resource);
    if (FAILED(ID3D11Resource_QueryInterface(resource, &IID_ID3D11Texture2D, (void**)&target))) goto cleanup;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(target, &td);
    if (td.SampleDesc.Count != 1 || td.ArraySize != 1 || (UINT)w > td.Width - (UINT)x || (UINT)h > td.Height - (UINT)y || (UINT)x >= td.Width || (UINT)y >= td.Height) goto cleanup;
    if (!_d3d11_renderer_internal__prepare_blur(w, h, view.Format)) goto cleanup;

    D3D11_VIEWPORT saved_viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11DeviceContext_RSGetViewports(ctx, &count, saved_viewports);
    D3D11_RECT saved_scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT scissor_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11DeviceContext_RSGetScissorRects(ctx, &scissor_count, saved_scissors);
    ID3D11Buffer* saved_constants = NULL_POINTER;
    ID3D11DeviceContext_PSGetConstantBuffers(ctx, 0, 1, &saved_constants);
    ID3D11ShaderResourceView* none = NULL_POINTER;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL_POINTER, NULL_POINTER);
    D3D11_BOX region = {(UINT)x, (UINT)y, 0, (UINT)(x+w), (UINT)(y+h), 1};
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource*)_d3d11_renderer_internal__g.blur_texture[0], 0, 0, 0, 0, resource, 0, &region);
    D3D11_VIEWPORT viewport = {0, 0, (float)w, (float)h, 0, 1};
    D3D11_RECT scissor = {0, 0, w, h};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &viewport);
    ID3D11DeviceContext_RSSetState(ctx, _d3d11_renderer_internal__g.rasterizer_state);
    ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &scissor);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL_POINTER, NULL_POINTER, 0xFFFFFFFF);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL_POINTER);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, _d3d11_renderer_internal__g.blur_vs, NULL_POINTER, 0);
    ID3D11DeviceContext_PSSetShader(ctx, _d3d11_renderer_internal__g.blur_ps, NULL_POINTER, 0);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &_d3d11_renderer_internal__g.text_sampler);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &_d3d11_renderer_internal__g.blur_constants);
    for (int pass = 0; pass < 6; pass++) {
        int source = pass & 1;
        int parameters[4] = {w, h, radius, source};
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource*)_d3d11_renderer_internal__g.blur_constants, 0, NULL_POINTER, parameters, 0, 0);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &_d3d11_renderer_internal__g.blur_rtv[1-source], NULL_POINTER);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &_d3d11_renderer_internal__g.blur_srv[source]);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none);
    }
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL_POINTER, NULL_POINTER);
    region.left = region.top = 0; region.right = w; region.bottom = h;
    ID3D11DeviceContext_CopySubresourceRegion(ctx, resource, 0, x, y, 0, (ID3D11Resource*)_d3d11_renderer_internal__g.blur_texture[0], 0, &region);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved_targets, saved_depth);
    ID3D11DeviceContext_RSSetViewports(ctx, count, saved_viewports);
    ID3D11DeviceContext_RSSetScissorRects(ctx, scissor_count, saved_scissors);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &saved_constants);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(saved_constants);
    ok = TRUE;
cleanup:
    for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(saved_targets[i]);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(saved_depth);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(target);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(resource);
    return ok;
}
