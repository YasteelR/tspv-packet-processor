#include "tspv_processor.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
    OFF_PACKET_ID = 1,
    OFF_EPOCH = 2,
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
static bool s_haveLastTime;
static uint32_t s_lastProcessedTime;

static RequestOldPacketFn s_requestFn = requestOldPacket;
static PostTSPVFn s_postFn = postTSPV;

static uint8_t nextId(uint8_t id)
{
    return (id == 255u) ? 1u : (uint8_t)(id + 1u);
}

/* Maps 1..255 -> 0..254 so modulo arithmetic on the 255-value ID space
 * (0 is reserved and never part of the sequence) is straightforward. */
static uint8_t idIndex(uint8_t id)
{
    return (uint8_t)(id - 1u);
}

/* Number of forward steps to go from `from` to `to` around the ID space. */
static uint8_t forwardDistance(uint8_t from, uint8_t to)
{
    int d = (int)idIndex(to) - (int)idIndex(from);
    if (d < 0) {
        d += 255;
    }
    return (uint8_t)d;
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

/* Posts a single TSPV only if its absolute time (packet epoch + this
 * TSPV's offset) is strictly newer than the last one actually posted -
 * enforces "cannot process TSPVs with an earlier date than one already
 * processed" from the spec, independent of PacketID sequencing. */
static void processTSPV(uint32_t epochTime, const uint8_t *tspv)
{
    uint8_t stat1 = tspv[0];
    uint8_t stat2 = tspv[1];
    uint8_t offset = tspv[2];
    float pv = readF32BE(&tspv[3]);

    uint32_t absTime = epochTime + offset;
    if (s_haveLastTime && absTime <= s_lastProcessedTime) {
        return;
    }
    if (s_postFn) {
        s_postFn(stat1, stat2, pv);
    }
    s_lastProcessedTime = absTime;
    s_haveLastTime = true;
}

static void processPacketBytes(const uint8_t *data)
{
    uint32_t epoch = readU32BE(&data[OFF_EPOCH]);
    processTSPV(epoch, &data[OFF_TSPV1]);
    processTSPV(epoch, &data[OFF_TSPV2]);
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

static PendingSlot *findPending(uint8_t id)
{
    for (unsigned i = 0; i < PENDING_CAPACITY; ++i) {
        if (s_pending[i].used && s_pending[i].id == id) {
            return &s_pending[i];
        }
    }
    return NULL;
}

/* Applies any buffered packets that have become contiguous with
 * s_nextExpectedId, in ID order, for as long as the buffer allows. */
static void flushPending(void)
{
    for (;;) {
        PendingSlot *slot = findPending(s_nextExpectedId);
        if (!slot) {
            break;
        }
        uint8_t data[TSPV_PACKET_LENGTH];
        uint8_t id = slot->id;
        memcpy(data, slot->data, TSPV_PACKET_LENGTH);
        slot->used = false;
        s_requested[id] = false;

        processPacketBytes(data);
        s_nextExpectedId = nextId(id);
    }
}

/* Called when a gap is too big for the device to ever fill (see the
 * TSPV_RECOVERABLE_GAP check in receiveMSG). Genuinely missing IDs are
 * unrecoverable and get skipped, but anything already sitting in the
 * pending buffer was actually received and must still be processed -
 * throwing it away would violate "process as much data as possible,
 * unless not possible" for data we already have in hand. */
static void skipUnrecoverableGapUpTo(uint8_t id)
{
    while (s_nextExpectedId != id) {
        PendingSlot *slot = findPending(s_nextExpectedId);
        if (slot) {
            uint8_t data[TSPV_PACKET_LENGTH];
            uint8_t pid = slot->id;
            memcpy(data, slot->data, TSPV_PACKET_LENGTH);
            slot->used = false;
            s_requested[pid] = false;
            processPacketBytes(data);
            s_nextExpectedId = nextId(pid);
        } else {
            /* Truly lost - the device no longer has it. */
            s_requested[s_nextExpectedId] = false;
            s_nextExpectedId = nextId(s_nextExpectedId);
        }
    }
}

/* First packet ever seen becomes the sequence's starting point; after
 * that, anything that isn't exactly the next expected PacketID gets
 * buffered and the gap is requested from the device. Once the missing
 * packet(s) land, flushPending() cascades whatever is now contiguous
 * back into order. */
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
        flushPending();
        return;
    }

    if (id == s_nextExpectedId) {
        s_requested[id] = false;
        processPacketBytes(data);
        s_nextExpectedId = nextId(id);
        flushPending();
        return;
    }

    uint8_t forwardGap = forwardDistance(s_nextExpectedId, id);
    uint8_t backwardGap = (uint8_t)(255u - forwardGap);

    if (backwardGap <= TSPV_RECOVERABLE_GAP) {
        /* This ID is behind what we already processed (a duplicate /
         * stale re-delivery) - ignore it, we cannot go backwards. */
        return;
    }

    if (forwardGap <= TSPV_RECOVERABLE_GAP) {
        /* Out of order, but still within the device's 4-packet
         * history: hold on to it and ask for whatever is missing in
         * between. Each missing ID is only requested once - if a
         * second out-of-order packet arrives before the first gap is
         * filled, IDs already outstanding must not be re-requested. */
        storePending(id, data);
        uint8_t missing = s_nextExpectedId;
        for (uint8_t i = 0; i < forwardGap; ++i) {
            requestIfNeeded(missing);
            missing = nextId(missing);
        }
        return;
    }

    /* Gap is bigger than the device's history: at least one missing ID
     * can never be recovered. Salvage whatever's already buffered on
     * the way there, skip the genuine holes, then process this packet. */
    skipUnrecoverableGapUpTo(id);
    processPacketBytes(data);
    s_nextExpectedId = nextId(id);
    flushPending();
}

void tspv_start(void)
{
    if (s_requestFn) {
        s_requestFn(0);
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
    s_haveLastTime = false;
    s_lastProcessedTime = 0;
    for (unsigned i = 0; i < PENDING_CAPACITY; ++i) {
        s_pending[i].used = false;
    }
    memset(s_requested, 0, sizeof(s_requested));
}
