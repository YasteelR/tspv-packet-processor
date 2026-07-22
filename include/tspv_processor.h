#ifndef TSPV_PROCESSOR_H
#define TSPV_PROCESSOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------
 * Wire format (20 bytes total, all multi-byte fields big-endian):
 *
 *   byte 0      Version
 *   byte 1      PacketID          (1..255, 0 is reserved)
 *   bytes 2-5   Epoch time        (32-bit unsigned seconds since 1970-01-01)
 *   bytes 6-12  TSPV #1           (stat1, stat2, offsetSeconds, float32)
 *   bytes 13-19 TSPV #2           (stat1, stat2, offsetSeconds, float32)
 *
 * TSPV absolute time = packet epoch time + TSPV offset (seconds).
 * The device guarantees TSPV #2's offset is >= TSPV #1's offset.
 * ------------------------------------------------------------------------ */

#define TSPV_PACKET_LENGTH        20u
#define TSPV_RECOVERABLE_GAP      4u   /* device only retains its last 4 packets */

/* ---- Interfaces required by the spec ------------------------------- */

/* Entry point: called by the transport layer whenever a packet arrives. */
void receiveMSG(uint8_t *data, uint8_t length);

/* Asks the device to resend a previously transmitted packet by ID.
 * Returns a pointer to the request buffer that would be sent to the
 * device (caller-owned in production; here it just supports logging/tests). */
uint8_t *requestOldPacket(uint8_t packetid);

/* Delivers one decoded TSPV to whatever consumes the data. */
void postTSPV(uint8_t stat1, uint8_t stat2, float pv);

/* Call once when the application starts, before any packet has been
 * received. PacketID 0 is reserved by the device for exactly this:
 * asking it to send its latest data. The reply is an ordinary packet
 * with a real, non-zero PacketID and is handled by receiveMSG() like
 * any other - whatever ID it carries becomes the sequence's starting
 * point, same as the first packet ever seen. */
void tspv_start(void);

/* ---- Test/simulation support ---------------------------------------
 * receiveMSG() calls requestOldPacket()/postTSPV() indirectly through
 * these hooks so unit tests can substitute mocks without touching
 * global function symbols. Production code can ignore this section
 * entirely - the default hooks are the real functions above. */

typedef uint8_t *(*RequestOldPacketFn)(uint8_t packetid);
typedef void (*PostTSPVFn)(uint8_t stat1, uint8_t stat2, float pv);

void tspv_setHooks(RequestOldPacketFn requestFn, PostTSPVFn postFn);
void tspv_resetState(void);

#ifdef __cplusplus
}
#endif

#endif /* TSPV_PROCESSOR_H */
