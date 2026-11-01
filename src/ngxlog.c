// ngxlog: frame generation for RDR2 under CrossOver/D3DMetal.
// Loads as version.dll (forwards everything to the real one) and hooks the
// game's DLSS (NGX) calls to grab each frame's depth and motion vectors. Those
// are handed to NVIDIA Streamline's DLSS Frame Generation (slfg.cpp), which
// D3DMetal runs on Apple's MetalFX frame interpolator. The older path that
// presents generated frames itself (fg.h) is kept as a fallback.
// No code is patched; only export/import/method tables are repointed.
#define WIN32_LEAN_AND_MEAN
#include "fg.h"
#include "slfg.h"
#include <dxgi1_4.h>
#include <psapi.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

static FILE *logf;
static CRITICAL_SECTION lk;
static wchar_t gDir[1024];  // folder of this DLL (= the game's folder)

static void L(const char *fmt, ...) {
    if (!logf) return;
    EnterCriticalSection(&lk);
    fprintf(logf, "[%8.3f] ", GetTickCount64() / 1000.0);
    va_list ap; va_start(ap, fmt); vfprintf(logf, fmt, ap); va_end(ap);
    fputc('\n', logf);
    fflush(logf);
    LeaveCriticalSection(&lk);
}

static void slog(const char *s) { L("%s", s); }

// Black box: the most recent detailed events, written to the log only on a crash.
#define RING 512
static char ring[RING][200];
static volatile LONG ringPos;
static void R(const char *fmt, ...) {
    LONG i = InterlockedIncrement(&ringPos) - 1;
    char *s = ring[i % RING];
    int n = snprintf(s, sizeof ring[0], "[%8.3f t%lu] ", GetTickCount64() / 1000.0, GetCurrentThreadId());
    va_list ap; va_start(ap, fmt); vsnprintf(s + n, sizeof ring[0] - n, fmt, ap); va_end(ap);
}
static void ring_dump(int last) {
    LONG end = ringPos, start = end > last ? end - last : 0;
    L("--- black box: last %ld events before this point:", end - start);
    for (LONG i = start; i < end; i++) L("  %s", ring[i % RING]);
    L("--- end of black box");
}

// ---- version.dll forwarding ----------------------------------------------
#define VERSION_EXPORTS \
    V(GetFileVersionInfoA) V(GetFileVersionInfoByHandle) V(GetFileVersionInfoExA) \
    V(GetFileVersionInfoExW) V(GetFileVersionInfoSizeA) V(GetFileVersionInfoSizeExA) \
    V(GetFileVersionInfoSizeExW) V(GetFileVersionInfoSizeW) V(GetFileVersionInfoW) \
    V(VerFindFileA) V(VerFindFileW) V(VerInstallFileA) V(VerInstallFileW) \
    V(VerLanguageNameA) V(VerLanguageNameW) V(VerQueryValueA) V(VerQueryValueW)

#define V(n) #n,
static const char *vnames[] = { VERSION_EXPORTS };
#undef V
#define NV (sizeof vnames / sizeof *vnames)
FARPROC vreal[NV];

// Each export is a bare jmp through vreal[i] (same order as vnames), so any
// signature passes through untouched.
#define STUB(i, n) __asm__(".text\n.globl " #n "\n" #n ": jmp *vreal+8*" #i "(%rip)\n");
STUB(0, GetFileVersionInfoA)       STUB(1, GetFileVersionInfoByHandle)
STUB(2, GetFileVersionInfoExA)     STUB(3, GetFileVersionInfoExW)
STUB(4, GetFileVersionInfoSizeA)   STUB(5, GetFileVersionInfoSizeExA)
STUB(6, GetFileVersionInfoSizeExW) STUB(7, GetFileVersionInfoSizeW)
STUB(8, GetFileVersionInfoW)       STUB(9, VerFindFileA)
STUB(10, VerFindFileW)             STUB(11, VerInstallFileA)
STUB(12, VerInstallFileW)          STUB(13, VerLanguageNameA)
STUB(14, VerLanguageNameW)         STUB(15, VerQueryValueA)
STUB(16, VerQueryValueW)

static void load_real_version(void) {
    wchar_t p[MAX_PATH];
    GetSystemDirectoryW(p, MAX_PATH);
    wcscat(p, L"\\version.dll");
    HMODULE m = LoadLibraryW(p);
    if (!m) { L("FATAL: could not load real version.dll (err %lu)", GetLastError()); return; }
    for (unsigned i = 0; i < NV; i++) vreal[i] = GetProcAddress(m, vnames[i]);
}

// ---- NGX parameter dumping (parameter access lives in fg.h) --------------
// Params can still hold pointers to textures the game already deleted (seen on
// DLSS re-create after fullscreen/pause), so only the 4 inputs the game sets on
// every evaluate are ever touched (`live`); everything else is logged as a pointer.
static void dump_params(void *p, int live) {
    static const char *res[] = { "Color", "Output", "Depth", "MotionVectors",
        "ExposureTexture", "TransparencyMask", "DLSS.Input.Bias.Current.Color.Mask" };
    static const char *uints[] = { "Width", "Height", "OutWidth", "OutHeight",
        "PerfQualityValue", "DLSS.Feature.Create.Flags", "Reset",
        "DLSS.Render.Subrect.Dimensions.Width", "DLSS.Render.Subrect.Dimensions.Height" };
    static const char *floats[] = { "Jitter.Offset.X", "Jitter.Offset.Y",
        "MV.Scale.X", "MV.Scale.Y", "Sharpness" };
    if (!p) { L("    params: NULL"); return; }
    uint64_t v;
    for (unsigned i = 0; i < sizeof res / sizeof *res; i++) {
        if (pget(p, GET_D3D12RES, res[i], &v) != NGX_OK || !v) continue;
        if (!live || i >= 4) { L("    res   %-40s %p", res[i], (void *)v); continue; }
        D3D12_RESOURCE_DESC d = ID3D12Resource_GetDesc((ID3D12Resource *)v);
        L("    res   %-40s %p  %llux%u fmt=%u flags=0x%x", res[i], (void *)v,
          (unsigned long long)d.Width, d.Height, d.Format, d.Flags);
    }
    for (unsigned i = 0; i < sizeof uints / sizeof *uints; i++)
        if (pget(p, GET_UINT, uints[i], &v) == NGX_OK)
            L("    uint  %-40s %u", uints[i], (unsigned)v);
    for (unsigned i = 0; i < sizeof floats / sizeof *floats; i++)
        if (pget(p, GET_FLOAT, floats[i], &v) == NGX_OK) {
            float f; memcpy(&f, &v, 4);
            L("    float %-40s %f", floats[i], f);
        }
}

// ---- NGX hooks -------------------------------------------------------------
#define NGX_FUNCS \
    X(Init) X(Init_Ext) X(Init_ProjectID) X(Shutdown) X(Shutdown1) \
    X(GetParameters) X(GetScratchBufferSize) X(CreateFeature) X(EvaluateFeature) \
    X(ReleaseFeature) X(GetFeatureRequirements) X(AllocateParameters) \
    X(GetCapabilityParameters) X(DestroyParameters)

enum {
#define X(n) F_##n,
    NGX_FUNCS
#undef X
    NF
};
static const char *fnames[] = {
#define X(n) "NVSDK_NGX_D3D12_" #n,
    NGX_FUNCS
#undef X
};

typedef intptr_t (*fn8)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t);
static fn8 orig[NF];
static volatile LONG calls[NF];

static void report(int f, intptr_t a, intptr_t b, intptr_t c, intptr_t d, intptr_t e, intptr_t r) {
    LONG n = InterlockedIncrement(&calls[f]);
    R("%s #%ld -> 0x%llx (%p %p %p %p)", fnames[f] + 16, n, (unsigned long long)r,
      (void *)a, (void *)b, (void *)c, (void *)d);
    // Per-frame calls: first 5, then every 1000th. Everything else: always, in full.
    int frequent = f == F_EvaluateFeature || f == F_GetParameters || f == F_GetScratchBufferSize;
    if (frequent && n > 5 && n % 1000) return;
    L("%s #%ld -> 0x%llx  thread=%lu", fnames[f], n, (unsigned long long)r, GetCurrentThreadId());
    switch (f) {
    case F_Init: case F_Init_Ext:
        L("    appId=%llu device=%p", (unsigned long long)a, (void *)c); break;
    case F_Init_ProjectID:
        L("    project=%s engine=%d device=%p", a ? (char *)a : "(null)", (int)b, (void *)e); break;
    case F_GetParameters: case F_AllocateParameters: case F_GetCapabilityParameters: {
        void *p = a ? *(void **)a : NULL;
        L("    params=%p", p);
        uint64_t v;
        if (f == F_GetCapabilityParameters && p && r == NGX_OK) {
            static const char *caps[] = { "SuperSampling.Available", "FrameGeneration.Available" };
            for (int i = 0; i < 2; i++) {
                int ok = pget(p, GET_INT, caps[i], &v);
                L("    cap %-28s %s %d", caps[i], ok == NGX_OK ? "=" : "(get failed)", (int)v);
            }
        }
        break;
    }
    case F_CreateFeature:
        // feature 1 = DLSS upscaling, 11 = frame generation
        L("    cmdlist=%p feature=%d handle=%u", (void *)a, (int)b,
          d && *(void **)d ? **(unsigned **)d : 0);
        dump_params((void *)c, 0);
        break;
    case F_EvaluateFeature:
        L("    cmdlist=%p handle=%u", (void *)a, b ? *(unsigned *)b : 0);
        dump_params((void *)c, 1);
        break;
    case F_ReleaseFeature:
        L("    handle=%u", a ? *(unsigned *)a : 0); break;
    }
}

// ---- Frame generation, part 1: capture inputs at the DLSS call ------------
// The generated frame is made later, at Present, from the finished backbuffer
// (HUD included). Depth and motion vectors are copied here so they're still
// valid then.
#define DLSS_FLAG_MV_LOWRES       (1u << 1)
#define DLSS_FLAG_DEPTH_INVERTED  (1u << 3)

static unsigned dlssHandle = ~0u, dlssFlags, renderW, renderH;  // from DLSS CreateFeature
// The game's DLSS handle object. Matched by pointer, not by its Id: Streamline's own
// frame-generation handle can have the same Id, and its release must not pause us.
static void *dlssH;
static CRITICAL_SECTION fgLock;
// Engines like RDR2's may run DLSS for frame N+1 before presenting frame N, so
// captures queue up and each Present takes the oldest (FIFO keeps them paired).
#define NCAP 4
static FgFrame capQ[NCAP];
static ID3D12Resource *capDepth[NCAP], *capMv[NCAP];  // each slot owns its copies
static unsigned capHead, capCount, capMaxSeen;

static void *fgParams, *fgHandle;
static ID3D12Resource *fgOut, *fgOutReal;
static unsigned fgW, fgH;
static LONG fgEvals, fgEvalFails, fgWarm;
static int fgGaveUp;  // D3DMetal refused to create one; it won't change mid-run
// Set from the DLSS side, handled on the next Present: 1 = DLSS released (stop
// feeding the generator; keep it), 2 = NGX restarted (old objects are dead; forget).
// The generator is never released mid-game: D3DMetal finishes frames in the
// background, and freeing it under that work crashed RDR2 (pause/cutscene).
static volatile LONG fgDrop;
static const char *volatile stage = "startup";  // where we are, for the crash log

static ID3D12Device *device_of(ID3D12Resource *r) {  // identity only, no reference kept
    ID3D12Device *d = NULL;
    if (SUCCEEDED(ID3D12Resource_GetDevice(r, &IID_ID3D12Device, (void **)&d))) ID3D12Device_Release(d);
    return d;
}

static float pgetf(void *p, const char *k) {
    uint64_t v; float f = 0;
    if (pget(p, GET_FLOAT, k, &v) == NGX_OK) memcpy(&f, &v, 4);
    return f;
}
static unsigned pgetu(void *p, int slot, const char *k, unsigned dflt) {
    uint64_t v;
    return pget(p, slot, k, &v) == NGX_OK && (unsigned)v ? (unsigned)v : dflt;
}

// Copies src into *dst on cmd, (re)creating *dst when src's shape changes.
static int copy_into(void *cmd, ID3D12Resource *src, ID3D12Resource **dst) {
    D3D12_RESOURCE_DESC sd = ID3D12Resource_GetDesc(src);
    if (*dst && device_of(*dst) != device_of(src)) *dst = NULL;  // old device: forget, don't touch
    if (*dst) {
        D3D12_RESOURCE_DESC dd = ID3D12Resource_GetDesc(*dst);
        if (dd.Width != sd.Width || dd.Height != sd.Height || dd.Format != sd.Format) {
            ID3D12Resource_Release(*dst);
            *dst = NULL;
        }
    }
    if (!*dst) {
        ID3D12Device *dev = NULL;
        if (FAILED(ID3D12Resource_GetDevice(src, &IID_ID3D12Device, (void **)&dev))) return 0;
        D3D12_HEAP_PROPERTIES hp = { .Type = D3D12_HEAP_TYPE_DEFAULT };
        ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &sd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)dst);
        ID3D12Device_Release(dev);
        if (!*dst) return 0;
    }
    ID3D12GraphicsCommandList_CopyResource((ID3D12GraphicsCommandList *)cmd, *dst, src);
    return 1;
}

static void fg_hook(int f, intptr_t a, intptr_t b, intptr_t c, intptr_t d, intptr_t r) {
    if (f == F_ReleaseFeature && a && (void *)a == dlssH) {
        if (slfg_active()) slfg_dlss_paused(1);
        L("FRAMEGEN DLSS released; pausing frame generation");
        dlssHandle = ~0u;
        dlssH = NULL;
        InterlockedCompareExchange(&fgDrop, 1, 0);
    }
    if (f == F_Shutdown || f == F_Shutdown1 || f == F_Init || f == F_Init_Ext || f == F_Init_ProjectID) {
        if (fgHandle) L("FRAMEGEN NGX restarted; forgetting frame generator");
        dlssHandle = ~0u;
        dlssH = NULL;
        fgDrop = 2;
    }
    if (r != NGX_OK || !c) return;
    if ((f == F_Init || f == F_Init_Ext) && slfg_active()) slfg_set_device((void *)c);
    void *p = (void *)c;
    if (f == F_CreateFeature && b == 1 && d && *(void **)d) {  // DLSS upscaler created
        if (slfg_active()) slfg_dlss_paused(0);
        dlssH = *(void **)d;
        dlssHandle = *(unsigned *)dlssH;
        dlssFlags = pgetu(p, GET_INT, "DLSS.Feature.Create.Flags", 0);
        renderW = pgetu(p, GET_UINT, "Width", 0);
        renderH = pgetu(p, GET_UINT, "Height", 0);
        L("FRAMEGEN sees DLSS handle=%u flags=0x%x render=%ux%u", dlssHandle, dlssFlags, renderW, renderH);
        return;
    }
    if (fgGaveUp || f != F_EvaluateFeature || !b || (void *)b != dlssH) return;

    uint64_t depth, mvecs;
    if (pget(p, GET_D3D12RES, "Depth", &depth) != NGX_OK || !depth ||
        pget(p, GET_D3D12RES, "MotionVectors", &mvecs) != NGX_OK || !mvecs) return;

    FgFrame fr = {
        .jitterX = pgetf(p, "Jitter.Offset.X"), .jitterY = pgetf(p, "Jitter.Offset.Y"),
        .mvScaleX = pgetf(p, "MV.Scale.X"), .mvScaleY = pgetf(p, "MV.Scale.Y"),
        .depthInverted = !!(dlssFlags & DLSS_FLAG_DEPTH_INVERTED),
        .reset = (int)pgetu(p, GET_INT, "Reset", 0) };
    unsigned outW = pgetu(p, GET_UINT, "OutWidth", 0), outH = pgetu(p, GET_UINT, "OutHeight", 0);
    uint64_t out;
    if ((!outW || !outH) && pget(p, GET_D3D12RES, "Output", &out) == NGX_OK && out) {
        D3D12_RESOURCE_DESC od = ID3D12Resource_GetDesc((ID3D12Resource *)out);
        outW = (unsigned)od.Width; outH = od.Height;
    }
    fr.depthW = pgetu(p, GET_UINT, "DLSS.Render.Subrect.Dimensions.Width", renderW);
    fr.depthH = pgetu(p, GET_UINT, "DLSS.Render.Subrect.Dimensions.Height", renderH);
    fr.mvW = (dlssFlags & DLSS_FLAG_MV_LOWRES) ? fr.depthW : outW;
    fr.mvH = (dlssFlags & DLSS_FLAG_MV_LOWRES) ? fr.depthH : outH;

    if (slfg_active()) {  // Streamline does generation + presentation; just feed it
        stage = "streamline frame";
        slfg_dlss_frame((void *)a, (void *)depth, (void *)mvecs, fr.depthW, fr.depthH, fr.mvW, fr.mvH,
                        outW, outH, fr.jitterX, fr.jitterY, fr.mvScaleX, fr.mvScaleY, fr.depthInverted, fr.reset);
        stage = "game";
        return;
    }
    stage = "dlss capture";
    EnterCriticalSection(&fgLock);
    if (capCount == NCAP) {  // presents stopped consuming (menus?) — drop the oldest
        fr.reset |= capQ[capHead].reset;
        capHead = (capHead + 1) % NCAP;
        capCount--;
    }
    unsigned slot = (capHead + capCount) % NCAP;
    if (copy_into((void *)a, (ID3D12Resource *)depth, &capDepth[slot]) &&
        copy_into((void *)a, (ID3D12Resource *)mvecs, &capMv[slot])) {
        fr.depth = capDepth[slot]; fr.mvecs = capMv[slot];
        capQ[slot] = fr;
        R("capture slot=%u depth=%ux%u mv=%ux%u jitter=%.3f,%.3f mvscale=%.2f,%.2f reset=%d queued=%u",
          slot, fr.depthW, fr.depthH, fr.mvW, fr.mvH, fr.jitterX, fr.jitterY, fr.mvScaleX, fr.mvScaleY,
          fr.reset, capCount + 1);
        if (++capCount > capMaxSeen) capMaxSeen = capCount;
    }
    LeaveCriticalSection(&fgLock);
}

// Every D3D12 NGX function takes only integers/pointers (<= 7 args), so one
// 8-arg pass-through shape fits them all on the Windows x64 ABI.
#define X(n) static intptr_t w_##n(intptr_t a, intptr_t b, intptr_t c, intptr_t d, \
                                   intptr_t e, intptr_t f, intptr_t g, intptr_t h) { \
        intptr_t r = orig[F_##n](a, b, c, d, e, f, g, h); \
        report(F_##n, a, b, c, d, e, r); \
        fg_hook(F_##n, a, b, c, d, r); \
        return r; }
NGX_FUNCS
#undef X
static void *wrappers[] = {
#define X(n) (void *)w_##n,
    NGX_FUNCS
#undef X
};

// Export RVAs are 32-bit, so jump thunks must sit within 4GB above the module.
static uint8_t *alloc_above(HMODULE m, size_t sz) {
    for (uintptr_t p = ((uintptr_t)m + 0x10000) & ~(uintptr_t)0xFFFF;
         p < (uintptr_t)m + 0x7FFF0000; p += 0x10000) {
        void *r = VirtualAlloc((void *)p, sz, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (r) return r;
    }
    return NULL;
}

// Points export `name` of module m at fn, via a jmp thunk at t (export entries
// are 32-bit offsets, so t must be near m). Returns the original or NULL.
static void *eat_hook(HMODULE m, const char *name, void *fn, uint8_t *t) {
    BYTE *b = (BYTE *)m;
    IMAGE_NT_HEADERS64 *nt = (void *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_EXPORT_DIRECTORY *ex = (void *)(b +
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
    DWORD *funcs = (DWORD *)(b + ex->AddressOfFunctions);
    DWORD *names = (DWORD *)(b + ex->AddressOfNames);
    WORD *ords = (WORD *)(b + ex->AddressOfNameOrdinals);
    for (DWORD i = 0; i < ex->NumberOfNames; i++) {
        if (strcmp((char *)(b + names[i]), name)) continue;
        DWORD *slot = &funcs[ords[i]];
        void *o = b + *slot;
        t[0] = 0xFF; t[1] = 0x25; memset(t + 2, 0, 4);  // jmp [rip+0]; dq fn
        memcpy(t + 6, &fn, 8);
        FlushInstructionCache(GetCurrentProcess(), t, 16);
        DWORD old;
        VirtualProtect(slot, 4, PAGE_READWRITE, &old);
        *slot = (DWORD)(t - b);
        VirtualProtect(slot, 4, old, &old);
        return o;
    }
    return NULL;
}

static void hook_nvngx(void) {
    HMODULE m = LoadLibraryW(L"nvngx.dll");
    if (!m) { L("nvngx.dll not found (err %lu) - is DLSS enabled for this bottle?", GetLastError()); return; }
    wchar_t path[MAX_PATH]; GetModuleFileNameW(m, path, MAX_PATH);
    L("nvngx.dll loaded at %p: %ls", (void *)m, path);

    uint8_t *thunks = alloc_above(m, NF * 16);
    if (!thunks) { L("FATAL: no memory near nvngx.dll for thunks"); return; }
    int hooked = 0;
    for (int f = 0; f < NF; f++) {
        orig[f] = (fn8)eat_hook(m, fnames[f], wrappers[f], thunks + f * 16);
        if (orig[f]) hooked++; else L("  export missing: %s", fnames[f]);
    }
    L("hooked %d/%d NGX D3D12 exports", hooked, NF);
}

// ---- Frame generation, part 2: generate and show at Present ---------------
// CrossOver's dxgi.dll keeps one method table per object type in its own
// (Windows-side) memory, shared by all instances, so swapping a slot there
// catches every swapchain. Only data is written; no code is patched.
enum { SC_PRESENT = 8, SC_GETBUFFER = 9, SC_SETFULLSCREEN = 10, SC_RESIZE = 13, SC_GETDESC1 = 18,
       SC_PRESENT1 = 22, SC_CURRENT_BB = 36, SC_RESIZE1 = 39 };
enum { FAC_CREATESC = 10, FAC_CREATESC_HWND = 15, FAC_CREATESC_CORE = 16, FAC_CREATESC_COMP = 24 };

static void **scVt, **facVt;
static void *oPresent, *oPresent1, *oGetBuffer, *oCurrentBB;
static void *oCreateSC, *oCreateSCHwnd, *oCreateSCCore, *oCreateSCComp;
static void *oCDF, *oCDF1, *oCDF2;
static ID3D12CommandQueue *gQueue;
static LARGE_INTEGER qpf, lastPresent;
static double frameMs = 33.0;  // smoothed time between the game's real frames
static LONG presents;

static void patch_slot(void **vt, int i, void *fn, void **o) {
    DWORD old;
    VirtualProtect(&vt[i], 8, PAGE_READWRITE, &old);
    *o = vt[i];
    vt[i] = fn;
    VirtualProtect(&vt[i], 8, old, &old);
}

// Our own command lists on the game's queue, ring-buffered behind a fence.
#define NSLOT 6
static ID3D12CommandAllocator *cmdAlloc[NSLOT];
static ID3D12GraphicsCommandList *cmdList[NSLOT];
static UINT64 slotDone[NSLOT], fenceVal;
static ID3D12Fence *fence;
static HANDLE fenceEvt;
static int slotNext, slotCur;

static ID3D12Device *cmdDevice;

static int cmd_init(ID3D12Resource *any) {
    // Anything from a previous device is dead; just forget it.
    memset(cmdAlloc, 0, sizeof cmdAlloc); memset(cmdList, 0, sizeof cmdList);
    memset(slotDone, 0, sizeof slotDone);
    fence = NULL; fenceVal = 0;
    cmdDevice = device_of(any);
    ID3D12Device *dev = NULL;
    if (FAILED(ID3D12Resource_GetDevice(any, &IID_ID3D12Device, (void **)&dev))) return 0;
    for (int i = 0; i < NSLOT; i++) {
        ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&cmdAlloc[i]);
        ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc[i], NULL,
            &IID_ID3D12GraphicsCommandList, (void **)&cmdList[i]);
        if (cmdList[i]) ID3D12GraphicsCommandList_Close(cmdList[i]);
    }
    ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    ID3D12Device_Release(dev);
    if (!fenceEvt) fenceEvt = CreateEventW(NULL, FALSE, FALSE, NULL);
    return fence && cmdList[NSLOT - 1];
}

static ID3D12GraphicsCommandList *cmd_begin(void) {
    slotCur = slotNext++ % NSLOT;
    if (ID3D12Fence_GetCompletedValue(fence) < slotDone[slotCur]) {
        ID3D12Fence_SetEventOnCompletion(fence, slotDone[slotCur], fenceEvt);
        WaitForSingleObject(fenceEvt, 500);
    }
    ID3D12CommandAllocator_Reset(cmdAlloc[slotCur]);
    ID3D12GraphicsCommandList_Reset(cmdList[slotCur], cmdAlloc[slotCur], NULL);
    return cmdList[slotCur];
}

static void cmd_submit(ID3D12GraphicsCommandList *c) {
    ID3D12GraphicsCommandList_Close(c);
    ID3D12CommandQueue_ExecuteCommandLists(gQueue, 1, (ID3D12CommandList **)&c);
    ID3D12CommandQueue_Signal(gQueue, fence, ++fenceVal);
    slotDone[slotCur] = fenceVal;
}

// ---- Pacing ----------------------------------------------------------------
// D3DMetal's Present returns at once and queues the frame, so frames must be
// paced by us or the screen drops them at random (jumpy). Each Present happens
// a whole number of refreshes after the previous one (see `hold`), so generated
// and real frames alternate evenly and the screen never gets more frames than
// it shows: the real game is capped at refresh / (2 * hold).
// Waiting for the GPU here was tried: it stopped the game working ahead and
// halved its frame rate.
static double refreshMs = 1000.0 / 60;
static LARGE_INTEGER lastShown;

static double ms_since(LARGE_INTEGER t) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (now.QuadPart - t.QuadPart) * 1000.0 / qpf.QuadPart;
}

// Each shown frame stays on screen a whole number of refreshes (`hold`), so the
// cadence is regular: e.g. ~27 real fps on 100 Hz -> every frame 2 refreshes =
// 50 shown / 25 real, instead of an irregular 1-2-1-2 refresh mix (stutter).
// `hold` is decided from what actually happens, not the measured frame time
// (our own waits inflate that): frames often arriving late -> hold one refresh
// longer; idling a refresh or more per frame -> the game has headroom, one less.
static int hold = 1, holdFixed;  // holdFixed: set by ngxlog_refresh.txt, no adapting
static double slackAvg, lateAvg;  // per real frame: ms we slept, fraction late

static void update_hold(double slept, int late) {
    if (holdFixed) return;
    slackAvg = slackAvg * 0.95 + slept * 0.05;
    lateAvg = lateAvg * 0.95 + late * 0.05;
    if (lateAvg > 0.3 && hold < 4) { hold++; lateAvg = 0; slackAvg = 0; }
    else if (hold > 1 && slackAvg > 2 * refreshMs * 0.9) { hold--; slackAvg = 0; lateAvg = 0; }
}

// Sleeps until `hold` refreshes after the last shown frame (less 0.5ms of slack
// for the compositor). Returns ms slept; *late is set if we were already past it.
static double wait_half_frame(int *late) {
    if (!lastShown.QuadPart) return 0;
    double target = hold * refreshMs - 0.5, slept = 0;
    if (ms_since(lastShown) > target + 1.5) *late = 1;
    for (double left; (left = target - ms_since(lastShown)) > 0; slept = target - left)
        Sleep(left > 2 ? 1 : 0);
    return slept;
}

// PACING stats over the last 300 frames, so smoothness can be measured.
static double stGen, stReal, stGame, stGenMax, stRealMax, stGenMin = 1e9, stRealMin = 1e9;
static int stN;
static void pacing_stat(double gapGen, double gapReal) {
    stGen += gapGen; stReal += gapReal; stGame += frameMs;
    if (gapGen > stGenMax) stGenMax = gapGen;
    if (gapGen < stGenMin) stGenMin = gapGen;
    if (gapReal > stRealMax) stRealMax = gapReal;
    if (gapReal < stRealMin) stRealMin = gapReal;
    if (++stN < 300) return;
    L("PACING last %d frames (refresh %.1fms, hold %d = %.0f shown fps): real->generated gap avg %.1f (%.1f-%.1f) ms, "
      "generated->real gap avg %.1f (%.1f-%.1f) ms, game frame avg %.1f ms",
      stN, refreshMs, hold, 1000.0 / (hold * refreshMs), stGen / stN, stGenMin, stGenMax, stReal / stN, stRealMin, stRealMax,
      stGame / stN);
    stGen = stReal = stGame = stGenMax = stRealMax = 0;
    stGenMin = stRealMin = 1e9;
    stN = 0;
}

static void barrier(ID3D12GraphicsCommandList *c, ID3D12Resource *r,
                    D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b = { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to } };
    ID3D12GraphicsCommandList_ResourceBarrier(c, 1, &b);
}

// (Re)creates the frame generator and its textures for a w x h backbuffer.
// ponytail: on resize the old generator/textures are leaked, not freed (see fgDrop).
static int fg_setup(void *cmd, ID3D12Resource *bb, unsigned w, unsigned h) {
    fgHandle = NULL;
    fgOut = fg_make_output(bb);
    fgOutReal = fg_make_output(bb);
    if (!fgParams) orig[F_AllocateParameters]((intptr_t)&fgParams, 0, 0, 0, 0, 0, 0, 0);
    fgWarm = 0;
    intptr_t r = (fgOut && fgOutReal && fgParams)
        ? fg_create((ngx_create_fn)orig[F_CreateFeature], cmd, fgParams, w, h, &fgHandle) : 0;
    L("FRAMEGEN create %ux%u -> 0x%llx handle=%p", w, h, (unsigned long long)r, fgHandle);
    fgW = w; fgH = h;
    return r == NGX_OK && fgHandle;
}

static ID3D12Resource *backbuffer(void *sc) {
    UINT i = ((UINT (*)(void *))oCurrentBB)(sc);
    ID3D12Resource *r = NULL;
    ((HRESULT (*)(void *, UINT, REFIID, void **))oGetBuffer)(sc, i, &IID_ID3D12Resource, (void **)&r);
    return r;
}

static HRESULT call_present(void *sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS *pp, int is1) {
    return is1 ? ((HRESULT (*)(void *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *))oPresent1)(sc, sync, flags, pp)
               : ((HRESULT (*)(void *, UINT, UINT))oPresent)(sc, sync, flags);
}

// One game Present becomes two. D3DMetal's generator runs one frame behind:
// given frame N it returns the frame between N-2 and N-1, plus N-1 itself as
// OutputReal. So we show the generated frame now and OutputReal half a frame
// later (one frame of added delay, same as DLSS frame generation).
static HRESULT present_hook(void *sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS *pp, int is1) {
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    if (lastPresent.QuadPart) {
        double ms = (t0.QuadPart - lastPresent.QuadPart) * 1000.0 / qpf.QuadPart;
        if (ms > 0 && ms < 200) frameMs = frameMs * 0.9 + ms * 0.1;
    }
    lastPresent = t0;
    LONG n = ++presents;

    stage = "present";
    LONG drop = InterlockedExchange(&fgDrop, 0);
    EnterCriticalSection(&fgLock);
    unsigned queued = capCount;
    int have = capCount && !drop;
    FgFrame fr = capQ[capHead];
    if (drop) capCount = 0;
    else if (have) { capHead = (capHead + 1) % NCAP; capCount--; }
    LeaveCriticalSection(&fgLock);
    if (drop == 2) {  // ponytail: forgotten objects leak (a few MB per NGX restart)
        fgHandle = NULL; fgOut = fgOutReal = NULL; fgW = fgH = 0; fgParams = NULL;
    }
    if (drop) fgWarm = 0;  // first outputs after a gap are stale
    R("present #%ld sc=%p sync=%u flags=0x%x is1=%d have=%d queued=%u drop=%ld frame=%.1fms",
      n, sc, sync, flags, is1, have, queued, drop, frameMs);
    if (!have || fgGaveUp || !gQueue || (flags & DXGI_PRESENT_TEST)) {
        HRESULT hp = call_present(sc, sync, flags, pp, is1);
        R("present #%ld passthrough -> 0x%lx", n, hp);
        return hp;
    }

    ID3D12Resource *bb = backbuffer(sc);
    if (bb && device_of(bb) != cmdDevice) {  // new device (or first frame): start fresh
        if (cmdDevice) L("FRAMEGEN graphics device changed; rebuilding");
        fgHandle = NULL; fgOut = fgOutReal = NULL; fgW = fgH = 0; fgParams = NULL;
        if (!cmd_init(bb)) cmdDevice = NULL;
    }
    if (!bb || !fence) {
        if (bb) ID3D12Resource_Release(bb);
        return call_present(sc, sync, flags, pp, is1);
    }
    D3D12_RESOURCE_DESC bd = ID3D12Resource_GetDesc(bb);
    ID3D12GraphicsCommandList *c = cmd_begin();
    if (bd.Width != fgW || bd.Height != fgH || !fgHandle) {
        stage = "present: create";
        if (!fg_setup(c, bb, (unsigned)bd.Width, bd.Height)) {
            fgGaveUp = 1;
            // D3DMetal 4 only enables frame gen for games whose folder contains
            // nvngx_dlssg.dll (any file with that name), and needs macOS 26+.
            L("FRAMEGEN giving up: CrossOver refused to create a frame generator. Put a file named "
              "nvngx_dlssg.dll next to the game's .exe (an empty one is fine); also needs macOS 26+.");
            cmd_submit(c);
            ID3D12Resource_Release(bb);
            return call_present(sc, sync, flags, pp, is1);
        }
        fr.reset = 1;
    }

    const D3D12_RESOURCE_STATES SRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier(c, bb, D3D12_RESOURCE_STATE_PRESENT, SRV);
    fr.color = bb; fr.hudless = NULL; fr.out = fgOut; fr.outReal = fgOutReal;
    fr.colorW = (unsigned)bd.Width; fr.colorH = bd.Height;
    stage = "present: generate";
    intptr_t er = fg_eval((ngx_eval_fn)orig[F_EvaluateFeature], c, fgHandle, fgParams, &fr);
    // D3DMetal 4 finishes the generated frame when this batch is submitted, so it
    // must be copied out in a later batch (same batch = last frame's result).
    cmd_submit(c);
    // Its first two outputs after (re)creation are blank; show the real frame meanwhile.
    int show = er == NGX_OK && ++fgWarm > 2;
    c = cmd_begin();
    if (show) {
        barrier(c, bb, SRV, D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12GraphicsCommandList_CopyResource(c, bb, fgOut);
        barrier(c, bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    } else {
        barrier(c, bb, SRV, D3D12_RESOURCE_STATE_PRESENT);
    }
    cmd_submit(c);
    ID3D12Resource_Release(bb);

    LONG e = ++fgEvals;
    if (er != NGX_OK ? ++fgEvalFails <= 5 : (e <= 5 || e % 1000 == 0))
        L("FRAMEGEN present #%ld eval #%ld -> 0x%llx  %ux%u fmt=%u depth=%ux%u mv=%ux%u frame=%.1fms sync=%u "
          "queued=%u (max %u) mvscale=%.2f,%.2f jitter=%.2f,%.2f inv=%d",
          n, e, (unsigned long long)er, fr.colorW, fr.colorH, bd.Format, fr.depthW, fr.depthH,
          fr.mvW, fr.mvH, frameMs, sync, queued, capMaxSeen, fr.mvScaleX, fr.mvScaleY,
          fr.jitterX, fr.jitterY, fr.depthInverted);
    if (!show) return call_present(sc, sync, flags, pp, is1);  // bb is still the real frame

    int late = 0;
    double slept = wait_half_frame(&late);
    stage = "present: show generated";
    double gapGen = lastShown.QuadPart ? ms_since(lastShown) : 0;
    LARGE_INTEGER ta, tc;
    HRESULT hr = call_present(sc, sync, flags, pp, is1);  // generated frame
    QueryPerformanceCounter(&lastShown);
    R("present #%ld generated -> 0x%lx, gap %.1fms (eval 0x%llx)", n, hr, gapGen, (unsigned long long)er);
    if (FAILED(hr)) return hr;

    stage = "present: show real";
    if ((bb = backbuffer(sc))) {
        c = cmd_begin();
        barrier(c, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12GraphicsCommandList_CopyResource(c, bb, fgOutReal);
        barrier(c, bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        cmd_submit(c);
        ID3D12Resource_Release(bb);
    }
    slept += wait_half_frame(&late);
    update_hold(slept, late);
    double gapReal = ms_since(lastShown);
    QueryPerformanceCounter(&ta);
    HRESULT hr2 = call_present(sc, sync, flags, pp, is1);  // real frame
    QueryPerformanceCounter(&tc);
    lastShown = tc;
    R("present #%ld real -> 0x%lx, gap %.1fms, whole hook %.2fms", n, hr2, gapReal,
      (tc.QuadPart - t0.QuadPart) * 1000.0 / qpf.QuadPart);
    pacing_stat(gapGen, gapReal);
    stage = "game";
    return hr2;
}

static HRESULT h_Present(void *sc, UINT s, UINT f) { return present_hook(sc, s, f, NULL, 0); }
static HRESULT h_Present1(void *sc, UINT s, UINT f, const DXGI_PRESENT_PARAMETERS *pp) {
    return present_hook(sc, s, f, pp, 1);
}

// Logged-only pass-throughs, to see what the game does to its swapchain.
static void *oResize, *oResize1, *oSetFullscreen;
static HRESULT h_Resize(void *sc, UINT n, UINT w, UINT h, UINT fmt, UINT fl) {
    stage = "ResizeBuffers";
    HRESULT hr = ((HRESULT (*)(void *, UINT, UINT, UINT, UINT, UINT))oResize)(sc, n, w, h, fmt, fl);
    L("SWAPCHAIN ResizeBuffers sc=%p count=%u %ux%u fmt=%u flags=0x%x -> 0x%lx", sc, n, w, h, fmt, fl, hr);
    stage = "game";
    return hr;
}
static HRESULT h_Resize1(void *sc, UINT n, UINT w, UINT h, UINT fmt, UINT fl, const UINT *mask, IUnknown *const *q) {
    stage = "ResizeBuffers1";
    HRESULT hr = ((HRESULT (*)(void *, UINT, UINT, UINT, UINT, UINT, const UINT *, IUnknown *const *))oResize1)(sc, n, w, h, fmt, fl, mask, q);
    L("SWAPCHAIN ResizeBuffers1 sc=%p count=%u %ux%u fmt=%u flags=0x%x -> 0x%lx", sc, n, w, h, fmt, fl, hr);
    stage = "game";
    return hr;
}
static HRESULT h_SetFullscreen(void *sc, BOOL on, void *out) {
    HRESULT hr = ((HRESULT (*)(void *, BOOL, void *))oSetFullscreen)(sc, on, out);
    L("SWAPCHAIN SetFullscreenState sc=%p on=%d -> 0x%lx", sc, on, hr);
    return hr;
}

static void hook_swapchain(void *sc, IUnknown *dev) {
    ID3D12CommandQueue *q = NULL;  // D3D12 swapchains are created on a queue
    if (dev && SUCCEEDED(IUnknown_QueryInterface(dev, &IID_ID3D12CommandQueue, (void **)&q))) {
        gQueue = q;
        ID3D12CommandQueue_Release(q);  // the swapchain keeps it alive
    }
    void **vt = *(void ***)sc;
    DXGI_SWAP_CHAIN_DESC1 d = { 0 };
    ((HRESULT (*)(void *, DXGI_SWAP_CHAIN_DESC1 *))vt[SC_GETDESC1])(sc, &d);
    L("SWAPCHAIN created sc=%p vt=%p queue=%p %ux%u fmt=%u buffers=%u swapeffect=%u flags=0x%x thread=%lu",
      sc, (void *)vt, (void *)gQueue, d.Width, d.Height, d.Format, d.BufferCount, d.SwapEffect, d.Flags,
      GetCurrentThreadId());
    if (vt == scVt) return;
    if (scVt) { L("PRESENT another swapchain type (%p), not hooked", (void *)vt); return; }
    scVt = vt;
    oGetBuffer = vt[SC_GETBUFFER];
    oCurrentBB = vt[SC_CURRENT_BB];
    patch_slot(vt, SC_PRESENT, h_Present, &oPresent);
    patch_slot(vt, SC_PRESENT1, h_Present1, &oPresent1);
    patch_slot(vt, SC_RESIZE, h_Resize, &oResize);
    patch_slot(vt, SC_RESIZE1, h_Resize1, &oResize1);
    patch_slot(vt, SC_SETFULLSCREEN, h_SetFullscreen, &oSetFullscreen);
    L("PRESENT hooked swapchain %p (queue %p)", sc, (void *)gQueue);
}

static HRESULT h_CreateSC(void *f, IUnknown *dev, void *desc, void **out) {
    HRESULT hr = ((HRESULT (*)(void *, IUnknown *, void *, void **))oCreateSC)(f, dev, desc, out);
    if (SUCCEEDED(hr) && out && *out) { if (!slfg_active()) hook_swapchain(*out, dev); }  // with Streamline: its own internal swapchain
    return hr;
}
static HRESULT h_CreateSCHwnd(void *f, IUnknown *dev, HWND w, void *d, void *fd, void *o, void **out) {
    HRESULT hr = ((HRESULT (*)(void *, IUnknown *, HWND, void *, void *, void *, void **))oCreateSCHwnd)(f, dev, w, d, fd, o, out);
    if (SUCCEEDED(hr) && out && *out) { if (!slfg_active()) hook_swapchain(*out, dev); }  // with Streamline: its own internal swapchain
    return hr;
}
static HRESULT h_CreateSCCore(void *f, IUnknown *dev, IUnknown *win, void *d, void *o, void **out) {
    HRESULT hr = ((HRESULT (*)(void *, IUnknown *, IUnknown *, void *, void *, void **))oCreateSCCore)(f, dev, win, d, o, out);
    if (SUCCEEDED(hr) && out && *out) { if (!slfg_active()) hook_swapchain(*out, dev); }  // with Streamline: its own internal swapchain
    return hr;
}
static HRESULT h_CreateSCComp(void *f, IUnknown *dev, void *d, void *o, void **out) {
    HRESULT hr = ((HRESULT (*)(void *, IUnknown *, void *, void *, void **))oCreateSCComp)(f, dev, d, o, out);
    if (SUCCEEDED(hr) && out && *out) { if (!slfg_active()) hook_swapchain(*out, dev); }  // with Streamline: its own internal swapchain
    return hr;
}

static void hook_factory(void *f) {
    void **vt = *(void ***)f;
    if (vt == facVt) return;
    if (facVt) { L("PRESENT another factory type (%p), not hooked", (void *)vt); return; }
    facVt = vt;
    patch_slot(vt, FAC_CREATESC, h_CreateSC, &oCreateSC);
    patch_slot(vt, FAC_CREATESC_HWND, h_CreateSCHwnd, &oCreateSCHwnd);
    patch_slot(vt, FAC_CREATESC_CORE, h_CreateSCCore, &oCreateSCCore);
    patch_slot(vt, FAC_CREATESC_COMP, h_CreateSCComp, &oCreateSCComp);
    L("PRESENT hooked DXGI factory %p", f);
}

// With Streamline present (its DLLs next to this one), the game gets Streamline's
// proxy factory, so its swapchain is Streamline's and NVIDIA's code presents.
static void on_factory(void **f) {
    if (slfg_startup(gDir)) *f = slfg_wrap_factory(*f);
    hook_factory(*f);
}
static HRESULT WINAPI h_CDF(REFIID r, void **f) {
    HRESULT hr = ((HRESULT (*)(REFIID, void **))oCDF)(r, f);
    if (SUCCEEDED(hr) && f && *f) on_factory(f);
    return hr;
}
static HRESULT WINAPI h_CDF1(REFIID r, void **f) {
    HRESULT hr = ((HRESULT (*)(REFIID, void **))oCDF1)(r, f);
    if (SUCCEEDED(hr) && f && *f) on_factory(f);
    return hr;
}
static HRESULT WINAPI h_CDF2(UINT fl, REFIID r, void **f) {
    HRESULT hr = ((HRESULT (*)(UINT, REFIID, void **))oCDF2)(fl, r, f);
    if (SUCCEEDED(hr) && f && *f) on_factory(f);
    return hr;
}

// D3D12CreateDevice: Streamline must be told the game's device.
static void *oCreateDevice;
static HRESULT WINAPI h_CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL fl, REFIID r, void **dev) {
    HRESULT hr = ((HRESULT (*)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **))oCreateDevice)(adapter, fl, r, dev);
    if (SUCCEEDED(hr) && dev && *dev && slfg_startup(gDir)) slfg_set_device(*dev);
    return hr;
}

// Repoints any import-table entry in module m that holds `from` to `to`.
static int iat_redirect(HMODULE m, void *from, void *to) {
    if (!from) return 0;
    BYTE *b = (BYTE *)m;
    IMAGE_NT_HEADERS64 *nt = (void *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    int n = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(b + dir.VirtualAddress); d->Name; d++)
        for (void **t = (void **)(b + d->FirstThunk); *t; t++) {
            if (*t != from) continue;
            DWORD old;
            VirtualProtect(t, 8, PAGE_READWRITE, &old);
            *t = to;
            VirtualProtect(t, 8, old, &old);
            n++;
        }
    return n;
}

static void hook_dxgi(void) {
    HMODULE m = LoadLibraryW(L"dxgi.dll");
    uint8_t *t = m ? alloc_above(m, 3 * 16) : NULL;
    if (!t) { L("PRESENT: can't hook dxgi.dll"); return; }
    oCDF = eat_hook(m, "CreateDXGIFactory", h_CDF, t);
    oCDF1 = eat_hook(m, "CreateDXGIFactory1", h_CDF1, t + 16);
    oCDF2 = eat_hook(m, "CreateDXGIFactory2", h_CDF2, t + 32);
    // The game may already have these resolved in its own import table.
    HMODULE exe = GetModuleHandleW(NULL);
    int n = iat_redirect(exe, oCDF, h_CDF) + iat_redirect(exe, oCDF1, h_CDF1) + iat_redirect(exe, oCDF2, h_CDF2);
    L("PRESENT hooked dxgi.dll exports %p %p %p (+%d game import entries)", oCDF, oCDF1, oCDF2, n);

    HMODULE d3d = LoadLibraryW(L"d3d12.dll");
    uint8_t *t2 = d3d ? alloc_above(d3d, 16) : NULL;
    if (t2 && (oCreateDevice = eat_hook(d3d, "D3D12CreateDevice", h_CreateDevice, t2)))
        L("hooked D3D12CreateDevice (+%d game import entries)", iat_redirect(exe, oCreateDevice, h_CreateDevice));
}

// Logs each newly loaded module (for an hour), so we can see which DLLs the game
// pulls in; NGX call totals are logged at 3 minutes.
static DWORD WINAPI watch_modules(void *unused) {
    (void)unused;
    static HMODULE seen[2048]; DWORD nseen = 0;
    for (int tick = 0; tick < 1800; tick++) {
        if (tick == 90) {
            L("--- NGX call totals after 3 minutes:");
            for (int f = 0; f < NF; f++) if (calls[f]) L("    %-40s %ld", fnames[f], calls[f]);
        }
        HMODULE mods[2048]; DWORD need = 0;
        if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) {
            for (DWORD i = 0; i < need / sizeof(HMODULE); i++) {
                DWORD j = 0;
                while (j < nseen && seen[j] != mods[i]) j++;
                if (j < nseen || nseen >= 2048) continue;
                seen[nseen++] = mods[i];
                wchar_t p[MAX_PATH]; GetModuleFileNameW(mods[i], p, MAX_PATH);
                L("module %p %ls", (void *)mods[i], p);
            }
        }
        Sleep(2000);
    }
    return 0;
}

// "module.dll+0x1234" for a code address, or NULL if it isn't inside a module.
static const char *where(ULONG_PTR a, char *buf, size_t n) {
    HMODULE m = NULL;
    char name[MAX_PATH];
    if (a < 0x10000 || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (void *)a, &m))
        return NULL;
    GetModuleFileNameA(m, name, MAX_PATH);
    const char *b = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
    snprintf(buf, n, "%s+0x%llx", b, (unsigned long long)(a - (ULONG_PTR)m));
    return buf;
}

// Logs where a crash happens (the game may survive some of these on purpose):
// registers, who called, a stack scan for code addresses, and the black box.
static LONG crashLogs;
static LONG CALLBACK on_exception(EXCEPTION_POINTERS *e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
    LONG k = InterlockedIncrement(&crashLogs);
    if (k > 10) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = e->ContextRecord;
    ULONG_PTR *info = e->ExceptionRecord->ExceptionInformation;
    char b1[300], b2[300];
    const char *w = where((ULONG_PTR)e->ExceptionRecord->ExceptionAddress, b1, sizeof b1);
    L("EXCEPTION #%ld 0x%lx at %p (%s) %s %p | stage=%s thread=%lu", k, code,
      e->ExceptionRecord->ExceptionAddress, w ? w : "not in any module",
      code == EXCEPTION_ACCESS_VIOLATION ? (info[0] == 8 ? "executing" : info[0] ? "writing" : "reading") : "",
      (void *)info[1], stage, GetCurrentThreadId());
    L("  rip=%llx rsp=%llx rbp=%llx rax=%llx rbx=%llx rcx=%llx rdx=%llx r8=%llx r9=%llx",
      c->Rip, c->Rsp, c->Rbp, c->Rax, c->Rbx, c->Rcx, c->Rdx, c->R8, c->R9);
    // After a call to a null pointer, [rsp] is the return address = the caller.
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    if (c->Rsp >= lo && c->Rsp + 8 <= hi) {
        ULONG_PTR ret = *(ULONG_PTR *)c->Rsp;
        L("  [rsp] = %llx (%s)  <- likely caller", (unsigned long long)ret,
          where(ret, b2, sizeof b2) ? b2 : "not in any module");
        int found = 0;
        for (ULONG_PTR p = c->Rsp; p + 8 <= hi && p < c->Rsp + 16384 && found < 40; p += 8) {
            ULONG_PTR v = *(ULONG_PTR *)p;
            if (where(v, b2, sizeof b2)) { L("  stack +0x%04llx: %s", (unsigned long long)(p - c->Rsp), b2); found++; }
        }
    } else {
        L("  stack pointer outside this thread's stack (%llx..%llx)", (unsigned long long)lo, (unsigned long long)hi);
    }
    if (k <= 3) ring_dump(k == 1 ? RING : 60);
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, void *res) {
    (void)res;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(h);
    InitializeCriticalSection(&lk);
    InitializeCriticalSection(&fgLock);
    QueryPerformanceFrequency(&qpf);

    wchar_t p[1024];  // long Steam library paths can exceed MAX_PATH
    GetModuleFileNameW(h, p, 1024);
    wchar_t *slash = wcsrchr(p, L'\\');
    wcscpy(gDir, p);
    if (wcsrchr(gDir, L'\\')) *wcsrchr(gDir, L'\\') = 0;
    wcscpy(slash ? slash + 1 : p, L"ngxlog.txt");
    logf = _wfopen(p, L"w");

    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    L("rdr2-mac-framegen v0.2 loaded into %s", exe);
    slfg_set_logger(slog);
    AddVectoredExceptionHandler(1, on_exception);
    DEVMODEW dm = { .dmSize = sizeof dm };
    if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency >= 30 &&
        dm.dmDisplayFrequency <= 240) refreshMs = 1000.0 / dm.dmDisplayFrequency;
    // Override: ngxlog_refresh.txt next to the DLL holding the shown frames per
    // second to pace at, e.g. 50 on a 100 Hz screen (every frame = 2 refreshes,
    // real game <= 25 fps). Fixed pacing; no automatic adjusting.
    wcscpy(slash ? slash + 1 : p, L"ngxlog_refresh.txt");
    FILE *rf = _wfopen(p, L"r");
    int hz = 0;
    if (rf) {
        if (fscanf(rf, "%d", &hz) == 1 && hz >= 20 && hz <= 240) { refreshMs = 1000.0 / hz; holdFixed = 1; }
        else hz = 0;
        fclose(rf);
    }
    load_real_version();
    hook_nvngx();
    hook_dxgi();
    CloseHandle(CreateThread(NULL, 0, watch_modules, NULL, 0, NULL));
    return TRUE;
}
