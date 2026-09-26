#ifndef SANA2_PORTLOCK_H
#define SANA2_PORTLOCK_H
/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Take SANA-II requests off a unit's request ports safely on SMP.
*/

/*
 * A SANA-II driver's request ports are filled by PutMsg() from the network
 * stack's task, while the driver's interrupts and softints take requests off
 * them. On SMP those run on different cores: PutMsg() and GetMsg() serialise
 * on the port's spinlock, and Disable() only stops the core it runs on. A
 * driver that peeks mp_MsgList.lh_Head and Remove()s under Disable() alone
 * tears the list - seen as a Tx producer calling a garbage opener's
 * tx_function and an idle core dying with a #GP at RIP 0x3 (e1000), and as
 * uploads wedging (pcnet32).
 *
 * So: take write and orphan requests with GetMsg(), take read requests with
 * TakeMatchingRequest(), and walk any other port list between PortLock() and
 * PortUnlock(). Never hold the lock across an opener callback (rx_function /
 * tx_function take semaphores) or a ReplyMsg() (which locks the reply port).
 */

#include <exec/types.h>
#include <exec/ports.h>
#include <devices/sana2.h>
#include <proto/exec.h>
#include <proto/kernel.h>

static inline APTR Sana2PortKernelBase(void)
{
    static APTR kernelbase;

    if (!kernelbase)
        kernelbase = OpenResource("kernel.resource");
    return kernelbase;
}

static inline void PortLock(struct MsgPort *port)
{
    Disable();
#if defined(__AROSPLATFORM_SMP__)
    {
        APTR KernelBase = Sana2PortKernelBase();
#if defined(__AROSEXEC_SMP__)
        KrnSpinLock(&port->mp_SpinLock, NULL, SPINLOCK_MODE_WRITE);
#else
        KrnSpinLock(&port->mp_Pad, NULL, SPINLOCK_MODE_WRITE);
#endif
    }
#endif
}

static inline void PortUnlock(struct MsgPort *port)
{
#if defined(__AROSPLATFORM_SMP__)
    {
        APTR KernelBase = Sana2PortKernelBase();
#if defined(__AROSEXEC_SMP__)
        KrnSpinUnLock(&port->mp_SpinLock);
#else
        KrnSpinUnLock(&port->mp_Pad);
#endif
    }
#endif
    Enable();
}

/*
 * Take the first read request a packet of packet_type satisfies off the port,
 * or NULL. With lengthtypes, any request for an 802.3 length-field type
 * (<= maxlength) also takes an 802.3 packet.
 */
static inline struct IOSana2Req *TakeMatchingRequest(struct MsgPort *port,
    ULONG packet_type, BOOL lengthtypes, ULONG maxlength)
{
    struct IOSana2Req *request, *found = NULL;

    PortLock(port);
    for (request = (APTR)port->mp_MsgList.lh_Head;
         request->ios2_Req.io_Message.mn_Node.ln_Succ != NULL;
         request = (APTR)request->ios2_Req.io_Message.mn_Node.ln_Succ)
    {
        if ((request->ios2_PacketType == packet_type) ||
            (lengthtypes && (request->ios2_PacketType <= maxlength) &&
             (packet_type <= maxlength)))
        {
            Remove((APTR)request);
            found = request;
            break;
        }
    }
    PortUnlock(port);

    return found;
}

/*
 * Put a taken request back at the head of its port: when the opener's packet
 * filter rejected the packet, or the hardware had no room to send it, the
 * request stays queued for the next attempt.
 */
static inline void RequeueRequest(struct MsgPort *port,
    struct IOSana2Req *request)
{
    PortLock(port);
    AddHead(&port->mp_MsgList, (APTR)request);
    PortUnlock(port);
}

/*
 * One Tx producer at a time. A driver's Tx softint is Cause()d by PutMsg()
 * on whichever core the network stack runs on, and by its own Tx-done path,
 * so on SMP it can run on two cores at once: both read the ring index, fill
 * the same slot and send one request twice. Disable() stops one core only,
 * and no spinlock may be held across the opener's tx_function (it takes
 * semaphores). So a second caller only records that there is more to send,
 * and the producer in progress drains again before it finishes:
 *
 *     if (!Sana2TxGateEnter(&unit->tx_gate))
 *         return FALSE;
 *     do
 *     {
 *         ...drain the write port...
 *     }
 *     while (Sana2TxGateLeave(&unit->tx_gate));
 *
 * The caller marks the request before trying the gate and the producer
 * reopens the gate before it looks, so no request is left behind.
 */
struct Sana2TxGate
{
    volatile ULONG busy;
    volatile ULONG again;
};

/* TRUE: the caller is now the producer, and must call Sana2TxGateLeave() */
static inline BOOL Sana2TxGateEnter(struct Sana2TxGate *gate)
{
    __atomic_store_n(&gate->again, 1, __ATOMIC_SEQ_CST);
    if (__atomic_exchange_n(&gate->busy, 1, __ATOMIC_SEQ_CST) != 0)
        return FALSE;
    __atomic_store_n(&gate->again, 0, __ATOMIC_SEQ_CST);
    return TRUE;
}

/* TRUE: work arrived on another core meanwhile - drain again */
static inline BOOL Sana2TxGateLeave(struct Sana2TxGate *gate)
{
    __atomic_store_n(&gate->busy, 0, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&gate->again, __ATOMIC_SEQ_CST) != 0 &&
        __atomic_exchange_n(&gate->busy, 1, __ATOMIC_SEQ_CST) == 0)
    {
        __atomic_store_n(&gate->again, 0, __ATOMIC_SEQ_CST);
        return TRUE;
    }
    return FALSE;
}

/*
 * Take every request on the port whose ios2_WireError has any of events set,
 * mark it with events and reply it - the replies after the lock is released.
 */
static inline void ReplyEventRequests(struct MsgPort *port, ULONG events)
{
    struct IOSana2Req *request, *next_request;
    struct List matched;

    NEWLIST(&matched);

    PortLock(port);
    for (request = (APTR)port->mp_MsgList.lh_Head;
         (next_request = (APTR)request->ios2_Req.io_Message.mn_Node.ln_Succ) != NULL;
         request = next_request)
    {
        if ((request->ios2_WireError & events) != 0)
        {
            request->ios2_WireError = events;
            Remove((APTR)request);
            AddTail(&matched, (APTR)request);
        }
    }
    PortUnlock(port);

    while ((request = (APTR)RemHead(&matched)) != NULL)
        ReplyMsg((APTR)request);
}

#endif /* SANA2_PORTLOCK_H */
