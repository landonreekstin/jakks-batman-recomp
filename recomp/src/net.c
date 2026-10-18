/* Netplay: see net.h. */
#include "net.h"

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

#include "state.h"

#define NET_MAGIC 0x4E544142u  /* "BATN" */
#define NET_PROTO 1
#define RING 256               /* input history, frames */
#define MAX_ROLLBACK 12        /* frames run past the peer's last known input before waiting */
#define SNAPS (MAX_ROLLBACK + 4)
#define HASH_EVERY 60          /* a settled frame's hash, once a second */
#define HASHES 32
#define MAX_SEND 64            /* inputs per packet */
#define PEER_TIMEOUT_MS 15000
#define SIM_QUEUE 512

enum { MSG_HELLO = 1, MSG_WELCOME, MSG_START };

typedef struct hello {
    uint32_t magic, proto, snap_size, coop;
    uint8_t id[32];
    uint16_t udp_port, pad;
} hello_t;

typedef struct welcome {
    uint32_t ok;
    char why[124];
} welcome_t;

typedef struct start_hdr {
    uint32_t epoch, frame, delay, pad;
} start_hdr_t;

/* the UDP packet (host byte order: both ends are x86) */
typedef struct __attribute__((packed)) pkt {
    uint32_t magic, epoch;
    uint32_t frame;          /* the sender's next frame to run */
    int32_t adv;             /* the sender's frame advantage: frame - the receiver's last frame it saw */
    uint32_t ack;            /* the sender has the receiver's inputs for every frame before this */
    uint32_t t_send, t_echo, t_hold;   /* ms: the sender's clock, the last of ours it saw, and how long ago */
    uint32_t hash_frame;     /* the sender's latest settled-frame hash (frame 0: none) */
    uint64_t hash;
    uint32_t first;          /* the inputs, for frames first.. */
    uint16_t n;
    uint16_t in[MAX_SEND];
} pkt_t;

struct netplay {
    int host;
    int tcp_listen, tcp, udp;
    struct sockaddr_storage peer;
    socklen_t peer_len;
    net_opts_t o;
    uint64_t t0;
    char status[160], why[128];
    int news;                   /* why holds a message not yet taken by net_event */
    int gone;

    uint8_t *rx;                /* TCP messages: type, length, payload */
    size_t rx_len, rx_cap;

    /* the session: from the frame `start` on, both machines run the same frames */
    int running;
    uint32_t epoch, start;
    uint32_t local_next;        /* first frame with no local input yet */
    uint32_t remote_next;       /* first frame with no input from the peer (all before it are here) */
    uint32_t peer_has;          /* the peer has our inputs for every frame before this */
    uint32_t checked;           /* the frames before this ran with the peer's real input */
    uint16_t local_in[RING], remote_in[RING], used[RING];   /* used: the peer's input a frame ran with */
    spg_snap_t *snaps;          /* the state before frame f, in snaps[f % SNAPS] */
    uint32_t snap_frame[SNAPS];
    spg_snap_t *scratch;

    uint32_t peer_frame;
    int32_t peer_adv;
    float adv_avg, peer_adv_avg;   /* smoothed: jitter alone mustn't make anyone wait */
    int have_peer_frame;
    uint32_t last_wait;
    uint32_t t_peer, t_peer_at, last_recv;
    int rtt;
    int rollbacks, rolled, last_roll, waits, stalls;

    uint32_t hash_next;
    struct { uint32_t frame; uint64_t h; } mine[HASHES], theirs[HASHES];
    uint32_t mine_latest;
    uint32_t desyncs, desync_frame;

    FILE *log;
    uint32_t logged_next;
    uint16_t logged[2];

    struct { uint64_t at; uint8_t data[sizeof(pkt_t)]; int len; } q[SIM_QUEUE];
    int qn;
    uint64_t rng;
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint32_t clock_ms(const netplay_t *n)
{
    return (uint32_t)(now_ms() - n->t0) + 1; /* 0 means "none" in packets */
}

static void say(netplay_t *n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(n->why, sizeof(n->why), fmt, ap);
    va_end(ap);
    n->news = 1;
    fprintf(stderr, "net: %s\n", n->why);
}

static netplay_t *alloc_net(const net_opts_t *o)
{
    netplay_t *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->tcp_listen = n->tcp = n->udp = -1;
    n->o = *o;
    if (n->o.delay < 0) n->o.delay = 0;
    if (n->o.delay > 8) n->o.delay = 8;
    n->t0 = now_ms();
    n->rng = n->t0 * 0x9E3779B97F4A7C15ull + (uint64_t)getpid();
    n->snaps = calloc(SNAPS, sizeof(spg_snap_t));
    n->scratch = calloc(1, sizeof(spg_snap_t));
    if (!n->snaps || !n->scratch) { free(n->snaps); free(n->scratch); free(n); return NULL; }
    return n;
}

static void set_nonblock(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

/* --- TCP messages --- */

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

/* reads what has arrived; returns a whole message's type (payload in *body, *len) or 0, and -1
 * when the connection is closed */
static int recv_msg(netplay_t *n, const uint8_t **body, uint32_t *len)
{
    if (n->rx_len >= 8) {   /* drop the message returned last time */
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
            if (h[1] > sizeof(spg_snap_t) + 4096) return -1;
            if (n->rx_len >= 8 + (size_t)h[1]) {
                h[0] |= 0x80000000u;   /* marks it as returned */
                memcpy(n->rx, h, 4);
                *body = n->rx + 8;
                *len = h[1];
                return (int)(h[0] & 0x7FFFFFFFu);
            }
        }
        if (n->rx_cap - n->rx_len < 65536) {
            n->rx_cap = n->rx_cap ? n->rx_cap * 2 : sizeof(spg_snap_t) + 8192;
            n->rx = realloc(n->rx, n->rx_cap);
        }
        const ssize_t k = recv(n->tcp, n->rx + n->rx_len, n->rx_cap - n->rx_len, 0);
        if (k == 0) return -1;
        if (k < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
        n->rx_len += (size_t)k;
    }
}

/* --- UDP, through the simulated network when one is set --- */

static uint64_t rnd(netplay_t *n)
{
    n->rng ^= n->rng << 13;
    n->rng ^= n->rng >> 7;
    n->rng ^= n->rng << 17;
    return n->rng;
}

static void udp_out(netplay_t *n, const void *p, int len)
{
    sendto(n->udp, p, (size_t)len, 0, (const struct sockaddr *)&n->peer, n->peer_len);
}

static void udp_send(netplay_t *n, const pkt_t *p, int len)
{
    if (!n->o.lat_ms && !n->o.jitter_ms && !n->o.loss_pct) { udp_out(n, p, len); return; }
    if ((int)(rnd(n) % 100) < n->o.loss_pct || n->qn == SIM_QUEUE) return;
    const uint64_t at = now_ms() + (uint64_t)n->o.lat_ms +
                        (n->o.jitter_ms ? rnd(n) % (uint64_t)(n->o.jitter_ms + 1) : 0);
    n->q[n->qn].at = at;
    memcpy(n->q[n->qn].data, p, (size_t)len);
    n->q[n->qn++].len = len;
}

static void udp_flush(netplay_t *n)
{
    const uint64_t t = now_ms();
    for (int i = 0; i < n->qn;)
        if (n->q[i].at <= t) {
            udp_out(n, n->q[i].data, n->q[i].len);
            n->q[i] = n->q[--n->qn];   /* reordering is part of the simulation */
        } else i++;
}

/* --- the session --- */

static uint16_t remote_at(const netplay_t *n, uint32_t f)
{
    return f < n->remote_next ? n->remote_in[f % RING] : n->remote_in[(n->remote_next - 1) % RING];
}

static void begin(netplay_t *n, spg_t *m, uint32_t epoch)
{
    const uint32_t s = m->frame, d = (uint32_t)n->o.delay;
    n->running = 1;
    n->epoch = epoch;
    n->start = s;
    n->local_next = n->remote_next = n->peer_has = s + d;
    for (uint32_t f = s; f < s + d + 1; f++)
        n->local_in[f % RING] = n->remote_in[f % RING] = n->used[f % RING] = 0;
    if (!d) n->remote_in[(s + RING - 1) % RING] = 0;   /* the prediction before any input */
    n->checked = s;
    for (int i = 0; i < SNAPS; i++) n->snap_frame[i] = UINT32_MAX;
    n->have_peer_frame = 0;
    n->adv_avg = n->peer_adv_avg = 0;
    n->hash_next = (s + HASH_EVERY - 1) / HASH_EVERY * HASH_EVERY;
    if (n->hash_next == 0) n->hash_next = HASH_EVERY;
    memset(n->mine, 0, sizeof(n->mine));
    memset(n->theirs, 0, sizeof(n->theirs));
    n->mine_latest = UINT32_MAX;
    n->logged_next = s;
    n->last_recv = clock_ms(n);
    n->qn = 0;
}

/* host: the game goes on from m's state on both machines (at a join, and to resync) */
static int host_start(netplay_t *n, spg_t *m)
{
    m->buttons = 0;        /* the first frames' inputs are 0 on both sides (the input delay) */
    m->coop.buttons2 = 0;
    spg_snapshot(m, n->scratch);
    const start_hdr_t h = { n->epoch + 1, m->frame, (uint32_t)n->o.delay, 0 };
    if (!send_msg(n->tcp, MSG_START, &h, sizeof(h), n->scratch, sizeof(spg_snap_t))) {
        say(n, "the guest's connection failed");
        n->gone = 1;
        return 0;
    }
    begin(n, m, h.epoch);
    return 1;
}

static void set_inputs(const netplay_t *n, spg_t *m, uint32_t f)
{
    const uint16_t mine = n->local_in[f % RING], theirs = remote_at(n, f);
    m->buttons = n->host ? mine : theirs;
    m->coop.buttons2 = n->host ? theirs : mine;
}

static void run_frame(netplay_t *n, spg_t *m, int render, int audio, net_run_fn run, void *ctx)
{
    const uint32_t f = m->frame;
    spg_snapshot(m, &n->snaps[f % SNAPS]);
    n->snap_frame[f % SNAPS] = f;
    set_inputs(n, m, f);
    n->used[f % RING] = remote_at(n, f);
    run(ctx, m, render, audio);
}

/* runs the frames from `from` again, with the inputs as now known */
static void roll_back(netplay_t *n, spg_t *m, uint32_t from, net_run_fn run, void *ctx)
{
    const uint32_t to = m->frame;
    if (n->snap_frame[from % SNAPS] != from) {   /* can't happen: the wait keeps it in range */
        say(n, "no snapshot for frame %u", from);
        return;
    }
    const int render = m->render;
    m->render = 0;
    spg_restore(m, &n->snaps[from % SNAPS]);
    m->render = render;
    while (m->frame < to)
        run_frame(n, m, m->frame + 1 == to, 0, run, ctx);
    n->rollbacks++;
    n->rolled += (int)(to - from);
    n->last_roll = (int)(to - from);
}

static void on_packet(netplay_t *n, const pkt_t *p, int len)
{
    if (len < (int)offsetof(pkt_t, in) || p->magic != NET_MAGIC || p->epoch != n->epoch) return;
    if (p->n > MAX_SEND || len < (int)(offsetof(pkt_t, in) + 2 * p->n)) return;
    const uint32_t t = clock_ms(n);
    n->last_recv = t;
    if (!n->have_peer_frame || (int32_t)(p->frame - n->peer_frame) >= 0) {
        n->peer_frame = p->frame;
        n->peer_adv = p->adv;
        n->peer_adv_avg = n->have_peer_frame ? n->peer_adv_avg * 0.9f + (float)p->adv * 0.1f : (float)p->adv;
        n->have_peer_frame = 1;
    }
    if ((int32_t)(p->ack - n->peer_has) > 0) n->peer_has = p->ack;
    if (p->t_send) { n->t_peer = p->t_send; n->t_peer_at = t; }
    if (p->t_echo) {
        const int rtt = (int)(t - p->t_echo) - (int)p->t_hold;
        if (rtt >= 0) n->rtt = n->rtt ? (n->rtt * 7 + rtt) / 8 : rtt;
    }
    for (uint32_t i = 0; i < p->n; i++) {
        const uint32_t f = p->first + i;
        if (f != n->remote_next) continue;   /* old, or after a gap (a later packet repeats it) */
        if ((int32_t)(f - n->local_next) > RING / 2) break;   /* would overrun the ring */
        n->remote_in[f % RING] = p->in[i];
        n->remote_next++;
    }
    if (p->hash_frame) {
        const int k = (int)(p->hash_frame / HASH_EVERY % HASHES);
        n->theirs[k].frame = p->hash_frame;
        n->theirs[k].h = p->hash;
    }
}

/* frames before min(remote_next, local_next, m->frame) are settled: their states are final */
static uint32_t settled_end(const netplay_t *n, const spg_t *m)
{
    uint32_t e = n->remote_next < n->local_next ? n->remote_next : n->local_next;
    return e < m->frame ? e : m->frame;
}

static void check_hashes(netplay_t *n, spg_t *m)
{
    const uint32_t e = settled_end(n, m);
    while (n->hash_next < e) {   /* the state after frame hash_next is the one before hash_next + 1 */
        const uint32_t f = n->hash_next + 1;
        const spg_snap_t *s = NULL;
        if (f == m->frame) {
            spg_snapshot(m, n->scratch);
            s = n->scratch;
        } else if (n->snap_frame[f % SNAPS] == f) s = &n->snaps[f % SNAPS];
        if (s) {
            const int k = (int)(n->hash_next / HASH_EVERY % HASHES);
            n->mine[k].frame = n->hash_next;
            n->mine[k].h = spg_snap_hash(s);
            n->mine_latest = n->hash_next;
        }
        n->hash_next += HASH_EVERY;
    }
    for (int k = 0; k < HASHES; k++)
        if (n->theirs[k].frame && n->theirs[k].frame == n->mine[k].frame) {
            if (n->theirs[k].h != n->mine[k].h) {
                n->desyncs++;
                n->desync_frame = n->theirs[k].frame;
                say(n, "desync at frame %u%s", n->desync_frame, n->host ? ": resyncing the guest" : "");
                memset(n->theirs, 0, sizeof(n->theirs));
                if (n->host) host_start(n, m);
                return;
            }
            n->theirs[k].frame = 0;   /* checked */
        }
}

static void send_inputs(netplay_t *n, const spg_t *m)
{
    pkt_t p;
    memset(&p, 0, sizeof(p));
    p.magic = NET_MAGIC;
    p.epoch = n->epoch;
    p.frame = m->frame;
    p.adv = n->have_peer_frame ? (int32_t)(m->frame - n->peer_frame) : 0;
    p.ack = n->remote_next;
    p.t_send = clock_ms(n);
    if (n->t_peer) {
        p.t_echo = n->t_peer;
        p.t_hold = p.t_send - n->t_peer_at;
    }
    if (n->mine_latest != UINT32_MAX) {
        const int k = (int)(n->mine_latest / HASH_EVERY % HASHES);
        p.hash_frame = n->mine[k].frame;
        p.hash = n->mine[k].h;
    }
    p.first = n->peer_has;
    uint32_t cnt = n->local_next - n->peer_has;
    if (cnt > MAX_SEND) cnt = MAX_SEND;
    p.n = (uint16_t)cnt;
    for (uint32_t i = 0; i < cnt; i++) p.in[i] = n->local_in[(p.first + i) % RING];
    udp_send(n, &p, (int)(offsetof(pkt_t, in) + 2 * cnt));
}

static void log_settled(netplay_t *n, const spg_t *m)
{
    if (!n->log) return;
    const uint32_t e = settled_end(n, m);
    for (; n->logged_next < e; n->logged_next++) {
        const uint32_t f = n->logged_next;
        const uint16_t mine = n->local_in[f % RING], theirs = n->remote_in[f % RING];
        const uint16_t p1 = n->host ? mine : theirs, p2 = n->host ? theirs : mine;
        if (f == n->start || p1 != n->logged[0] || p2 != n->logged[1])
            fprintf(n->log, "%u %04x %04x\n", f, p1, p2);
        n->logged[0] = p1;
        n->logged[1] = p2;
    }
    fflush(n->log);
}

static int tcp_poll(netplay_t *n, spg_t *m)
{
    const uint8_t *b;
    uint32_t len;
    int t;
    while ((t = recv_msg(n, &b, &len)) > 0) {
        if (t == MSG_START && !n->host && len == sizeof(start_hdr_t) + sizeof(spg_snap_t)) {
            start_hdr_t h;
            memcpy(&h, b, sizeof(h));
            memcpy(n->scratch, b + sizeof(h), sizeof(spg_snap_t));
            const int render = m->render;
            spg_restore(m, n->scratch);
            m->render = render;
            n->o.delay = (int)h.delay;
            begin(n, m, h.epoch);
            if (h.epoch > 1) say(n, "resynced with the host at frame %u", h.frame);
        } else if (t == MSG_WELCOME && len == sizeof(welcome_t)) {
            welcome_t w;
            memcpy(&w, b, sizeof(w));
            if (!w.ok) {
                w.why[sizeof(w.why) - 1] = 0;
                say(n, "the host refused: %s", w.why);
                return -1;
            }
        }
    }
    return t < 0 ? -1 : 0;
}

int net_tick(netplay_t *n, spg_t *m, uint16_t local, uint32_t end, net_run_fn run, void *ctx)
{
    udp_flush(n);
    for (;;) {
        pkt_t p;
        struct sockaddr_storage from;
        socklen_t fl = sizeof(from);
        const ssize_t k = recvfrom(n->udp, &p, sizeof(p), 0, (struct sockaddr *)&from, &fl);
        if (k < 0) break;
        if (k >= 8 && p.magic == NET_MAGIC && p.epoch == n->epoch) {   /* follow the peer's address */
            memcpy(&n->peer, &from, fl);
            n->peer_len = fl;
        }
        on_packet(n, &p, (int)k);
    }
    if (!n->gone && tcp_poll(n, m) < 0) {
        say(n, n->host ? "player 2 left" : "the host left");
        n->gone = 1;
    }
    if (!n->gone && clock_ms(n) - n->last_recv > PEER_TIMEOUT_MS) {
        say(n, "nothing from the peer for %d s", PEER_TIMEOUT_MS / 1000);
        n->gone = 1;
    }

    /* the frames already run whose prediction turned out wrong: from the first, again */
    const uint32_t lim = m->frame < n->remote_next ? m->frame : n->remote_next;
    while (n->checked < lim && n->used[n->checked % RING] == n->remote_in[n->checked % RING]) n->checked++;
    if (n->checked < lim) {
        roll_back(n, m, n->checked, run, ctx);
        n->checked = lim;
    }
    check_hashes(n, m);
    log_settled(n, m);
    if (n->gone) return -1;

    if (n->local_next <= m->frame + (uint32_t)n->o.delay && (!end || n->local_next < end)) {
        n->local_in[n->local_next % RING] = local;
        n->local_next++;
    }
    send_inputs(n, m);
    udp_flush(n);

    snprintf(n->status, sizeof(n->status), "%s ping %d ms  delay %d  rollback %d",
             n->host ? "P1" : "P2", n->rtt, n->o.delay, n->last_roll);
    if (end && m->frame >= end) return 0;
    if (m->frame >= n->local_next) return 0;
    if (m->frame >= n->remote_next + MAX_ROLLBACK) {   /* too far ahead of what the peer sent */
        n->stalls++;
        snprintf(n->status, sizeof(n->status), "%s waiting for %s (ping %d ms)", n->host ? "P1" : "P2",
                 n->host ? "player 2" : "the host", n->rtt);
        return 0;
    }
    /* keep the two machines level: both see the other behind by the one-way latency, so a
     * difference of 2 is one frame ahead (averaged over about 10 frames) */
    if (n->have_peer_frame) {
        const int32_t adv = (int32_t)(m->frame - n->peer_frame);
        n->adv_avg = n->adv_avg * 0.9f + (float)adv * 0.1f;
        if (n->adv_avg - n->peer_adv_avg >= 2.0f && m->frame - n->last_wait >= 20) {
            n->last_wait = m->frame;
            n->waits++;
            return 0;
        }
    }
    run_frame(n, m, 1, 1, run, ctx);
    return 1;
}

int net_settled(const netplay_t *n, const spg_t *m, uint32_t end)
{
    return n->running && m->frame >= end && n->remote_next >= end && n->local_next >= end &&
           n->peer_has >= end && n->checked >= end;
}

/* --- setting up --- */

static int bind_any(int port, int type, int *family)
{
    int fd = socket(AF_INET6, type, 0);
    if (fd >= 0) {
        const int no = 0, yes = 1;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        struct sockaddr_in6 a = { .sin6_family = AF_INET6, .sin6_port = htons((uint16_t)port), .sin6_addr = in6addr_any };
        if (bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0) { *family = AF_INET6; return fd; }
        close(fd);
    }
    fd = socket(AF_INET, type, 0);
    if (fd < 0) return -1;
    const int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = INADDR_ANY };
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0) { *family = AF_INET; return fd; }
    close(fd);
    return -1;
}

netplay_t *net_host(int port, const net_opts_t *o)
{
    netplay_t *n = alloc_net(o);
    if (!n) return NULL;
    n->host = 1;
    int fam;
    n->tcp_listen = bind_any(port, SOCK_STREAM, &fam);
    if (n->tcp_listen < 0 || listen(n->tcp_listen, 1) < 0) {
        fprintf(stderr, "net: can't listen on TCP port %d: %s\n", port, strerror(errno));
        net_close(n);
        return NULL;
    }
    n->udp = bind_any(port, SOCK_DGRAM, &fam);
    if (n->udp < 0) {
        fprintf(stderr, "net: can't use UDP port %d: %s\n", port, strerror(errno));
        net_close(n);
        return NULL;
    }
    set_nonblock(n->tcp_listen);
    set_nonblock(n->udp);
    fprintf(stderr, "net: waiting for player 2 on port %d (TCP and UDP)\n", port);
    return n;
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

int net_accept(netplay_t *n, spg_t *m, const uint8_t id[32])
{
    if (n->running) return 1;
    struct sockaddr_storage a;
    socklen_t al = sizeof(a);
    const int fd = accept(n->tcp_listen, (struct sockaddr *)&a, &al);
    if (fd < 0) return 0;
    const int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    char addr[INET6_ADDRSTRLEN] = "?";
    if (a.ss_family == AF_INET6) inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&a)->sin6_addr, addr, sizeof(addr));
    else inet_ntop(AF_INET, &((struct sockaddr_in *)&a)->sin_addr, addr, sizeof(addr));

    uint32_t h[2];
    hello_t hi;
    welcome_t w = { 1, "" };
    if (!read_timeout(fd, h, sizeof(h), 3000) || h[0] != MSG_HELLO || h[1] != sizeof(hi) ||
        !read_timeout(fd, &hi, sizeof(hi), 3000) || hi.magic != NET_MAGIC) {
        fprintf(stderr, "net: %s isn't a Batman guest\n", addr);
        close(fd);
        return 0;
    }
    if (hi.proto != NET_PROTO || hi.snap_size != sizeof(spg_snap_t))
        snprintf(w.why, sizeof(w.why), "a different build (protocol %u, state %u bytes; host %u, %zu)",
                 hi.proto, hi.snap_size, NET_PROTO, sizeof(spg_snap_t));
    else if (memcmp(hi.id, id, 32))
        snprintf(w.why, sizeof(w.why), "a different ROM or mod set (both need the same --mods)");
    else if (!hi.coop || !m->coop.enabled)
        snprintf(w.why, sizeof(w.why), "co-op is off");
    w.ok = !w.why[0];
    if (!send_msg(fd, MSG_WELCOME, &w, sizeof(w), NULL, 0) || !w.ok) {
        fprintf(stderr, "net: refused %s: %s\n", addr, w.why);
        close(fd);
        return 0;
    }
    n->tcp = fd;
    set_nonblock(fd);
    close(n->tcp_listen);   /* one guest */
    n->tcp_listen = -1;
    memcpy(&n->peer, &a, al);
    n->peer_len = al;
    if (a.ss_family == AF_INET6) ((struct sockaddr_in6 *)&n->peer)->sin6_port = htons(hi.udp_port);
    else ((struct sockaddr_in *)&n->peer)->sin_port = htons(hi.udp_port);
    say(n, "player 2 joined from %s", addr);
    return host_start(n, m) ? 1 : -1;
}

netplay_t *net_join(const char *addr, int port, const net_opts_t *o, const uint8_t id[32])
{
    netplay_t *n = alloc_net(o);
    if (!n) return NULL;
    char ports[16];
    snprintf(ports, sizeof(ports), "%d", port);
    struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *res = NULL, *ai;
    const int e = getaddrinfo(addr, ports, &hints, &res);
    if (e) {
        fprintf(stderr, "net: %s: %s\n", addr, gai_strerror(e));
        net_close(n);
        return NULL;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        n->tcp = socket(ai->ai_family, SOCK_STREAM, 0);
        if (n->tcp < 0) continue;
        if (connect(n->tcp, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(n->tcp);
        n->tcp = -1;
    }
    if (!ai) {
        fprintf(stderr, "net: can't connect to %s port %d: %s\n", addr, port, strerror(errno));
        freeaddrinfo(res);
        net_close(n);
        return NULL;
    }
    memcpy(&n->peer, ai->ai_addr, ai->ai_addrlen);
    n->peer_len = ai->ai_addrlen;
    n->udp = socket(ai->ai_family, SOCK_DGRAM, 0);
    freeaddrinfo(res);
    struct sockaddr_storage me;
    socklen_t ml = sizeof(me);
    memset(&me, 0, sizeof(me));
    me.ss_family = n->peer.ss_family;
    if (n->udp < 0 || bind(n->udp, (struct sockaddr *)&me, n->peer.ss_family == AF_INET6 ?
                           sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in)) < 0 ||
        getsockname(n->udp, (struct sockaddr *)&me, &ml) < 0) {
        fprintf(stderr, "net: UDP: %s\n", strerror(errno));
        net_close(n);
        return NULL;
    }
    const int yes = 1;
    setsockopt(n->tcp, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    hello_t hi = { NET_MAGIC, NET_PROTO, sizeof(spg_snap_t), 1, {0}, 0, 0 };
    memcpy(hi.id, id, 32);
    hi.udp_port = ntohs(me.ss_family == AF_INET6 ? ((struct sockaddr_in6 *)&me)->sin6_port
                                                 : ((struct sockaddr_in *)&me)->sin_port);
    if (!send_msg(n->tcp, MSG_HELLO, &hi, sizeof(hi), NULL, 0)) {
        fprintf(stderr, "net: the host closed the connection\n");
        net_close(n);
        return NULL;
    }
    set_nonblock(n->tcp);
    set_nonblock(n->udp);
    fprintf(stderr, "net: connected to %s port %d, waiting for the game\n", addr, port);
    return n;
}

int net_wait_start(netplay_t *n, spg_t *m)
{
    if (n->running) return 1;
    if (tcp_poll(n, m) < 0) {
        if (!n->why[0]) say(n, "the host closed the connection");
        n->gone = 1;
        return -1;
    }
    if (n->running) say(n, "joined at frame %u (input delay %d)", n->start, n->o.delay);
    return n->running;
}

const char *net_status(const netplay_t *n)
{
    return n->status[0] ? n->status : n->why;
}

uint32_t net_input_frame(const netplay_t *n)
{
    return n->local_next;
}

const char *net_event(netplay_t *n)
{
    if (!n->news) return NULL;
    n->news = 0;
    return n->why;
}

int net_is_host(const netplay_t *n)
{
    return n->host;
}

uint32_t net_epoch(const netplay_t *n)
{
    return n->epoch;
}

void net_log_inputs(netplay_t *n, FILE *f)
{
    n->log = f;
}

void net_close(netplay_t *n)
{
    if (!n) return;
    if (n->running || n->rollbacks)
        fprintf(stderr, "net: %d rollbacks (%d frames run again), %d waits to keep level, %d stalls, "
                        "%u desyncs, ping %d ms\n", n->rollbacks, n->rolled, n->waits, n->stalls, n->desyncs, n->rtt);
    if (n->tcp >= 0) close(n->tcp);
    if (n->tcp_listen >= 0) close(n->tcp_listen);
    if (n->udp >= 0) close(n->udp);
    free(n->rx);
    free(n->snaps);
    free(n->scratch);
    free(n);
}
