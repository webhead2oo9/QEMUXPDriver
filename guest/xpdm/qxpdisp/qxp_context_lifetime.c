#include "qxp_context_lifetime.h"

int qxp_context_lifetime_is_free(const QXP_CONTEXT_LIFETIME *lifetime)
{
    return lifetime && lifetime->references == 0 &&
           lifetime->destroy_pending == 0 &&
           lifetime->publication_state == QXP_CONTEXT_SLOT_FREE;
}

int qxp_context_lifetime_is_reserved(const QXP_CONTEXT_LIFETIME *lifetime)
{
    return lifetime && lifetime->references == 0 &&
           lifetime->destroy_pending == 0 &&
           lifetime->publication_state == QXP_CONTEXT_SLOT_RESERVED;
}

int qxp_context_lifetime_is_published(const QXP_CONTEXT_LIFETIME *lifetime)
{
    return lifetime &&
           lifetime->publication_state == QXP_CONTEXT_SLOT_PUBLISHED;
}

int qxp_context_lifetime_reserve(QXP_CONTEXT_LIFETIME *lifetime)
{
    if (!qxp_context_lifetime_is_free(lifetime)) return 0;
    lifetime->publication_state = QXP_CONTEXT_SLOT_RESERVED;
    return 1;
}

int qxp_context_lifetime_publish(QXP_CONTEXT_LIFETIME *lifetime)
{
    if (!qxp_context_lifetime_is_reserved(lifetime)) return 0;
    lifetime->publication_state = QXP_CONTEXT_SLOT_PUBLISHED;
    return 1;
}

int qxp_context_lifetime_rollback(QXP_CONTEXT_LIFETIME *lifetime)
{
    if (!qxp_context_lifetime_is_reserved(lifetime)) return 0;
    lifetime->publication_state = QXP_CONTEXT_SLOT_FREE;
    return 1;
}

int qxp_context_lifetime_try_acquire(QXP_CONTEXT_LIFETIME *lifetime)
{
    if (!qxp_context_lifetime_is_published(lifetime) ||
        lifetime->destroy_pending ||
        lifetime->references == ~0UL) return 0;
    ++lifetime->references;
    return 1;
}

int qxp_context_lifetime_begin_destroy(QXP_CONTEXT_LIFETIME *lifetime)
{
    if (!qxp_context_lifetime_is_published(lifetime)) return 0;
    lifetime->destroy_pending = 1;
    return lifetime->references == 0;
}

int qxp_context_lifetime_release(QXP_CONTEXT_LIFETIME *lifetime)
{
    if (!qxp_context_lifetime_is_published(lifetime) ||
        !lifetime->references) return 0;
    --lifetime->references;
    return lifetime->destroy_pending && lifetime->references == 0;
}
