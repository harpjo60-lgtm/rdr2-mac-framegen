// menu.cpp: the in-game settings menu (Ctrl+Shift+M), drawn with Dear ImGui
// onto the game's backbuffer right before it's presented. The HUD-free copy
// is taken earlier in the frame, so frame generation treats the menu as HUD
// and doesn't warp it. Settings live in framegen.ini next to the DLL.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
#include "slfg.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

FgSettings fgset = { 1, 0, 1 };  // frame gen on, game's own DLSS resolution, HUD fix on
static wchar_t iniPath[1100];

extern "C" void menu_load(const wchar_t *dir) {
    swprintf(iniPath, 1100, L"%ls\\framegen.ini", dir);
    FILE *f = _wfopen(iniPath, L"r");
    if (!f) return;
    char k[32]; int v;
    while (fscanf(f, " %31[^=]=%d", k, &v) == 2) {
        if (!strcmp(k, "framegen")) fgset.framegen = !!v;
        else if (!strcmp(k, "native")) fgset.native = !!v;
        else if (!strcmp(k, "hudfix")) fgset.hudfix = !!v;
    }
    fclose(f);
}
static void save(void) {
    FILE *f = _wfopen(iniPath, L"w");
    if (!f) return;
    fprintf(f, "framegen=%d\nnative=%d\n", fgset.framegen, fgset.native);
    if (!fgset.hudfix) fprintf(f, "hudfix=0\n");  // debugging only; not in the menu
    fclose(f);
}

static CRITICAL_SECTION lock;  // window messages arrive on another thread than Present
static volatile bool ready, open;
static HWND wnd;
static WNDPROC oWndProc;
static ID3D12DescriptorHeap *srvHeap, *rtvHeap;
static ID3D12CommandAllocator *alloc[3];
static ID3D12GraphicsCommandList *list;
static ID3D12Fence *fence;
static UINT64 fenceVal[3], fenceN;

// While the menu is open it gets the keyboard and mouse and the game doesn't.
static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (open) {
        EnterCriticalSection(&lock);
        ImGui_ImplWin32_WndProcHandler(h, m, w, l);
        LeaveCriticalSection(&lock);
        if ((m >= WM_KEYFIRST && m <= WM_KEYLAST) || (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || m == WM_INPUT)
            return m == WM_INPUT ? DefWindowProcW(h, m, w, l) : 0;
    }
    return CallWindowProcW(oWndProc, h, m, w, l);
}

static bool init(IDXGISwapChain3 *sc, ID3D12Resource *bb) {
    ID3D12Device *dev = nullptr;
    if (FAILED(bb->GetDevice(IID_PPV_ARGS(&dev)))) return false;
    DXGI_SWAP_CHAIN_DESC sd;
    sc->GetDesc(&sd);
    wnd = sd.OutputWindow;
    D3D12_DESCRIPTOR_HEAP_DESC hd{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    D3D12_DESCRIPTOR_HEAP_DESC rd{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap));
    dev->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&rtvHeap));
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    for (auto &a : alloc) dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0], nullptr, IID_PPV_ARGS(&list));
    if (!srvHeap || !rtvHeap || !fence || !list) { dev->Release(); return false; }
    list->Close();

    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;  // our settings file, not ImGui's window layout
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.FontGlobalScale = sd.BufferDesc.Height >= 1400 ? 1.6f : 1.3f;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(wnd);
    ImGui_ImplDX12_Init(dev, 3, bb->GetDesc().Format, srvHeap, srvHeap->GetCPUDescriptorHandleForHeapStart(),
                        srvHeap->GetGPUDescriptorHandleForHeapStart());
    dev->Release();
    oWndProc = (WNDPROC)SetWindowLongPtrW(wnd, GWLP_WNDPROC, (LONG_PTR)wnd_proc);
    return true;
}

static void draw(void) {
    ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);
    ImGui::Begin("Frame Generation", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);
    bool changed = false;
    bool fg = fgset.framegen, nat = fgset.native;
    changed |= ImGui::Checkbox("Frame generation", &fg);
    changed |= ImGui::Checkbox("Native resolution (no upscaling)", &nat);
    ImGui::TextDisabled("  Takes effect after restarting the game.");
    ImGui::Separator();
    ImGui::TextDisabled("Ctrl+Shift+M to close. Arrow keys + Space, or the mouse.");
    ImGui::End();
    if (changed) { fgset.framegen = fg; fgset.native = nat; save(); }
}

extern "C" void menu_present(void *swapchain, void *queue) {
    static bool keyWas, failed, first = true;
    if (first) { first = false; open = getenv("FG_MENU_OPEN") != nullptr; }  // for testing without a keyboard
    bool key = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState('M') & 0x8000);
    if (key && !keyWas) open = !open;
    keyWas = key;
    if (!open || failed || !queue) return;

    IDXGISwapChain3 *sc = nullptr;
    ID3D12Resource *bb = nullptr;
    if (FAILED(((IUnknown *)swapchain)->QueryInterface(IID_PPV_ARGS(&sc)))) return;
    sc->GetBuffer(sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
    if (bb && !ready) {
        InitializeCriticalSection(&lock);
        ready = init(sc, bb);
        failed = !ready;
    }
    sc->Release();
    if (!bb || failed) { if (bb) bb->Release(); return; }
    ImGui::GetIO().MouseDrawCursor = true;  // the game hides the system cursor

    int s = (int)(fenceN % 3);
    if (fence->GetCompletedValue() < fenceVal[s]) fence->SetEventOnCompletion(fenceVal[s], nullptr);  // blocks
    EnterCriticalSection(&lock);
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    draw();
    ImGui::Render();
    LeaveCriticalSection(&lock);

    ID3D12Device *dev = nullptr;
    bb->GetDevice(IID_PPV_ARGS(&dev));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(bb, nullptr, rtv);
    dev->Release();
    alloc[s]->Reset();
    list->Reset(alloc[s], nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Transition = { bb, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET };
    list->ResourceBarrier(1, &b);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->SetDescriptorHeaps(1, &srvHeap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list->ResourceBarrier(1, &b);
    list->Close();
    ID3D12CommandQueue *q = (ID3D12CommandQueue *)queue;
    q->ExecuteCommandLists(1, (ID3D12CommandList **)&list);
    q->Signal(fence, fenceVal[s] = ++fenceN);
    bb->Release();
}
