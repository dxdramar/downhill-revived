/* The start window (port/hle/splash.c) is a Windows GDI window shown while the first dhd.exe
   relaunches itself; the Mac starts in one process and opens the game's window straight away
   (finding 075), so there is none. */
#include "splash.h"

void splash_show(void) {}
void splash_step(int percent, const char *text) { (void)percent; (void)text; }
void splash_close(void) {}
