#include "tspv_processor.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
    OFF_PACKET_ID = 1,
    OFF_TSPV1 = 6,
    OFF_TSPV2 = 13
};

/* A little headroom for packets that arrive while we're still waiting
 * on the device to resend whatever is missing. */
#define PENDING_CAPACITY 8u

typedef struct {
    bool used;
    uint8_t id;
    uint8_t data[TSPV_PACKET_LENGTH];
} PendingSlot;

static PendingSlot s_pending[PENDING_CAPACITY];
static bool s_requested[256];

static bool s_haveBaseline;
static uint8_t s_nextExpectedId;

static RequestOldPacketFn s_requestFn = requestOldPacket;
static PostTSPVFn s_postFn = postTSPV;

static uint8_t nextId(uint8_t id)
{
    return (id == 255u) ? 1u : (uint8_t)(id + 1u);
}

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

static void processPacketBytes(const uint8_t *data)
{
    postOneTSPV(&data[OFF_TSPV1]);
    postOneTSPV(&data[OFF_TSPV2]);
}

static void storePending(uint8_t id, const uint8_t *data)
{
    for (unsigned i = 0; i < PENDING_CAPACITY; ++i) {
        if (!s_pending[i].used) {
            s_pending[i].used = true;
            s_pending[i].id = id;
            memcpy(s_pending[i].data, data, TSPV_PACKET_LENGTH);
            return;
        }
    }
    /* Buffer full - drop it rather than crash. Shouldn't happen given
     * the device only ever has a handful of packets outstanding. */
}

static void requestIfNeeded(uint8_t id)
{
    if (!s_requested[id]) {
        s_requested[id] = true;
        if (s_requestFn) {
            s_requestFn(id);
        }
    }
}

/* First packet ever seen becomes the sequence's starting point; after
 * that, anything that isn't exactly the next expected PacketID gets
 * buffered and the gap is requested from the device. Applying buffered
 * packets back into order once the gap fills isn't wired up yet. */
void receiveMSG(uint8_t *data, uint8_t length)
{
    if (!data || length != TSPV_PACKET_LENGTH) {
        return;
    }

    uint8_t id = data[OFF_PACKET_ID];
    if (id == 0u) {
        return; /* reserved for outbound "send latest data" requests */
    }

    if (!s_haveBaseline) {
        s_haveBaseline = true;
        s_nextExpectedId = nextId(id);
        processPacketBytes(data);
        return;
    }

    if (id == s_nextExpectedId) {
        s_requested[id] = false;
        processPacketBytes(data);
        s_nextExpectedId = nextId(id);
        return;
    }

    /* Out of order: hold on to it and ask the device to resend
     * whatever is missing in between. Each missing ID is only
     * requested once - if a second out-of-order packet arrives before
     * the first gap is filled, IDs already outstanding must not be
     * re-requested. */
    storePending(id, data);
    uint8_t missing = s_nextExpectedId;
    while (missing != id) {
        requestIfNeeded(missing);
        missing = nextId(missing);
    }
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
    s_requestFn = requestFn ? requestFn : requestOldPacket;
    s_postFn = postFn ? postFn : postTSPV;
}

void tspv_resetState(void)
{
    s_haveBaseline = false;
    s_nextExpectedId = 0;
    for (unsigned i = 0; i < PENDING_CAPACITY; ++i) {
        s_pending[i].used = false;
    }
    memset(s_requested, 0, sizeof(s_requested));
}
