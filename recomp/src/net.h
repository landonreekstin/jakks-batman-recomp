/* Netplay: co-op over the network (notes/netplay.md).
 *
 * Both machines run the whole game. Only the controllers travel: the host is P1, the guest P2.
 * The game is deterministic (it matches MAME, and save states continue exactly), so equal inputs
 * give equal games. Rollback keeps the local player's input immediate: the peer's input is
 * predicted (its last one), and when the real one turns out different, the machine goes back to
 * a snapshot of that frame and runs forward again.
 *
 *   - TCP (the port given) carries the handshake and whole states: the start, and a resync after
 *     a desync. UDP (the same port number) carries the inputs, each packet repeating every input
 *     the peer hasn't acknowledged yet, so a lost packet costs nothing.
 *   - The guest starts from the host's state (EEPROM included), so it can join at any time.
 *   - Every second both send a hash of a settled frame; the host resyncs the guest if they differ.
 *
 * POSIX sockets; both ends must be little-endian (x86), and run the same build, ROM and mods. */
#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stdio.h>

#include "spg.h"

#define NET_PORT 7845

typedef struct net_opts {
    int delay;                          /* input delay, frames (2) */
    int lat_ms, jitter_ms, loss_pct;    /* a simulated network on the sending side, for tests */
} net_opts_t;

typedef struct netplay netplay_t;

/* runs one frame of m with the inputs net has set: render it, play its sound */
typedef void (*net_run_fn)(void *ctx, spg_t *m, int render, int audio);

/* the host listens on port (TCP and UDP); NULL on failure (printed) */
netplay_t *net_host(int port, const net_opts_t *o);
/* the guest starts connecting to addr:port (id: mods_content_id); net_wait_start goes on with it.
 * Never NULL unless out of memory: a failure shows up there. */
netplay_t *net_join(const char *addr, int port, const net_opts_t *o, const uint8_t id[32]);

/* host, every frame until it returns 1: a guest has joined, and the game goes on from m's current
 * state on both machines. 0: nobody yet. */
int net_accept(netplay_t *n, spg_t *m, const uint8_t id[32]);
/* guest, every frame until it returns 1: the host's state is in m. -1: refused or gone
 * (net_status says why) */
int net_wait_start(netplay_t *n, spg_t *m);
/* guest: the host answered (so a failure after it is the host's refusal or its leaving) */
int net_connected(const netplay_t *n);
/* the latest event's text (why a connection failed or ended) */
const char *net_reason(const netplay_t *n);
/* guest: the host refused us (a different build, ROM or mod set): no use trying again */
int net_refused(const netplay_t *n);

/* one tick (1/60 s): sends and receives, rolls back where a prediction was wrong, and runs the
 * next frame unless it has to wait for the peer. local: this player's buttons (port A layout).
 * Returns the frames advanced (0 or 1), or -1 once the peer has gone. Frames from `end` on are
 * not run (0: no end). */
int net_tick(netplay_t *n, spg_t *m, uint16_t local, uint32_t end, net_run_fn run, void *ctx);
/* every frame before end has both players' real inputs, is run with them, and the peer has them */
int net_settled(const netplay_t *n, const spg_t *m, uint32_t end);

/* one line for the screen: ping, rollback, waiting, desyncs */
const char *net_status(const netplay_t *n);
/* the latest event (joined, left, desync, resync), once; NULL when there is none */
const char *net_event(netplay_t *n);
int net_is_host(const netplay_t *n);
/* the frame the next local input given to net_tick is for (now + the input delay) */
uint32_t net_input_frame(const netplay_t *n);
/* writes the settled inputs as an input script ("frame p1 p2"), from the start on */
void net_log_inputs(netplay_t *n, FILE *f);
/* a counter of the session (re)starts, so a front end can notice a resync */
uint32_t net_epoch(const netplay_t *n);
void net_close(netplay_t *n);

#endif
