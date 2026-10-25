/* Mac build (finding 075): WebSocket and HTTP through the native host's NSURLSession
   (port/mac/native/net.m), for port/mac/ws_mac.c. Calls block the calling module thread. */
#ifndef DHD_MAC_NET_H
#define DHD_MAC_NET_H
#include <stddef.h>
#include <stdint.h>

#define NET_IMPORT(name) __attribute__((import_module("net"), import_name(#name)))
/* Connects; a handle, or 0 with why in error (a C string, at most cap bytes). */
NET_IMPORT(ws_open) uint32_t mac_ws_open(const char *url, char *error, uint32_t cap);
/* Waits for the next message: its size, with *kind 0 text, 1 binary; or -1 when the connection
   ended, *kind then the close code (1006: lost) and its reason taken by mac_ws_take. */
NET_IMPORT(ws_next) int32_t mac_ws_next(uint32_t ws, int32_t *kind);
/* The message (or close reason) mac_ws_next announced, into out (its size, at most cap). */
NET_IMPORT(ws_take) uint32_t mac_ws_take(uint32_t ws, void *out, uint32_t cap);
NET_IMPORT(ws_send) int mac_ws_send(uint32_t ws, int binary, const void *data, uint32_t n);  /* 0: failed */
NET_IMPORT(ws_close) void mac_ws_close(uint32_t ws, int code);  /* a pending mac_ws_next returns -1 */
NET_IMPORT(ws_free) void mac_ws_free(uint32_t ws);
/* A blocking GET (ws:// and wss:// as http:// and https://): the body's length, -1 on error. */
NET_IMPORT(http_get) int32_t mac_http_get(const char *url, char *out, uint32_t cap);

#endif
