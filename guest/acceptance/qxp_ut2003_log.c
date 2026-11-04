#include "qxp_ut2003_log.h"

#include <stddef.h>

static int qxp_ascii_prefix(const char *text, const char *prefix)
{
    size_t index;

    if (!text || !prefix) return 0;
    for (index = 0; prefix[index]; ++index) {
        char left = text[index];
        char right = prefix[index];

        if (!left) return 0;
        if (left >= 'A' && left <= 'Z') left = (char)(left - 'A' + 'a');
        if (right >= 'A' && right <= 'Z')
            right = (char)(right - 'A' + 'a');
        if (left != right) return 0;
    }
    return 1;
}

static int qxp_ascii_equal(const char *text, const char *expected)
{
    size_t length;

    if (!qxp_ascii_prefix(text, expected)) return 0;
    for (length = 0; expected[length]; ++length) { }
    return text[length] == '\0';
}

static int qxp_map_prefix(const char *line, const char *prefix)
{
    size_t length;
    char next;

    if (!qxp_ascii_prefix(line, prefix)) return 0;
    for (length = 0; prefix[length]; ++length) { }
    next = line[length];
    return !next || next == '?' || next == ' ';
}

void qxp_ut2003_log_init(QXP_UT2003_LOG_STATE *state)
{
    if (!state) return;
    state->gameplay_phase = QXP_UT2003_GAMEPLAY_NONE;
}

void qxp_ut2003_log_consume(QXP_UT2003_LOG_STATE *state,
                            const char *line)
{
    static const char browse[] = "Log: Browse: ";
    static const char browse_antalus[] = "Log: Browse: DM-Antalus";
    static const char load[] = "Log: LoadMap: ";
    static const char load_antalus[] = "Log: LoadMap: DM-Antalus";
    static const char game_class_prefix[] = "Log: Game class is ";
    static const char deathmatch_class[] =
        "Log: Game class is 'xDeathMatch'";
    static const char level_prefix[] = "Log: Level is Level ";
    static const char antalus_level[] =
        "Log: Level is Level DM-Antalus.myLevel";
    static const char bring_prefix[] = "Log: Bringing Level ";
    static const char bring_antalus[] =
        "Log: Bringing Level DM-Antalus.myLevel up for play (";

    if (!state || !line) return;
    if (qxp_ascii_prefix(line, browse)) {
        state->gameplay_phase =
            qxp_map_prefix(line, browse_antalus) ?
                QXP_UT2003_GAMEPLAY_BROWSED_ANTALUS :
                QXP_UT2003_GAMEPLAY_NONE;
        return;
    }
    if (qxp_ascii_prefix(line, load)) {
        state->gameplay_phase =
            state->gameplay_phase ==
                QXP_UT2003_GAMEPLAY_BROWSED_ANTALUS &&
            qxp_map_prefix(line, load_antalus) ?
                QXP_UT2003_GAMEPLAY_LOADED_ANTALUS :
                QXP_UT2003_GAMEPLAY_NONE;
        return;
    }
    if (qxp_ascii_prefix(line, game_class_prefix)) {
        state->gameplay_phase =
            state->gameplay_phase ==
                QXP_UT2003_GAMEPLAY_LOADED_ANTALUS &&
            qxp_ascii_equal(line, deathmatch_class) ?
                QXP_UT2003_GAMEPLAY_DEATHMATCH_CLASS :
                QXP_UT2003_GAMEPLAY_NONE;
        return;
    }
    if (qxp_ascii_prefix(line, level_prefix)) {
        state->gameplay_phase =
            state->gameplay_phase ==
                QXP_UT2003_GAMEPLAY_DEATHMATCH_CLASS &&
            qxp_ascii_equal(line, antalus_level) ?
                QXP_UT2003_GAMEPLAY_ANTALUS_LEVEL :
                QXP_UT2003_GAMEPLAY_NONE;
        return;
    }
    if (qxp_ascii_prefix(line, bring_prefix)) {
        state->gameplay_phase =
            state->gameplay_phase ==
                QXP_UT2003_GAMEPLAY_ANTALUS_LEVEL &&
            qxp_ascii_prefix(line, bring_antalus) ?
                QXP_UT2003_GAMEPLAY_READY :
                QXP_UT2003_GAMEPLAY_NONE;
    }
}

int qxp_ut2003_log_gameplay_ready(
    const QXP_UT2003_LOG_STATE *state)
{
    return state &&
           state->gameplay_phase == QXP_UT2003_GAMEPLAY_READY;
}

int qxp_ut2003_visual_should_stop(unsigned long launch_elapsed_ms,
                                  int visual_ready,
                                  unsigned long visual_elapsed_ms)
{
    if (visual_ready)
        return visual_elapsed_ms >= QXP_UT2003_MENU_OBSERVE_MS;
    return launch_elapsed_ms >= QXP_UT2003_MENU_STARTUP_TIMEOUT_MS +
                                QXP_UT2003_MENU_POLL_SLACK_MS;
}
