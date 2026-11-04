#ifndef QXP_CONTEXT_LIFETIME_H
#define QXP_CONTEXT_LIFETIME_H

/* Callers serialize every operation on this state with their object-table
 * lock.  The helper deliberately contains no platform synchronization so the
 * same transition contract can be exercised by a native deterministic test. */
typedef struct qxp_context_lifetime {
    unsigned long references;
    unsigned long destroy_pending;
    unsigned long publication_state;
} QXP_CONTEXT_LIFETIME;

#define QXP_CONTEXT_SLOT_FREE 0UL
#define QXP_CONTEXT_SLOT_RESERVED 1UL
#define QXP_CONTEXT_SLOT_PUBLISHED 2UL

int qxp_context_lifetime_is_free(const QXP_CONTEXT_LIFETIME *lifetime);
int qxp_context_lifetime_is_reserved(const QXP_CONTEXT_LIFETIME *lifetime);
int qxp_context_lifetime_is_published(const QXP_CONTEXT_LIFETIME *lifetime);
int qxp_context_lifetime_reserve(QXP_CONTEXT_LIFETIME *lifetime);
int qxp_context_lifetime_publish(QXP_CONTEXT_LIFETIME *lifetime);
int qxp_context_lifetime_rollback(QXP_CONTEXT_LIFETIME *lifetime);
int qxp_context_lifetime_try_acquire(QXP_CONTEXT_LIFETIME *lifetime);
int qxp_context_lifetime_begin_destroy(QXP_CONTEXT_LIFETIME *lifetime);
int qxp_context_lifetime_release(QXP_CONTEXT_LIFETIME *lifetime);

#endif
