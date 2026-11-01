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
void slfg_wrap_swapchain(void **swapchain);  // replaces *swapchain with Streamline's proxy
void slfg_dlss_frame(void *cmdlist, void *depth, void *mvecs, unsigned depthW, unsigned depthH,
                     unsigned mvW, unsigned mvH, unsigned outW, unsigned outH,
                     float jitterX, float jitterY, float mvScaleX, float mvScaleY,
                     int depthInverted, int reset);
void slfg_dlss_paused(int paused);
#ifdef __cplusplus
}
#endif
