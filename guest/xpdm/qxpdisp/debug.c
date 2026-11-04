#include "qxpdisp.h"

static volatile LONG qxp_debug_lock;

static VOID qxp_debug_acquire(VOID)
{
    volatile LONG *guard = &qxp_debug_lock;

    __asm {
        mov ecx, guard
qxp_debug_lock_retry:
        mov eax, 1
        xchg eax, dword ptr [ecx]
        test eax, eax
        jz qxp_debug_lock_acquired
        rep nop
        jmp qxp_debug_lock_retry
qxp_debug_lock_acquired:
    }
}

static VOID qxp_debug_release(VOID)
{
    volatile LONG *guard = &qxp_debug_lock;

    __asm {
        mov ecx, guard
        xor eax, eax
        xchg eax, dword ptr [ecx]
    }
}

/* QEMU's debugcon answers a read with 0xE9; an unassigned port floats to
 * 0xFF.  Probing once lets an ordinary run drop -debugcon and pay nothing:
 * each character below is an OUT, and an OUT is a VM exit that costs far more
 * than the drawing it describes.  Racing probes compute the same answer, so
 * the unsynchronised store is deliberate. */
static LONG qxp_debug_state;

static BOOL qxp_debug_present(VOID)
{
    LONG state = qxp_debug_state;
    UCHAR probe;

    if (!state) {
        __asm {
            mov dx, 0E9h
            in al, dx
            mov probe, al
        }
        state = (probe == 0xE9) ? 1 : -1;
        qxp_debug_state = state;
    }
    return state > 0;
}

static VOID qxp_debug_write_character(UCHAR character)
{
    __asm {
        mov dx, 0E9h
        mov al, character
        out dx, al
    }
}

static VOID qxp_debug_write_message(const char *message)
{
    UCHAR character;

    while ((character = (UCHAR)*message++) != 0)
        qxp_debug_write_character(character);
    qxp_debug_write_character('\n');
}

static VOID qxp_debug_write_value(ULONG value)
{
    static const char digits[] = "0123456789ABCDEF";
    LONG shift;

    qxp_debug_write_character('0');
    qxp_debug_write_character('x');
    for (shift = 28; shift >= 0; shift -= 4) {
        qxp_debug_write_character(
            (UCHAR)digits[(value >> shift) & 0xF]);
    }
    qxp_debug_write_character('\n');
}

VOID QxpDebug(const char *message)
{
    if (!message || !qxp_debug_present()) return;
    qxp_debug_acquire();
    qxp_debug_write_message(message);
    qxp_debug_release();
}

VOID QxpDebugValue(ULONG value)
{
    if (!qxp_debug_present()) return;

    qxp_debug_acquire();
    qxp_debug_write_value(value);
    qxp_debug_release();
}

/* Emit one multi-line diagnostic while excluding every other qxpdisp debug
 * writer.  This makes the bounded gap records attributable even when DDI
 * callbacks execute concurrently. */
VOID QxpDebugRecord(const char *record, const char *event,
                    const ULONG *values, ULONG count)
{
    ULONG index;

    if (!record || !qxp_debug_present()) return;
    if (!values) count = 0;
    qxp_debug_acquire();
    qxp_debug_write_message(record);
    if (event) qxp_debug_write_message(event);
    for (index = 0; index < count; ++index)
        qxp_debug_write_value(values[index]);
    qxp_debug_release();
}

VOID QxpDebugVerbose(const char *message)
{
#ifdef QXP_VERBOSE_TRACE
    QxpDebug(message);
#else
    (void)message;
#endif
}

VOID QxpDebugVerboseValue(ULONG value)
{
#ifdef QXP_VERBOSE_TRACE
    QxpDebugValue(value);
#else
    (void)value;
#endif
}
