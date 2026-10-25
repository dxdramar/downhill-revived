/* Sound output on the Mac (finding 075): the host side of port/mac/include/mmdeviceapi.h. The
   module's mixer (port/hle/sound.c) writes 16-bit stereo frames into a ring; the default output
   unit plays from it on Core Audio's thread (converting to the device's rate and format) and
   wakes the mixer each time it takes frames, as WASAPI's event does on Windows. One writer, one
   reader: the indices are atomics, the frames plain. */
#import <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#include <stdatomic.h>
#include "native.h"

struct w2c_audio;

static int16_t *ring;
static uint32_t ring_frames;
static _Atomic uint64_t written, played;
static AudioComponentInstance unit;
static pthread_mutex_t wake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;

static OSStatus render(void *context, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *time, UInt32 bus,
                       UInt32 frames, AudioBufferList *data)
{
    (void)context; (void)flags; (void)time; (void)bus;
    int16_t *out = data->mBuffers[0].mData;
    uint64_t from = atomic_load_explicit(&played, memory_order_relaxed);
    uint64_t have = atomic_load_explicit(&written, memory_order_acquire) - from;
    uint32_t n = have < frames ? (uint32_t)have : frames;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t at = (uint32_t)((from + i) % ring_frames);
        out[i * 2] = ring[at * 2];
        out[i * 2 + 1] = ring[at * 2 + 1];
    }
    memset(out + n * 2, 0, (frames - n) * 4);  /* short of frames: silence, sound.c counts the gap */
    atomic_store_explicit(&played, from + n, memory_order_release);
    pthread_mutex_lock(&wake_lock);
    pthread_cond_broadcast(&wake);
    pthread_mutex_unlock(&wake_lock);
    return noErr;
}

u32 w2c_audio_open(struct w2c_audio *unused, u32 rate, u32 frames)
{
    (void)unused;
    if (unit) return ring_frames;
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
                                   kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    if (!c || AudioComponentInstanceNew(c, &unit)) { unit = NULL; return 0; }
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 16;
    f.mBytesPerFrame = f.mBytesPerPacket = 4;
    f.mFramesPerPacket = 1;
    AURenderCallbackStruct callback = {render, NULL};
    if (AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) ||
        AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback,
                             sizeof callback) ||
        AudioUnitInitialize(unit)) {
        fprintf(stderr, "audio: the output didn't start\n");
        AudioComponentInstanceDispose(unit);
        unit = NULL;
        return 0;
    }
    ring_frames = frames < 1024 ? 1024 : frames;
    ring = calloc(ring_frames, 4);
    fprintf(stderr, "audio: Core Audio output, %u Hz stereo, %u-frame ring\n", rate, ring_frames);
    return ring_frames;
}

void w2c_audio_start(struct w2c_audio *unused, u32 run)
{
    (void)unused;
    if (unit) run ? AudioOutputUnitStart(unit) : AudioOutputUnitStop(unit);
}

u32 w2c_audio_queued(struct w2c_audio *unused)
{
    (void)unused;
    return (u32)(atomic_load(&written) - atomic_load(&played));
}

void w2c_audio_write(struct w2c_audio *unused, u32 frames, u32 count)
{
    (void)unused;
    if (!ring) return;
    uint64_t head = atomic_load_explicit(&written, memory_order_relaxed);
    uint64_t room = ring_frames - (head - atomic_load_explicit(&played, memory_order_acquire));
    if (count > room) count = (u32)room;  /* sound.c asks for no more than the padding leaves */
    const int16_t *in = at(frames);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t k = (uint32_t)((head + i) % ring_frames);
        ring[k * 2] = in[i * 2];
        ring[k * 2 + 1] = in[i * 2 + 1];
    }
    atomic_store_explicit(&written, head + count, memory_order_release);
}

void w2c_audio_wait(struct w2c_audio *unused, u32 ms)
{
    (void)unused;
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_nsec += (long)(ms % 1000) * 1000000;
    t.tv_sec += ms / 1000 + t.tv_nsec / 1000000000;
    t.tv_nsec %= 1000000000;
    pthread_mutex_lock(&wake_lock);
    pthread_cond_timedwait(&wake, &wake_lock, &t);
    pthread_mutex_unlock(&wake_lock);
}
