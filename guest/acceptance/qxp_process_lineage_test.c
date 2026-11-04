#include <stdio.h>

#include "qxp_process_lineage.h"

static int require(int condition, const char *message)
{
    if (condition) return 1;
    fprintf(stderr, "FAIL: %s\n", message);
    return 0;
}

int main(void)
{
    if (!require(QxpProcessIsOwnedSuccessor(
                     100, 101, 100, 100, 0, 1, 1000, 1001),
                 "direct post-launch child was rejected") ||
        !require(QxpProcessIsOwnedSuccessor(
                     100, 101, 100, 100, 0, 1, 1000, 1000),
                 "equal creation boundary was rejected") ||
        !require(!QxpProcessIsOwnedSuccessor(
                     100, 101, 99, 100, 0, 1, 1000, 1001),
                 "foreign-parent process was accepted") ||
        !require(!QxpProcessIsOwnedSuccessor(
                     100, 101, 100, 100, 0, 1, 1000, 999),
                 "pre-launch process was accepted") ||
        !require(!QxpProcessIsOwnedSuccessor(
                     100, 100, 100, 100, 0, 1, 1000, 1001),
                 "active process was re-adopted") ||
        !require(!QxpProcessIsOwnedSuccessor(
                     100, 101, 100, 99, 101, 1, 1000, 1001),
                 "retained process was re-adopted") ||
        !require(!QxpProcessIsOwnedSuccessor(
                     100, 101, 100, 100, 0, 0, 1000, 1001),
                 "wrong executable was accepted") ||
        !require(!QxpProcessIsOwnedSuccessor(
                     0, 101, 100, 100, 0, 1, 1000, 1001),
                 "zero launcher identity was accepted") ||
        !require(!QxpProcessIsOwnedSuccessor(
                     100, 101, 100, 100, 0, 1, 0, 1001),
                 "missing launcher creation time was accepted")) return 1;
    /* Launcher 100 created at stamp 1000; the snapshot below mixes a live
     * launcher, a legitimate post-launch child/grandchild/great-grandchild,
     * and every shape that must fail closed: a pre-existing healthy
     * executable, a parent cycle, foreign or pre-launch ancestry, and an
     * already-owned pid. */
    {
        static const unsigned long pids[] = {
            100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110
        };
        static const unsigned long parents[] = {
            4, 100, 101, 999, 100, 106, 105, 108, 100, 4, 102
        };
        static const unsigned long long stamps[] = {
            1000, 1001, 1002, 1003, 999, 1004, 1005, 1006, 500, 100, 1007
        };
        static const unsigned long owned[] = { 100 };
        static const unsigned long handoff_pids[] = { 111 };
        static const unsigned long handoff_parents[] = { 101 };
        static const unsigned long long handoff_stamps[] = { 1002 };
        static const unsigned long handoff_owned[] = { 100, 101 };
        unsigned count = sizeof(pids) / sizeof(pids[0]);

        if (!require(QxpProcessIsOwnedDescendant(
                         101, 1001, pids, parents, stamps, count,
                         owned, 1, 1000),
                     "direct post-launch child was rejected") ||
            !require(QxpProcessIsOwnedDescendant(
                         102, 1002, pids, parents, stamps, count,
                         owned, 1, 1000),
                     "post-launch grandchild was rejected") ||
            !require(QxpProcessIsOwnedDescendant(
                         110, 1007, pids, parents, stamps, count,
                         owned, 1, 1000),
                     "deep post-launch descendant was rejected") ||
            !require(QxpProcessIsOwnedDescendant(
                         101, 1000, pids, parents, stamps, count,
                         owned, 1, 1000),
                     "equal creation boundary descendant was rejected") ||
            !require(QxpProcessIsOwnedDescendant(
                         111, 1002, handoff_pids, handoff_parents,
                         handoff_stamps, 1, handoff_owned, 2, 1000),
                     "descendant of exited adopted process was rejected") ||
            !require(!QxpProcessIsOwnedDescendant(
                         103, 1003, pids, parents, stamps, count,
                         owned, 1, 1000),
                     "descendant with foreign ancestry was accepted") ||
            !require(!QxpProcessIsOwnedDescendant(
                         104, 999, pids, parents, stamps, count,
                         owned, 1, 1000),
                     "pre-launch child was accepted") ||
            !require(!QxpProcessIsOwnedDescendant(
                         105, 1004, pids, parents, stamps, count,
                         owned, 1, 1000),
                     "parent-cycle process inherited ownership") ||
            !require(!QxpProcessIsOwnedDescendant(
                         106, 1005, pids, parents, stamps, count,
                         owned, 1, 1000),
                     "parent-cycle counterpart inherited ownership") ||
            !require(!QxpProcessIsOwnedDescendant(
                         107, 1006, pids, parents, stamps, count,
                         owned, 1, 1000),
                     "descendant through pre-launch ancestor was accepted") ||
            !require(!QxpProcessIsOwnedDescendant(
                         108, 500, pids, parents, stamps, count,
                         owned, 1, 1000),
                     "pre-launch ancestor was adopted") ||
            !require(!QxpProcessIsOwnedDescendant(
                         109, 100, pids, parents, stamps, count,
                         owned, 1, 1000),
                     "healthy pre-existing executable was adopted") ||
            !require(!QxpProcessIsOwnedDescendant(
                         100, 1000, pids, parents, stamps, count,
                         owned, 1, 1000),
                     "owned launcher pid was re-adopted after reuse") ||
            !require(!QxpProcessIsOwnedDescendant(
                         0, 1001, pids, parents, stamps, count,
                         owned, 1, 1000),
                     "zero candidate identity was accepted") ||
            !require(!QxpProcessIsOwnedDescendant(
                         101, 0, pids, parents, stamps, count,
                         owned, 1, 1000),
                     "missing candidate creation time was accepted") ||
            !require(!QxpProcessIsOwnedDescendant(
                         101, 1001, pids, parents, stamps, count,
                         owned, 1, 0),
                     "missing launcher creation time was accepted") ||
            !require(!QxpProcessIsOwnedDescendant(
                         999, 1001, pids, parents, stamps, count,
                         owned, 1, 1000),
                     "candidate outside the snapshot was accepted"))
            return 1;
    }
    puts("Max Payne process lineage tests passed");
    return 0;
}
