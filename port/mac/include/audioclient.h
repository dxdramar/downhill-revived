/* Mac build: see mmdeviceapi.h. */
#ifndef DHD_MAC_AUDIOCLIENT_H
#define DHD_MAC_AUDIOCLIENT_H
#include "mmdeviceapi.h"

/* duration in 100 ns units: the ring holds that much */
static inline HRESULT IAudioClient_Initialize(IAudioClient *c, AUDCLNT_SHAREMODE mode, DWORD flags,
                                              REFERENCE_TIME duration, REFERENCE_TIME period,
                                              const WAVEFORMATEX *format, const GUID *session)
{
    (void)mode; (void)flags; (void)period; (void)session;
    if (format->nChannels != 2 || format->wBitsPerSample != 16) return E_INVALIDARG;
    c->frames = mac_audio_open(format->nSamplesPerSec, (uint32_t)(duration * format->nSamplesPerSec / 10000000));
    return c->frames ? S_OK : E_FAIL;
}
/* The event is the output's: sound.c's wait on it becomes mac_audio_wait (port/mac/win32.c). */
void win32_audio_event(HANDLE event);
static inline HRESULT IAudioClient_SetEventHandle(IAudioClient *c, HANDLE event)
{
    c->event = event;
    win32_audio_event(event);
    return S_OK;
}
static inline HRESULT IAudioClient_GetBufferSize(IAudioClient *c, UINT32 *frames) { *frames = c->frames; return S_OK; }
static inline HRESULT IAudioClient_GetCurrentPadding(IAudioClient *c, UINT32 *frames)
{
    (void)c;
    *frames = mac_audio_queued();
    return S_OK;
}
static inline HRESULT IAudioClient_GetService(IAudioClient *c, const GUID *iid, void **out)
{
    static IAudioRenderClient render;
    (void)iid;
    render.client = c;
    *out = &render;
    return S_OK;
}
static inline HRESULT IAudioClient_Start(IAudioClient *c) { (void)c; mac_audio_start(1); return S_OK; }
static inline HRESULT IAudioClient_Stop(IAudioClient *c) { (void)c; mac_audio_start(0); return S_OK; }
static inline ULONG IAudioClient_Release(IAudioClient *c) { (void)c; return 0; }

/* GetBuffer hands out one block of module memory; ReleaseBuffer sends what was written. */
static inline HRESULT IAudioRenderClient_GetBuffer(IAudioRenderClient *r, UINT32 frames, BYTE **out)
{
    static int16_t block[48000 * 2];
    (void)r;
    if (frames > 48000) return E_INVALIDARG;
    *out = (BYTE *)block;
    return S_OK;
}
static inline HRESULT IAudioRenderClient_ReleaseBuffer(IAudioRenderClient *r, UINT32 frames, DWORD flags)
{
    BYTE *block;
    (void)flags;
    IAudioRenderClient_GetBuffer(r, 0, &block);
    if (frames) mac_audio_write(block, frames);
    return S_OK;
}
static inline ULONG IAudioRenderClient_Release(IAudioRenderClient *r) { (void)r; return 0; }
#endif
