/* Controllers on the Mac (finding 075): port/hle/gamepad.c's interface over the native host's
   GameController framework (port/mac/native/pads.m), which keeps the players' slots and maps
   the buttons by position as gamepad.c does. */
#include "hle.h"
#include <mac_pad.h>

_Static_assert(sizeof(HostPad) == 18, "pads.m writes HostPad's layout");

int gamepad_start(void)
{
    static int running = -1;
    if (running < 0) running = mac_pad_start();
    return running;
}

int gamepad_read(int player, HostPad *out)
{
    if (player < 0 || player >= 4 || !gamepad_start()) return 0;
    return mac_pad_read(player, out);
}

void gamepad_vibrate(int player, int light, int heavy)
{
    if (player >= 0 && player < 4) mac_pad_vibrate(player, light ? 1 : 0, heavy & 255);
}
