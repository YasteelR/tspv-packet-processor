# TSPV Packet Processor

A small C library that receives a 20-byte packet once a minute, parses the
header and the two embedded TSPV (Time-Stamped Present Value) structures,
detects out-of-order/missing packets, re-requests them from the device, and
posts each TSPV in strict time order once it is safe to do so.

## Packet format

```
byte 0      Version
byte 1      PacketID            1..255 (0 is reserved for the app's
                                 "send me the latest data" request)
bytes 2-5   Epoch time          32-bit unsigned, big-endian
bytes 6-12  TSPV #1             stat1, stat2, offsetSeconds, float32 (BE)
bytes 13-19 TSPV #2             stat1, stat2, offsetSeconds, float32 (BE)
```

A TSPV's absolute time is `packet epoch time + TSPV offset`. Per the spec,
TSPV #2's offset is always >= TSPV #1's offset.

## Required interfaces

Implemented exactly as specified in `include/tspv_processor.h` /
`src/tspv_processor.c`:

```c
void receiveMSG(uint8_t *data, uint8_t length);
uint8_t *requestOldPacket(uint8_t packetid);
void postTSPV(uint8_t stat1, uint8_t stat2, float pv);
```

`requestOldPacket` and `postTSPV` have simple default implementations
(they log to stdout) since the actual transmit/consume mechanism is
hardware/application specific. `receiveMSG` calls them indirectly through
two function-pointer hooks (`tspv_setHooks`) so unit tests can substitute
mocks and assert on exactly what was requested/posted without touching
global symbols or capturing stdout.

One additional function, `tspv_start(void)`, is provided beyond the three
named in the spec: PacketID 0 is described as "reserved for asking the
device the latest data when you start your app", but that request has to
actually be sent by *something*. Call `tspv_start()` once at application
startup, before any packet has arrived - it issues that request via the
same `requestOldPacket` mechanism (`requestOldPacket(0)`). The device's
reply is just an ordinary packet with a real, non-zero ID, so it's handled
by `receiveMSG` exactly like the first packet ever seen.

### A note on `postTSPV` and "time"

The spec says the output should post "the individual TSPV float, as well as
its time and status bytes", but the mandated signature -
`postTSPV(uint8_t stat1, uint8_t stat2, float pv)` - has no parameter for
time. Both can't be satisfied literally. This implementation honours the
exact mandated signature ("use the function header") and uses each TSPV's
absolute time (`epoch + offset`) *internally* instead - to enforce the
"cannot process a TSPV with an earlier date than one already processed"
ordering rule - rather than adding a fourth parameter the signature doesn't
allow. If the intent was actually to surface the time to the consumer, the
one-line change is to widen the signature to
`postTSPV(uint8_t stat1, uint8_t stat2, uint32_t absTime, float pv)`; the
value is already computed at the call site.

## Algorithm

State kept between calls to `receiveMSG`:
- `nextExpectedId` — the next PacketID we expect in sequence.
- `lastProcessedTime` — the absolute time of the last TSPV actually posted.
- a small buffer of packets that arrived early, keyed by PacketID.
- a set of PacketIDs currently pending a re-request (to avoid asking twice).

For each incoming packet:

1. **First packet ever seen** → accepted unconditionally as the sequence's
   starting point (there is nothing earlier to validate against).
2. **`id == nextExpectedId`** → process immediately, advance the expected
   ID, then cascade-apply any buffered packets that are now contiguous.
3. **`id` is behind `nextExpectedId`** (within the device's 4-packet
   history) → a stale duplicate re-delivery of something already
   processed. Ignored — the spec forbids processing an earlier date than
   one already handled.
4. **`id` is ahead of `nextExpectedId` by <= 4** → the gap is still inside
   the device's 4-packet retransmit window: buffer the packet and call
   `requestOldPacket()` for each missing ID in between (each ID is only
   requested once until it's resolved).
5. **`id` is ahead of `nextExpectedId` by > 4** → at least one missing ID
   can never be recovered (the device has already discarded it from its
   own 4-packet history). Per "process as much data as possible unless
   not possible", genuinely missing IDs are skipped, but anything already
   sitting in the buffer *was* actually received and is still processed
   (in order, ahead of the new packet) rather than thrown away - only
   the true holes are given up on.

Within a packet, each TSPV is posted individually only if its absolute
time is strictly newer than the last TSPV actually posted; otherwise it is
silently skipped (this also protects against a mid-sequence packet whose
clock/time is inconsistent, without breaking PacketID sequencing).

The 255-value ID space (1..255, 0 reserved) wraps correctly, e.g.
`254 -> 255 -> 1`.

### Simplifications / assumptions

This is intentionally scoped to match the take-home problem, not a full
production transport stack:
- The pending-packet buffer has a small fixed capacity (8 slots), which is
  comfortably more than the 4-packet recoverable window; overflow beyond
  that (shouldn't happen given the spec's constraints) drops the oldest
  slot rather than growing unbounded.
- `requestOldPacket`/`postTSPV` just log; wiring them to real hardware I/O
  is a one-line change at the call site since they're already isolated
  behind hooks.
- Multi-byte fields are assumed big-endian (standard network byte order);
  the worked example's epoch bytes decode to a plausible real-world date
  under that assumption, though the example alone can't fully prove it.
- "The device stores the last 4 sent packets" is read as: a gap of up to
  4 missing PacketIDs is treated as recoverable, more than 4 is not. The
  spec doesn't pin down whether that 4-packet window includes the packet
  that was just sent or not, so a gap of exactly 4 could, in the strictest
  reading, include one ID the device has already evicted. Given the
  ambiguity, this implementation takes the more generous reading (always
  attempt recovery for a gap of <= 4) rather than guessing conservatively;
  a real integration would confirm the exact retention window against the
  device's actual protocol documentation.

## Project layout

```
include/tspv_processor.h        Public API + hooks
src/tspv_processor.c            Implementation (parsing, sequencing, recovery)
src/main.c                      Demo: simulates a packet stream end-to-end
tests/test_tspv_processor.c     Unit tests, including the required unitTEST1()
CMakeLists.txt                  Build for the library, demo, and tests
```

## Building & running

Requires a C compiler and CMake.

```powershell
cmake -S . -B build -G "MinGW Makefiles"   # or omit -G to use your default generator
cmake --build build

.\build\tspv_tests.exe     # unit tests
.\build\tspv_demo.exe      # demo packet stream
```

`tspv_tests` prints a `PASS`/`FAIL` line for any failing assertion and a
final `N assertions, N failures` summary, exiting non-zero on failure —
suitable for CI.
