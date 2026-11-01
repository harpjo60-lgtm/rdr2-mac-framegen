// fg.h: drives D3DMetal's built-in NGX frame generator (feature 11, "DLSSG"),
// which it backs with MetalFX frame interpolation. Shared by the RDR2 DLL and
// the standalone test so both run the exact same calls.
#pragma once
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS  // MSVC-correct struct returns (GetDesc)
#include <windows.h>
#include <d3d12.h>
#include <stdint.h>

#define NGX_OK 1
#define NGX_FEATURE_FRAMEGEN 11

// NVSDK_NGX_Parameter vtable slots (MSVC orders overloads in reverse).
enum { SET_VOIDP, SET_D3D12RES, SET_D3D11RES, SET_INT, SET_UINT, SET_DOUBLE, SET_FLOAT, SET_ULL,
       GET_VOIDP, GET_D3D12RES, GET_D3D11RES, GET_INT, GET_UINT, GET_DOUBLE, GET_FLOAT, GET_ULL };
#define PSLOT(p, i) ((*(void ***)(p))[i])
static inline void pset_res(void *p, const char *k, void *v) { ((void (*)(void *, const char *, void *))PSLOT(p, SET_D3D12RES))(p, k, v); }
static inline void pset_uint(void *p, const char *k, unsigned v) { ((void (*)(void *, const char *, unsigned))PSLOT(p, SET_UINT))(p, k, v); }
static inline void pset_int(void *p, const char *k, int v) { ((void (*)(void *, const char *, int))PSLOT(p, SET_INT))(p, k, v); }
static inline void pset_float(void *p, const char *k, float v) { ((void (*)(void *, const char *, float))PSLOT(p, SET_FLOAT))(p, k, v); }
// Out buffer is always 8 bytes so a type mismatch can't overrun it.
static inline int pget(void *p, int slot, const char *k, uint64_t *out) {
    *out = 0;
    return ((int (*)(void *, const char *, uint64_t *))PSLOT(p, slot))(p, k, out);
}

typedef intptr_t (*ngx_alloc_fn)(void **);
typedef intptr_t (*ngx_create_fn)(void *cmd, int feature, void *params, void **handle);
typedef intptr_t (*ngx_eval_fn)(void *cmd, void *handle, void *params, void *cb);

// One frame's worth of inputs, taken from the game's DLSS upscaling call.
typedef struct {
    ID3D12Resource *color;   // the frame to interpolate from (backbuffer, HUD included)
    ID3D12Resource *hudless; // same frame without HUD, or NULL
    ID3D12Resource *depth, *mvecs;   // render-res
    ID3D12Resource *out, *outReal;   // generated frame / D3DMetal's copy of the real one
    unsigned colorW, colorH, depthW, depthH, mvW, mvH;
    float jitterX, jitterY, mvScaleX, mvScaleY;
    int depthInverted, reset;
} FgFrame;

// RDR2 never gives DLSS its camera, so these are fixed. D3DMetal passes them
// straight to MetalFX (verified in its code: CameraFOV -> setFieldOfView:), and
// MetalFX wants the vertical FOV in DEGREES (Apple docs) — not radians as in
// NVIDIA's DLSS-G. RDR2 near plane is 0.15 m (reversed-Z); FOV 60 / far 10000
// are OptiScaler's defaults for games that give no camera data.
// ponytail: fixed FOV; if the in-game FOV slider is far from 60, generated frames
// warp slightly — read the game's camera if that matters.
#define FG_CAMERA_NEAR 0.15f
#define FG_CAMERA_FAR  10000.0f
#define FG_CAMERA_FOV  60.0f  // degrees, vertical

// A UAV-capable texture shaped like `like` (the generated frame's destination).
static inline ID3D12Resource *fg_make_output(ID3D12Resource *like) {
    D3D12_RESOURCE_DESC d = ID3D12Resource_GetDesc(like);
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    d.MipLevels = 1;
    ID3D12Device *dev = NULL;
    if (FAILED(ID3D12Resource_GetDevice(like, &IID_ID3D12Device, (void **)&dev))) return NULL;
    D3D12_HEAP_PROPERTIES hp = { .Type = D3D12_HEAP_TYPE_DEFAULT };
    ID3D12Resource *r = NULL;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&r);
    ID3D12Device_Release(dev);
    return r;
}

static inline intptr_t fg_create(ngx_create_fn create, void *cmd, void *params, unsigned w, unsigned h, void **handle) {
    pset_uint(params, "Width", w);
    pset_uint(params, "Height", h);
    pset_uint(params, "CreationNodeMask", 1);
    pset_uint(params, "VisibilityNodeMask", 1);
    *handle = NULL;
    return create(cmd, NGX_FEATURE_FRAMEGEN, params, handle);
}

static inline intptr_t fg_eval(ngx_eval_fn eval, void *cmd, void *handle, void *p, const FgFrame *f) {
    pset_res(p, "DLSSG.Backbuffer", f->color);
    pset_res(p, "DLSSG.HUDLess", f->hudless);  // NULL = none (params persist between calls)
    pset_res(p, "DLSSG.Depth", f->depth);
    pset_res(p, "DLSSG.MVecs", f->mvecs);
    pset_res(p, "DLSSG.OutputInterpolated", f->out);
    pset_res(p, "DLSSG.OutputReal", f->outReal);
    pset_uint(p, "DLSSG.BackbufferSubrectWidth", f->colorW);
    pset_uint(p, "DLSSG.BackbufferSubrectHeight", f->colorH);
    pset_uint(p, "DLSSG.HUDLessSubrectWidth", f->colorW);
    pset_uint(p, "DLSSG.HUDLessSubrectHeight", f->colorH);
    pset_uint(p, "DLSSG.DepthSubrectWidth", f->depthW);
    pset_uint(p, "DLSSG.DepthSubrectHeight", f->depthH);
    pset_uint(p, "DLSSG.MVecsSubrectWidth", f->mvW);
    pset_uint(p, "DLSSG.MVecsSubrectHeight", f->mvH);
    // D3DMetal hands this straight to MetalFX, which wants pixel units — the same
    // meaning as DLSS's MV.Scale, so it passes through unchanged (measured: fgmotion.c).
    pset_float(p, "DLSSG.MvecScaleX", f->mvScaleX);
    pset_float(p, "DLSSG.MvecScaleY", f->mvScaleY);
    pset_float(p, "DLSSG.JitterOffsetX", f->jitterX);
    pset_float(p, "DLSSG.JitterOffsetY", f->jitterY);
    pset_float(p, "DLSSG.CameraNear", FG_CAMERA_NEAR);
    pset_float(p, "DLSSG.CameraFar", FG_CAMERA_FAR);
    pset_float(p, "DLSSG.CameraFOV", FG_CAMERA_FOV);
    pset_float(p, "DLSSG.CameraAspectRatio", (float)f->colorW / (float)f->colorH);
    pset_int(p, "DLSSG.DepthInverted", f->depthInverted);
    pset_int(p, "DLSSG.Reset", f->reset);
    pset_int(p, "DLSSG.run_lowres_mvec_pass", f->mvW != f->colorW);
    return eval(cmd, handle, p, NULL);
}
