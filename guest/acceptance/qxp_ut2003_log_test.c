#include "qxp_ut2003_log.h"

#include <stdio.h>
#include <stdlib.h>

static void require(int condition, const char *message)
{
    if (condition) return;
    fprintf(stderr, "qxp_ut2003_log_test: %s\n", message);
    exit(1);
}

static void consume(QXP_UT2003_LOG_STATE *state,
                    const char *const *lines, unsigned count)
{
    unsigned index;

    qxp_ut2003_log_init(state);
    for (index = 0; index < count; ++index)
        qxp_ut2003_log_consume(state, lines[index]);
}

int main(void)
{
    static const char *const valid_gameplay[] = {
        "Log: Browse: DM-Antalus?Name=Player?game=XGame.xDeathMatch",
        "Log: LoadMap: DM-Antalus?Name=Player?game=XGame.xDeathMatch",
        "Log: Static mesh batches: 2745936 vertex bytes",
        "Log: Game class is 'xDeathMatch'",
        "Log: Level is Level DM-Antalus.myLevel",
        "Log: Bringing Level DM-Antalus.myLevel up for play (0) appSeconds: 0.000005..."
    };
    static const char *const gameplay_missing_load[] = {
        "Log: Browse: DM-Antalus?game=XGame.xDeathMatch",
        "Log: Game class is 'xDeathMatch'",
        "Log: Level is Level DM-Antalus.myLevel",
        "Log: Bringing Level DM-Antalus.myLevel up for play (0)"
    };
    static const char *const gameplay_wrong_class[] = {
        "Log: Browse: DM-Antalus",
        "Log: LoadMap: DM-Antalus",
        "Log: Game class is 'GameInfo'",
        "Log: Level is Level DM-Antalus.myLevel",
        "Log: Bringing Level DM-Antalus.myLevel up for play (0)"
    };
    static const char *const gameplay_wrong_level[] = {
        "Log: Browse: DM-Antalus",
        "Log: LoadMap: DM-Antalus",
        "Log: Game class is 'xDeathMatch'",
        "Log: Level is Level DM-Asbestos.myLevel",
        "Log: Bringing Level DM-Antalus.myLevel up for play (0)"
    };
    static const char *const gameplay_wrong_order[] = {
        "Log: LoadMap: DM-Antalus",
        "Log: Browse: DM-Antalus",
        "Log: Game class is 'xDeathMatch'",
        "Log: Level is Level DM-Antalus.myLevel",
        "Log: Bringing Level DM-Antalus.myLevel up for play (0)"
    };
    static const char *const gameplay_case_insensitive[] = {
        "log: browse: dm-antalus?game=xgame.xdeathmatch",
        "LOG: LOADMAP: DM-ANTALUS",
        "log: game class is 'XDEATHMATCH'",
        "LOG: LEVEL IS LEVEL DM-ANTALUS.MYLEVEL",
        "log: bringing level dm-antalus.mylevel up for play (0)"
    };
    QXP_UT2003_LOG_STATE state;

    consume(&state, valid_gameplay,
            sizeof(valid_gameplay) / sizeof(valid_gameplay[0]));
    require(qxp_ut2003_log_gameplay_ready(&state),
            "observed DM-Antalus startup sequence was rejected");
    consume(&state, gameplay_missing_load,
            sizeof(gameplay_missing_load) / sizeof(gameplay_missing_load[0]));
    require(!qxp_ut2003_log_gameplay_ready(&state),
            "gameplay sequence without LoadMap was accepted");
    consume(&state, gameplay_wrong_class,
            sizeof(gameplay_wrong_class) / sizeof(gameplay_wrong_class[0]));
    require(!qxp_ut2003_log_gameplay_ready(&state),
            "non-deathmatch game class was accepted");
    consume(&state, gameplay_wrong_level,
            sizeof(gameplay_wrong_level) / sizeof(gameplay_wrong_level[0]));
    require(!qxp_ut2003_log_gameplay_ready(&state),
            "wrong gameplay level was accepted");
    consume(&state, gameplay_wrong_order,
            sizeof(gameplay_wrong_order) / sizeof(gameplay_wrong_order[0]));
    require(!qxp_ut2003_log_gameplay_ready(&state),
            "out-of-order gameplay startup was accepted");
    consume(&state, gameplay_case_insensitive,
            sizeof(gameplay_case_insensitive) /
                sizeof(gameplay_case_insensitive[0]));
    require(qxp_ut2003_log_gameplay_ready(&state),
            "case-insensitive gameplay startup was rejected");

    require(!qxp_ut2003_visual_should_stop(300999UL, 0, 0UL),
            "visual-ready wait stopped before startup timeout plus slack");
    require(qxp_ut2003_visual_should_stop(301000UL, 0, 0UL),
            "missing visual-ready boundary did not stop after polling slack");
    require(!qxp_ut2003_visual_should_stop(600000UL, 1, 179999UL),
            "late visual-ready boundary used the launch-relative deadline");
    require(qxp_ut2003_visual_should_stop(600000UL, 1, 180000UL),
            "full post-ready visual dwell did not stop observation");

    puts("UT2003 Demo 2206 gameplay-log and visual-timing tests passed");
    return 0;
}
