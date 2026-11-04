#ifndef QXP_UT2003_LOG_H
#define QXP_UT2003_LOG_H

/* UT2003 gameplay-log ordering and normal-menu observation timing. */
#define QXP_UT2003_MENU_OBSERVE_MS 180000UL
#define QXP_UT2003_MENU_STARTUP_TIMEOUT_MS 300000UL
#define QXP_UT2003_MENU_POLL_SLACK_MS 1000UL
typedef struct qxp_ut2003_log_state {
    unsigned gameplay_phase;
} QXP_UT2003_LOG_STATE;

enum {
    QXP_UT2003_GAMEPLAY_NONE = 0,
    QXP_UT2003_GAMEPLAY_BROWSED_ANTALUS = 1,
    QXP_UT2003_GAMEPLAY_LOADED_ANTALUS = 2,
    QXP_UT2003_GAMEPLAY_DEATHMATCH_CLASS = 3,
    QXP_UT2003_GAMEPLAY_ANTALUS_LEVEL = 4,
    QXP_UT2003_GAMEPLAY_READY = 5
};

void qxp_ut2003_log_init(QXP_UT2003_LOG_STATE *state);
void qxp_ut2003_log_consume(QXP_UT2003_LOG_STATE *state,
                            const char *line);
int qxp_ut2003_log_gameplay_ready(
    const QXP_UT2003_LOG_STATE *state);
int qxp_ut2003_visual_should_stop(unsigned long launch_elapsed_ms,
                                  int visual_ready,
                                  unsigned long visual_elapsed_ms);

#endif
