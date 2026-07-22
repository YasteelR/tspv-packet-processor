#include "tspv_processor.h"

#include <stdio.h>
#include <string.h>

static PostTSPVFn s_postFn = postTSPV;

static uint32_t readU32BE(const uint8_t *b)
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

static float readF32BE(const uint8_t *b)
{
    uint32_t bits = readU32BE(b);
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

static void postOneTSPV(const uint8_t *tspv)
{
    uint8_t stat1 = tspv[0];
    uint8_t stat2 = tspv[1];
    float pv = readF32BE(&tspv[3]);
    if (s_postFn) {
        s_postFn(stat1, stat2, pv);
    }
}

/* First pass: assume every packet that arrives is the next one in
 * sequence and just decode + post both TSPVs. PacketID gap detection/
 * recovery and TSPV time-ordering are not handled yet. */
void receiveMSG(uint8_t *data, uint8_t length)
{
    if (!data || length != TSPV_PACKET_LENGTH) {
        return;
    }

    postOneTSPV(&data[6]);  /* TSPV #1 */
    postOneTSPV(&data[13]); /* TSPV #2 */
}

uint8_t *requestOldPacket(uint8_t packetid)
{
    static uint8_t requestBuffer[2];
    requestBuffer[0] = 0x00; /* "resend packet" opcode, device-protocol specific */
    requestBuffer[1] = packetid;
    printf("[requestOldPacket] requesting retransmission of packet ID %u\n",
           (unsigned)packetid);
    return requestBuffer;
}

void postTSPV(uint8_t stat1, uint8_t stat2, float pv)
{
    printf("[postTSPV] stat1=0x%02X stat2=0x%02X pv=%g\n", stat1, stat2,
           (double)pv);
}

void tspv_setHooks(RequestOldPacketFn requestFn, PostTSPVFn postFn)
{
    (void)requestFn; /* no gap detection yet, nothing to hook up */
    s_postFn = postFn ? postFn : postTSPV;
}

void tspv_resetState(void)
{
    /* no sequencing state exists yet */
}
