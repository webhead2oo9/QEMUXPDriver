#include <stdio.h>
#include <string.h>

#include "qxpdisp/qxp_context_lifetime.h"

static int require(int condition, const char *message)
{
    if (condition) return 1;
    fprintf(stderr, "context lifetime test failed: %s\n", message);
    return 0;
}

int main(void)
{
    QXP_CONTEXT_LIFETIME lifetime;
    unsigned long failure_step;

    memset(&lifetime, 0, sizeof(lifetime));
    if (!require(qxp_context_lifetime_is_free(&lifetime),
                 "zeroed slot must be free")) return 1;
    if (!require(qxp_context_lifetime_reserve(&lifetime),
                 "free slot reservation")) return 1;
    if (!require(qxp_context_lifetime_is_reserved(&lifetime) &&
                 !qxp_context_lifetime_is_free(&lifetime) &&
                 !qxp_context_lifetime_is_published(&lifetime),
                 "reserved slot state")) return 1;
    if (!require(!qxp_context_lifetime_reserve(&lifetime),
                 "reserved slot must reject reuse")) return 1;
    if (!require(!qxp_context_lifetime_try_acquire(&lifetime),
                 "reserved slot must reject dispatch")) return 1;
    if (!require(!qxp_context_lifetime_begin_destroy(&lifetime),
                 "reserved slot must reject public destroy")) return 1;
    if (!require(qxp_context_lifetime_rollback(&lifetime) &&
                 qxp_context_lifetime_is_free(&lifetime),
                 "reserved slot rollback")) return 1;
    if (!require(!qxp_context_lifetime_rollback(&lifetime),
                 "free slot must reject duplicate rollback")) return 1;

    /* Transport-open, texture-upload, and target-bind failures all converge on
     * the same unpublished rollback.  At every boundary the slot must remain
     * invisible and become immediately reusable after cleanup. */
    for (failure_step = 0; failure_step < 3; ++failure_step) {
        if (!require(qxp_context_lifetime_reserve(&lifetime),
                     "failure-injection reservation")) return 1;
        if (!require(!qxp_context_lifetime_try_acquire(&lifetime) &&
                     !qxp_context_lifetime_begin_destroy(&lifetime),
                     "failure-injection slot invisibility")) return 1;
        if (!require(qxp_context_lifetime_rollback(&lifetime) &&
                     qxp_context_lifetime_is_free(&lifetime),
                     "failure-injection rollback and reuse")) return 1;
    }

    if (!require(qxp_context_lifetime_reserve(&lifetime) &&
                 qxp_context_lifetime_publish(&lifetime),
                 "successful reservation publication")) return 1;
    if (!require(qxp_context_lifetime_is_published(&lifetime) &&
                 !qxp_context_lifetime_is_reserved(&lifetime),
                 "published slot state")) return 1;
    if (!require(!qxp_context_lifetime_publish(&lifetime) &&
                 !qxp_context_lifetime_rollback(&lifetime),
                 "published slot rejects duplicate publish and rollback")) {
        return 1;
    }

    /* A normal completed dispatch must never authorize teardown while the
     * context remains live. */
    if (!require(qxp_context_lifetime_try_acquire(&lifetime),
                 "live dispatch acquisition")) return 1;
    if (!require(!qxp_context_lifetime_release(&lifetime),
                 "live dispatch release must not authorize teardown")) {
        return 1;
    }

    /* Deterministic lookup -> unlock -> destroy -> use interleave.  Two
     * dispatches own pins, destruction becomes pending, the slot rejects a
     * third dispatch, and only the final release authorizes teardown/reuse. */
    if (!require(qxp_context_lifetime_try_acquire(&lifetime) &&
                 qxp_context_lifetime_try_acquire(&lifetime),
                 "two dispatch acquisitions")) return 1;
    if (!require(!qxp_context_lifetime_begin_destroy(&lifetime),
                 "destroy must defer while dispatches are pinned")) return 1;
    if (!require(!qxp_context_lifetime_try_acquire(&lifetime),
                 "pending destroy must reject a new dispatch")) return 1;
    if (!require(lifetime.references == 2,
                 "rejected acquisition must not change the pin count")) {
        return 1;
    }
    if (!require(!qxp_context_lifetime_release(&lifetime),
                 "non-final dispatch release must defer teardown")) return 1;
    if (!require(lifetime.references == 1,
                 "non-final release must retain the last pin")) return 1;
    if (!require(qxp_context_lifetime_release(&lifetime),
                 "last dispatch release must authorize teardown")) return 1;
    if (!require(lifetime.references == 0,
                 "last release must drain the pin count")) return 1;
    if (!require(!qxp_context_lifetime_release(&lifetime),
                 "release underflow must fail closed")) return 1;

    memset(&lifetime, 0, sizeof(lifetime));
    if (!require(qxp_context_lifetime_reserve(&lifetime) &&
                 qxp_context_lifetime_publish(&lifetime),
                 "idle destroy setup")) return 1;
    if (!require(qxp_context_lifetime_begin_destroy(&lifetime),
                 "idle destroy must authorize immediate teardown")) return 1;
    if (!require(!qxp_context_lifetime_try_acquire(&lifetime),
                 "destroyed idle slot must reject acquisition")) return 1;

    memset(&lifetime, 0, sizeof(lifetime));
    if (!require(qxp_context_lifetime_reserve(&lifetime) &&
                 qxp_context_lifetime_publish(&lifetime),
                 "overflow setup")) return 1;
    lifetime.references = ~0UL;
    if (!require(!qxp_context_lifetime_try_acquire(&lifetime),
                 "reference overflow must fail closed")) return 1;

    memset(&lifetime, 0xff, sizeof(lifetime));
    if (!require(!qxp_context_lifetime_is_free(&lifetime) &&
                 !qxp_context_lifetime_reserve(&lifetime) &&
                 !qxp_context_lifetime_publish(&lifetime) &&
                 !qxp_context_lifetime_rollback(&lifetime),
                 "corrupt state must fail closed")) return 1;

    puts("context publication and lifetime tests passed");
    return 0;
}
