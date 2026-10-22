/*
 * Event loop for AmigaOS/MorphOS/AROS
 * Copyright (c) 2002-2019, Jouni Malinen <j@w1.fi>
 * Copyright (c) 2010-2026, Neil Cafferkey
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 *
 * Timeouts come from timer.device, everything else arrives as an Exec
 * signal: the SANA-II backend registers its message port's signal bit with
 * eloop_register_signal(), and CTRL-C is the terminate signal. There are no
 * sockets to wait on; the socket table only exists so callers see the
 * registration succeed.
 */

#include "includes.h"

#include "common.h"
#include "trace.h"
#include "list.h"
#include "eloop.h"

#include <exec/types.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <devices/timer.h>

#include <proto/exec.h>


struct eloop_sock {
	int sock;
	void *eloop_data;
	void *user_data;
	eloop_sock_handler handler;
};

struct eloop_timeout {
	struct dl_list list;
	struct os_reltime time;
	void *eloop_data;
	void *user_data;
	eloop_timeout_handler handler;
};

struct eloop_signal {
	int sig;
	void *user_data;
	eloop_signal_handler handler;
	int signaled;
};

struct eloop_sock_table {
	size_t count;
	struct eloop_sock *table;
};

struct eloop_data {
	struct eloop_sock_table readers;
	struct eloop_sock_table writers;
	struct eloop_sock_table exceptions;

	struct dl_list timeout;

	size_t signal_count;
	struct eloop_signal *signals;
	int signaled;
	int pending_terminate;

	int terminate;

	struct MsgPort *timer_port;
	struct timerequest *timer_request;
	int timer_pending;
	struct os_reltime timer_time;
};

static struct eloop_data eloop;


int eloop_init(void)
{
	os_memset(&eloop, 0, sizeof(eloop));
	dl_list_init(&eloop.timeout);

	eloop.timer_port = CreateMsgPort();
	if (eloop.timer_port == NULL)
		return -1;
	eloop.timer_request = (struct timerequest *)
		CreateIORequest(eloop.timer_port, sizeof(struct timerequest));
	if (eloop.timer_request == NULL) {
		DeleteMsgPort(eloop.timer_port);
		eloop.timer_port = NULL;
		return -1;
	}

	if (OpenDevice((CONST_STRPTR) "timer.device", UNIT_VBLANK,
		       (struct IORequest *) eloop.timer_request, 0) != 0) {
		DeleteIORequest((struct IORequest *) eloop.timer_request);
		DeleteMsgPort(eloop.timer_port);
		eloop.timer_request = NULL;
		eloop.timer_port = NULL;
		return -1;
	}

	return 0;
}


static int eloop_sock_table_add_sock(struct eloop_sock_table *table,
				     int sock, eloop_sock_handler handler,
				     void *eloop_data, void *user_data)
{
	struct eloop_sock *tmp;

	if (table == NULL)
		return -1;

	tmp = os_realloc_array(table->table, table->count + 1,
			       sizeof(struct eloop_sock));
	if (tmp == NULL)
		return -1;

	tmp[table->count].sock = sock;
	tmp[table->count].eloop_data = eloop_data;
	tmp[table->count].user_data = user_data;
	tmp[table->count].handler = handler;
	table->count++;
	table->table = tmp;

	wpa_printf(MSG_DEBUG,
		   "ELOOP: socket %d registered, but this loop only waits on Exec signals",
		   sock);

	return 0;
}


static void eloop_sock_table_remove_sock(struct eloop_sock_table *table,
					 int sock)
{
	size_t i;

	if (table == NULL || table->table == NULL || table->count == 0)
		return;

	for (i = 0; i < table->count; i++) {
		if (table->table[i].sock == sock)
			break;
	}
	if (i == table->count)
		return;
	if (i != table->count - 1) {
		os_memmove(&table->table[i], &table->table[i + 1],
			   (table->count - i - 1) *
			   sizeof(struct eloop_sock));
	}
	table->count--;
}


int eloop_register_read_sock(int sock, eloop_sock_handler handler,
			     void *eloop_data, void *user_data)
{
	return eloop_register_sock(sock, EVENT_TYPE_READ, handler,
				   eloop_data, user_data);
}


void eloop_unregister_read_sock(int sock)
{
	eloop_unregister_sock(sock, EVENT_TYPE_READ);
}


static struct eloop_sock_table *eloop_get_sock_table(eloop_event_type type)
{
	switch (type) {
	case EVENT_TYPE_READ:
		return &eloop.readers;
	case EVENT_TYPE_WRITE:
		return &eloop.writers;
	case EVENT_TYPE_EXCEPTION:
		return &eloop.exceptions;
	}

	return NULL;
}


int eloop_register_sock(int sock, eloop_event_type type,
			eloop_sock_handler handler,
			void *eloop_data, void *user_data)
{
	struct eloop_sock_table *table;

	table = eloop_get_sock_table(type);
	return eloop_sock_table_add_sock(table, sock, handler,
					 eloop_data, user_data);
}


void eloop_unregister_sock(int sock, eloop_event_type type)
{
	struct eloop_sock_table *table;

	table = eloop_get_sock_table(type);
	eloop_sock_table_remove_sock(table, sock);
}


int eloop_register_event(void *event, size_t event_size,
			 eloop_event_handler handler,
			 void *eloop_data, void *user_data)
{
	return -1;
}


void eloop_unregister_event(void *event, size_t event_size)
{
}


int eloop_sock_requeue(void)
{
	return 0;
}


int eloop_register_timeout(unsigned int secs, unsigned int usecs,
			   eloop_timeout_handler handler,
			   void *eloop_data, void *user_data)
{
	struct eloop_timeout *timeout, *tmp;
	os_time_t now_sec;

	timeout = os_zalloc(sizeof(*timeout));
	if (timeout == NULL)
		return -1;
	if (os_get_reltime(&timeout->time) < 0) {
		os_free(timeout);
		return -1;
	}
	now_sec = timeout->time.sec;
	timeout->time.sec += secs;
	if (timeout->time.sec < now_sec)
		goto overflow;
	timeout->time.usec += usecs;
	while (timeout->time.usec >= 1000000) {
		timeout->time.sec++;
		timeout->time.usec -= 1000000;
	}
	if (timeout->time.sec < now_sec)
		goto overflow;
	timeout->eloop_data = eloop_data;
	timeout->user_data = user_data;
	timeout->handler = handler;
	wpa_trace_add_ref(timeout, eloop, eloop_data);
	wpa_trace_add_ref(timeout, user, user_data);
	wpa_trace_record(timeout);

	/* Maintain timeouts in order of increasing time */
	dl_list_for_each(tmp, &eloop.timeout, struct eloop_timeout, list) {
		if (os_reltime_before(&timeout->time, &tmp->time)) {
			dl_list_add(tmp->list.prev, &timeout->list);
			return 0;
		}
	}
	dl_list_add_tail(&eloop.timeout, &timeout->list);

	return 0;

overflow:
	/*
	 * Integer overflow - assume long enough timeout to be assumed
	 * to be infinite, i.e., the timeout would never happen.
	 */
	wpa_printf(MSG_DEBUG,
		   "ELOOP: Too long timeout (secs=%u usecs=%u) to ever happen - ignore it",
		   secs,usecs);
	os_free(timeout);
	return 0;
}


static void eloop_remove_timeout(struct eloop_timeout *timeout)
{
	dl_list_del(&timeout->list);
	wpa_trace_remove_ref(timeout, eloop, timeout->eloop_data);
	wpa_trace_remove_ref(timeout, user, timeout->user_data);
	os_free(timeout);
}


int eloop_cancel_timeout(eloop_timeout_handler handler,
			 void *eloop_data, void *user_data)
{
	struct eloop_timeout *timeout, *prev;
	int removed = 0;

	dl_list_for_each_safe(timeout, prev, &eloop.timeout,
			      struct eloop_timeout, list) {
		if (timeout->handler == handler &&
		    (timeout->eloop_data == eloop_data ||
		     eloop_data == ELOOP_ALL_CTX) &&
		    (timeout->user_data == user_data ||
		     user_data == ELOOP_ALL_CTX)) {
			eloop_remove_timeout(timeout);
			removed++;
		}
	}

	return removed;
}


int eloop_cancel_timeout_one(eloop_timeout_handler handler,
			     void *eloop_data, void *user_data,
			     struct os_reltime *remaining)
{
	struct eloop_timeout *timeout, *prev;
	int removed = 0;
	struct os_reltime now;

	os_get_reltime(&now);
	remaining->sec = remaining->usec = 0;

	dl_list_for_each_safe(timeout, prev, &eloop.timeout,
			      struct eloop_timeout, list) {
		if (timeout->handler == handler &&
		    (timeout->eloop_data == eloop_data) &&
		    (timeout->user_data == user_data)) {
			removed = 1;
			if (os_reltime_before(&now, &timeout->time))
				os_reltime_sub(&timeout->time, &now, remaining);
			eloop_remove_timeout(timeout);
			break;
		}
	}
	return removed;
}


int eloop_is_timeout_registered(eloop_timeout_handler handler,
				void *eloop_data, void *user_data)
{
	struct eloop_timeout *tmp;

	dl_list_for_each(tmp, &eloop.timeout, struct eloop_timeout, list) {
		if (tmp->handler == handler &&
		    tmp->eloop_data == eloop_data &&
		    tmp->user_data == user_data)
			return 1;
	}

	return 0;
}


int eloop_deplete_timeout(unsigned int req_secs, unsigned int req_usecs,
			  eloop_timeout_handler handler, void *eloop_data,
			  void *user_data)
{
	struct os_reltime now, requested, remaining;
	struct eloop_timeout *tmp;

	dl_list_for_each(tmp, &eloop.timeout, struct eloop_timeout, list) {
		if (tmp->handler == handler &&
		    tmp->eloop_data == eloop_data &&
		    tmp->user_data == user_data) {
			requested.sec = req_secs;
			requested.usec = req_usecs;
			os_get_reltime(&now);
			os_reltime_sub(&tmp->time, &now, &remaining);
			if (os_reltime_before(&requested, &remaining)) {
				eloop_cancel_timeout(handler, eloop_data,
						     user_data);
				eloop_register_timeout(requested.sec,
						       requested.usec,
						       handler, eloop_data,
						       user_data);
				return 1;
			}
			return 0;
		}
	}

	return -1;
}


int eloop_replenish_timeout(unsigned int req_secs, unsigned int req_usecs,
			    eloop_timeout_handler handler, void *eloop_data,
			    void *user_data)
{
	struct os_reltime now, requested, remaining;
	struct eloop_timeout *tmp;

	dl_list_for_each(tmp, &eloop.timeout, struct eloop_timeout, list) {
		if (tmp->handler == handler &&
		    tmp->eloop_data == eloop_data &&
		    tmp->user_data == user_data) {
			requested.sec = req_secs;
			requested.usec = req_usecs;
			os_get_reltime(&now);
			os_reltime_sub(&tmp->time, &now, &remaining);
			if (os_reltime_before(&remaining, &requested)) {
				eloop_cancel_timeout(handler, eloop_data,
						     user_data);
				eloop_register_timeout(requested.sec,
						       requested.usec,
						       handler, eloop_data,
						       user_data);
				return 1;
			}
			return 0;
		}
	}

	return -1;
}


static void eloop_handle_signals(ULONG sigs)
{
	size_t i;

	for (i = 0; i < eloop.signal_count; i++) {
		if (sigs & (1UL << eloop.signals[i].sig)) {
			eloop.signals[i].signaled++;
			eloop.signaled++;
		}
	}
}


static void eloop_process_pending_signals(void)
{
	size_t i;

	if (eloop.signaled == 0)
		return;
	eloop.signaled = 0;

	if (eloop.pending_terminate) {
		eloop.pending_terminate = 0;
	}

	for (i = 0; i < eloop.signal_count; i++) {
		if (eloop.signals[i].signaled) {
			eloop.signals[i].signaled = 0;
			eloop.signals[i].handler(eloop.signals[i].sig,
						 eloop.signals[i].user_data);
		}
	}
}


int eloop_register_signal(int sig, eloop_signal_handler handler,
			  void *user_data)
{
	struct eloop_signal *tmp;

	tmp = os_realloc_array(eloop.signals, eloop.signal_count + 1,
			       sizeof(struct eloop_signal));
	if (tmp == NULL)
		return -1;

	tmp[eloop.signal_count].sig = sig;
	tmp[eloop.signal_count].user_data = user_data;
	tmp[eloop.signal_count].handler = handler;
	tmp[eloop.signal_count].signaled = 0;
	eloop.signal_count++;
	eloop.signals = tmp;

	return 0;
}


int eloop_register_signal_terminate(eloop_signal_handler handler,
				    void *user_data)
{
	/* The C library would otherwise turn CTRL-C into SIGINT and exit
	 * without the shutdown sequence running */
	signal(SIGINT, SIG_IGN);

	return eloop_register_signal(SIGBREAKB_CTRL_C, handler, user_data);
}


int eloop_register_signal_reconfig(eloop_signal_handler handler,
				   void *user_data)
{
	return 0;
}


static void eloop_timer_cancel(void)
{
	if (!eloop.timer_pending)
		return;
	AbortIO((struct IORequest *) eloop.timer_request);
	WaitIO((struct IORequest *) eloop.timer_request);
	eloop.timer_pending = 0;
}


void eloop_run(void)
{
	struct os_reltime tv, now;
	ULONG sig_mask, sigs, timer_bit;
	size_t i;

	timer_bit = 1UL << eloop.timer_port->mp_SigBit;

	while (!eloop.terminate &&
	       (!dl_list_empty(&eloop.timeout) || eloop.signal_count > 0)) {
		struct eloop_timeout *timeout;

		/* Arm timer.device for the earliest timeout. A timeout that
		 * has already passed is run without waiting. */
		timeout = dl_list_first(&eloop.timeout, struct eloop_timeout,
					list);
		if (timeout) {
			os_get_reltime(&now);
			if (!os_reltime_before(&now, &timeout->time)) {
				eloop_timer_cancel();
				dl_list_del(&timeout->list);
				timeout->handler(timeout->eloop_data,
						 timeout->user_data);
				wpa_trace_remove_ref(timeout, eloop,
						     timeout->eloop_data);
				wpa_trace_remove_ref(timeout, user,
						     timeout->user_data);
				os_free(timeout);
				continue;
			}
			/* A timeout registered from a handler may be earlier
			 * than the one the timer was armed for */
			if (eloop.timer_pending &&
			    os_reltime_before(&timeout->time,
					      &eloop.timer_time))
				eloop_timer_cancel();
			if (!eloop.timer_pending) {
				eloop.timer_time = timeout->time;
				os_reltime_sub(&timeout->time, &now, &tv);
				eloop.timer_request->tr_node.io_Command =
					TR_ADDREQUEST;
				eloop.timer_request->tr_time.tv_secs = tv.sec;
				eloop.timer_request->tr_time.tv_micro =
					tv.usec;
				SendIO((struct IORequest *)
				       eloop.timer_request);
				eloop.timer_pending = 1;
			}
		}

		sig_mask = eloop.timer_pending ? timer_bit : 0;
		for (i = 0; i < eloop.signal_count; i++)
			sig_mask |= 1UL << eloop.signals[i].sig;

		sigs = Wait(sig_mask);

		if (sigs & timer_bit) {
			/* The request has been replied; the timeout itself
			 * is run at the top of the loop */
			while (GetMsg(eloop.timer_port) != NULL)
				;
			eloop.timer_pending = 0;
		}

		eloop_handle_signals(sigs);
		eloop_process_pending_signals();
	}

	eloop.terminate = 0;
}


void eloop_terminate(void)
{
	eloop.terminate = 1;
}


void eloop_destroy(void)
{
	struct eloop_timeout *timeout, *prev;

	dl_list_for_each_safe(timeout, prev, &eloop.timeout,
			      struct eloop_timeout, list) {
		eloop_remove_timeout(timeout);
	}
	os_free(eloop.readers.table);
	os_free(eloop.writers.table);
	os_free(eloop.exceptions.table);
	os_free(eloop.signals);

	if (eloop.timer_request != NULL) {
		eloop_timer_cancel();
		CloseDevice((struct IORequest *) eloop.timer_request);
		DeleteIORequest((struct IORequest *) eloop.timer_request);
	}
	if (eloop.timer_port != NULL)
		DeleteMsgPort(eloop.timer_port);
	eloop.timer_request = NULL;
	eloop.timer_port = NULL;
}


int eloop_terminated(void)
{
	return eloop.terminate || eloop.pending_terminate;
}


void eloop_wait_for_read_sock(int sock)
{
}
