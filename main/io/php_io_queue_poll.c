/*
   +----------------------------------------------------------------------+
   | Copyright © The PHP Group and Contributors.                          |
   +----------------------------------------------------------------------+
   | This source file is subject to the Modified BSD License that is      |
   | bundled with this package in the file LICENSE, and is available      |
   | through the World Wide Web at <https://www.php.net/license/>.        |
   |                                                                      |
   | SPDX-License-Identifier: BSD-3-Clause                                |
   +----------------------------------------------------------------------+
*/

/* The poll queue: a Poll context and its timers. Poll ops complete as
 * Done with the triggered events, any other op with a pollable descriptor
 * completes as Ready for its ready_events. Timer ops and op deadlines are
 * timers of the context. Ops without a descriptor complete as Unsupported.
 *
 * The context keeps exactly one registration per descriptor and the queue
 * multiplexes interest over it: its armed events are the union of the
 * submitted ops' events, re-armed at submit and withdrawn at completion or
 * cancel, so a retained registration nobody is waiting on contributes
 * nothing. A persistent op's add() and remove() bracket the registration;
 * between runs it stays disarmed. */

#include "php.h"
#include "main/php_io_hooks.h"
#include "main/php_poll.h"

#include <errno.h>

#define PHP_IO_POLL_MIN_EVENTS 64

typedef struct _php_io_poll_req php_io_poll_req;
typedef struct _php_io_poll_fdreg php_io_poll_fdreg;

/* One descriptor in the context */
struct _php_io_poll_fdreg {
	int fd;
	uint32_t armed;             /* events registered in the context */
	bool in_ctx;
	uint32_t n_retained;        /* persistent registrations, add() minus remove() */
	php_io_poll_req *reqs;      /* submitted ops on the descriptor */
};

struct _php_io_poll_req {
	php_io_op *op;
	void *data;
	php_io_op_result result;
	php_io_poll_req *group;     /* member: the Any's request */
	uint32_t index;             /* member: position in the Any */
	php_poll_timer *timer;      /* the op's deadline, or the Timer op itself */
	php_io_poll_fdreg *fdreg;   /* the descriptor the op waits on */
	uint32_t events;            /* the interest on fdreg */
	php_io_poll_req *fd_next;   /* fdreg->reqs */
	bool done;                  /* member: result recorded */
	bool ready;                 /* top-level: in the ready list */
	bool fired;                 /* group: in the fired list */
	php_io_poll_req **members;  /* group */
	uint32_t n_members;         /* group */
	php_io_poll_req *prev;      /* outstanding list, top-level only */
	php_io_poll_req *next;
};

typedef struct {
	php_io_queue base;
	php_poll_ctx *ctx;
	HashTable fdregs;           /* fd -> php_io_poll_fdreg */
	uint32_t n_armed;           /* registrations with events armed */
	php_io_poll_req *outstanding;
	uint32_t pending;
	php_io_poll_req **ready;
	uint32_t n_ready;
	uint32_t ready_cap;
	php_io_poll_req **fired;
	uint32_t n_fired;
	uint32_t fired_cap;
	php_poll_event *events;
	uint32_t events_cap;
} php_io_poll_queue;

static int php_io_poll_error_to_errno(php_poll_error err)
{
	switch (err) {
		case PHP_POLL_ERR_NOMEM: return ENOMEM;
		case PHP_POLL_ERR_INVALID: return EBADF;
		case PHP_POLL_ERR_EXISTS: return EEXIST;
		case PHP_POLL_ERR_NOTFOUND: return ENOENT;
		case PHP_POLL_ERR_INTERRUPTED: return EINTR;
		case PHP_POLL_ERR_PERMISSION: return EPERM;
		case PHP_POLL_ERR_TOOBIG: return EMFILE;
		case PHP_POLL_ERR_AGAIN: return EAGAIN;
		case PHP_POLL_ERR_NOSUPPORT: return ENOTSUP;
		default: return EIO;
	}
}

/* Lists */

static void php_io_poll_list_push(php_io_poll_req ***list, uint32_t *n, uint32_t *cap, php_io_poll_req *req)
{
	if (*n == *cap) {
		*cap = *cap ? *cap * 2 : 16;
		*list = safe_erealloc(*list, *cap, sizeof(**list), 0);
	}
	(*list)[(*n)++] = req;
}

static void php_io_poll_list_remove(php_io_poll_req **list, uint32_t *n, php_io_poll_req *req)
{
	for (uint32_t i = 0; i < *n; i++) {
		if (list[i] == req) {
			memmove(&list[i], &list[i + 1], (*n - i - 1) * sizeof(*list));
			(*n)--;
			return;
		}
	}
	ZEND_UNREACHABLE();
}

/* Descriptor registrations */

static php_io_poll_fdreg *php_io_poll_fdreg_get(php_io_poll_queue *q, int fd, bool create)
{
	php_io_poll_fdreg *reg = zend_hash_index_find_ptr(&q->fdregs, (zend_ulong) fd);
	if (!reg && create) {
		reg = ecalloc(1, sizeof(*reg));
		reg->fd = fd;
		zend_hash_index_add_new_ptr(&q->fdregs, (zend_ulong) fd, reg);
	}
	return reg;
}

static void php_io_poll_fdreg_drop(php_io_poll_queue *q, php_io_poll_fdreg *reg)
{
	if (reg->in_ctx) {
		php_poll_remove(q->ctx, reg->fd);
		if (reg->armed) {
			q->n_armed--;
		}
	}
	zend_hash_index_del(&q->fdregs, (zend_ulong) reg->fd);
	efree(reg);
}

/* Bring the context's interest in line with the submitted ops. On failure
 * the error is left in the context and the registration is unchanged. */
static zend_result php_io_poll_fdreg_sync(php_io_poll_queue *q, php_io_poll_fdreg *reg)
{
	uint32_t wanted = 0;
	for (php_io_poll_req *r = reg->reqs; r; r = r->fd_next) {
		wanted |= r->events;
	}

	if (!reg->reqs && !reg->n_retained) {
		php_io_poll_fdreg_drop(q, reg);
		return SUCCESS;
	}
	if (!reg->in_ctx) {
		if (!wanted) {
			return SUCCESS;
		}
		if (php_poll_add(q->ctx, reg->fd, wanted, reg) != SUCCESS) {
			return FAILURE;
		}
		reg->in_ctx = true;
	} else if (wanted != reg->armed) {
		if (php_poll_modify(q->ctx, reg->fd, wanted, reg) != SUCCESS) {
			return FAILURE;
		}
	}
	if (!reg->armed && wanted) {
		q->n_armed++;
	} else if (reg->armed && !wanted) {
		q->n_armed--;
	}
	reg->armed = wanted;
	return SUCCESS;
}

static void php_io_poll_fdreg_unlink(php_io_poll_req *req)
{
	php_io_poll_req **link = &req->fdreg->reqs;
	while (*link != req) {
		link = &(*link)->fd_next;
	}
	*link = req->fd_next;
	req->fd_next = NULL;
}

/* Requests */

static php_io_poll_req *php_io_poll_req_create(php_io_poll_queue *q, php_io_op *op, void *data)
{
	php_io_poll_req *req = ecalloc(1, sizeof(*req));
	req->op = op;
	req->data = data;
	op->queue = &q->base;
	op->queue_data = req;
	op->in_flight = false;
	return req;
}

static void php_io_poll_req_unregister(php_io_poll_queue *q, php_io_poll_req *req)
{
	if (req->fdreg) {
		php_io_poll_fdreg *reg = req->fdreg;
		php_io_poll_fdreg_unlink(req);
		req->fdreg = NULL;
		/* Withdrawing interest cannot fail in a way that matters here */
		php_io_poll_fdreg_sync(q, reg);
	}
	if (req->timer) {
		php_poll_timer_remove(q->ctx, req->timer);
		req->timer = NULL;
	}
}

static void php_io_poll_req_free(php_io_poll_req *req)
{
	req->op->queue = NULL;
	req->op->queue_data = NULL;
	efree(req);
}

static void php_io_poll_req_free_top(php_io_poll_queue *q, php_io_poll_req *req)
{
	if (req->prev) {
		req->prev->next = req->next;
	} else {
		q->outstanding = req->next;
	}
	if (req->next) {
		req->next->prev = req->prev;
	}
	q->pending--;
	php_io_poll_req_free(req);
}

static void php_io_poll_req_complete(php_io_poll_queue *q, php_io_poll_req *req,
		php_io_status status, int64_t res, int error)
{
	php_io_poll_req_unregister(q, req);

	if (req->group) {
		if (!req->done) {
			req->done = true;
			req->result.status = status;
			req->result.index = req->index;
			req->result.res = res;
			req->result.error = error;
			if (!req->group->fired) {
				req->group->fired = true;
				php_io_poll_list_push(&q->fired, &q->n_fired, &q->fired_cap, req->group);
			}
		}
		return;
	}

	ZEND_ASSERT(!req->ready);
	req->ready = true;
	req->result.status = status;
	req->result.index = 0;
	req->result.res = res;
	req->result.error = error;
	php_io_poll_list_push(&q->ready, &q->n_ready, &q->ready_cap, req);
}

static void php_io_poll_req_arm(php_io_poll_queue *q, php_io_poll_req *req)
{
	php_io_op *op = req->op;

	if (op->type == PHP_IO_OP_TIMER) {
		if (!php_deadline_is_infinite(&op->deadline)) {
			req->timer = php_poll_timer_add(q->ctx, op->deadline.hrtime, 0, req);
		}
		return;
	}

	uint32_t events = op->type == PHP_IO_OP_POLL ? op->u.poll.events : op->ready_events;
	if (events & (PHP_POLL_PROCESS | PHP_POLL_SIGNAL)) {
		/* A pidfd or signalfd, readable when there is something to take */
		events = PHP_POLL_READ;
	}
	events &= PHP_POLL_READ | PHP_POLL_WRITE | PHP_POLL_ERROR | PHP_POLL_HUP | PHP_POLL_RDHUP | PHP_POLL_PRI;
	if (op->fd == SOCK_ERR || events == 0) {
		php_io_poll_req_complete(q, req, PHP_IO_UNSUPPORTED, 0, 0);
		return;
	}

	php_io_poll_fdreg *reg = php_io_poll_fdreg_get(q, (int) op->fd, true);
	req->fdreg = reg;
	req->events = events;
	req->fd_next = reg->reqs;
	reg->reqs = req;

	if (php_io_poll_fdreg_sync(q, reg) != SUCCESS) {
		php_poll_error err = php_poll_get_error(q->ctx);
		php_io_poll_fdreg_unlink(req);
		req->fdreg = NULL;
		php_io_poll_fdreg_sync(q, reg);
		if (err == PHP_POLL_ERR_NOSUPPORT) {
			php_io_poll_req_complete(q, req, PHP_IO_UNSUPPORTED, 0, 0);
		} else {
			php_io_poll_req_complete(q, req, PHP_IO_DONE, -1, php_io_poll_error_to_errno(err));
		}
		return;
	}

	if (!php_deadline_is_infinite(&op->deadline)) {
		req->timer = php_poll_timer_add(q->ctx, op->deadline.hrtime, 0, req);
	}
}

/* A descriptor reported ready: every op whose interest it meets completes;
 * an error or hangup completes all of them. */
static void php_io_poll_fdreg_fire(php_io_poll_queue *q, php_io_poll_fdreg *reg, uint32_t revents)
{
	bool failure = (revents & (PHP_POLL_ERROR | PHP_POLL_HUP)) != 0;
	php_io_poll_req *r = reg->reqs;
	while (r) {
		php_io_poll_req *next = r->fd_next;
		if (failure || (r->events & revents)) {
			php_io_status status = r->op->type == PHP_IO_OP_POLL ? PHP_IO_DONE : PHP_IO_READY;
			php_io_poll_req_complete(q, r, status, revents, 0);
		}
		r = next;
	}
}

static void php_io_poll_group_release_members(php_io_poll_queue *q, php_io_poll_req *req)
{
	for (uint32_t i = 0; i < req->n_members; i++) {
		php_io_poll_req *m = req->members[i];
		php_io_poll_req_unregister(q, m);
		php_io_poll_req_free(m);
	}
	if (req->members) {
		efree(req->members);
		req->members = NULL;
	}
	req->n_members = 0;
}

/* The Any completes once at least one member did: collect the members that
 * completed, withdraw the interest of the rest. */
static void php_io_poll_group_fold(php_io_poll_queue *q, php_io_poll_req *req)
{
	php_io_op *op = req->op;
	uint32_t n_results = 0;

	for (uint32_t i = 0; i < req->n_members; i++) {
		php_io_poll_req *m = req->members[i];
		if (m->done) {
			if (op->u.any.results) {
				op->u.any.results[n_results] = m->result;
			}
			n_results++;
		}
	}
	op->u.any.n_results = n_results;
	php_io_poll_group_release_members(q, req);

	req->fired = false;
	req->ready = true;
	req->result.status = PHP_IO_DONE;
	req->result.index = 0;
	req->result.res = 0;
	req->result.error = 0;
	php_io_poll_list_push(&q->ready, &q->n_ready, &q->ready_cap, req);
}

static void php_io_poll_fold_all(php_io_poll_queue *q)
{
	while (q->n_fired) {
		php_io_poll_req *req = q->fired[0];
		php_io_poll_list_remove(q->fired, &q->n_fired, req);
		php_io_poll_group_fold(q, req);
	}
}

/* Queue operations */

static zend_result php_io_poll_queue_submit(php_io_queue *base, php_io_op *op, void *data)
{
	php_io_poll_queue *q = (php_io_poll_queue *) base;

	if (op->queue) {
		errno = EALREADY;
		return FAILURE;
	}

	php_io_poll_req *req = php_io_poll_req_create(q, op, data);
	req->next = q->outstanding;
	if (q->outstanding) {
		q->outstanding->prev = req;
	}
	q->outstanding = req;
	q->pending++;

	if (op->type == PHP_IO_OP_ANY) {
		uint32_t n = op->u.any.n;
		op->u.any.n_results = 0;
		req->n_members = n;
		req->members = n ? safe_emalloc(n, sizeof(*req->members), 0) : NULL;
		for (uint32_t i = 0; i < n; i++) {
			php_io_poll_req *m = php_io_poll_req_create(q, op->u.any.ops[i], NULL);
			m->group = req;
			m->index = i;
			req->members[i] = m;
		}
		for (uint32_t i = 0; i < n; i++) {
			php_io_poll_req_arm(q, req->members[i]);
		}
		if (n == 0 && !req->fired) {
			/* Nothing could ever complete it */
			req->fired = true;
			php_io_poll_list_push(&q->fired, &q->n_fired, &q->fired_cap, req);
		}
	} else {
		php_io_poll_req_arm(q, req);
	}

	return SUCCESS;
}

static zend_result php_io_poll_queue_cancel(php_io_queue *base, php_io_op *op)
{
	php_io_poll_queue *q = (php_io_poll_queue *) base;
	php_io_poll_req *req = op->queue_data;

	if (op->queue != base || !req) {
		errno = ENOENT;
		return FAILURE;
	}
	if (req->group) {
		/* Members are withdrawn by their Any */
		errno = EINVAL;
		return FAILURE;
	}

	if (op->type == PHP_IO_OP_ANY) {
		php_io_poll_group_release_members(q, req);
		if (req->fired) {
			php_io_poll_list_remove(q->fired, &q->n_fired, req);
		}
	} else {
		php_io_poll_req_unregister(q, req);
	}
	if (req->ready) {
		php_io_poll_list_remove(q->ready, &q->n_ready, req);
	}
	php_io_poll_req_free_top(q, req);
	return SUCCESS;
}

/* A persistent op: retain the descriptor's registration across runs */
static zend_result php_io_poll_queue_add(php_io_queue *base, php_io_op *op)
{
	php_io_poll_queue *q = (php_io_poll_queue *) base;

	if (op->fd == SOCK_ERR) {
		errno = EBADF;
		return FAILURE;
	}
	php_io_poll_fdreg *reg = php_io_poll_fdreg_get(q, (int) op->fd, true);
	reg->n_retained++;
	return SUCCESS;
}

static void php_io_poll_queue_remove(php_io_queue *base, php_io_op *op)
{
	php_io_poll_queue *q = (php_io_poll_queue *) base;

	if (op->fd == SOCK_ERR) {
		return;
	}
	php_io_poll_fdreg *reg = php_io_poll_fdreg_get(q, (int) op->fd, false);
	if (reg && reg->n_retained) {
		reg->n_retained--;
		php_io_poll_fdreg_sync(q, reg);
	}
}

static uint32_t php_io_poll_queue_deliver(php_io_poll_queue *q, php_io_queue_completion *out, uint32_t max)
{
	uint32_t n = MIN(max, q->n_ready);

	for (uint32_t i = 0; i < n; i++) {
		php_io_poll_req *req = q->ready[i];
		out[i].op = req->op;
		out[i].data = req->data;
		out[i].result = req->result;
		php_io_poll_req_free_top(q, req);
	}
	memmove(q->ready, &q->ready[n], (q->n_ready - n) * sizeof(*q->ready));
	q->n_ready -= n;
	return n;
}

static int php_io_poll_queue_wait(php_io_queue *base, php_io_queue_completion *out, uint32_t max,
		const struct timespec *timeout)
{
	php_io_poll_queue *q = (php_io_poll_queue *) base;
	zend_hrtime_t limit = ZEND_HRTIME_T_MAX;

	if (max == 0) {
		return 0;
	}

	if (timeout) {
		zend_hrtime_t now = zend_hrtime();
		zend_hrtime_t rel = (zend_hrtime_t) timeout->tv_sec * ZEND_NANO_IN_SEC + timeout->tv_nsec;
		limit = rel < ZEND_HRTIME_T_MAX - now ? now + rel : ZEND_HRTIME_T_MAX;
	}

	if (q->events_cap < max) {
		q->events_cap = max;
		q->events = safe_erealloc(q->events, q->events_cap, sizeof(*q->events), 0);
	}

	for (;;) {
		php_io_poll_fold_all(q);
		if (q->n_ready) {
			return (int) php_io_poll_queue_deliver(q, out, max);
		}

		if (limit == ZEND_HRTIME_T_MAX && q->n_armed == 0 && php_poll_timer_count(q->ctx) == 0) {
			/* Nothing can ever complete: an infinite timer, or nothing at all */
			errno = EDEADLK;
			return -1;
		}

		/* The context bounds the wait by its own timers */
		struct timespec ts, *pts = NULL;
		if (limit != ZEND_HRTIME_T_MAX) {
			zend_hrtime_t now = zend_hrtime();
			zend_hrtime_t remaining = limit > now ? limit - now : 0;
			ts.tv_sec = remaining / ZEND_NANO_IN_SEC;
			ts.tv_nsec = remaining % ZEND_NANO_IN_SEC;
			pts = &ts;
		}

		int n = php_poll_wait(q->ctx, q->events, (int) q->events_cap, pts);
		if (n < 0) {
			php_poll_error err = php_poll_get_error(q->ctx);
			if (err == PHP_POLL_ERR_INTERRUPTED) {
				/* Restart with the remaining time */
				continue;
			}
			errno = php_io_poll_error_to_errno(err);
			return -1;
		}

		/* Descriptors first: completing a timed out request drops its
		 * registration when nothing else holds it, and the same reap may
		 * still carry an event for that registration. A request whose
		 * readiness came in this reap keeps it, and its timer is skipped. */
		for (int i = 0; i < n; i++) {
			if (!(q->events[i].revents & PHP_POLL_TIMER)) {
				php_io_poll_fdreg_fire(q, q->events[i].data, q->events[i].revents);
			}
		}
		for (int i = 0; i < n; i++) {
			if (q->events[i].revents & PHP_POLL_TIMER) {
				php_io_poll_req *req = q->events[i].data;
				if (req->ready || req->done) {
					/* Its deadline and its readiness landed in the same reap */
					continue;
				}
				php_io_status status = req->op->type == PHP_IO_OP_TIMER ? PHP_IO_DONE : PHP_IO_TIMEOUT;
				php_io_poll_req_complete(q, req, status, 0, 0);
			}
		}

		php_io_poll_fold_all(q);
		if (q->n_ready) {
			return (int) php_io_poll_queue_deliver(q, out, max);
		}
		if (limit != ZEND_HRTIME_T_MAX && zend_hrtime() >= limit) {
			return 0;
		}
	}
}

static void php_io_poll_queue_orphan(php_io_queue *base, php_io_op *op)
{
	/* Readiness ops never reference a buffer, so this is a plain cancel */
	php_io_poll_queue_cancel(base, op);
}

static uint32_t php_io_poll_queue_count_pending(php_io_queue *base)
{
	return ((php_io_poll_queue *) base)->pending;
}

static uint32_t php_io_poll_queue_hook_flags(php_io_queue *base)
{
	return 0;
}

static void php_io_poll_queue_destroy(php_io_queue *base)
{
	php_io_poll_queue *q = (php_io_poll_queue *) base;

	while (q->outstanding) {
		php_io_poll_queue_cancel(base, q->outstanding->op);
	}
	ZEND_ASSERT(q->pending == 0 && q->n_ready == 0 && q->n_fired == 0);

	/* Retained registrations of persistent ops nobody removed */
	php_io_poll_fdreg *reg;
	ZEND_HASH_FOREACH_PTR(&q->fdregs, reg) {
		if (reg->in_ctx) {
			php_poll_remove(q->ctx, reg->fd);
		}
		efree(reg);
	} ZEND_HASH_FOREACH_END();
	zend_hash_destroy(&q->fdregs);

	php_poll_destroy(q->ctx);
	if (q->ready) {
		efree(q->ready);
	}
	if (q->fired) {
		efree(q->fired);
	}
	if (q->events) {
		efree(q->events);
	}
	efree(q);
}

static const php_io_queue_ops php_io_poll_queue_ops = {
	.submit = php_io_poll_queue_submit,
	.cancel = php_io_poll_queue_cancel,
	.add = php_io_poll_queue_add,
	.remove = php_io_poll_queue_remove,
	.wait = php_io_poll_queue_wait,
	.orphan = php_io_poll_queue_orphan,
	.drain = NULL,
	.count_pending = php_io_poll_queue_count_pending,
	.hook_flags = php_io_poll_queue_hook_flags,
	.destroy = php_io_poll_queue_destroy,
};

PHPAPI php_io_queue *php_io_queue_create_poll(php_poll_backend_type backend)
{
	php_poll_ctx *ctx = php_poll_create(backend, 0);
	if (!ctx) {
		return NULL;
	}
	if (php_poll_init(ctx) != SUCCESS) {
		php_poll_destroy(ctx);
		return NULL;
	}

	php_io_poll_queue *q = ecalloc(1, sizeof(*q));
	q->base.ops = &php_io_poll_queue_ops;
	q->ctx = ctx;
	zend_hash_init(&q->fdregs, 8, NULL, NULL, 0);
	q->events_cap = PHP_IO_POLL_MIN_EVENTS;
	q->events = safe_emalloc(q->events_cap, sizeof(*q->events), 0);
	return &q->base;
}
