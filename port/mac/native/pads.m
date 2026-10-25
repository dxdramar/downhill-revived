/* Controllers on the Mac (finding 075): the host side of port/mac/include/mac_pad.h, over
   Apple's GameController framework, which knows the DualSense, DualShock 4, Xbox, Switch Pro and
   MFi pads by USB and Bluetooth. As in port/hle/gamepad.c: players are slots, a pad that connects
   takes the first free one and keeps it until it disconnects; buttons go by position (the bottom
   face button is Cross whatever its label), Create/Share/View is SELECT, Options/Menu START, the
   triggers L2/R2 with their pressure. The framework calls back on a queue of ours with each
   change; the game thread copies the latest state. Vibration plays on the pad's haptics engine
   (the DualSense's and DualShock 4's motors, an Xbox pad's rumble). */
#import <CoreHaptics/CoreHaptics.h>
#import <GameController/GameController.h>
#include <pthread.h>
#include <string.h>
#include "native.h"

struct w2c_pad;

enum { SLOTS = 4 };
enum { PAD_SELECT = 0x0001, PAD_L3 = 0x0002, PAD_R3 = 0x0004, PAD_START = 0x0008, PAD_UP = 0x0010,
       PAD_RIGHT = 0x0020, PAD_DOWN = 0x0040, PAD_LEFT = 0x0080, PAD_L2 = 0x0100, PAD_R2 = 0x0200,
       PAD_L1 = 0x0400, PAD_R1 = 0x0800, PAD_TRIANGLE = 0x1000, PAD_CIRCLE = 0x2000, PAD_CROSS = 0x4000,
       PAD_SQUARE = 0x8000 };

/* hle.h's HostPad, as the module lays it out (port/mac/gamepad_mac.c checks the size) */
typedef struct __attribute__((packed)) {
    uint16_t buttons;
    uint8_t right_x, right_y, left_x, left_y;
    uint8_t pressure[12];
} HostPad;
_Static_assert(sizeof(HostPad) == 18, "HostPad");

typedef struct {
    GCController *controller;   /* nil: free */
    HostPad latest;
    int light, heavy;           /* vibration playing */
    CHHapticEngine *engine;
    id<CHHapticAdvancedPatternPlayer> player;
} Slot;

static Slot slots[SLOTS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static dispatch_queue_t queue;  /* the framework's handlers and the vibration */

static const uint16_t pressure_order[12] = {PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
                                            PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2};

static uint8_t axis_byte(float v, int flip)  /* -1..1 (up positive) to 0..255 (down positive) */
{
    int x = 128 + (int)lroundf((flip ? -v : v) * 127.5f);
    return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
}

static HostPad state_of(GCExtendedGamepad *g)
{
    HostPad p;
    memset(&p, 0, sizeof p);
    const struct { GCControllerButtonInput *b; uint16_t button; } map[] = {
        {g.buttonA, PAD_CROSS}, {g.buttonB, PAD_CIRCLE}, {g.buttonX, PAD_SQUARE}, {g.buttonY, PAD_TRIANGLE},
        {g.buttonOptions, PAD_SELECT}, {g.buttonMenu, PAD_START},
        {g.leftThumbstickButton, PAD_L3}, {g.rightThumbstickButton, PAD_R3},
        {g.leftShoulder, PAD_L1}, {g.rightShoulder, PAD_R1},
        {g.dpad.up, PAD_UP}, {g.dpad.down, PAD_DOWN}, {g.dpad.left, PAD_LEFT}, {g.dpad.right, PAD_RIGHT},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (map[i].b.pressed) p.buttons |= map[i].button;
    uint8_t l2 = (uint8_t)lroundf(g.leftTrigger.value * 255), r2 = (uint8_t)lroundf(g.rightTrigger.value * 255);
    if (l2 > 30) p.buttons |= PAD_L2;
    if (r2 > 30) p.buttons |= PAD_R2;
    p.left_x = axis_byte(g.leftThumbstick.xAxis.value, 0);
    p.left_y = axis_byte(g.leftThumbstick.yAxis.value, 1);
    p.right_x = axis_byte(g.rightThumbstick.xAxis.value, 0);
    p.right_y = axis_byte(g.rightThumbstick.yAxis.value, 1);
    for (int i = 0; i < 12; i++)
        p.pressure[i] = pressure_order[i] == PAD_L2 ? l2 : pressure_order[i] == PAD_R2 ? r2
                      : p.buttons & pressure_order[i] ? 255 : 0;
    return p;
}

static void stop_haptics(Slot *s)
{
    [s->player stopAtTime:CHHapticTimeImmediate error:nil];
    [s->engine stopWithCompletionHandler:nil];
    s->player = nil;
    s->engine = nil;
    s->light = s->heavy = 0;
}

/* On the queue: a pad came; it takes the first free slot. */
static void attach(GCController *c)
{
    GCExtendedGamepad *g = c.extendedGamepad;
    if (!g) {
        fprintf(stderr, "controller: %s has no gamepad profile; not used\n", c.vendorName.UTF8String ?: "a pad");
        return;
    }
    int i;
    pthread_mutex_lock(&lock);
    for (i = 0; i < SLOTS && slots[i].controller != c; i++) {}
    if (i == SLOTS)
        for (i = 0; i < SLOTS && slots[i].controller; i++) {}
    if (i < SLOTS) {
        slots[i].controller = c;
        slots[i].latest = state_of(g);
    }
    pthread_mutex_unlock(&lock);
    if (i == SLOTS) return;
    c.playerIndex = (GCControllerPlayerIndex)i;  /* the DualSense's player lights */
    c.handlerQueue = queue;
    int slot = i;
    g.valueChangedHandler = ^(GCExtendedGamepad *pad, GCControllerElement *element) {
        (void)element;
        HostPad p = state_of(pad);
        static int trace = -1;  /* DHD_PAD_TRACE=1: each change of buttons or a stick's direction */
        if (trace < 0) trace = getenv("DHD_PAD_TRACE") != NULL;
        if (trace) {
            static HostPad shown[SLOTS];
            HostPad *was = &shown[slot];
            if (p.buttons != was->buttons || abs(p.left_x - was->left_x) > 40 || abs(p.left_y - was->left_y) > 40 ||
                abs(p.right_x - was->right_x) > 40 || abs(p.right_y - was->right_y) > 40) {
                fprintf(stderr, "pad %d: buttons %04X left %3u,%3u right %3u,%3u L2 %3u R2 %3u\n", slot + 1, p.buttons,
                        p.left_x, p.left_y, p.right_x, p.right_y, p.pressure[10], p.pressure[11]);
                *was = p;
            }
        }
        pthread_mutex_lock(&lock);
        if (slots[slot].controller == pad.controller) slots[slot].latest = p;
        pthread_mutex_unlock(&lock);
    };
    fprintf(stderr, "controller: %s is player %d\n", c.vendorName.UTF8String ?: "a pad", slot + 1);
}

static void detach(GCController *c)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < SLOTS; i++)
        if (slots[i].controller == c) {
            stop_haptics(&slots[i]);
            slots[i].controller = nil;
            fprintf(stderr, "controller: player %d disconnected\n", i + 1);
        }
    pthread_mutex_unlock(&lock);
}

u32 w2c_pad_start(struct w2c_pad *unused)
{
    (void)unused;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        queue = dispatch_queue_create("dhd controllers", DISPATCH_QUEUE_SERIAL);
        /* the game reads the pads when its window isn't in front too, as with SDL on Windows */
        GCController.shouldMonitorBackgroundEvents = YES;
        NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
        NSOperationQueue *on_queue = [NSOperationQueue new];
        on_queue.underlyingQueue = queue;
        [center addObserverForName:GCControllerDidConnectNotification object:nil queue:on_queue
                        usingBlock:^(NSNotification *n) { attach(n.object); }];
        [center addObserverForName:GCControllerDidDisconnectNotification object:nil queue:on_queue
                        usingBlock:^(NSNotification *n) { detach(n.object); }];
        dispatch_sync(queue, ^{
            for (GCController *c in GCController.controllers) attach(c);
        });
    });
    return 1;
}

u32 w2c_pad_read(struct w2c_pad *unused, u32 player, u32 out)
{
    (void)unused;
    if (player >= SLOTS) return 0;
    pthread_mutex_lock(&lock);
    int present = slots[player].controller != nil;
    if (present) memcpy(at(out), &slots[player].latest, sizeof(HostPad));
    pthread_mutex_unlock(&lock);
    return present;
}

/* On the queue: the slot's pad plays (light, heavy): the small motor on or off, the large one
   at a speed, as one continuous haptic whose intensity and sharpness follow them. */
static void vibrate(Slot *s, int light, int heavy)
{
    if (light == s->light && heavy == s->heavy) return;
    s->light = light, s->heavy = heavy;
    if (!light && !heavy) {
        [s->player stopAtTime:CHHapticTimeImmediate error:nil];
        s->player = nil;
        return;
    }
    float intensity = fmaxf(heavy / 255.0f, light ? 0.5f : 0), sharpness = heavy ? 0.2f : 0.9f;
    NSError *error = nil;
    if (!s->engine) {
        s->engine = [s->controller.haptics createEngineWithLocality:GCHapticsLocalityDefault];
        if (!s->engine) return;  /* a pad without motors */
        s->engine.playsHapticsOnly = YES;
        if (![s->engine startAndReturnError:&error]) { s->engine = nil; return; }
    }
    if (!s->player) {
        CHHapticEvent *e = [[CHHapticEvent alloc]
            initWithEventType:CHHapticEventTypeHapticContinuous
                   parameters:@[[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1],
                                [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.5]]
                 relativeTime:0 duration:30];  /* stopped or renewed long before */
        CHHapticPattern *pattern = [[CHHapticPattern alloc] initWithEvents:@[e] parameters:@[] error:&error];
        s->player = pattern ? [s->engine createAdvancedPlayerWithPattern:pattern error:&error] : nil;
        s->player.loopEnabled = YES;
        if (![s->player startAtTime:CHHapticTimeImmediate error:&error]) { s->player = nil; return; }
    }
    [s->player sendParameters:@[[[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:intensity relativeTime:0],
                                [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticSharpnessControl value:sharpness - 0.5f relativeTime:0]]
                       atTime:CHHapticTimeImmediate error:nil];
}

void w2c_pad_vibrate(struct w2c_pad *unused, u32 player, u32 light, u32 heavy)
{
    (void)unused;
    if (player >= SLOTS || !queue) return;
    dispatch_async(queue, ^{
        pthread_mutex_lock(&lock);
        Slot *s = &slots[player];
        if (s->controller) vibrate(s, (int)light, (int)heavy);
        pthread_mutex_unlock(&lock);
    });
}

/* At exit: no pad left vibrating. */
void pads_stop(void)
{
    if (!queue) return;
    dispatch_sync(queue, ^{
        pthread_mutex_lock(&lock);
        for (int i = 0; i < SLOTS; i++) if (slots[i].controller) stop_haptics(&slots[i]);
        pthread_mutex_unlock(&lock);
    });
}
