// slfg.cpp: frame generation through NVIDIA Streamline (sl.dlss_g), the same
// library Cyberpunk ships. Streamline owns presentation and pacing (proxy
// swapchain + its own present thread); CrossOver/D3DMetal does the actual
// interpolation behind NGX. We only feed it what RDR2 gives DLSS each frame.
// Needs sl.interposer/common/dlss_g/reflex/pcl.dll next to version.dll.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <sl_matrix_helpers.h>
#include "slfg.h"

static void (*logf_)(const char *);
static void LOG(const char *fmt, ...) {
    if (!logf_) return;
    char b[600];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    logf_(b);
}

static int state;  // 0 = not tried, 1 = active, -1 = unavailable
static HMODULE slMod;
static decltype(&slInit) pInit;
static decltype(&slSetD3DDevice) pSetDevice;
static decltype(&slIsFeatureSupported) pSupported;
static decltype(&slUpgradeInterface) pUpgrade;
static decltype(&slGetNewFrameToken) pNewToken;
static decltype(&slSetConstants) pSetConstants;
static decltype(&slSetTagForFrame) pSetTag;
static decltype(&slGetFeatureFunction) pFeatureFn;
static PFun_slDLSSGSetOptions *pDLSSGSetOptions;
static PFun_slDLSSGGetState *pDLSSGGetState;
static PFun_slReflexSetOptions *pReflexSetOptions;
static PFun_slReflexSleep *pReflexSleep;
static PFun_slPCLSetMarker *pMarker;

static const sl::ViewportHandle viewport{0u};
static sl::FrameToken *token;  // frame being built (set at frame start)
static uint32_t frameIndex;
static int fgOn, wantOn = 1, supported = 1;
static LONG presents, dlssFrames, missed, onAt;  // dlssFrames: DLSS evaluates since the last present

// Streamline repeats some harmless messages every frame (missing CrossOver NVAPI
// functions), which would grow the log by ~80 MB an hour. Each distinct message
// (by source file:line) is logged 3 times, then hidden.
static void on_sl_log(sl::LogType type, const char *msg) {
    if (type == sl::LogType::eInfo) return;  // warnings and errors only
    const char *p = msg;
    while (*p == '[') { const char *e = strchr(p, ']'); if (!e) break; p = e + 1; }  // skip [time][streamline][level][tid][uptime]
    unsigned h = 2166136261u;
    for (const char *q = p; *q && *q != '['; q++) h = (h ^ (unsigned char)*q) * 16777619u;  // "file.cpp:123"
    static unsigned seen[128], count[128];
    unsigned i = h % 128;
    while (seen[i] && seen[i] != h) i = (i + 1) % 128;  // ponytail: 128 distinct messages max, then all repeat-log
    seen[i] = h;
    if (++count[i] > 3) return;
    if (count[i] == 3) { LOG("SL %s (3rd time, further repeats not logged): %.*s", type == sl::LogType::eError ? "ERROR" : "warn",
                             (int)strcspn(p, "\r\n"), p); return; }
    LOG("SL %s: %.*s", type == sl::LogType::eError ? "ERROR" : "warn", (int)strcspn(p, "\r\n"), p);
}

extern "C" void slfg_set_logger(void (*f)(const char *)) { logf_ = f; }
extern "C" int slfg_active(void) { return state == 1; }

// Loads Streamline from `dir` (version.dll's folder). Safe to call repeatedly.
extern "C" int slfg_startup(const wchar_t *dir) {
    if (state) return state == 1;
    state = -1;
    wchar_t path[1100];
    swprintf(path, 1100, L"%ls\\sl.interposer.dll", dir);
    HMODULE m = slMod = LoadLibraryW(path);
    if (!m) { LOG("STREAMLINE not found (%ls) - using built-in presenting", path); return 0; }
#define GET(var, name) var = (decltype(var))GetProcAddress(m, name)
    GET(pInit, "slInit"); GET(pSetDevice, "slSetD3DDevice"); GET(pSupported, "slIsFeatureSupported");
    GET(pUpgrade, "slUpgradeInterface"); GET(pNewToken, "slGetNewFrameToken");
    GET(pSetConstants, "slSetConstants"); GET(pSetTag, "slSetTagForFrame"); GET(pFeatureFn, "slGetFeatureFunction");
#undef GET
    if (!pInit || !pUpgrade || !pFeatureFn) { LOG("STREAMLINE: sl.interposer.dll is missing exports"); return 0; }

    static sl::Feature features[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
    static const wchar_t *paths[1];
    static wchar_t dirCopy[1024];
    wcsncpy(dirCopy, dir, 1023);
    paths[0] = dirCopy;
    sl::Preferences pref{};
    pref.logLevel = sl::LogLevel::eDefault;
    pref.pathsToPlugins = paths;
    pref.numPathsToPlugins = 1;
    // No pathToLogsAndData: no sl.log file; Streamline's warnings/errors go to ngxlog.txt.
    pref.logMessageCallback = on_sl_log;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 3;
    pref.renderAPI = sl::RenderAPI::eD3D12;
    pref.applicationId = 231313132;  // NVIDIA's sample/test id; NGX features need one
    sl::Result r = pInit(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk) { LOG("STREAMLINE slInit failed (%d)", (int)r); return 0; }

    LOG("STREAMLINE initialized");
    state = 1;
    return 1;
}

// Feature functions only exist once Streamline has a device (slSetD3DDevice).
static void get_feature_functions(void) {
    void *f = nullptr;
    if (pFeatureFn(sl::kFeatureDLSS_G, "slDLSSGSetOptions", f) == sl::Result::eOk) pDLSSGSetOptions = (PFun_slDLSSGSetOptions *)f;
    if (pFeatureFn(sl::kFeatureDLSS_G, "slDLSSGGetState", f) == sl::Result::eOk) pDLSSGGetState = (PFun_slDLSSGGetState *)f;
    if (pFeatureFn(sl::kFeatureReflex, "slReflexSetOptions", f) == sl::Result::eOk) pReflexSetOptions = (PFun_slReflexSetOptions *)f;
    if (pFeatureFn(sl::kFeatureReflex, "slReflexSleep", f) == sl::Result::eOk) pReflexSleep = (PFun_slReflexSleep *)f;
    if (pFeatureFn(sl::kFeaturePCL, "slPCLSetMarker", f) == sl::Result::eOk) pMarker = (PFun_slPCLSetMarker *)f;
    LOG("STREAMLINE functions: DLSS-G options %p state %p, reflex %p/%p, markers %p", (void *)pDLSSGSetOptions,
        (void *)pDLSSGGetState, (void *)pReflexSetOptions, (void *)pReflexSleep, (void *)pMarker);
}

// Called with the game's real D3D12 device (from its first DLSS init).
extern "C" void slfg_set_device(void *device) {
    static void *done;
    if (state != 1 || !device || device == done) return;
    done = device;
    sl::Result r = pSetDevice(device);
    ID3D12Device *d = (ID3D12Device *)device;
    LUID luid = d->GetAdapterLuid();
    sl::AdapterInfo ai{};
    ai.deviceLUID = (uint8_t *)&luid;
    ai.deviceLUIDSizeInBytes = sizeof luid;
    sl::Result s = pSupported(sl::kFeatureDLSS_G, ai);
    LOG("STREAMLINE device %p set (%d); DLSS-G supported: %s (%d)", device, (int)r,
        s == sl::Result::eOk ? "YES" : "NO", (int)s);
    if (s != sl::Result::eOk) wantOn = supported = 0;
    get_feature_functions();
    if (pReflexSetOptions) {
        sl::ReflexOptions ro{};
        ro.mode = sl::ReflexMode::eLowLatency;  // DLSS-G requires Reflex
        LOG("STREAMLINE reflex low-latency -> %d", (int)pReflexSetOptions(ro));
    }
}

static void set_mode(int on) {
    if (!pDLSSGSetOptions) return;
    sl::DLSSGOptions o{};
    o.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    o.numFramesToGenerate = 1;
    sl::Result r = pDLSSGSetOptions(viewport, o);
    if (on != fgOn) LOG("STREAMLINE frame generation %s -> %d", on ? "ON" : "OFF", (int)r);
    if (on && !fgOn) onAt = presents;
    fgOn = on;
}

static void begin_frame(void) {  // new frame token + Reflex sleep + simulation markers
    ++frameIndex;
    if (pNewToken(token, &frameIndex) != sl::Result::eOk) { token = nullptr; return; }
    if (pReflexSleep) pReflexSleep(*token);
    if (pMarker) { pMarker(sl::PCLMarker::eSimulationStart, *token); }
}

// Per frame, from the game's DLSS evaluate: camera constants + depth/MV tags.
extern "C" void slfg_dlss_frame(void *cmdlist, void *depth, void *mvecs, unsigned depthW, unsigned depthH,
                                unsigned mvW, unsigned mvH, unsigned outW, unsigned outH,
                                float jitterX, float jitterY, float mvScaleX, float mvScaleY,
                                int depthInverted, int reset) {
    if (state != 1) return;
    begin_frame();  // one DLSS evaluate per game frame (verified in RDR2 logs) = one frame token
    InterlockedIncrement(&dlssFrames);
    if (!token) return;
    if (pMarker) { pMarker(sl::PCLMarker::eSimulationEnd, *token); pMarker(sl::PCLMarker::eRenderSubmitStart, *token); }

    // RDR2 gives DLSS no camera, so build one: vertical FOV 60 deg, near 0.15
    // (RDR2's near plane), far 10000, reversed-Z perspective, row-vector form.
    const float fov = 60.0f * 3.14159265f / 180.0f, nearZ = 0.15f, farZ = 10000.0f;
    const float aspect = outH ? (float)outW / outH : 16.0f / 9.0f, f = 1.0f / tanf(fov / 2);
    sl::Constants c{};
    for (int i = 0; i < 4; i++) c.cameraViewToClip[i] = sl::float4(0, 0, 0, 0);
    c.cameraViewToClip[0].x = f / aspect;
    c.cameraViewToClip[1].y = f;
    c.cameraViewToClip[2].z = depthInverted ? nearZ / (nearZ - farZ) : farZ / (farZ - nearZ);
    c.cameraViewToClip[2].w = 1;
    c.cameraViewToClip[3].z = depthInverted ? -farZ * nearZ / (nearZ - farZ) : -nearZ * farZ / (farZ - nearZ);
    sl::matrixFullInvert(c.clipToCameraView, c.cameraViewToClip);
    sl::float4x4 id{};
    for (int i = 0; i < 4; i++) id[i] = sl::float4(i == 0, i == 1, i == 2, i == 3);
    c.clipToLensClip = id;
    c.clipToPrevClip = id;   // camera motion is already in the motion vectors
    c.prevClipToClip = id;
    c.jitterOffset = { jitterX, jitterY };
    c.mvecScale = { mvW ? mvScaleX / mvW : 1, mvH ? mvScaleY / mvH : 1 };  // Streamline wants [-1,1]
    c.cameraPinholeOffset = { 0, 0 };
    c.cameraPos = { 0, 0, 0 };
    c.cameraUp = { 0, 1, 0 };
    c.cameraRight = { 1, 0, 0 };
    c.cameraFwd = { 0, 0, 1 };
    c.cameraNear = nearZ;
    c.cameraFar = farZ;
    c.cameraFOV = fov;  // radians, per Streamline
    c.cameraAspectRatio = aspect;
    c.depthInverted = depthInverted ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = reset || !fgOn ? sl::Boolean::eTrue : sl::Boolean::eFalse;  // fresh start after frame gen was off
    sl::Result rc = pSetConstants(c, *token, viewport);

    // Copied by Streamline right now (eOnlyValidNow), on the game's command list.
    const uint32_t srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    sl::Resource rd(sl::ResourceType::eTex2d, depth, srv), rm(sl::ResourceType::eTex2d, mvecs, srv);
    sl::Extent ed{ 0, 0, depthW, depthH }, em{ 0, 0, mvW, mvH };
    sl::ResourceTag tags[] = {
        sl::ResourceTag(&rd, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &ed),
        sl::ResourceTag(&rm, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &em),
    };
    sl::Result rt = pSetTag(*token, viewport, tags, 2, (sl::CommandBuffer *)cmdlist);
    static int logged;
    if (logged++ < 3 || rc != sl::Result::eOk || rt != sl::Result::eOk)
        if (logged < 20) LOG("STREAMLINE frame %u: constants -> %d, tags -> %d (depth %ux%u mv %ux%u out %ux%u)",
                             frameIndex, (int)rc, (int)rt, depthW, depthH, mvW, mvH, outW, outH);
}

// ---- Present markers + frame-gen on/off around resizes ---------------------
// The game holds Streamline's proxy swapchain; we patch the proxy's method
// table (in sl.interposer.dll) to add the Reflex present markers it requires.
static void *oPresent, *oPresent1, *oResize, *oResize1, *oFullscreen;
static void *gameSc;  // the game's proxy (other proxies share its method table)

// Presents with no DLSS evaluate before them (pause menu, DLSS off) give frame
// generation nothing new to interpolate, so it goes off until DLSS frames come
// back. Three in a row, not one, in case evaluate and Present ever run out of order.
static void before_present(void) {
    if (InterlockedExchange(&dlssFrames, 0)) missed = 0;
    else if (++missed >= 3 && fgOn) set_mode(0);
    if (!token) return;
    if (pMarker) { pMarker(sl::PCLMarker::eRenderSubmitEnd, *token); pMarker(sl::PCLMarker::ePresentStart, *token); }
    if (wantOn && !fgOn && !missed) set_mode(1);
}
static void after_present(void) {
    if (token && pMarker) pMarker(sl::PCLMarker::ePresentEnd, *token);
    LONG n = ++presents;
    if (pDLSSGGetState && (n == onAt + 60 || n % 3600 == 0)) {  // status=0x0: real generated frames
        sl::DLSSGState s{};
        if (pDLSSGGetState(viewport, s, nullptr) == sl::Result::eOk)
            LOG("STREAMLINE status after %ld presents: status=0x%x framesPresented=%u vsyncSupport=%d minSize=%u vram=%llu MB",
                n, (unsigned)s.status, s.numFramesActuallyPresented, (int)s.bIsVsyncSupportAvailable,
                s.minWidthOrHeight, (unsigned long long)(s.estimatedVRAMUsageInBytes >> 20));
    }
}
static HRESULT STDMETHODCALLTYPE h_Present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    if ((void *)sc != gameSc || (flags & DXGI_PRESENT_TEST))  // not the game's own, or only asks "is the window visible?"
        return ((HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT))oPresent)(sc, sync, flags);
    before_present();
    HRESULT hr = ((HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT))oPresent)(sc, sync, flags);
    after_present();
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_Present1(IDXGISwapChain1 *sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS *pp) {
    if ((void *)sc != gameSc || (flags & DXGI_PRESENT_TEST))
        return ((HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *))oPresent1)(sc, sync, flags, pp);
    before_present();
    HRESULT hr = ((HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *))oPresent1)(sc, sync, flags, pp);
    after_present();
    return hr;
}
// Streamline's guide: frame generation must be off while resizing/switching fullscreen.
static HRESULT STDMETHODCALLTYPE h_Resize(IDXGISwapChain *sc, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT fl) {
    set_mode(0);
    HRESULT hr = ((HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT))oResize)(sc, n, w, h, fmt, fl);
    LOG("STREAMLINE ResizeBuffers %ux%u -> 0x%lx (frame gen resumes on next present)", w, h, hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_Resize1(IDXGISwapChain3 *sc, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT fl,
                                           const UINT *mask, IUnknown *const *q) {
    set_mode(0);
    return ((HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain3 *, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT *, IUnknown *const *))oResize1)(sc, n, w, h, fmt, fl, mask, q);
}
static HRESULT STDMETHODCALLTYPE h_Fullscreen(IDXGISwapChain *sc, BOOL on, IDXGIOutput *out) {
    set_mode(0);
    HRESULT hr = ((HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, BOOL, IDXGIOutput *))oFullscreen)(sc, on, out);
    LOG("STREAMLINE SetFullscreenState(%d) -> 0x%lx", on, hr);
    return hr;
}

static void patch(void **vt, int i, void *fn, void **o) {
    DWORD old;
    VirtualProtect(&vt[i], sizeof(void *), PAGE_READWRITE, &old);
    *o = vt[i];
    vt[i] = fn;
    VirtualProtect(&vt[i], sizeof(void *), old, &old);
}

static int in_sl(void *p) {
    HMODULE owner = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)p, &owner);
    return owner == slMod;
}

// The swapchain the game got from Streamline's proxy factory: already a proxy.
// Hook the proxy's methods for the markers. (Swapchains Streamline creates on
// the native factory are its own internal ones; never wrap or patch those.)
static void game_swapchain(void **sc) {
    static void **vt;
    if (!sc || !*sc) return;
    void **v = *(void ***)*sc;
    LOG("STREAMLINE game swapchain %p, methods %p %s", *sc, (void *)v, in_sl(v) ? "in sl.interposer" : "NOT Streamline's - markers not hooked");
    gameSc = *sc;
    if (!in_sl(v) || v == vt) return;
    vt = v;
    patch(v, 8, (void *)h_Present, &oPresent);
    patch(v, 22, (void *)h_Present1, &oPresent1);
    patch(v, 13, (void *)h_Resize, &oResize);
    patch(v, 39, (void *)h_Resize1, &oResize1);
    patch(v, 10, (void *)h_Fullscreen, &oFullscreen);
}

static void *oFacSC, *oFacHwnd, *oFacCore, *oFacComp;
static HRESULT STDMETHODCALLTYPE pf_CreateSC(void *f, IUnknown *dev, void *desc, void **out) {
    HRESULT hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, IUnknown *, void *, void **))oFacSC)(f, dev, desc, out);
    if (SUCCEEDED(hr)) game_swapchain(out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE pf_CreateSCHwnd(void *f, IUnknown *dev, HWND w, void *d, void *fd, void *o, void **out) {
    HRESULT hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, IUnknown *, HWND, void *, void *, void *, void **))oFacHwnd)(f, dev, w, d, fd, o, out);
    if (SUCCEEDED(hr)) game_swapchain(out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE pf_CreateSCCore(void *f, IUnknown *dev, IUnknown *win, void *d, void *o, void **out) {
    HRESULT hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, IUnknown *, IUnknown *, void *, void *, void **))oFacCore)(f, dev, win, d, o, out);
    if (SUCCEEDED(hr)) game_swapchain(out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE pf_CreateSCComp(void *f, IUnknown *dev, void *d, void *o, void **out) {
    HRESULT hr = ((HRESULT(STDMETHODCALLTYPE *)(void *, IUnknown *, void *, void *, void **))oFacComp)(f, dev, d, o, out);
    if (SUCCEEDED(hr)) game_swapchain(out);
    return hr;
}

// Returns Streamline's proxy for a freshly created DXGI factory and hooks the
// proxy's swapchain-creation methods; the game creates its swapchain through
// it, so Streamline owns presentation and the game holds Streamline's proxy.
extern "C" void *slfg_wrap_factory(void *factory) {
    static void **vt;
    if (state != 1 || !factory) return factory;
    void *p = factory;
    sl::Result r = pUpgrade(&p);
    LOG("STREAMLINE factory %p -> proxy %p (%d)", factory, p, (int)r);
    if (r != sl::Result::eOk) return factory;
    void **v = *(void ***)p;
    if (in_sl(v) && v != vt) {  // methods 10/15/16/24: CreateSwapChain, ForHwnd, ForCoreWindow, ForComposition
        vt = v;
        patch(v, 10, (void *)pf_CreateSC, &oFacSC);
        patch(v, 15, (void *)pf_CreateSCHwnd, &oFacHwnd);
        patch(v, 16, (void *)pf_CreateSCCore, &oFacCore);
        patch(v, 24, (void *)pf_CreateSCComp, &oFacComp);
    }
    return p;
}

// From the DLSS side when DLSS is released/restarted (pause menus etc.).
extern "C" void slfg_dlss_paused(int paused) { wantOn = !paused && supported; if (paused) set_mode(0); }
