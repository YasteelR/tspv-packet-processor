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

/* ---- mock hook: capture what receiveMSG() posts --------------------- */

#define MAX_RECORDS 8

typedef struct {
    uint8_t stat1;
    uint8_t stat2;
    float pv;
} PostedRecord;

static PostedRecord s_posted[MAX_RECORDS];
static int s_postedCount;

static void mockPost(uint8_t stat1, uint8_t stat2, float pv)
{
    if (s_postedCount < MAX_RECORDS) {
        s_posted[s_postedCount].stat1 = stat1;
        s_posted[s_postedCount].stat2 = stat2;
        s_posted[s_postedCount].pv = pv;
        s_postedCount++;
    }
}

static void installMocks(void)
{
    s_postedCount = 0;
    tspv_resetState();
    tspv_setHooks(NULL, mockPost);
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
    uint8_t packet[20] = {0};

    receiveMSG(packet, 19); /* wrong length */
    CHECK(s_postedCount == 0);

    receiveMSG(packet, 20);
    CHECK(s_postedCount == 2);
}

int main(void)
{
    RUN_TEST(unitTEST1);
    RUN_TEST(test_MalformedLengthIsIgnored);

    printf("\n%d assertions, %d failures\n", g_assertions, g_failures);
    return g_failures == 0 ? 0 : 1;
}
