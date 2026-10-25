/* WebSocket on the Mac (finding 075): port/net/ws_winhttp.h over the native host's NSURLSession
   (mac_net.h), threaded as ws_winhttp.c is: a receive thread that connects, reads messages and
   reconnects when told to, and a send thread that drains a queue and ticks the handler every
   10 ms. */
#include "../net/ws_winhttp.h"

#include <windows.h>
#include <mac_net.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Out { struct Out *next; int binary; size_t n; uint8_t data[]; } Out;

struct WsConn {
    WsHandler h;
    char url[512];
    uint32_t socket;             /* the open WebSocket, 0 between connections */
    CRITICAL_SECTION lock;       /* socket and the queue */
    Out *head, *tail;
    HANDLE wake, stop_event, io_thread, send_thread;
    volatile LONG stopping;
};

static void clear_queue(WsConn *c)
{
    Out *m;
    while ((m = c->head))
        c->head = m->next, free(m);
    c->tail = NULL;
}

static DWORD WINAPI io_main(void *arg)
{
    WsConn *c = arg;
    int attempt = 0;
    while (!c->stopping) {
        char reason[160] = "";
        int code = -1;
        const char *next;
        uint32_t ws = mac_ws_open(c->url, reason, sizeof reason);
        if (ws) {
            attempt = 0;
            EnterCriticalSection(&c->lock);
            clear_queue(c);
            c->socket = ws;
            LeaveCriticalSection(&c->lock);
            c->h.on_open(c->h.ctx, c);
            for (;;) {
                int32_t kind = 0, n = mac_ws_next(ws, &kind);
                if (n < 0) {
                    reason[mac_ws_take(ws, reason, sizeof reason - 1)] = 0;
                    code = kind;
                    break;
                }
                uint8_t *buf = malloc((size_t)n + 1);
                mac_ws_take(ws, buf, (uint32_t)n);
                buf[n] = 0;
                c->h.on_message(c->h.ctx, kind == 1, buf, (size_t)n);
                free(buf);
            }
            EnterCriticalSection(&c->lock);
            c->socket = 0;
            LeaveCriticalSection(&c->lock);
            mac_ws_free(ws);
        }
        next = c->h.on_close(c->h.ctx, code, reason, attempt++);
        if (!next || c->stopping)
            break;
        snprintf(c->url, sizeof c->url, "%s", next);
        WaitForSingleObject(c->stop_event, attempt < 4 ? 500u << attempt : 8000);
    }
    return 0;
}

/* Drops the connection from outside the receive thread: its pending receive ends. */
static void force_close(WsConn *c, int code)
{
    EnterCriticalSection(&c->lock);
    if (c->socket)
        mac_ws_close(c->socket, code);
    LeaveCriticalSection(&c->lock);
}

static DWORD WINAPI send_main(void *arg)
{
    WsConn *c = arg;
    while (!c->stopping) {
        Out *m;
        WaitForSingleObject(c->wake, 10);
        for (;;) {
            uint32_t socket;
            EnterCriticalSection(&c->lock);
            if ((m = c->head) && !(c->head = m->next))
                c->tail = NULL;
            socket = c->socket;
            LeaveCriticalSection(&c->lock);
            if (!m)
                break;
            if (socket)
                mac_ws_send(socket, m->binary, m->data, (uint32_t)m->n);
            free(m);
        }
        if (c->h.on_tick && c->h.on_tick(c->h.ctx))
            force_close(c, 1006);
    }
    return 0;
}

WsConn *ws_start(const char *url, const WsHandler *h)
{
    WsConn *c = calloc(1, sizeof *c);
    c->h = *h;
    snprintf(c->url, sizeof c->url, "%s", url);
    InitializeCriticalSection(&c->lock);
    c->wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    c->stop_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    c->io_thread = CreateThread(NULL, 0, io_main, c, 0, NULL);
    c->send_thread = CreateThread(NULL, 0, send_main, c, 0, NULL);
    return c;
}

void ws_send(WsConn *c, int binary, const void *data, size_t n)
{
    Out *m;
    EnterCriticalSection(&c->lock);
    if (!c->socket) {
        LeaveCriticalSection(&c->lock);
        return;
    }
    m = malloc(sizeof *m + n);
    m->next = NULL, m->binary = binary, m->n = n;
    memcpy(m->data, data, n);
    if (c->tail)
        c->tail->next = m;
    else
        c->head = m;
    c->tail = m;
    LeaveCriticalSection(&c->lock);
    SetEvent(c->wake);
}

void ws_stop(WsConn *c)
{
    for (int i = 0; i < 50 && c->head; i++)  /* let the queue drain (a leave message) */
        Sleep(10);
    InterlockedExchange(&c->stopping, 1);
    SetEvent(c->stop_event);
    SetEvent(c->wake);
    WaitForSingleObject(c->send_thread, INFINITE);
    force_close(c, 1000);
    WaitForSingleObject(c->io_thread, INFINITE);
    CloseHandle(c->io_thread), CloseHandle(c->send_thread);
    CloseHandle(c->wake), CloseHandle(c->stop_event);
    clear_queue(c);
    DeleteCriticalSection(&c->lock);
    free(c);
}

int http_get(const char *url, char *out, size_t cap)
{
    if (!cap) return -1;
    return mac_http_get(url, out, (uint32_t)cap);
}
