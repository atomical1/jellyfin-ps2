/*
 * Network start-up, HTTP requests, streams and timed sleep; see net.c.
 */

#ifndef NET_H
#define NET_H

/*
 * Optional progress reporting. main.c points this at ui_boot so each step
 * appears on screen before it runs. If the console freezes, the last
 * message you saw names the call that blocked.
 */
typedef void (*NetProgress)(const char *msg);
void net_set_progress(NetProgress fn);

/* Report a line through the same hook, from outside net.c. */
void net_say(const char *msg);

/*
 * Sleep for roughly this many milliseconds, yielding the CPU. Never busy
 * wait on this hardware: the stack needs EE time to process what you are
 * waiting for.
 */
void net_snooze(int ms);
void net_snooze_init(void);

/*
 * fn runs on every vertical blank, inside the interrupt, so it keeps
 * running even when a thread hogs the CPU. Only i-prefixed kernel calls.
 */
void net_set_vblank_hook(void (*fn)(void));
void net_add_vblank_hook(void (*fn)(void));   /* a second, independent one */  /* once, early: sleeps count vertical blanks */

/* Brings up the network stack. Modules are loaded separately by mods.c. */
int net_start(void);
const char *net_link_help(void);

/* Tear the stack down before handing control back to the browser. */
void net_stop(void);

/* The console's own address once the stack is up. For display only. */
const char *net_local_ip(void);
const char *net_how(void);     /* "static", "from router"... "" before start */

/*
 * What the last http_get actually saw: status, bytes read, and the
 * Content-Length it was promised. A short read is the difference between
 * "the server is wrong" and "our socket gave up early".
 */
const char *net_last_info(void);

/* Body length of the last reply, headers excluded. */
int net_last_body_len(void);

/* First and last bytes of the last body, for diagnosing a bad parse. */
/* The whole last response, headers included, for dumping to a file. */
const char *net_last_raw(void);
int         net_last_raw_len(void);

/* Per-call recv sizes from the last request. Their sum should equal the
 * raw length; a mismatch localises where bytes went missing. */
int net_recv_count(void);
int net_recv_at(int i);


/* One line of diagnostics for the error screen: what lwIP thinks its
 * address is, and the byte-swapped port from the last connect attempt. */
const char *net_status(void);

/*
 * HTTP/1.0 GET. 1.0 on purpose: it has no chunked transfer encoding, so
 * the body is always plain and close-delimited and we never need a
 * dechunker.
 *
 * token may be NULL for an unauthenticated request.
 * On return *body points into an internal buffer, valid until the next call.
 * Returns 0 if the exchange completed; check *status for the HTTP result.
 */
/*
 * A streaming GET: opens the connection, sends the request, skips the
 * response headers, and hands back the socket for the caller to read from.
 *
 * Separate from http_get because that reads the whole body into one buffer,
 * which is wrong for something that never ends. Returns a socket, or
 * negative on failure.
 */
/*
 * The same, waiting up to wait_ms for the reply to start (net_stream_open
 * waits one second). For replies the server has to prepare first, like
 * subtitles it must extract from the film.
 */
int  net_stream_open_wait(const char *host, int port, const char *path,
                          const char *token, int wait_ms);
int  net_stream_open(const char *host, int port, const char *path,
                     const char *token);

/* Reads up to len bytes. 0 means the server closed, negative is an error. */
int  net_stream_read(int sock, void *buf, int len);

void net_stream_set_timeout(int sock, int ms);
void net_stream_close(int sock);

/*
 * A one-shot request with any method - POST or DELETE - for telling the
 * server playback has stopped. The reply is read only as far as the status
 * line. body may be NULL; it is sent as JSON. Returns the HTTP status, or
 * negative if the exchange failed.
 */
int  net_http_send(const char *host, int port, const char *method,
                   const char *path, const char *body);

int http_get(const char *host, int port, const char *path,
             const char *token, int *status, char **body);

/* The same, as a POST with a JSON body (NULL sends {}). */
int http_post(const char *host, int port, const char *path,
              const char *token, const char *json, int *status, char **body);

/* The DeviceId every request carries: the signed-in user's, see jf.c. */
void net_set_device_id(const char *id);

/*
 * The server's date (year, month 1-12, day) from the last reply's Date:
 * header. 1 if known, 0 before any reply carried one.
 */
int net_server_date(int *y, int *m, int *d);

/*
 * A byte range of the same resource, using an HTTP Range header.
 *
 * Needed because this stack drops the last 10 to 26 bytes of any full-size
 * TCP segment. Every failure we logged put the hole at raw offset ~1450,
 * and the Ethernet MSS is 1460. Responses that fit inside one undersized
 * segment arrive perfectly; anything larger loses a piece of its middle.
 *
 * So: never ask for more than fits in one short segment.
 *
 * On success copies up to len bytes into out, returns the count, and sets
 * *total to the resource size from Content-Range. Negative on failure.
 */
int http_get_range(const char *host, int port, const char *path,
                   const char *token, long start, int len,
                   char *out, int *status, long *total);

#endif
