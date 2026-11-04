#ifndef QXP_PROFILE_TIMELINE_H
#define QXP_PROFILE_TIMELINE_H

/* Caller serialization is required.  Keeping the accounting independent of
 * the DDI makes reordered callback timestamps deterministic to unit-test. */
typedef struct qxp_profile_timeline {
    DWORD active_calls;
    DWORD calls;
    ULONGLONG first_ticks;
    ULONGLONG last_ticks;
    ULONGLONG previous_finish_ticks;
    DWORD gap_count;
    ULONGLONG gap_ticks;
    ULONGLONG gap_max_ticks;
    DWORD overlap_count;
    DWORD gap_lt_1ms;
    DWORD gap_1_10ms;
    DWORD gap_10_50ms;
    DWORD gap_50_100ms;
    DWORD gap_100_250ms;
    DWORD gap_ge_250ms;
    ULONGLONG frequency;
} QXP_PROFILE_TIMELINE;

static void qxp_profile_timeline_increment(DWORD *value)
{
    if (value && *value != 0xffffffffUL) ++*value;
}

static void qxp_profile_timeline_add(ULONGLONG *total, ULONGLONG value)
{
    ULONGLONG maximum = ~(ULONGLONG)0;

    if (!total) return;
    if (*total > maximum - value)
        *total = maximum;
    else
        *total += value;
}

static ULONGLONG qxp_profile_timeline_threshold(ULONGLONG frequency,
                                                DWORD milliseconds)
{
    ULONGLONG maximum = ~(ULONGLONG)0;
    ULONGLONG whole;
    ULONGLONG fraction;

    if (!frequency || !milliseconds) return 0;
    whole = frequency / 1000;
    if (whole > maximum / milliseconds) return maximum;
    whole *= milliseconds;
    fraction = ((frequency % 1000) * milliseconds + 999) / 1000;
    if (whole > maximum - fraction) return maximum;
    return whole + fraction;
}

static void qxp_profile_timeline_bucket_gap(QXP_PROFILE_TIMELINE *timeline,
                                            ULONGLONG gap)
{
    ULONGLONG frequency;

    if (!timeline || !timeline->frequency) return;
    frequency = timeline->frequency;
    if (gap < qxp_profile_timeline_threshold(frequency, 1))
        qxp_profile_timeline_increment(&timeline->gap_lt_1ms);
    else if (gap < qxp_profile_timeline_threshold(frequency, 10))
        qxp_profile_timeline_increment(&timeline->gap_1_10ms);
    else if (gap < qxp_profile_timeline_threshold(frequency, 50))
        qxp_profile_timeline_increment(&timeline->gap_10_50ms);
    else if (gap < qxp_profile_timeline_threshold(frequency, 100))
        qxp_profile_timeline_increment(&timeline->gap_50_100ms);
    else if (gap < qxp_profile_timeline_threshold(frequency, 250))
        qxp_profile_timeline_increment(&timeline->gap_100_250ms);
    else
        qxp_profile_timeline_increment(&timeline->gap_ge_250ms);
}

static void qxp_profile_timeline_begin(QXP_PROFILE_TIMELINE *timeline,
                                       LONGLONG start_ticks,
                                       LONGLONG frequency_ticks)
{
    ULONGLONG start;
    ULONGLONG gap;

    if (!timeline) return;
    if (!timeline->frequency && frequency_ticks > 0)
        timeline->frequency = (ULONGLONG)frequency_ticks;
    start = start_ticks > 0 ? (ULONGLONG)start_ticks : 0;
    if (start && (!timeline->first_ticks || start < timeline->first_ticks))
        timeline->first_ticks = start;
    if (timeline->active_calls) {
        qxp_profile_timeline_increment(&timeline->overlap_count);
    } else if (start && timeline->previous_finish_ticks) {
        if (start > timeline->previous_finish_ticks) {
            gap = start - timeline->previous_finish_ticks;
            qxp_profile_timeline_increment(&timeline->gap_count);
            qxp_profile_timeline_add(&timeline->gap_ticks, gap);
            if (gap > timeline->gap_max_ticks)
                timeline->gap_max_ticks = gap;
            qxp_profile_timeline_bucket_gap(timeline, gap);
        } else if (start < timeline->previous_finish_ticks) {
            qxp_profile_timeline_increment(&timeline->overlap_count);
        }
    }
    if (timeline->active_calls != 0xffffffffUL)
        ++timeline->active_calls;
}

static void qxp_profile_timeline_finish(QXP_PROFILE_TIMELINE *timeline,
                                        LONGLONG finish_ticks)
{
    ULONGLONG finish;

    if (!timeline) return;
    qxp_profile_timeline_increment(&timeline->calls);
    finish = finish_ticks > 0 ? (ULONGLONG)finish_ticks : 0;
    if (finish > timeline->last_ticks) timeline->last_ticks = finish;
    if (timeline->active_calls) --timeline->active_calls;
    if (!timeline->active_calls && timeline->last_ticks)
        timeline->previous_finish_ticks = timeline->last_ticks;
}

static ULONGLONG qxp_profile_timeline_span(
    const QXP_PROFILE_TIMELINE *timeline)
{
    if (!timeline || timeline->last_ticks <= timeline->first_ticks) return 0;
    return timeline->last_ticks - timeline->first_ticks;
}

#endif
