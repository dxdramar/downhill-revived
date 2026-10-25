/* Mac build (finding 075): the part of WASAPI port/hle/sound.c's output uses, over the native
   host's Core Audio output (port/mac/native/audio.m). One device, one stream: 16-bit stereo PCM
   at the rate sound.c asks for, into a ring the output unit plays from. */
#ifndef DHD_MAC_MMDEVICEAPI_H
#define DHD_MAC_MMDEVICEAPI_H
#include <d3d11.h>  /* HRESULT, GUID */

#define AUDIO_IMPORT(name) __attribute__((import_module("audio"), import_name(#name)))
/* Opens the output: frames is the ring's size; returns it, 0 if there is no output. */
AUDIO_IMPORT(open) uint32_t mac_audio_open(uint32_t rate, uint32_t frames);
AUDIO_IMPORT(start) void mac_audio_start(int run);
AUDIO_IMPORT(queued) uint32_t mac_audio_queued(void);  /* frames written and not played yet */
AUDIO_IMPORT(write) void mac_audio_write(const void *frames, uint32_t count);
AUDIO_IMPORT(wait) void mac_audio_wait(uint32_t ms);    /* until the output takes frames, at most ms */

typedef int64_t REFERENCE_TIME;
typedef enum { eRender, eCapture, eAll } EDataFlow;
typedef enum { eConsole, eMultimedia, eCommunications } ERole;
typedef enum { AUDCLNT_SHAREMODE_SHARED, AUDCLNT_SHAREMODE_EXCLUSIVE } AUDCLNT_SHAREMODE;
#define AUDCLNT_STREAMFLAGS_EVENTCALLBACK 0x00040000u
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000u
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000u
#define CLSCTX_ALL 0x17u
#define COINIT_MULTITHREADED 0x0u
#define WAVE_FORMAT_PCM 1
typedef struct {
    WORD wFormatTag, nChannels; DWORD nSamplesPerSec, nAvgBytesPerSec; WORD nBlockAlign, wBitsPerSample, cbSize;
} WAVEFORMATEX;

typedef struct IMMDeviceEnumerator { int unused; } IMMDeviceEnumerator;
typedef struct IMMDevice { int unused; } IMMDevice;
typedef struct IAudioClient { uint32_t frames; HANDLE event; } IAudioClient;
typedef struct IAudioRenderClient { IAudioClient *client; } IAudioRenderClient;

static inline HRESULT CoInitializeEx(void *reserved, DWORD model) { (void)reserved; (void)model; return S_OK; }
static inline void CoUninitialize(void) {}
static inline HRESULT CoCreateInstance(const GUID *clsid, void *outer, DWORD context, const GUID *iid, void **out)
{
    static IMMDeviceEnumerator enumerator;
    (void)clsid; (void)outer; (void)context; (void)iid;
    *out = &enumerator;
    return S_OK;
}
static inline HRESULT IMMDeviceEnumerator_GetDefaultAudioEndpoint(IMMDeviceEnumerator *e, EDataFlow flow, ERole role,
                                                                  IMMDevice **out)
{
    static IMMDevice device;
    (void)e; (void)flow; (void)role;
    *out = &device;
    return S_OK;
}
static inline ULONG IMMDeviceEnumerator_Release(IMMDeviceEnumerator *e) { (void)e; return 0; }
static inline ULONG IMMDevice_Release(IMMDevice *d) { (void)d; return 0; }
static inline HRESULT IMMDevice_Activate(IMMDevice *d, const GUID *iid, DWORD context, void *params, void **out)
{
    static IAudioClient client;
    (void)d; (void)iid; (void)context; (void)params;
    *out = &client;
    return S_OK;
}
#include "audioclient.h"
#endif
