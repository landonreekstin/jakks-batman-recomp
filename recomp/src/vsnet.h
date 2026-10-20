/* Versus netplay: up to 8 machines in one match (notes/versus.md).
 *
 * Like the co-op netplay (net.h), every machine runs the whole match and only controller bits
 * travel, with rollback hiding the latency. The match state is a versus_t (versus.h), which is
 * small, so rollback snapshots and resyncs are cheap. It is a star:
 *   - the host plays slot 0 and listens (TCP for the handshake and whole states, UDP on the same
 *     port number for inputs). Each guest gets the first CPU slot of the match;
 *   - a guest sends its own inputs to the host; the host sends each guest every other slot's
 *     inputs. Each packet repeats what the receiver hasn't acknowledged, so losses cost nothing;
 *   - CPU slots need nothing: the bots are part of the deterministic step;
 *   - a join, a leave or a desync (found by a hash of a settled frame every second) makes the host
 *     send its current state to every guest, and everyone goes on from there.
 * One human per machine. POSIX sockets, little-endian machines, the same build, ROM and mods. */
#ifndef VSNET_H
#define VSNET_H

#include <stdint.h>
#include <stdio.h>

#include "net.h"      /* net_opts_t, NET_PORT */
#include "versus.h"

typedef struct vsnet vsnet_t;

/* host: listens on port (TCP and UDP) for guests; NULL on failure (printed) */
vsnet_t *vsnet_host(int port, const net_opts_t *o, const uint8_t id[32]);
/* guest: starts connecting; vsnet_wait_start goes on with it */
vsnet_t *vsnet_join(const char *addr, int port, const net_opts_t *o, const uint8_t id[32]);
/* guest, every frame until 1: the host's match is in v. -1: refused or gone (vsnet_reason) */
int vsnet_wait_start(vsnet_t *n, versus_t *v);
int vsnet_refused(const vsnet_t *n);
const char *vsnet_reason(const vsnet_t *n);

/* one tick (1/60 s): takes guests in (host), sends and receives, rolls back where a prediction
 * was wrong, and runs the next frame unless it has to wait. local: this machine's player's
 * buttons. Returns the frames advanced (0 or 1), -1 once the host has gone (guest). Frames from
 * `end` on are not run (VSNET_NO_END: no end). */
#define VSNET_NO_END UINT32_MAX
int vsnet_tick(vsnet_t *n, versus_t *v, const uint16_t *rom, uint16_t local, uint32_t end);
/* every human slot's inputs for every frame before end are here, run with, and known to the
 * other machines */
int vsnet_settled(const vsnet_t *n, const versus_t *v, uint32_t end);

int vsnet_slot(const vsnet_t *n);      /* this machine's slot, -1 before the start */
int vsnet_players(const vsnet_t *n);   /* machines in the match */
int vsnet_is_host(const vsnet_t *n);
uint32_t vsnet_input_frame(const vsnet_t *n);   /* the frame the next local input is for */
const char *vsnet_status(const vsnet_t *n);
const char *vsnet_event(vsnet_t *n);   /* the latest event, once */
/* writes the settled inputs as an input script ("frame m1 .. m8") */
void vsnet_log_inputs(vsnet_t *n, FILE *f);
void vsnet_close(vsnet_t *n);

#endif
