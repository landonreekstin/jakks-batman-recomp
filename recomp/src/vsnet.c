/* Versus netplay: see vsnet.h. The socket handling follows net.c (the co-op netplay). */
#include "vsnet.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define VSN_MAGIC 0x53564142u  /* "BAVS" */
#define VSN_PROTO 1
#define RING 256               /* input history, frames */
#define MAX_ROLLBACK 12        /* frames run past the latest known input before waiting */
#define SNAPS (MAX_ROLLBACK + 4)
#define HASH_EVERY 60
#define HASHES 32
#define MAX_UP 64              /* inputs per guest packet */
#define MAX_DOWN 32            /* inputs per slot per host packet */
#define GUESTS (VS_MAX - 1)
#define PEER_TIMEOUT_MS 15000
#define SIM_QUEUE 1024

enum { MSG_HELLO = 1, MSG_WELCOME, MSG_START };

typedef struct hello {
    uint32_t magic, proto, state_size, pad;
    uint8_t id[32];
    uint16_t udp_port, pad2;
} hello_t;

typedef struct welcome {
    uint32_t ok;
    char why[124];
} welcome_t;

typedef struct start_hdr {
    uint32_t epoch, frame, delay, owned, slot, pad;
} start_hdr_t;

/* guest -> host */
typedef struct __attribute__((packed)) up {
    uint32_t magic, epoch;
    uint32_t frame;
    int32_t adv;
    uint32_t t_send, t_echo, t_hold;
    uint32_t hash_frame;
    uint64_t hash;
    uint32_t ack[VS_MAX];    /* the guest has slot s's inputs for every frame before ack[s] */
    uint32_t first;
    uint16_t slot, n;
    uint16_t in[MAX_UP];
} up_t;

/* host -> guest */
typedef struct __attribute__((packed)) down {
    uint32_t magic, epoch;
    uint32_t frame;
    int32_t adv;
    uint32_t t_send, t_echo, t_hold;
    uint32_t ack;            /* the host has this guest's inputs for every frame before it */
    uint32_t first[VS_MAX];
    uint16_t n[VS_MAX];
    uint16_t in[VS_MAX * MAX_DOWN];   /* slot 0's, then slot 1's, ... (n[s] each) */
} down_t;

typedef struct guest {
    int tcp;
    struct sockaddr_storage addr;
    socklen_t addr_len;
    int slot;                /* -1: free */
    uint32_t has[VS_MAX];    /* its acks */
    uint32_t frame;
    int have_frame;
    int32_t adv;
    float adv_avg;
    uint32_t t_peer, t_peer_at, last_recv;
    int rtt;
    struct { uint32_t frame; uint64_t h; } hash[HASHES];
    char name[64];
} guest_t;

struct vsnet {
    int host;
    int tcp_listen, tcp, udp;     /* guest: tcp is the host connection */
    struct sockaddr_storage peer; /* guest: the host */
    socklen_t peer_len;
    net_opts_t o;
    uint8_t id[32];
    uint64_t t0, rng;
    char status[160], why[128], peer_name[128];
    int news, gone, refused, connecting, connected;
    uint64_t connect_by;
    uint8_t *rx;                  /* guest: TCP from the host */
    size_t rx_len, rx_cap;

    guest_t g[GUESTS];            /* host */
    int want_start;               /* host: send everyone the state at the next tick */

    /* the session */
    int running;
    uint32_t epoch, start;
    int slot;                     /* ours */
    uint32_t owned;               /* slots played by a machine */
    uint32_t next[VS_MAX];        /* first frame without slot s's input */
    uint32_t host_has;            /* guest: the host has our inputs before this */
    uint32_t checked;
    uint16_t in[VS_MAX][RING], used[VS_MAX][RING];
    versus_t snaps[SNAPS];
    uint32_t snap_frame[SNAPS];

    /* guest: the host's clock and frame */
    uint32_t peer_frame;
    int32_t peer_adv;
    float adv_avg, peer_adv_avg;
    int have_peer_frame;
    uint32_t t_peer, t_peer_at, last_recv;
    int rtt;
    uint32_t last_wait;
    int rollbacks, rolled, last_roll, waits, stalls;

    uint32_t hash_next, mine_latest;
    struct { uint32_t frame; uint64_t h; } mine[HASHES];
    uint32_t desyncs;

    FILE *log;
    uint32_t logged_next;
    uint16_t logged[VS_MAX];

    struct { uint64_t at; struct sockaddr_storage to; socklen_t to_len; int len; uint8_t data[sizeof(down_t)]; } q[SIM_QUEUE];
    int qn;
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint32_t clock_ms(const vsnet_t *n) { return (uint32_t)(now_ms() - n->t0) + 1; }

static void say(vsnet_t *n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(n->why, sizeof(n->why), fmt, ap);
    va_end(ap);
    n->news = 1;
    fprintf(stderr, "vsnet: %s\n", n->why);
}

static uint64_t rnd(vsnet_t *n)
{
    n->rng ^= n->rng << 13;
    n->rng ^= n->rng >> 7;
    n->rng ^= n->rng << 17;
    return n->rng;
}

static vsnet_t *alloc_net(const net_opts_t *o, const uint8_t id[32])
{
    vsnet_t *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->tcp_listen = n->tcp = n->udp = -1;
    n->o = *o;
    if (n->o.delay < 0) n->o.delay = 0;
    if (n->o.delay > 8) n->o.delay = 8;
    memcpy(n->id, id, 32);
    n->t0 = now_ms();
    n->rng = n->t0 * 0x9E3779B97F4A7C15ull + (uint64_t)getpid();
    n->slot = -1;
    for (int i = 0; i < GUESTS; i++) { n->g[i].tcp = -1; n->g[i].slot = -1; }
    return n;
}

static void set_nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

/* --- TCP --- */

static int send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len) {
        const ssize_t k = send(fd, p, len, MSG_NOSIGNAL);
        if (k < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pf = { .fd = fd, .events = POLLOUT };
                if (poll(&pf, 1, 5000) <= 0) return 0;
                continue;
            }
            if (errno == EINTR) continue;
            return 0;
        }
        p += k;
        len -= (size_t)k;
    }
    return 1;
}

static int send_msg(int fd, uint32_t type, const void *a, size_t alen, const void *b, size_t blen)
{
    const uint32_t h[2] = { type, (uint32_t)(alen + blen) };
    return send_all(fd, h, sizeof(h)) && (!alen || send_all(fd, a, alen)) && (!blen || send_all(fd, b, blen));
}

/* guest: one whole message from the host (type, *body, *len), 0 for none yet, -1 when closed */
static int recv_msg(vsnet_t *n, const uint8_t **body, uint32_t *len)
{
    if (n->rx_len >= 8) {
        uint32_t h[2];
        memcpy(h, n->rx, 8);
        if (h[0] & 0x80000000u) {
            const size_t used = 8 + h[1];
            memmove(n->rx, n->rx + used, n->rx_len - used);
            n->rx_len -= used;
        }
    }
    for (;;) {
        if (n->rx_len >= 8) {
            uint32_t h[2];
            memcpy(h, n->rx, 8);
            if (h[1] > sizeof(versus_t) + 4096) return -1;
            if (n->rx_len >= 8 + (size_t)h[1]) {
                h[0] |= 0x80000000u;
                memcpy(n->rx, h, 4);
                *body = n->rx + 8;
                *len = h[1];
                return (int)(h[0] & 0x7FFFFFFFu);
            }
        }
        if (n->rx_cap - n->rx_len < 16384) {
            n->rx_cap = n->rx_cap ? n->rx_cap * 2 : sizeof(versus_t) + 32768;
            n->rx = realloc(n->rx, n->rx_cap);
        }
        const ssize_t k = recv(n->tcp, n->rx + n->rx_len, n->rx_cap - n->rx_len, 0);
        if (k == 0) return -1;
        if (k < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
        n->rx_len += (size_t)k;
    }
}

static int read_timeout(int fd, void *buf, size_t len, int ms)
{
    uint8_t *p = buf;
    while (len) {
        struct pollfd pf = { .fd = fd, .events = POLLIN };
        if (poll(&pf, 1, ms) <= 0) return 0;
        const ssize_t k = recv(fd, p, len, 0);
        if (k <= 0) {
            if (k < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            return 0;
        }
        p += k;
        len -= (size_t)k;
    }
    return 1;
}

/* --- UDP, through the simulated network when one is set --- */

static void udp_send(vsnet_t *n, const struct sockaddr_storage *to, socklen_t to_len, const void *p, int len)
{
    if (!n->o.lat_ms && !n->o.jitter_ms && !n->o.loss_pct) {
        sendto(n->udp, p, (size_t)len, 0, (const struct sockaddr *)to, to_len);
        return;
    }
    if ((int)(rnd(n) % 100) < n->o.loss_pct || n->qn == SIM_QUEUE) return;
    n->q[n->qn].at = now_ms() + (uint64_t)n->o.lat_ms + (n->o.jitter_ms ? rnd(n) % (uint64_t)(n->o.jitter_ms + 1) : 0);
    n->q[n->qn].to = *to;
    n->q[n->qn].to_len = to_len;
    memcpy(n->q[n->qn].data, p, (size_t)len);
    n->q[n->qn++].len = len;
}

static void udp_flush(vsnet_t *n)
{
    const uint64_t t = now_ms();
    for (int i = 0; i < n->qn;)
        if (n->q[i].at <= t) {
            sendto(n->udp, n->q[i].data, (size_t)n->q[i].len, 0, (const struct sockaddr *)&n->q[i].to, n->q[i].to_len);
            n->q[i] = n->q[--n->qn];
        } else i++;
}

/* --- the session --- */

static int remote(const vsnet_t *n, int s) { return (n->owned >> s & 1) && s != n->slot; }

static uint16_t input_at(const vsnet_t *n, int s, uint32_t f)
{
    if (!(n->owned >> s & 1)) return 0;
    return f < n->next[s] ? n->in[s][f % RING] : n->in[s][(n->next[s] - 1) % RING];
}

static void begin(vsnet_t *n, const versus_t *v, uint32_t epoch, uint32_t owned, int slot)
{
    const uint32_t s = v->frame, d = (uint32_t)n->o.delay;
    n->running = 1;
    n->epoch = epoch;
    n->start = s;
    n->owned = owned;
    n->slot = slot;
    for (int k = 0; k < VS_MAX; k++) {
        n->next[k] = s + d;
        for (uint32_t f = s - 1; f != s + d + 1; f++) n->in[k][f % RING] = n->used[k][f % RING] = 0;
    }
    n->host_has = s + d;
    n->checked = s;
    for (int i = 0; i < SNAPS; i++) n->snap_frame[i] = UINT32_MAX;
    n->have_peer_frame = 0;
    n->adv_avg = n->peer_adv_avg = 0;
    n->hash_next = (s + HASH_EVERY - 1) / HASH_EVERY * HASH_EVERY;
    if (!n->hash_next) n->hash_next = HASH_EVERY;
    memset(n->mine, 0, sizeof(n->mine));
    n->mine_latest = UINT32_MAX;
    n->logged_next = s;
    n->last_recv = clock_ms(n);
    for (int i = 0; i < GUESTS; i++) {
        guest_t *g = &n->g[i];
        for (int k = 0; k < VS_MAX; k++) g->has[k] = s + d;
        g->have_frame = 0;
        g->adv_avg = 0;
        g->last_recv = n->last_recv;
        memset(g->hash, 0, sizeof(g->hash));
    }
    n->qn = 0;
}

static void drop_guest(vsnet_t *n, guest_t *g, versus_t *v, const char *why)
{
    say(n, "P%d (%s) left: %s", g->slot + 1, g->name, why);
    if (g->slot >= 0) versus_release_slot(v, g->slot);
    if (g->tcp >= 0) close(g->tcp);
    g->tcp = -1;
    g->slot = -1;
    n->want_start = 1;
}

/* host: everyone goes on from the host's current state (a join, a leave, a desync) */
static void host_start(vsnet_t *n, versus_t *v)
{
    uint32_t owned = 1;   /* slot 0: the host */
    for (int i = 0; i < GUESTS; i++)
        if (n->g[i].slot >= 0) owned |= 1u << n->g[i].slot;
    const uint32_t epoch = n->epoch + 1;
    for (int i = 0; i < GUESTS; i++) {
        guest_t *g = &n->g[i];
        if (g->slot < 0) continue;
        const start_hdr_t h = { epoch, v->frame, (uint32_t)n->o.delay, owned, (uint32_t)g->slot, 0 };
        if (!send_msg(g->tcp, MSG_START, &h, sizeof(h), v, sizeof(*v))) {
            drop_guest(n, g, v, "its connection failed");
            host_start(n, v);   /* again, without it */
            return;
        }
    }
    begin(n, v, epoch, owned, 0);
    n->want_start = 0;
}

static void run_frame(vsnet_t *n, versus_t *v, const uint16_t *rom)
{
    const uint32_t f = v->frame;
    n->snaps[f % SNAPS] = *v;
    n->snap_frame[f % SNAPS] = f;
    uint16_t b[VS_MAX];
    for (int s = 0; s < VS_MAX; s++) b[s] = n->used[s][f % RING] = input_at(n, s, f);
    versus_step(v, rom, b);
}

static void roll_back(vsnet_t *n, versus_t *v, const uint16_t *rom, uint32_t from)
{
    const uint32_t to = v->frame;
    if (n->snap_frame[from % SNAPS] != from) { say(n, "no snapshot for frame %u", from); return; }
    *v = n->snaps[from % SNAPS];
    while (v->frame < to) run_frame(n, v, rom);
    n->rollbacks++;
    n->rolled += (int)(to - from);
    n->last_roll = (int)(to - from);
}

/* frames before this have every slot's real input */
static uint32_t known_end(const vsnet_t *n)
{
    uint32_t e = UINT32_MAX;
    for (int s = 0; s < VS_MAX; s++)
        if ((n->owned >> s & 1) && n->next[s] < e) e = n->next[s];
    return e;
}

static uint32_t settled_end(const vsnet_t *n, const versus_t *v)
{
    const uint32_t e = known_end(n);
    return e < v->frame ? e : v->frame;
}

static void take_input(vsnet_t *n, int s, uint32_t first, int cnt, const uint16_t *in)
{
    for (int i = 0; i < cnt; i++) {
        const uint32_t f = first + (uint32_t)i;
        if (f != n->next[s]) continue;   /* old, or after a gap (a later packet repeats it) */
        if ((int32_t)(f - n->next[n->slot]) > RING / 2) break;
        n->in[s][f % RING] = in[i];
        n->next[s]++;
    }
}

static guest_t *guest_by_slot(vsnet_t *n, int slot)
{
    for (int i = 0; i < GUESTS; i++)
        if (n->g[i].slot == slot && slot >= 0) return &n->g[i];
    return NULL;
}

static void on_up(vsnet_t *n, const up_t *p, int len, const struct sockaddr_storage *from, socklen_t fl)
{
    if (len < (int)offsetof(up_t, in) || p->magic != VSN_MAGIC || p->epoch != n->epoch) return;
    if (p->n > MAX_UP || len < (int)(offsetof(up_t, in) + 2 * p->n) || p->slot >= VS_MAX) return;
    guest_t *g = guest_by_slot(n, p->slot);
    if (!g) return;
    memcpy(&g->addr, from, fl);   /* follow its address */
    g->addr_len = fl;
    const uint32_t t = clock_ms(n);
    g->last_recv = t;
    if (!g->have_frame || (int32_t)(p->frame - g->frame) >= 0) {
        g->frame = p->frame;
        g->adv = p->adv;
        g->have_frame = 1;
    }
    for (int s = 0; s < VS_MAX; s++)
        if ((int32_t)(p->ack[s] - g->has[s]) > 0) g->has[s] = p->ack[s];
    if (p->t_send) { g->t_peer = p->t_send; g->t_peer_at = t; }
    if (p->t_echo) {
        const int rtt = (int)(t - p->t_echo) - (int)p->t_hold;
        if (rtt >= 0) g->rtt = g->rtt ? (g->rtt * 7 + rtt) / 8 : rtt;
    }
    take_input(n, p->slot, p->first, p->n, p->in);
    if (p->hash_frame) {
        const int k = (int)(p->hash_frame / HASH_EVERY % HASHES);
        g->hash[k].frame = p->hash_frame;
        g->hash[k].h = p->hash;
    }
}

static void on_down(vsnet_t *n, const down_t *p, int len)
{
    if (len < (int)offsetof(down_t, in) || p->magic != VSN_MAGIC || p->epoch != n->epoch) return;
    int total = 0;
    for (int s = 0; s < VS_MAX; s++) {
        if (p->n[s] > MAX_DOWN) return;
        total += p->n[s];
    }
    if (len < (int)(offsetof(down_t, in) + 2 * total)) return;
    const uint32_t t = clock_ms(n);
    n->last_recv = t;
    if (!n->have_peer_frame || (int32_t)(p->frame - n->peer_frame) >= 0) {
        n->peer_frame = p->frame;
        n->peer_adv = p->adv;
        n->peer_adv_avg = n->have_peer_frame ? n->peer_adv_avg * 0.9f + (float)p->adv * 0.1f : (float)p->adv;
        n->have_peer_frame = 1;
    }
    if ((int32_t)(p->ack - n->host_has) > 0) n->host_has = p->ack;
    if (p->t_send) { n->t_peer = p->t_send; n->t_peer_at = t; }
    if (p->t_echo) {
        const int rtt = (int)(t - p->t_echo) - (int)p->t_hold;
        if (rtt >= 0) n->rtt = n->rtt ? (n->rtt * 7 + rtt) / 8 : rtt;
    }
    int o = 0;
    for (int s = 0; s < VS_MAX; s++) {
        if (remote(n, s)) take_input(n, s, p->first[s], p->n[s], p->in + o);
        o += p->n[s];
    }
}

static void check_hashes(vsnet_t *n, versus_t *v)
{
    const uint32_t e = settled_end(n, v);
    while (n->hash_next < e) {   /* the state after frame hash_next = the snapshot before hash_next + 1 */
        const uint32_t f = n->hash_next + 1;
        const versus_t *s = f == v->frame ? v : n->snap_frame[f % SNAPS] == f ? &n->snaps[f % SNAPS] : NULL;
        if (s) {
            const int k = (int)(n->hash_next / HASH_EVERY % HASHES);
            n->mine[k].frame = n->hash_next;
            n->mine[k].h = versus_hash(s);
            n->mine_latest = n->hash_next;
        }
        n->hash_next += HASH_EVERY;
    }
    if (!n->host) return;
    for (int i = 0; i < GUESTS; i++) {
        guest_t *g = &n->g[i];
        if (g->slot < 0) continue;
        for (int k = 0; k < HASHES; k++)
            if (g->hash[k].frame && g->hash[k].frame == n->mine[k].frame) {
                if (g->hash[k].h != n->mine[k].h) {
                    n->desyncs++;
                    say(n, "P%d desynced at frame %u: resyncing", g->slot + 1, g->hash[k].frame);
                    memset(g->hash, 0, sizeof(g->hash));
                    n->want_start = 1;
                    return;
                }
                g->hash[k].frame = 0;
            }
    }
}

static void send_up(vsnet_t *n, const versus_t *v)
{
    up_t p;
    memset(&p, 0, sizeof(p));
    p.magic = VSN_MAGIC;
    p.epoch = n->epoch;
    p.frame = v->frame;
    p.adv = n->have_peer_frame ? (int32_t)(v->frame - n->peer_frame) : 0;
    p.t_send = clock_ms(n);
    if (n->t_peer) { p.t_echo = n->t_peer; p.t_hold = p.t_send - n->t_peer_at; }
    if (n->mine_latest != UINT32_MAX) {
        const int k = (int)(n->mine_latest / HASH_EVERY % HASHES);
        p.hash_frame = n->mine[k].frame;
        p.hash = n->mine[k].h;
    }
    for (int s = 0; s < VS_MAX; s++) p.ack[s] = n->next[s];
    p.slot = (uint16_t)n->slot;
    p.first = n->host_has;
    uint32_t cnt = n->next[n->slot] - n->host_has;
    if (cnt > MAX_UP) cnt = MAX_UP;
    p.n = (uint16_t)cnt;
    for (uint32_t i = 0; i < cnt; i++) p.in[i] = n->in[n->slot][(p.first + i) % RING];
    udp_send(n, &n->peer, n->peer_len, &p, (int)(offsetof(up_t, in) + 2 * cnt));
}

static void send_down(vsnet_t *n, const versus_t *v, guest_t *g)
{
    down_t p;
    memset(&p, 0, sizeof(p));
    p.magic = VSN_MAGIC;
    p.epoch = n->epoch;
    p.frame = v->frame;
    p.adv = g->have_frame ? (int32_t)(v->frame - g->frame) : 0;
    p.t_send = clock_ms(n);
    if (g->t_peer) { p.t_echo = g->t_peer; p.t_hold = p.t_send - g->t_peer_at; }
    p.ack = n->next[g->slot];
    int o = 0;
    for (int s = 0; s < VS_MAX; s++) {
        if (!(n->owned >> s & 1) || s == g->slot) continue;
        p.first[s] = g->has[s];
        uint32_t cnt = n->next[s] - g->has[s];
        if ((int32_t)cnt < 0) cnt = 0;
        if (cnt > MAX_DOWN) cnt = MAX_DOWN;
        p.n[s] = (uint16_t)cnt;
        for (uint32_t i = 0; i < cnt; i++) p.in[o++] = n->in[s][(p.first[s] + i) % RING];
    }
    udp_send(n, &g->addr, g->addr_len, &p, (int)(offsetof(down_t, in) + 2 * o));
}

static void log_settled(vsnet_t *n, const versus_t *v)
{
    if (!n->log) return;
    const uint32_t e = settled_end(n, v);
    for (; n->logged_next < e; n->logged_next++) {
        const uint32_t f = n->logged_next;
        uint16_t b[VS_MAX];
        int changed = f == n->start;
        for (int s = 0; s < VS_MAX; s++) {
            b[s] = input_at(n, s, f);
            changed |= b[s] != n->logged[s];
            n->logged[s] = b[s];
        }
        if (changed) {
            fprintf(n->log, "%u", f);
            for (int s = 0; s < VS_MAX; s++) fprintf(n->log, " %04x", b[s]);
            fputc('\n', n->log);
        }
    }
    fflush(n->log);
}

/* host: a guest at the door */
static void host_accept(vsnet_t *n, versus_t *v, const uint16_t *rom)
{
    struct sockaddr_storage a;
    socklen_t al = sizeof(a);
    const int fd = accept(n->tcp_listen, (struct sockaddr *)&a, &al);
    if (fd < 0) return;
    const int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    char addr[INET6_ADDRSTRLEN] = "?";
    if (a.ss_family == AF_INET6) inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&a)->sin6_addr, addr, sizeof(addr));
    else inet_ntop(AF_INET, &((struct sockaddr_in *)&a)->sin_addr, addr, sizeof(addr));
    uint32_t h[2];
    hello_t hi;
    welcome_t w = { 1, "" };
    if (!read_timeout(fd, h, sizeof(h), 3000) || h[0] != MSG_HELLO || h[1] != sizeof(hi) ||
        !read_timeout(fd, &hi, sizeof(hi), 3000) || hi.magic != VSN_MAGIC) {
        fprintf(stderr, "vsnet: %s isn't a Batman versus guest\n", addr);
        close(fd);
        return;
    }
    guest_t *g = NULL;
    for (int i = 0; i < GUESTS && !g; i++)
        if (n->g[i].slot < 0) g = &n->g[i];
    int slot = -1;
    if (hi.proto != VSN_PROTO || hi.state_size != sizeof(versus_t))
        snprintf(w.why, sizeof(w.why), "a different build (protocol %u, state %u bytes; host %u, %zu)",
                 hi.proto, hi.state_size, VSN_PROTO, sizeof(versus_t));
    else if (memcmp(hi.id, n->id, 32))
        snprintf(w.why, sizeof(w.why), "a different ROM or mod set (both need the same --mods)");
    else if (!g || (slot = versus_take_slot(v, rom)) < 0)
        snprintf(w.why, sizeof(w.why), "the match is full (a free CPU slot, or the select screen, is needed)");
    w.ok = !w.why[0];
    if (!send_msg(fd, MSG_WELCOME, &w, sizeof(w), NULL, 0) || !w.ok) {
        fprintf(stderr, "vsnet: refused %s: %s\n", addr, w.why);
        if (slot >= 0) versus_release_slot(v, slot);
        close(fd);
        return;
    }
    memset(g, 0, sizeof(*g));
    g->tcp = fd;
    set_nonblock(fd);
    g->slot = slot;
    memcpy(&g->addr, &a, al);
    g->addr_len = al;
    if (a.ss_family == AF_INET6) ((struct sockaddr_in6 *)&g->addr)->sin6_port = htons(hi.udp_port);
    else ((struct sockaddr_in *)&g->addr)->sin_port = htons(hi.udp_port);
    snprintf(g->name, sizeof(g->name), "%s", addr);
    say(n, "P%d joined from %s", slot + 1, addr);
    n->want_start = 1;
}

/* host: guests that closed their connection */
static void host_poll_guests(vsnet_t *n, versus_t *v)
{
    const uint32_t t = clock_ms(n);
    for (int i = 0; i < GUESTS; i++) {
        guest_t *g = &n->g[i];
        if (g->slot < 0) continue;
        uint8_t buf[256];
        const ssize_t k = recv(g->tcp, buf, sizeof(buf), 0);
        if (k == 0 || (k < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            drop_guest(n, g, v, "the connection closed");
        else if (t - g->last_recv > PEER_TIMEOUT_MS)
            drop_guest(n, g, v, "nothing from it for 15 s");
    }
}

/* guest: messages from the host */
static int guest_poll_tcp(vsnet_t *n, versus_t *v)
{
    const uint8_t *b;
    uint32_t len;
    int t;
    while ((t = recv_msg(n, &b, &len)) > 0) {
        if (t == MSG_START && len == sizeof(start_hdr_t) + sizeof(versus_t)) {
            start_hdr_t h;
            memcpy(&h, b, sizeof(h));
            memcpy(v, b + sizeof(h), sizeof(*v));
            n->o.delay = (int)h.delay;
            const int again = n->running;
            begin(n, v, h.epoch, h.owned, (int)h.slot);
            if (again) say(n, "went on with the host's match at frame %u", h.frame);
        } else if (t == MSG_WELCOME && len == sizeof(welcome_t)) {
            welcome_t w;
            memcpy(&w, b, sizeof(w));
            if (!w.ok) {
                w.why[sizeof(w.why) - 1] = 0;
                say(n, "the host refused: %s", w.why);
                n->refused = 1;
                return -1;
            }
        }
    }
    return t < 0 ? -1 : 0;
}

int vsnet_tick(vsnet_t *n, versus_t *v, const uint16_t *rom, uint16_t local, uint32_t end)
{
    udp_flush(n);
    if (n->host) {
        host_accept(n, v, rom);
        host_poll_guests(n, v);
        if (n->want_start || !n->running) host_start(n, v);
    }
    for (;;) {
        union { up_t u; down_t d; } p;
        struct sockaddr_storage from;
        socklen_t fl = sizeof(from);
        const ssize_t k = recvfrom(n->udp, &p, sizeof(p), 0, (struct sockaddr *)&from, &fl);
        if (k < 0) break;
        if (n->host) on_up(n, &p.u, (int)k, &from, fl);
        else on_down(n, &p.d, (int)k);
    }
    if (!n->host) {
        if (!n->gone && guest_poll_tcp(n, v) < 0) { if (!n->refused) say(n, "the host left"); n->gone = 1; }
        if (!n->gone && clock_ms(n) - n->last_recv > PEER_TIMEOUT_MS) {
            say(n, "nothing from the host for %d s", PEER_TIMEOUT_MS / 1000);
            n->gone = 1;
        }
        if (n->gone) return -1;
    }

    /* frames already run whose predictions turned out wrong: again from the first of them */
    uint32_t bad = UINT32_MAX;
    for (int s = 0; s < VS_MAX; s++) {
        if (!remote(n, s)) continue;
        const uint32_t lim = v->frame < n->next[s] ? v->frame : n->next[s];
        for (uint32_t f = n->checked; f < lim; f++)
            if (n->used[s][f % RING] != n->in[s][f % RING]) { if (f < bad) bad = f; break; }
    }
    if (bad != UINT32_MAX) roll_back(n, v, rom, bad);
    {
        const uint32_t e = settled_end(n, v);
        if (e > n->checked) n->checked = e;
    }
    check_hashes(n, v);
    if (n->host && n->want_start) host_start(n, v);
    log_settled(n, v);

    const int me = n->slot;
    if (n->next[me] <= v->frame + (uint32_t)n->o.delay && n->next[me] < end) {
        n->in[me][n->next[me] % RING] = local;
        n->next[me]++;
    }
    if (n->host) {
        for (int i = 0; i < GUESTS; i++)
            if (n->g[i].slot >= 0) send_down(n, v, &n->g[i]);
    } else send_up(n, v);
    udp_flush(n);

    int players = 1, worst_rtt = 0;
    for (int i = 0; i < GUESTS; i++)
        if (n->g[i].slot >= 0) { players++; if (n->g[i].rtt > worst_rtt) worst_rtt = n->g[i].rtt; }
    if (n->host) snprintf(n->status, sizeof(n->status), "HOST P1  %d players  ping %d ms  rollback %d", players, worst_rtt, n->last_roll);
    else snprintf(n->status, sizeof(n->status), "P%d  ping %d ms  rollback %d", me + 1, n->rtt, n->last_roll);

    if (v->frame >= end) return 0;
    if (v->frame >= n->next[me]) return 0;
    if (v->frame >= known_end(n) + MAX_ROLLBACK) {
        n->stalls++;
        snprintf(n->status, sizeof(n->status), "P%d waiting for the other players", me + 1);
        return 0;
    }
    /* keep level: a guest a frame ahead of the host, or the host ahead of its slowest guest, waits
     * a frame now and then (averaged, so that jitter alone doesn't make anyone wait) */
    if (v->frame - n->last_wait >= 20) {
        float ahead = 0;
        if (n->host) {
            for (int i = 0; i < GUESTS; i++) {
                guest_t *g = &n->g[i];
                if (g->slot < 0 || !g->have_frame) continue;
                g->adv_avg = g->adv_avg * 0.9f + (float)(int32_t)(v->frame - g->frame) * 0.1f;
                const float d = g->adv_avg - (float)g->adv;
                if (d > ahead) ahead = d;
            }
        } else if (n->have_peer_frame) {
            n->adv_avg = n->adv_avg * 0.9f + (float)(int32_t)(v->frame - n->peer_frame) * 0.1f;
            ahead = n->adv_avg - n->peer_adv_avg;
        }
        if (ahead >= 2.0f) {
            n->last_wait = v->frame;
            n->waits++;
            return 0;
        }
    }
    run_frame(n, v, rom);
    return 1;
}

int vsnet_settled(const vsnet_t *n, const versus_t *v, uint32_t end)
{
    if (!n->running || v->frame < end || n->checked < end) return 0;
    for (int s = 0; s < VS_MAX; s++)
        if ((n->owned >> s & 1) && n->next[s] < end) return 0;
    if (!n->host) return n->host_has >= end;
    for (int i = 0; i < GUESTS; i++) {
        const guest_t *g = &n->g[i];
        if (g->slot < 0) continue;
        for (int s = 0; s < VS_MAX; s++)
            if ((n->owned >> s & 1) && s != g->slot && g->has[s] < end) return 0;
    }
    return 1;
}

/* --- setting up --- */

static int bind_any(int port, int type)
{
    int fd = socket(AF_INET6, type, 0);
    if (fd >= 0) {
        const int no = 0, yes = 1;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        struct sockaddr_in6 a = { .sin6_family = AF_INET6, .sin6_port = htons((uint16_t)port), .sin6_addr = in6addr_any };
        if (bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0) return fd;
        close(fd);
    }
    fd = socket(AF_INET, type, 0);
    if (fd < 0) return -1;
    const int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = INADDR_ANY };
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0) return fd;
    close(fd);
    return -1;
}

vsnet_t *vsnet_host(int port, const net_opts_t *o, const uint8_t id[32])
{
    vsnet_t *n = alloc_net(o, id);
    if (!n) return NULL;
    n->host = 1;
    n->slot = 0;
    n->tcp_listen = bind_any(port, SOCK_STREAM);
    if (n->tcp_listen < 0 || listen(n->tcp_listen, GUESTS) < 0) {
        fprintf(stderr, "vsnet: can't listen on TCP port %d: %s\n", port, strerror(errno));
        vsnet_close(n);
        return NULL;
    }
    n->udp = bind_any(port, SOCK_DGRAM);
    if (n->udp < 0) {
        fprintf(stderr, "vsnet: can't use UDP port %d: %s\n", port, strerror(errno));
        vsnet_close(n);
        return NULL;
    }
    set_nonblock(n->tcp_listen);
    set_nonblock(n->udp);
    fprintf(stderr, "vsnet: hosting a versus match on port %d (TCP and UDP)\n", port);
    return n;
}

vsnet_t *vsnet_join(const char *addr, int port, const net_opts_t *o, const uint8_t id[32])
{
    vsnet_t *n = alloc_net(o, id);
    if (!n) return NULL;
    char ports[16];
    snprintf(ports, sizeof(ports), "%d", port);
    snprintf(n->peer_name, sizeof(n->peer_name), "%s", addr);
    struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *res = NULL;
    const int e = getaddrinfo(addr, ports, &hints, &res);
    if (e) {
        say(n, "can't find %s: %s", addr, gai_strerror(e));
        return n;
    }
    memcpy(&n->peer, res->ai_addr, res->ai_addrlen);
    n->peer_len = res->ai_addrlen;
    freeaddrinfo(res);
    n->tcp = socket(n->peer.ss_family, SOCK_STREAM, 0);
    n->udp = socket(n->peer.ss_family, SOCK_DGRAM, 0);
    if (n->tcp < 0 || n->udp < 0) { say(n, "no sockets: %s", strerror(errno)); return n; }
    set_nonblock(n->tcp);
    set_nonblock(n->udp);
    if (connect(n->tcp, (struct sockaddr *)&n->peer, n->peer_len) < 0 && errno != EINPROGRESS) {
        say(n, "can't reach %s: %s", addr, strerror(errno));
        return n;
    }
    n->connecting = 1;
    n->connect_by = now_ms() + 4000;
    return n;
}

static int send_hello(vsnet_t *n)
{
    struct sockaddr_storage me;
    socklen_t ml = sizeof(me);
    memset(&me, 0, sizeof(me));
    me.ss_family = n->peer.ss_family;
    if (bind(n->udp, (struct sockaddr *)&me, n->peer.ss_family == AF_INET6 ? sizeof(struct sockaddr_in6)
                                                                          : sizeof(struct sockaddr_in)) < 0 ||
        getsockname(n->udp, (struct sockaddr *)&me, &ml) < 0) {
        say(n, "UDP: %s", strerror(errno));
        return 0;
    }
    const int yes = 1;
    setsockopt(n->tcp, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    hello_t hi = { VSN_MAGIC, VSN_PROTO, sizeof(versus_t), 0, {0}, 0, 0 };
    memcpy(hi.id, n->id, 32);
    hi.udp_port = ntohs(me.ss_family == AF_INET6 ? ((struct sockaddr_in6 *)&me)->sin6_port
                                                 : ((struct sockaddr_in *)&me)->sin_port);
    if (!send_msg(n->tcp, MSG_HELLO, &hi, sizeof(hi), NULL, 0)) {
        say(n, "%s closed the connection", n->peer_name);
        return 0;
    }
    return 1;
}

int vsnet_wait_start(vsnet_t *n, versus_t *v)
{
    if (n->running) return 1;
    if (n->tcp < 0 || n->gone) { n->gone = 1; return -1; }
    if (n->connecting) {
        struct pollfd pf = { .fd = n->tcp, .events = POLLOUT };
        if (poll(&pf, 1, 0) == 0) {
            if (now_ms() < n->connect_by) return 0;
            say(n, "no answer from %s (is the VPN on?)", n->peer_name);
            n->gone = 1;
            return -1;
        }
        int err = 0;
        socklen_t el = sizeof(err);
        getsockopt(n->tcp, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) {
            if (err == ECONNREFUSED) say(n, "nobody is hosting at %s yet", n->peer_name);
            else say(n, "can't reach %s: %s", n->peer_name, strerror(err));
            n->gone = 1;
            return -1;
        }
        n->connecting = 0;
        if (!send_hello(n)) { n->gone = 1; return -1; }
        n->connected = 1;
    }
    if (guest_poll_tcp(n, v) < 0) {
        if (!n->news) say(n, "the host closed the connection");
        n->gone = 1;
        return -1;
    }
    if (n->running) say(n, "joined as P%d at frame %u (input delay %d)", n->slot + 1, n->start, n->o.delay);
    return n->running;
}

int vsnet_refused(const vsnet_t *n) { return n->refused; }
const char *vsnet_reason(const vsnet_t *n) { return n->why; }
int vsnet_slot(const vsnet_t *n) { return n->running ? n->slot : -1; }
int vsnet_is_host(const vsnet_t *n) { return n->host; }
uint32_t vsnet_input_frame(const vsnet_t *n) { return n->slot >= 0 ? n->next[n->slot] : 0; }
const char *vsnet_status(const vsnet_t *n) { return n->status[0] ? n->status : n->why; }
void vsnet_log_inputs(vsnet_t *n, FILE *f) { n->log = f; }

int vsnet_players(const vsnet_t *n)
{
    int k = 0;
    for (int s = 0; s < VS_MAX; s++) k += n->owned >> s & 1;
    return k;
}

const char *vsnet_event(vsnet_t *n)
{
    if (!n->news) return NULL;
    n->news = 0;
    return n->why;
}

void vsnet_close(vsnet_t *n)
{
    if (!n) return;
    if (n->running)
        fprintf(stderr, "vsnet: %d rollbacks (%d frames run again), %d waits to keep level, %d stalls, %u desyncs\n",
                n->rollbacks, n->rolled, n->waits, n->stalls, n->desyncs);
    for (int i = 0; i < GUESTS; i++)
        if (n->g[i].tcp >= 0) close(n->g[i].tcp);
    if (n->tcp >= 0) close(n->tcp);
    if (n->tcp_listen >= 0) close(n->tcp_listen);
    if (n->udp >= 0) close(n->udp);
    free(n->rx);
    free(n);
}
