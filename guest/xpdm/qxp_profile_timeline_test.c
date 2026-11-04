#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "qxpdisp/qxp_profile_timeline.h"

static int failures;

static void check(int condition, const char *message)
{
    if (condition) return;
    fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

static void test_reordered_events(void)
{
    QXP_PROFILE_TIMELINE timeline;

    memset(&timeline, 0, sizeof(timeline));
    qxp_profile_timeline_begin(&timeline, 110, 1000);
    qxp_profile_timeline_begin(&timeline, 100, 1000);
    qxp_profile_timeline_finish(&timeline, 200);
    qxp_profile_timeline_finish(&timeline, 180);
    check(timeline.first_ticks == 100, "reordered starts retain minimum");
    check(timeline.last_ticks == 200, "reordered finishes retain maximum");
    check(timeline.previous_finish_ticks == 200,
          "completed cluster retains maximum finish boundary");
    check(timeline.overlap_count == 1,
          "reordered overlapping start is recorded");

    qxp_profile_timeline_begin(&timeline, 210, 1000);
    qxp_profile_timeline_finish(&timeline, 220);
    check(timeline.gap_count == 1 && timeline.gap_ticks == 10,
          "next gap begins at maximum cluster finish");
    check(timeline.gap_max_ticks == 10, "maximum gap is retained");
    check(qxp_profile_timeline_span(&timeline) == 120,
          "span uses minimum start and maximum finish");
}

static void test_equal_boundary(void)
{
    QXP_PROFILE_TIMELINE timeline;

    memset(&timeline, 0, sizeof(timeline));
    qxp_profile_timeline_begin(&timeline, 1000, 1000);
    qxp_profile_timeline_finish(&timeline, 1100);
    qxp_profile_timeline_begin(&timeline, 1100, 1000);
    qxp_profile_timeline_finish(&timeline, 1200);
    check(timeline.calls == 2, "equal-boundary calls are counted");
    check(timeline.gap_count == 0 && timeline.gap_ticks == 0,
          "equal boundary is not a positive gap");
    check(timeline.overlap_count == 0,
          "equal boundary is not an overlap");
    check(qxp_profile_timeline_span(&timeline) == 200,
          "equal-boundary span is retained");
}

static void test_gap_buckets(void)
{
    QXP_PROFILE_TIMELINE timeline;
    static const LONGLONG gaps[] = {
        500, 5000, 20000, 75000, 150000, 300000
    };
    LONGLONG finish;
    DWORD index;

    memset(&timeline, 0, sizeof(timeline));
    qxp_profile_timeline_begin(&timeline, 1000000, 1000000);
    finish = 1000100;
    qxp_profile_timeline_finish(&timeline, finish);
    for (index = 0; index < sizeof(gaps) / sizeof(gaps[0]); ++index) {
        qxp_profile_timeline_begin(&timeline, finish + gaps[index], 1000000);
        finish += gaps[index] + 100;
        qxp_profile_timeline_finish(&timeline, finish);
    }
    check(timeline.calls == 7 && timeline.gap_count == 6,
          "all positive gaps are counted");
    check(timeline.gap_ticks == 550500 &&
          timeline.gap_max_ticks == 300000,
          "gap total and maximum are correct");
    check(timeline.gap_lt_1ms == 1 && timeline.gap_1_10ms == 1 &&
          timeline.gap_10_50ms == 1 && timeline.gap_50_100ms == 1 &&
          timeline.gap_100_250ms == 1 && timeline.gap_ge_250ms == 1,
          "millisecond buckets cover every positive gap once");
}

static void test_late_begin_accounting(void)
{
    QXP_PROFILE_TIMELINE timeline;

    memset(&timeline, 0, sizeof(timeline));
    qxp_profile_timeline_begin(&timeline, 200, 1000);
    qxp_profile_timeline_finish(&timeline, 300);
    qxp_profile_timeline_begin(&timeline, 250, 1000);
    qxp_profile_timeline_finish(&timeline, 350);
    check(timeline.overlap_count == 1 && timeline.gap_count == 0,
          "late accounting preserves timestamp overlap");
    check(timeline.first_ticks == 200 && timeline.last_ticks == 350,
          "late accounting preserves timeline extrema");
}

int main(void)
{
    test_reordered_events();
    test_equal_boundary();
    test_gap_buckets();
    test_late_begin_accounting();
    if (failures) return 1;
    puts("DP2 profile timeline tests passed");
    return 0;
}
