/* The macOS host's own services for host.c (finding 091): its dialogs (dialogs_apple.m). */
#ifndef PORT_HOST_MAC_H
#define PORT_HOST_MAC_H

#include <stddef.h>

enum { HOST_MAC_OK = 0, HOST_MAC_OK_CANCEL = 1, HOST_MAC_RETRY_CANCEL = 2 };
/* A message box; 1 when OK (or Retry) was chosen. */
int host_mac_alert(const char *title, const char *text, int buttons, int warning);
/* The file-open dialog for one file of this extension; 1 with its path, 0 if cancelled. */
int host_mac_choose_file(const char *title, const char *extension, char *path, size_t size);

#endif
