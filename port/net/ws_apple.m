/* WebSocket and HTTP(S) on macOS (finding 091): ws_winhttp.h over NSURLSession, which has TLS
   (ws_posix.c has none, so wss://dhd.skuza.app needs this). Threaded as ws_winhttp.c is: a
   receive thread that connects, reads messages and reconnects when told to, and a send thread
   that drains a queue and ticks the handler every 10 ms. Each connection has its own session,
   whose delegate notes the opening and the close; its messages queue until the receive thread
   takes them. */
#import <Foundation/Foundation.h>
#include "ws_winhttp.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- one NSURLSession WebSocket ---- */

@interface WsSocket : NSObject <NSURLSessionWebSocketDelegate>
@end

@implementation WsSocket {
@public
    NSURLSession *session;
    NSURLSessionWebSocketTask *task;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    NSMutableArray<NSURLSessionWebSocketMessage *> *queue;
    int opened, failed, closed, close_code;
    NSString *reason;  /* why it failed or closed */
}

- (instancetype)init
{
    if ((self = [super init])) {
        pthread_mutex_init(&lock, NULL);
        pthread_cond_init(&changed, NULL);
        queue = [NSMutableArray new];
    }
    return self;
}

- (void)dealloc
{
    pthread_mutex_destroy(&lock);
    pthread_cond_destroy(&changed);
}

- (void)end:(int)code reason:(NSString *)why
{
    pthread_mutex_lock(&lock);
    if (!opened && !failed) failed = 1, reason = why;
    else if (!closed) closed = 1, close_code = code, reason = why;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

- (void)receive
{
    __weak WsSocket *weak = self;
    [task receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage *message, NSError *error) {
        WsSocket *s = weak;
        if (!s) return;
        if (error) { [s end:1006 reason:error.localizedDescription]; return; }
        pthread_mutex_lock(&s->lock);
        [s->queue addObject:message];
        pthread_cond_broadcast(&s->changed);
        pthread_mutex_unlock(&s->lock);
        [s receive];
    }];
}

- (void)URLSession:(NSURLSession *)s webSocketTask:(NSURLSessionWebSocketTask *)t didOpenWithProtocol:(NSString *)protocol
{
    (void)s; (void)t; (void)protocol;
    pthread_mutex_lock(&lock);
    opened = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    [self receive];
}

- (void)URLSession:(NSURLSession *)s webSocketTask:(NSURLSessionWebSocketTask *)t
    didCloseWithCode:(NSURLSessionWebSocketCloseCode)code reason:(NSData *)why
{
    (void)s; (void)t;
    [self end:(int)code reason:why ? [[NSString alloc] initWithData:why encoding:NSUTF8StringEncoding] : @""];
}

- (void)URLSession:(NSURLSession *)s task:(NSURLSessionTask *)t didCompleteWithError:(NSError *)error
{
    (void)s;
    NSHTTPURLResponse *r = (NSHTTPURLResponse *)t.response;
    NSString *why = error ? error.localizedDescription
                  : [r isKindOfClass:NSHTTPURLResponse.class] && r.statusCode != 101
                  ? [NSString stringWithFormat:@"HTTP %ld", (long)r.statusCode] : @"connection lost";
    [self end:1006 reason:why];
}
@end

/* Connects (waits up to 15 s); nil with why in error. */
static WsSocket *socket_open(const char *url, char *error, size_t cap)
{
    @autoreleasepool {
        NSURL *u = [NSURL URLWithString:@(url)];
        if (!u) { snprintf(error, cap, "bad URL %s", url); return nil; }
        WsSocket *c = [WsSocket new];
        NSURLSessionConfiguration *config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
        config.timeoutIntervalForRequest = 10;
        c->session = [NSURLSession sessionWithConfiguration:config delegate:c delegateQueue:nil];
        c->task = [c->session webSocketTaskWithURL:u];
        c->task.maximumMessageSize = 64 << 20;
        [c->task resume];
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 15;
        pthread_mutex_lock(&c->lock);
        while (!c->opened && !c->failed)
            if (pthread_cond_timedwait(&c->changed, &c->lock, &until)) c->failed = 1, c->reason = @"timed out";
        int ok = c->opened && !c->failed;
        NSString *why = c->reason;
        pthread_mutex_unlock(&c->lock);
        if (ok) return c;
        snprintf(error, cap, "can't connect to %s (%s)", url, (why ?: @"failed").UTF8String);
        [c->task cancel];
        [c->session invalidateAndCancel];
        return nil;
    }
}

/* Waits for the next message: 1 with it (*binary set), 0 when the connection ended (code and
   reason set). */
static int socket_next(WsSocket *c, NSData **data, int *binary, int *code, char *reason, size_t cap)
{
    pthread_mutex_lock(&c->lock);
    while (!c->queue.count && !c->closed) pthread_cond_wait(&c->changed, &c->lock);
    int got = c->queue.count > 0;
    if (got) {
        NSURLSessionWebSocketMessage *m = c->queue.firstObject;
        [c->queue removeObjectAtIndex:0];
        *binary = m.type == NSURLSessionWebSocketMessageTypeData;
        *data = *binary ? m.data : [m.string dataUsingEncoding:NSUTF8StringEncoding];
    } else {
        *code = c->close_code;
        snprintf(reason, cap, "%s", (c->reason ?: @"").UTF8String);
    }
    pthread_mutex_unlock(&c->lock);
    return got;
}

static void socket_send(WsSocket *c, int binary, const void *data, size_t n)
{
    @autoreleasepool {
        NSData *bytes = [NSData dataWithBytes:data length:n];
        NSURLSessionWebSocketMessage *m = binary
            ? [[NSURLSessionWebSocketMessage alloc] initWithData:bytes]
            : [[NSURLSessionWebSocketMessage alloc] initWithString:[[NSString alloc] initWithData:bytes encoding:NSUTF8StringEncoding] ?: @""];
        __weak WsSocket *weak = c;
        [c->task sendMessage:m completionHandler:^(NSError *error) {
            if (error) [weak end:1006 reason:error.localizedDescription];
        }];
    }
}

/* Ends the connection from any thread: a pending socket_next returns 0. */
static void socket_close(WsSocket *c, int code)
{
    [c->task cancelWithCloseCode:code == 1000 ? NSURLSessionWebSocketCloseCodeNormalClosure
                                              : NSURLSessionWebSocketCloseCodeGoingAway reason:nil];
    [c end:code reason:code == 1000 ? @"closed" : @"dropped"];
}

static void socket_free(WsSocket *c)
{
    [c->task cancel];
    [c->session invalidateAndCancel];  /* the session holds its delegate until then */
}

/* ---- ws_winhttp.h ---- */

typedef struct Out { struct Out *next; int binary; size_t n; uint8_t data[]; } Out;

struct WsConn {
    WsHandler h;
    char url[512];
    WsSocket *socket;            /* the open WebSocket, nil between connections */
    pthread_mutex_t lock;        /* socket and the queue */
    pthread_cond_t wake;         /* the send thread's: a message queued, or stopping */
    pthread_cond_t stop_wait;    /* the receive thread's backoff */
    Out *head, *tail;
    pthread_t io_thread, send_thread;
    volatile int stopping;
};

static void clear_queue(WsConn *c)
{
    Out *m;
    while ((m = c->head)) c->head = m->next, free(m);
    c->tail = NULL;
}

static void *io_main(void *arg)
{
    WsConn *c = arg;
    int attempt = 0;
    pthread_setname_np("ws receive");
    while (!c->stopping) {
        char reason[160] = "";
        int code = -1;
        const char *next;
        WsSocket *ws = socket_open(c->url, reason, sizeof reason);
        if (ws) {
            attempt = 0;
            pthread_mutex_lock(&c->lock);
            clear_queue(c);
            c->socket = ws;
            pthread_mutex_unlock(&c->lock);
            c->h.on_open(c->h.ctx, c);
            for (;;) {
                @autoreleasepool {
                    NSData *data = nil;
                    int binary = 0;
                    if (!socket_next(ws, &data, &binary, &code, reason, sizeof reason)) break;
                    size_t n = data.length;
                    uint8_t *buf = malloc(n + 1);
                    memcpy(buf, data.bytes, n);
                    buf[n] = 0;
                    c->h.on_message(c->h.ctx, binary, buf, n);
                    free(buf);
                }
            }
            pthread_mutex_lock(&c->lock);
            c->socket = nil;
            pthread_mutex_unlock(&c->lock);
            socket_free(ws);
        }
        next = c->h.on_close(c->h.ctx, code, reason, attempt++);
        if (!next || c->stopping) break;
        snprintf(c->url, sizeof c->url, "%s", next);
        unsigned ms = attempt < 4 ? 500u << attempt : 8000;
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += ms / 1000;
        until.tv_nsec += (long)(ms % 1000) * 1000000;
        if (until.tv_nsec >= 1000000000) until.tv_sec++, until.tv_nsec -= 1000000000;
        pthread_mutex_lock(&c->lock);
        if (!c->stopping) pthread_cond_timedwait(&c->stop_wait, &c->lock, &until);
        pthread_mutex_unlock(&c->lock);
    }
    return NULL;
}

/* Drops the connection from outside the receive thread: its pending receive ends. */
static void force_close(WsConn *c, int code)
{
    pthread_mutex_lock(&c->lock);
    if (c->socket) socket_close(c->socket, code);
    pthread_mutex_unlock(&c->lock);
}

static void *send_main(void *arg)
{
    WsConn *c = arg;
    pthread_setname_np("ws send");
    while (!c->stopping) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 10 * 1000000;
        if (until.tv_nsec >= 1000000000) until.tv_sec++, until.tv_nsec -= 1000000000;
        pthread_mutex_lock(&c->lock);
        if (!c->head && !c->stopping) pthread_cond_timedwait(&c->wake, &c->lock, &until);
        pthread_mutex_unlock(&c->lock);
        for (;;) {
            Out *m;
            WsSocket *socket;
            pthread_mutex_lock(&c->lock);
            if ((m = c->head) && !(c->head = m->next)) c->tail = NULL;
            socket = c->socket;
            pthread_mutex_unlock(&c->lock);
            if (!m) break;
            if (socket) socket_send(socket, m->binary, m->data, m->n);
            free(m);
        }
        if (c->h.on_tick && c->h.on_tick(c->h.ctx)) force_close(c, 1006);
    }
    return NULL;
}

WsConn *ws_start(const char *url, const WsHandler *h)
{
    WsConn *c = calloc(1, sizeof *c);
    c->h = *h;
    snprintf(c->url, sizeof c->url, "%s", url);
    pthread_mutex_init(&c->lock, NULL);
    pthread_cond_init(&c->wake, NULL);
    pthread_cond_init(&c->stop_wait, NULL);
    pthread_create(&c->io_thread, NULL, io_main, c);
    pthread_create(&c->send_thread, NULL, send_main, c);
    return c;
}

void ws_send(WsConn *c, int binary, const void *data, size_t n)
{
    pthread_mutex_lock(&c->lock);
    if (!c->socket) {
        pthread_mutex_unlock(&c->lock);
        return;
    }
    Out *m = malloc(sizeof *m + n);
    m->next = NULL, m->binary = binary, m->n = n;
    memcpy(m->data, data, n);
    if (c->tail) c->tail->next = m;
    else c->head = m;
    c->tail = m;
    pthread_cond_signal(&c->wake);
    pthread_mutex_unlock(&c->lock);
}

void ws_stop(WsConn *c)
{
    for (int i = 0; i < 50 && c->head; i++) usleep(10000);  /* let the queue drain (a leave message) */
    pthread_mutex_lock(&c->lock);
    c->stopping = 1;
    pthread_cond_broadcast(&c->wake);
    pthread_cond_broadcast(&c->stop_wait);
    pthread_mutex_unlock(&c->lock);
    pthread_join(c->send_thread, NULL);
    force_close(c, 1000);
    pthread_join(c->io_thread, NULL);
    clear_queue(c);
    pthread_mutex_destroy(&c->lock);
    pthread_cond_destroy(&c->wake);
    pthread_cond_destroy(&c->stop_wait);
    free(c);
}

/* A blocking GET (ws:// and wss:// as http:// and https://): the body's length, -1 on error. */
int http_get(const char *url, char *out, size_t cap)
{
    @autoreleasepool {
        if (!cap) return -1;
        NSString *s = @(url);
        if ([s hasPrefix:@"wss://"]) s = [@"https://" stringByAppendingString:[s substringFromIndex:6]];
        else if ([s hasPrefix:@"ws://"]) s = [@"http://" stringByAppendingString:[s substringFromIndex:5]];
        NSURL *u = [NSURL URLWithString:s];
        if (!u) return -1;
        NSURLSessionConfiguration *config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
        config.timeoutIntervalForRequest = 10;
        NSURLSession *session = [NSURLSession sessionWithConfiguration:config];
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block int length = -1;
        [[session dataTaskWithURL:u completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
            if (!error && [response isKindOfClass:NSHTTPURLResponse.class] &&
                ((NSHTTPURLResponse *)response).statusCode == 200) {
                size_t n = data.length < cap - 1 ? data.length : cap - 1;
                memcpy(out, data.bytes, n);
                out[n] = 0;
                length = (int)n;
            }
            dispatch_semaphore_signal(done);
        }] resume];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        [session finishTasksAndInvalidate];
        return length;
    }
}
