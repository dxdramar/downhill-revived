/* WebSocket and HTTP for the module on the Mac (finding 075): the host side of
   port/mac/include/mac_net.h, over NSURLSession. Each connection has its own session, whose
   delegate notes the opening and the close; its messages queue here until the module's receive
   thread takes them (mac_ws_next blocks on a condition). */
#import <Foundation/Foundation.h>
#include <pthread.h>
#include "native.h"

struct w2c_net;

enum { CONNECTIONS = 32 };

@interface Connection : NSObject <NSURLSessionWebSocketDelegate>
@end

@implementation Connection {
@public
    NSURLSession *session;
    NSURLSessionWebSocketTask *task;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    NSMutableArray<NSURLSessionWebSocketMessage *> *queue;
    int opened, failed, closed, close_code;
    NSString *reason;       /* why it failed or closed */
    NSData *announced;      /* what mac_ws_next announced, for mac_ws_take */
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
    __weak Connection *weak = self;
    [task receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage *message, NSError *error) {
        Connection *c = weak;
        if (!c) return;
        if (error) { [c end:1006 reason:error.localizedDescription]; return; }
        pthread_mutex_lock(&c->lock);
        [c->queue addObject:message];
        pthread_cond_broadcast(&c->changed);
        pthread_mutex_unlock(&c->lock);
        [c receive];
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
    (void)s; (void)t;
    NSHTTPURLResponse *r = (NSHTTPURLResponse *)t.response;
    NSString *why = error ? error.localizedDescription
                  : [r isKindOfClass:NSHTTPURLResponse.class] && r.statusCode != 101
                  ? [NSString stringWithFormat:@"HTTP %ld", (long)r.statusCode] : @"connection lost";
    [self end:1006 reason:why];
}
@end

static Connection *connections[CONNECTIONS];
static pthread_mutex_t table = PTHREAD_MUTEX_INITIALIZER;

static Connection *connection(u32 h)
{
    pthread_mutex_lock(&table);
    Connection *c = h && h <= CONNECTIONS ? connections[h - 1] : nil;
    pthread_mutex_unlock(&table);
    return c;
}

static NSURL *url_of(u32 text)
{
    NSString *s = @(string_at(text));
    if ([s hasPrefix:@"wss://"]) s = [@"https://" stringByAppendingString:[s substringFromIndex:6]];
    else if ([s hasPrefix:@"ws://"]) s = [@"http://" stringByAppendingString:[s substringFromIndex:5]];
    return [NSURL URLWithString:s];
}

static void put_string(NSString *s, u32 out, u32 cap)
{
    if (!cap) return;
    const char *text = s.UTF8String ?: "";
    snprintf(at(out), cap, "%s", text);
}

u32 w2c_net_ws_open(struct w2c_net *unused, u32 url, u32 error, u32 cap)
{
    (void)unused;
    @autoreleasepool {
        NSString *text = @(string_at(url));
        NSURL *u = [NSURL URLWithString:text];
        if (!u) { put_string([@"bad URL " stringByAppendingString:text], error, cap); return 0; }
        Connection *c = [Connection new];
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
            if (pthread_cond_timedwait(&c->changed, &c->lock, &until)) { c->failed = 1, c->reason = @"timed out"; }
        int ok = c->opened && !c->failed;
        NSString *why = c->reason;
        pthread_mutex_unlock(&c->lock);
        if (!ok) {
            put_string([NSString stringWithFormat:@"can't connect to %@ (%@)", text, why ?: @"failed"], error, cap);
            [c->task cancel];
            [c->session invalidateAndCancel];
            return 0;
        }
        pthread_mutex_lock(&table);
        u32 h = 0;
        for (u32 i = 0; i < CONNECTIONS && !h; i++)
            if (!connections[i]) connections[i] = c, h = i + 1;
        pthread_mutex_unlock(&table);
        if (!h) {
            put_string(@"too many connections", error, cap);
            [c->task cancel];
            [c->session invalidateAndCancel];
        }
        return h;
    }
}

u32 w2c_net_ws_next(struct w2c_net *unused, u32 h, u32 kind)
{
    (void)unused;
    Connection *c = connection(h);
    if (!c) { *(int32_t *)at(kind) = 1006; return (u32)-1; }
    pthread_mutex_lock(&c->lock);
    while (!c->queue.count && !c->closed)
        pthread_cond_wait(&c->changed, &c->lock);
    int32_t result;
    if (c->queue.count) {
        NSURLSessionWebSocketMessage *m = c->queue.firstObject;
        [c->queue removeObjectAtIndex:0];
        int binary = m.type == NSURLSessionWebSocketMessageTypeData;
        c->announced = binary ? m.data : [m.string dataUsingEncoding:NSUTF8StringEncoding];
        *(int32_t *)at(kind) = binary;
        result = (int32_t)c->announced.length;
    } else {
        c->announced = [c->reason ?: @"" dataUsingEncoding:NSUTF8StringEncoding];
        *(int32_t *)at(kind) = c->close_code;
        result = -1;
    }
    pthread_mutex_unlock(&c->lock);
    return (u32)result;
}

u32 w2c_net_ws_take(struct w2c_net *unused, u32 h, u32 out, u32 cap)
{
    (void)unused;
    Connection *c = connection(h);
    if (!c) return 0;
    pthread_mutex_lock(&c->lock);
    NSData *d = c->announced;
    u32 n = d.length < cap ? (u32)d.length : cap;
    memcpy(at(out), d.bytes, n);
    c->announced = nil;
    pthread_mutex_unlock(&c->lock);
    return n;
}

u32 w2c_net_ws_send(struct w2c_net *unused, u32 h, u32 binary, u32 data, u32 n)
{
    (void)unused;
    Connection *c = connection(h);
    if (!c) return 0;
    @autoreleasepool {
        NSData *bytes = [NSData dataWithBytes:at(data) length:n];
        NSURLSessionWebSocketMessage *m = binary
            ? [[NSURLSessionWebSocketMessage alloc] initWithData:bytes]
            : [[NSURLSessionWebSocketMessage alloc] initWithString:[[NSString alloc] initWithData:bytes encoding:NSUTF8StringEncoding] ?: @""];
        __weak Connection *weak = c;
        [c->task sendMessage:m completionHandler:^(NSError *error) {
            if (error) [weak end:1006 reason:error.localizedDescription];
        }];
    }
    return 1;
}

void w2c_net_ws_close(struct w2c_net *unused, u32 h, u32 code)
{
    (void)unused;
    Connection *c = connection(h);
    if (!c) return;
    [c->task cancelWithCloseCode:code == 1000 ? NSURLSessionWebSocketCloseCodeNormalClosure
                                              : NSURLSessionWebSocketCloseCodeGoingAway reason:nil];
    [c end:(int)code reason:code == 1000 ? @"closed" : @"dropped"];
}

void w2c_net_ws_free(struct w2c_net *unused, u32 h)
{
    (void)unused;
    pthread_mutex_lock(&table);
    Connection *c = h && h <= CONNECTIONS ? connections[h - 1] : nil;
    if (c) connections[h - 1] = nil;
    pthread_mutex_unlock(&table);
    [c->task cancel];
    [c->session invalidateAndCancel];  /* the session holds its delegate until then */
}

u32 w2c_net_http_get(struct w2c_net *unused, u32 url, u32 out, u32 cap)
{
    (void)unused;
    @autoreleasepool {
        NSURL *u = url_of(url);
        if (!u || !cap) return (u32)-1;
        NSURLSessionConfiguration *config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
        config.timeoutIntervalForRequest = 10;
        NSURLSession *session = [NSURLSession sessionWithConfiguration:config];
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block int32_t length = -1;
        [[session dataTaskWithURL:u completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
            if (!error && [response isKindOfClass:NSHTTPURLResponse.class] &&
                ((NSHTTPURLResponse *)response).statusCode == 200) {
                u32 n = data.length < cap - 1 ? (u32)data.length : cap - 1;
                memcpy(at(out), data.bytes, n);
                ((char *)at(out))[n] = 0;
                length = (int32_t)n;
            }
            dispatch_semaphore_signal(done);
        }] resume];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        [session finishTasksAndInvalidate];
        return (u32)length;
    }
}
