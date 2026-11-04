// slfg.h: C interface to slfg.cpp (frame generation through NVIDIA Streamline).
#pragma once
#include <wchar.h>
#ifdef __cplusplus
extern "C" {
#endif
void slfg_set_logger(void (*f)(const char *));
int slfg_startup(const wchar_t *dir);      // 1 if Streamline is loaded and running
int slfg_active(void);
void slfg_set_device(void *d3d12Device);
void *slfg_wrap_factory(void *factory);    // returns Streamline's proxy (or the input)
void slfg_dlss_frame(void *cmdlist, void *depth, void *mvecs, unsigned depthW, unsigned depthH,
                     unsigned mvW, unsigned mvH, unsigned outW, unsigned outH,
                     float jitterX, float jitterY, float mvScaleX, float mvScaleY,
                     int depthInverted, int reset);
void slfg_dlss_paused(int paused);
void hudless_present(void *backbuffer);   // in ngxlog.c

// menu.cpp: settings (framegen.ini) and the Ctrl+Shift+M menu
typedef struct { int framegen, native, hudfix; } FgSettings;
extern FgSettings fgset;
void menu_load(const wchar_t *dir);
void menu_present(void *swapchain, void *queue);  // draws the menu if open
void *hudless_take(void);                 // in ngxlog.c: this frame's HUD-free copy, or NULL
#ifdef __cplusplus
}
#endif
