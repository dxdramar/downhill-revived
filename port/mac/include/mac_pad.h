/* Mac build (finding 075): controllers through the native host's GameController framework
   (port/mac/native/pads.m): DualSense, DualShock 4, Xbox, Switch Pro and the other pads macOS
   knows, by USB or Bluetooth. */
#ifndef DHD_MAC_PAD_H
#define DHD_MAC_PAD_H
#include <stdint.h>

#define PAD_IMPORT(name) __attribute__((import_module("pad"), import_name(#name)))
PAD_IMPORT(start) int mac_pad_start(void);  /* 1: the framework is watching for pads */
/* Player i's pad as hle.h's HostPad (buttons, sticks, pressures) at out; 0: none for that player. */
PAD_IMPORT(read) int mac_pad_read(int player, void *out);
PAD_IMPORT(vibrate) void mac_pad_vibrate(int player, int light, int heavy);

#endif
