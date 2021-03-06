#include "futex_impl.h"
#include "pthread_impl.h"

void __pthread_testcancel(void);
int __pthread_setcancelstate(int, int*);

/*
 * struct waiter
 *
 * Waiter objects have automatic storage on the waiting thread, and
 * are used in building a linked list representing waiters currently
 * waiting on the condition variable or a group of waiters woken
 * together by a broadcast or signal; in the case of signal, this is a
 * degenerate list of one member.
 *
 * Waiter lists attached to the condition variable itself are
 * protected by the lock on the cv. Detached waiter lists are never
 * modified again, but can only be traversed in reverse order, and are
 * protected by the "barrier" locks in each node, which are unlocked
 * in turn to control wake order.
 *
 * Since process-shared cond var semantics do not necessarily allow
 * one thread to see another's automatic storage (they may be in
 * different processes), the waiter list is not used for the
 * process-shared case, but the structure is still used to store data
 * needed by the cancellation cleanup handler.
 */

struct waiter {
    struct waiter *prev, *next;
    volatile int state, barrier;
    volatile int* notify;
};

/* Self-synchronized-destruction-safe lock functions */

static inline void lock(volatile int* l) {
    if (a_cas(l, 0, 1)) {
        a_cas(l, 1, 2);
        do
            __wait(l, 0, 2);
        while (a_cas(l, 0, 2));
    }
}

static inline void unlock(volatile int* l) {
    if (a_swap(l, 0) == 2)
        __wake(l, 1);
}

static inline void unlock_requeue(volatile int* l, volatile int* r) {
    a_store(l, 0);
    _mx_futex_requeue((void*)l, /* wake count */ 0, /* l futex value */ 0,
                      (void*)r, /* requeue count */ 1);
}

enum {
    WAITING,
    SIGNALED,
    LEAVING,
};

int pthread_cond_timedwait(pthread_cond_t* restrict c, pthread_mutex_t* restrict m,
                           const struct timespec* restrict ts) {
    struct waiter node = {};
    int e, seq, clock = c->_c_clock, cs, oldstate, tmp;
    volatile int* fut;

    if ((m->_m_type & 15) && (m->_m_lock & INT_MAX) != __thread_get_tid())
        return EPERM;

    if (ts && ts->tv_nsec >= 1000000000UL)
        return EINVAL;

    __pthread_testcancel();

    lock(&c->_c_lock);

    seq = node.barrier = 2;
    fut = &node.barrier;
    node.state = WAITING;
    /* Add our waiter node onto the condvar's list.  We add the node to the
     * head of the list, but this is logically the end of the queue. */
    node.next = c->_c_head;
    c->_c_head = &node;
    if (!c->_c_tail)
        c->_c_tail = &node;
    else
        node.next->prev = &node;

    unlock(&c->_c_lock);

    pthread_mutex_unlock(m);

    __pthread_setcancelstate(PTHREAD_CANCEL_MASKED, &cs);
    if (cs == PTHREAD_CANCEL_DISABLE)
        __pthread_setcancelstate(cs, 0);

    /* Wait to be signaled.  There are multiple ways this loop could exit:
     *  1) After being woken by __private_cond_signal().
     *  2) After being woken by pthread_mutex_unlock(), after we were
     *     requeued from the condvar's futex to the mutex's futex (by
     *     pthread_cond_timedwait() in another thread).
     *  3) After a timeout.
     *  4) On Linux, interrupted by an asynchronous signal.  This does
     *     not apply on Magenta. */
    do
        e = __timedwait_cp(fut, seq, clock, ts);
    while (*fut == seq && !e);

    oldstate = a_cas(&node.state, WAITING, LEAVING);

    if (oldstate == WAITING) {
        /* The wait timed out.  So far, this thread was not signaled
         * by pthread_cond_signal()/broadcast() -- this thread was
         * able to move state.node out of the WAITING state before any
         * __private_cond_signal() call could do that.
         *
         * This thread must therefore remove the waiter node from the
         * list itself. */

        /* Access to cv object is valid because this waiter was not
         * yet signaled and a new signal/broadcast cannot return
         * after seeing a LEAVING waiter without getting notified
         * via the futex notify below. */

        lock(&c->_c_lock);

        /* Remove our waiter node from the list. */
        if (c->_c_head == &node)
            c->_c_head = node.next;
        else if (node.prev)
            node.prev->next = node.next;
        if (c->_c_tail == &node)
            c->_c_tail = node.prev;
        else if (node.next)
            node.next->prev = node.prev;

        unlock(&c->_c_lock);

        /* It is possible that __private_cond_signal() saw our waiter node
         * after we set node.state to LEAVING but before we removed the
         * node from the list.  If so, it will have set node.notify and
         * will be waiting on it, and we need to wake it up.
         *
         * This is rather complex.  An alternative would be to eliminate
         * the node.state field and always claim _c_lock if we could have
         * got a timeout.  However, that presumably has higher overhead
         * (since it contends _c_lock and involves more atomic ops). */
        if (node.notify) {
            if (a_fetch_add(node.notify, -1) == 1)
                __wake(node.notify, 1);
        }
    } else {
        /* This thread was at least partially signaled by
         * pthread_cond_signal()/broadcast().  That might have raced
         * with a timeout, so we need to wait for this thread to be
         * fully signaled.  We need to wait until another thread sets
         * node.barrier to 0.  (This lock() call will also set
         * node.barrier to non-zero, but that side effect is
         * unnecessary here.) */
        lock(&node.barrier);
    }

    /* Errors locking the mutex override any existing error or
     * cancellation, since the caller must see them to know the
     * state of the mutex. */
    if ((tmp = pthread_mutex_lock(m)))
        e = tmp;

    if (oldstate == WAITING)
        goto done;

    /* By this point, our part of the waiter list cannot change further.
     * It has been unlinked from the condvar by __private_cond_signal().
     * It consists only of waiters that were woken explicitly by
     * pthread_cond_signal()/broadcast().  Any timed-out waiters would have
     * removed themselves from the list before __private_cond_signal()
     * signaled the first node.barrier in our list.
     *
     * It is therefore safe now to read node.next and node.prev without
     * holding _c_lock. */

    /* As an optimization, we only update _m_waiters at the beginning and
     * end of the woken list. */
    if (!node.next)
        a_inc(&m->_m_waiters);

    /* Unlock the barrier that's holding back the next waiter, and
     * either wake it or requeue it to the mutex. */
    if (node.prev)
        unlock_requeue(&node.prev->barrier, &m->_m_lock);
    else
        a_dec(&m->_m_waiters);

    /* Since a signal was consumed, cancellation is not permitted. */
    if (e == ECANCELED)
        e = 0;

done:
    __pthread_setcancelstate(cs, 0);

    if (e == ECANCELED) {
        __pthread_testcancel();
        __pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, 0);
    }

    return e;
}

/* This will wake upto |n| threads that are waiting on the condvar.  This
 * is used to implement pthread_cond_signal() (for n=1) and
 * pthread_cond_broadcast() (for n=-1). */
void __private_cond_signal(void* condvar, int n) {
    pthread_cond_t* c = condvar;
    struct waiter *p, *first = 0;
    volatile int ref = 0;
    int cur;

    lock(&c->_c_lock);
    for (p = c->_c_tail; n && p; p = p->prev) {
        if (a_cas(&p->state, WAITING, SIGNALED) != WAITING) {
            /* This waiter timed out, and it marked itself as in the
             * LEAVING state.  However, it hasn't yet claimed _c_lock
             * (since we claimed the lock first) and so it hasn't yet
             * removed itself from the list.  We will wait for the waiter
             * to remove itself from the list and to notify us of that. */
            ref++;
            p->notify = &ref;
        } else {
            n--;
            if (!first)
                first = p;
        }
    }
    /* Split the list, leaving any remainder on the cv. */
    if (p) {
        if (p->next)
            p->next->prev = 0;
        p->next = 0;
    } else {
        c->_c_head = 0;
    }
    c->_c_tail = p;
    unlock(&c->_c_lock);

    /* Wait for any waiters in the LEAVING state to remove
     * themselves from the list before returning or allowing
     * signaled threads to proceed. */
    while ((cur = ref))
        __wait(&ref, 0, cur);

    /* Allow first signaled waiter, if any, to proceed. */
    if (first)
        unlock(&first->barrier);
}
