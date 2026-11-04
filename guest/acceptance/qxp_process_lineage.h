#ifndef QXP_PROCESS_LINEAGE_H
#define QXP_PROCESS_LINEAGE_H

/* A same-named process is attributable to the launched stub only when it is a
 * direct successor created no earlier than that stub.  Exclusions prevent the
 * active or already-retained process from being selected again. */
static inline int QxpProcessIsOwnedSuccessor(
    unsigned long launcher_process_id,
    unsigned long candidate_process_id,
    unsigned long candidate_parent_process_id,
    unsigned long active_process_id,
    unsigned long retained_process_id,
    int executable_matches,
    unsigned long long launcher_creation_stamp,
    unsigned long long candidate_creation_stamp)
{
    return launcher_process_id != 0 && candidate_process_id != 0 &&
        candidate_process_id != active_process_id &&
        candidate_process_id != retained_process_id && executable_matches &&
        candidate_parent_process_id == launcher_process_id &&
        launcher_creation_stamp != 0 && candidate_creation_stamp != 0 &&
        candidate_creation_stamp >= launcher_creation_stamp;
}

/* Fail-closed descendant ownership for observer process handoff.  A
 * candidate belongs to the observed launch only when its ancestor chain
 * reaches a process the observation already owns (the launcher or a
 * previously adopted successor) without crossing a process that predates
 * the launcher or is absent from the snapshot.  The table_* arrays describe
 * one toolhelp snapshot: entry i is process table_process_ids[i] with parent
 * table_parent_process_ids[i] and creation stamp table_creation_stamps[i]
 * (0 when the stamp could not be read, which fails closed).  A candidate
 * already owned by the observation is rejected so an exited-pid reuse can
 * never be re-adopted.  The walk is bounded by table_count, so a parent
 * cycle with no attributable successor exhausts the bound and fails instead
 * of inheriting another process. */
static inline int QxpProcessIsOwnedDescendant(
    unsigned long candidate_process_id,
    unsigned long long candidate_creation_stamp,
    const unsigned long *table_process_ids,
    const unsigned long *table_parent_process_ids,
    const unsigned long long *table_creation_stamps,
    unsigned table_count,
    const unsigned long *owned_process_ids,
    unsigned owned_count,
    unsigned long long launcher_creation_stamp)
{
    unsigned long current;
    unsigned depth;
    unsigned index;

    if (!candidate_process_id || !launcher_creation_stamp ||
        !candidate_creation_stamp ||
        candidate_creation_stamp < launcher_creation_stamp) return 0;
    for (index = 0; index < owned_count; ++index) {
        if (candidate_process_id == owned_process_ids[index]) return 0;
    }
    for (index = 0; index < table_count; ++index) {
        if (table_process_ids[index] == candidate_process_id) break;
    }
    if (index == table_count) return 0;
    current = table_parent_process_ids[index];
    for (depth = 0; depth < table_count; ++depth) {
        if (!current) return 0;
        for (index = 0; index < owned_count; ++index) {
            if (current == owned_process_ids[index]) return 1;
        }
        for (index = 0; index < table_count; ++index) {
            if (table_process_ids[index] == current) break;
        }
        if (index == table_count || !table_creation_stamps[index] ||
            table_creation_stamps[index] < launcher_creation_stamp) return 0;
        current = table_parent_process_ids[index];
    }
    return 0;
}

#endif
