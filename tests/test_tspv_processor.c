/* Lightweight, dependency-free unit tests for the TSPV packet processor.
 * No external test framework is used so the whole project builds with
 * nothing but a C compiler (see CMakeLists.txt). */

#include "tspv_processor.h"

#include <stdio.h>
#include <string.h>

/* ---- tiny assertion framework ---------------------------------------- */

static int g_failures = 0;
static int g_assertions = 0;
static const char *g_currentTest = "";

#define CHECK(cond)                                                        \
    do {                                                                    \
        g_assertions++;                                                     \
        if (!(cond)) {                                                      \
            g_failures++;                                                   \
            printf("  FAIL [%s] %s (%s:%d)\n", g_currentTest, #cond,        \
                   __FILE__, __LINE__);                                     \
        }                                                                    \
    } while (0)

#define RUN_TEST(fn)                                                        \
    do {                                                                    \
        g_currentTest = #fn;                                                \
        printf("Running %s...\n", #fn);                                    \
        fn();                                                               \
    } while (0)

/* ---- packet-building helper for tests that need specific IDs/times -- */

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

/* ---- mock hooks: capture what receiveMSG() posts/requests ----------- */

/* Sized for the largest multi-packet cascade scenario tested below. */
#define MAX_RECORDS 32

typedef struct {
    uint8_t stat1;
    uint8_t stat2;
    float pv;
} PostedRecord;

static PostedRecord s_posted[MAX_RECORDS];
static int s_postedCount;

static uint8_t s_requestedIds[MAX_RECORDS];
static int s_requestedCount;

static void mockPost(uint8_t stat1, uint8_t stat2, float pv)
{
    if (s_postedCount < MAX_RECORDS) {
        s_posted[s_postedCount].stat1 = stat1;
        s_posted[s_postedCount].stat2 = stat2;
        s_posted[s_postedCount].pv = pv;
        s_postedCount++;
    }
}

static uint8_t *mockRequest(uint8_t packetid)
{
    static uint8_t dummy;
    if (s_requestedCount < MAX_RECORDS) {
        s_requestedIds[s_requestedCount++] = packetid;
    }
    return &dummy;
}

static void installMocks(void)
{
    s_postedCount = 0;
    s_requestedCount = 0;
    tspv_resetState();
    tspv_setHooks(mockRequest, mockPost);
}

/* ------------------------------------------------------------------------
 * unitTEST1 - required by the assignment. Uses the exact example packet
 * from the problem statement:
 *   01 04  62 55 76 5E  02 20 02 7F FF FF FF  03 10 0A 7F FF FF FF
 * and checks that both TSPVs are parsed and posted with the right
 * status bytes, in the right order, with the right float bit pattern.
 * ---------------------------------------------------------------------- */
void unitTEST1(void)
{
    g_currentTest = "unitTEST1";
    installMocks();

    uint8_t packet[20] = {
        0x01, 0x04,             /* version, packetID */
        0x62, 0x55, 0x76, 0x5E, /* epoch time */
        0x02, 0x20, 0x02, 0x7F, 0xFF, 0xFF, 0xFF, /* TSPV #1 */
        0x03, 0x10, 0x0A, 0x7F, 0xFF, 0xFF, 0xFF  /* TSPV #2 */
    };

    receiveMSG(packet, sizeof packet);

    CHECK(s_postedCount == 2);
    if (s_postedCount == 2) {
        CHECK(s_posted[0].stat1 == 0x02);
        CHECK(s_posted[0].stat2 == 0x20);
        CHECK(s_posted[1].stat1 == 0x03);
        CHECK(s_posted[1].stat2 == 0x10);

        /* 7F FF FF FF is a quiet-NaN bit pattern; equality never holds
         * for NaN, so compare the raw bits that were decoded instead. */
        uint32_t expectedBits = 0x7FFFFFFFu;
        uint32_t actualBits0, actualBits1;
        memcpy(&actualBits0, &s_posted[0].pv, sizeof(actualBits0));
        memcpy(&actualBits1, &s_posted[1].pv, sizeof(actualBits1));
        CHECK(actualBits0 == expectedBits);
        CHECK(actualBits1 == expectedBits);
    }
}

static void test_MalformedLengthIsIgnored(void)
{
    installMocks();
    uint8_t packet[20];
    buildPacket(packet, 1, 1, 1000000u, 0, 0, 0, 1.0f, 0, 0, 10, 1.1f);

    receiveMSG(packet, 19); /* wrong length */
    CHECK(s_postedCount == 0);

    receiveMSG(packet, 20);
    CHECK(s_postedCount == 2);
}

static void test_OutOfOrderPacketBuffersAndRequestsGap(void)
{
    installMocks();
    uint32_t epoch = 1000000u;
    uint8_t pkt[20];

    /* Baseline: first packet ever seen, accepted unconditionally. */
    buildPacket(pkt, 1, 1, epoch, 0, 0, 0, 1.0f, 0, 0, 10, 1.1f);
    receiveMSG(pkt, sizeof pkt);
    CHECK(s_postedCount == 2);
    CHECK(s_requestedCount == 0);

    /* Packet 5 arrives early: 2, 3, 4 are missing and must be
     * requested. Packet 5 itself isn't posted yet - it's buffered
     * until the gap in front of it is filled. */
    buildPacket(pkt, 1, 5, epoch + 240, 0, 0, 0, 5.0f, 0, 0, 10, 5.1f);
    receiveMSG(pkt, sizeof pkt);
    CHECK(s_postedCount == 2);
    CHECK(s_requestedCount == 3);
    if (s_requestedCount == 3) {
        CHECK(s_requestedIds[0] == 2);
        CHECK(s_requestedIds[1] == 3);
        CHECK(s_requestedIds[2] == 4);
    }
}

static void test_SecondEarlyPacketDoesNotReRequestOutstandingIds(void)
{
    installMocks();
    uint32_t epoch = 1500000u;
    uint8_t pkt[20];

    buildPacket(pkt, 1, 1, epoch, 0, 0, 0, 1.0f, 0, 0, 10, 1.1f);
    receiveMSG(pkt, sizeof pkt);

    /* Packet 5 arrives early -> requests 2, 3, 4. */
    buildPacket(pkt, 1, 5, epoch + 240, 0, 0, 0, 5.0f, 0, 0, 10, 5.1f);
    receiveMSG(pkt, sizeof pkt);
    CHECK(s_requestedCount == 3);

    /* Packet 6 arrives before the gap is filled: 2, 3, 4 are still
     * outstanding and must not be requested a second time - only the
     * newly-discovered gap (5) should trigger a fresh request. */
    buildPacket(pkt, 1, 6, epoch + 300, 0, 0, 0, 6.0f, 0, 0, 10, 6.1f);
    receiveMSG(pkt, sizeof pkt);
    CHECK(s_requestedCount == 4);
    if (s_requestedCount == 4) {
        CHECK(s_requestedIds[3] == 5);
    }
}

static void test_GapFillCascadesBufferedPacketsInOrder(void)
{
    installMocks();
    uint32_t epoch = 1800000u;
    uint8_t pkt[20];

    buildPacket(pkt, 1, 1, epoch, 0, 0, 0, 10.0f, 0, 0, 10, 10.1f);
    receiveMSG(pkt, sizeof pkt);
    CHECK(s_postedCount == 2);

    /* 5 arrives early -> buffered, requests 2, 3, 4. */
    buildPacket(pkt, 1, 5, epoch + 240, 0, 0, 0, 50.0f, 0, 0, 10, 50.1f);
    receiveMSG(pkt, sizeof pkt);

    /* 4 arrives (still missing 2, 3) -> also buffered. */
    buildPacket(pkt, 1, 4, epoch + 180, 0, 0, 0, 40.0f, 0, 0, 10, 40.1f);
    receiveMSG(pkt, sizeof pkt);
    CHECK(s_postedCount == 2); /* nothing new posted yet */

    /* 2 arrives: matches next-expected, applied immediately. */
    buildPacket(pkt, 1, 2, epoch + 60, 0, 0, 0, 20.0f, 0, 0, 10, 20.1f);
    receiveMSG(pkt, sizeof pkt);
    CHECK(s_postedCount == 4);

    /* 3 arrives: fills the last hole, cascading 3 -> 4 -> 5 in order. */
    buildPacket(pkt, 1, 3, epoch + 120, 0, 0, 0, 30.0f, 0, 0, 10, 30.1f);
    receiveMSG(pkt, sizeof pkt);

    CHECK(s_postedCount == 10);
    if (s_postedCount == 10) {
        float expected[10] = {10.0f, 10.1f, 20.0f, 20.1f, 30.0f,
                               30.1f, 40.0f, 40.1f, 50.0f, 50.1f};
        for (int i = 0; i < 10; ++i) {
            CHECK(s_posted[i].pv == expected[i]);
        }
    }
}

static void test_StaleDuplicateIsIgnored(void)
{
    installMocks();
    uint32_t epoch = 2100000u;
    uint8_t pkt[20];

    buildPacket(pkt, 1, 1, epoch, 0, 0, 0, 1.0f, 0, 0, 10, 1.1f);
    receiveMSG(pkt, sizeof pkt);
    buildPacket(pkt, 1, 2, epoch + 60, 0, 0, 0, 2.0f, 0, 0, 10, 2.1f);
    receiveMSG(pkt, sizeof pkt);
    CHECK(s_postedCount == 4);

    /* Packet 1 (already processed, 1 step behind nextExpectedId=3) is
     * redelivered - must be ignored, not treated as a huge forward gap
     * that requests everything from 3 around to 1. */
    buildPacket(pkt, 1, 1, epoch, 0, 0, 0, 1.0f, 0, 0, 10, 1.1f);
    receiveMSG(pkt, sizeof pkt);
    CHECK(s_postedCount == 4);
    CHECK(s_requestedCount == 0);
}

static void test_UnrecoverableGapSkipsAhead(void)
{
    installMocks();
    uint32_t epoch = 2400000u;
    uint8_t pkt[20];

    buildPacket(pkt, 1, 1, epoch, 0, 0, 0, 1.0f, 0, 0, 10, 1.1f);
    receiveMSG(pkt, sizeof pkt);

    /* Packet 5 arrives early -> buffered, requests issued for 2,3,4. */
    buildPacket(pkt, 1, 5, epoch + 240, 0, 0, 0, 5.0f, 0, 0, 10, 5.1f);
    receiveMSG(pkt, sizeof pkt);
    CHECK(s_requestedCount == 3);

    /* Packet 20 arrives: the gap from "2" to "20" is 18, well beyond
     * what the device can still supply (it only keeps its last 4) -
     * skip ahead, dropping the buffered packet 5 and the outstanding
     * requests, and process 20 directly. */
    buildPacket(pkt, 1, 20, epoch + 900, 0, 0, 0, 20.0f, 0, 0, 10, 20.1f);
    receiveMSG(pkt, sizeof pkt);

    CHECK(s_postedCount == 4); /* packet 1 (2) + packet 20 (2); 5 was dropped */
    if (s_postedCount == 4) {
        CHECK(s_posted[2].pv == 20.0f);
        CHECK(s_posted[3].pv == 20.1f);
    }
    CHECK(s_requestedCount == 3); /* no new requests were made for the skip */
}

int main(void)
{
    RUN_TEST(unitTEST1);
    RUN_TEST(test_MalformedLengthIsIgnored);
    RUN_TEST(test_OutOfOrderPacketBuffersAndRequestsGap);
    RUN_TEST(test_SecondEarlyPacketDoesNotReRequestOutstandingIds);
    RUN_TEST(test_GapFillCascadesBufferedPacketsInOrder);
    RUN_TEST(test_StaleDuplicateIsIgnored);
    RUN_TEST(test_UnrecoverableGapSkipsAhead);

    printf("\n%d assertions, %d failures\n", g_assertions, g_failures);
    return g_failures == 0 ? 0 : 1;
}
