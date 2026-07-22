/* Demo driver: feeds a small simulated stream of packets through
 * receiveMSG() so the sequencing / recovery behaviour can be observed
 * end to end. This is not a unit test - see tests/test_tspv_processor.c
 * for the actual test suite. */

#include "tspv_processor.h"

#include <stdio.h>
#include <string.h>

static void writeU32BE(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
}

static void writeF32BE(uint8_t *b, float f)
{
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    writeU32BE(b, bits);
}

static void buildPacket(uint8_t out[20], uint8_t version, uint8_t packetId,
                         uint32_t epoch, uint8_t stat1a, uint8_t stat2a,
                         uint8_t offsetA, float pvA, uint8_t stat1b,
                         uint8_t stat2b, uint8_t offsetB, float pvB)
{
    out[0] = version;
    out[1] = packetId;
    writeU32BE(&out[2], epoch);

    out[6] = stat1a;
    out[7] = stat2a;
    out[8] = offsetA;
    writeF32BE(&out[9], pvA);

    out[13] = stat1b;
    out[14] = stat2b;
    out[15] = offsetB;
    writeF32BE(&out[16], pvB);
}

int main(void)
{
    tspv_resetState();
    uint32_t epoch = 1700000000u;
    uint8_t pkt[20];

    printf("--- packet 4: baseline, in order ---\n");
    buildPacket(pkt, 1, 4, epoch, 0x02, 0x20, 0, 21.5f, 0x03, 0x10, 10, 21.7f);
    receiveMSG(pkt, sizeof pkt);

    printf("\n--- packet 5: in order ---\n");
    buildPacket(pkt, 1, 5, epoch + 60, 0x02, 0x20, 0, 22.0f, 0x03, 0x10, 10, 22.1f);
    receiveMSG(pkt, sizeof pkt);

    printf("\n--- packet 8 arrives early (6 and 7 missing) ---\n");
    buildPacket(pkt, 1, 8, epoch + 240, 0x02, 0x20, 0, 24.0f, 0x03, 0x10, 10, 24.1f);
    receiveMSG(pkt, sizeof pkt); /* expect requests for 6 and 7, nothing posted yet */

    printf("\n--- packet 7 arrives (still missing 6, gets buffered) ---\n");
    buildPacket(pkt, 1, 7, epoch + 180, 0x02, 0x20, 0, 23.0f, 0x03, 0x10, 10, 23.1f);
    receiveMSG(pkt, sizeof pkt); /* still nothing posted, 6 still missing */

    printf("\n--- packet 6 arrives (fills the gap, cascade 6-7-8) ---\n");
    buildPacket(pkt, 1, 6, epoch + 120, 0x02, 0x20, 0, 23.0f, 0x03, 0x10, 10, 23.1f);
    receiveMSG(pkt, sizeof pkt); /* 6, 7, 8 all post now, in order */

    printf("\n--- packet 20 arrives (gap of 11, unrecoverable, skip ahead) ---\n");
    buildPacket(pkt, 1, 20, epoch + 900, 0x02, 0x20, 0, 30.0f, 0x03, 0x10, 10, 30.1f);
    receiveMSG(pkt, sizeof pkt); /* no requests for 9..19, posts 20 directly */

    printf("\n--- packet 20 resent as a stale duplicate ---\n");
    receiveMSG(pkt, sizeof pkt); /* ignored, already processed */

    return 0;
}
