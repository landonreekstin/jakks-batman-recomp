# Netplay

Online co-op (`recomp/src/net.c`): the host is Batman (P1), the guest Robin (P2). Both machines run
the whole game. Only the controller bits travel between them.

## Why this works

- The game is deterministic: it matches MAME, and a save state continues exactly. With the same
  start state and the same inputs, both machines produce the same game.
- The guest starts from the host's state, EEPROM included, so it can join at any time.
  `spg_snap_t` is about 33 KB and goes over TCP once.
- Both sides check the ROM and mod set (`mods_content_id`), the build (the state size) and that
  co-op is on before playing.

## Rollback

Without rollback, every input would wait for the network round trip. Instead:

- **Local input** is used after an input delay (`--net-delay`, 2 frames). On a LAN or VPN that
  hides most of the latency on its own.
- **The peer's input,** when it hasn't arrived, is predicted: it is the peer's last known input.
- **When the real input differs from the prediction,** the machine restores the snapshot taken
  before that frame and runs to the present again. The re-run frames are silent and not drawn.
  - There is one snapshot per frame, in a ring of 16.
  - A re-run frame costs about 1 ms. The interpreter runs at about 16× real time.
- **A wait:** a machine more than 12 frames past the peer's last input waits for it.
- **Keeping level:** each side sends its frame advantage. One that is on average a frame ahead
  skips a frame, at most once every 20 frames. Smoothing over about 10 frames is needed: without
  it, a 30 ms jitter made both sides skip about 7% of their frames.

## Joining (the SDL front end)

- **The guest keeps trying.** If nobody is hosting yet, or the VPN is off, it shows why and
  tries again every 2 s. When the host quits, it goes back to waiting and joins the next game by
  itself.
- **It stops only when the host refuses it:** a different build, ROM or mod set. It shows the
  reason.
- **The host** plays on its own until player 2 connects. When player 2 leaves, it carries on and
  listens again.

## Wire

- **TCP**, on the port given (default 7845), carries three messages: HELLO (guest: protocol, state
  size, mod-set id, UDP port), WELCOME (ok, or why not), and START (epoch, frame, delay, whole
  state).
- **UDP**, on the same port number, carries one packet a tick each way:
  - the sender's frame and advantage;
  - an ack: the frame of the first input it is still missing;
  - ping timestamps;
  - its latest settled-frame hash;
  - every input the peer hasn't acked (at most 64).

  A lost packet costs nothing, because the next one repeats its inputs.
- **The epoch** numbers each START. Packets from an older epoch are dropped.
- **Byte order:** both ends must be little-endian.

## Desyncs

- Every 60 frames, both hash a settled frame: the state after a frame that ran with both players'
  real inputs. The hash covers RAM, video RAM, the PPU registers, P2's co-op words and the frame.
- If the hashes differ, the host sends START with its current state again.
- Tested by knocking the guest's P1 X off by 8 px at one frame (`BATMAN_NET_CORRUPT`, batman-rt
  only). The next hash catches it, and both end in the same state.

## Testing (`tests/net.sh`)

- Two batman-rt processes on loopback play `tests/net_inputs.py` random input for both players.
  The network is simulated on the sending side, by default 40 ms latency, up to 30 ms jitter and
  10% loss each way. That gives about 135 ms ping and about 40 rollbacks per 900 frames.
- **Checks:**
  - the host and the guest end in the same state hash;
  - their settled input logs (`--net-log`) are identical;
  - replaying that log offline from the start state (`--net-start`) gives the same hash;
  - the knocked-off guest is found and resynced.
- **Results:**
  - passed in Street Party (session1 at 6000) and in *Search for a Smile*;
  - the detective level run had about 65 rollbacks per side over 1500 frames, and 0 desyncs.

## Open

- Tested only on loopback, with simulated latency, jitter and loss; not yet between two machines on
  the VPN.
- The time sync only acts at a 1-frame difference, so the machine that is behind does most of the
  rollbacks.
- The guest's EEPROM is the host's, and the guest never saves it.
- Driving levels are still single player, for both machines.
