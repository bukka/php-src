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

#include "php.h"
#include "main/hooks/io_hooks.h"
#include "ext/standard/file.h"
#include "ext/standard/io_poll.h"

#include <errno.h>
#include <time.h>

PHPAPI void (*php_io_op_zobj_detach)(zend_object *zobj) = NULL;

/* Operation constructors */

static void php_io_op_init(php_io_op *op, php_io_op_type type, zend_object *handle,
		php_socket_t fd, uint32_t ready_events, php_deadline dl)
{
	memset(op, 0, sizeof(*op));
	op->type = type;
	op->handle = handle;
	op->fd = fd;
	op->ready_events = ready_events;
	op->deadline = dl;
}

PHPAPI void php_io_op_poll(php_io_op *op, zend_object *handle, php_socket_t fd, uint32_t events, php_deadline dl)
{
	php_io_op_init(op, PHP_IO_OP_POLL, handle, fd, events, dl);
	op->u.poll.events = events;
}

PHPAPI void php_io_op_timer(php_io_op *op, php_deadline dl)
{
	php_io_op_init(op, PHP_IO_OP_TIMER, NULL, SOCK_ERR, PHP_POLL_TIMER, dl);
}

PHPAPI void php_io_op_read(php_io_op *op, zend_object *handle, php_socket_t fd, void *buf, size_t len, int64_t off, php_deadline dl)
{
	php_io_op_init(op, PHP_IO_OP_READ, handle, fd, PHP_POLL_READ, dl);
	op->u.io.buf = buf;
	op->u.io.len = len;
	op->u.io.offset = off;
}

PHPAPI void php_io_op_write(php_io_op *op, zend_object *handle, php_socket_t fd, const void *buf, size_t len, int64_t off, php_deadline dl)
{
	php_io_op_init(op, PHP_IO_OP_WRITE, handle, fd, PHP_POLL_WRITE, dl);
	op->u.io.buf = (void *) buf;
	op->u.io.len = len;
	op->u.io.offset = off;
}

PHPAPI void php_io_op_recv(php_io_op *op, zend_object *handle, php_socket_t fd, void *buf, size_t len, int flags, php_deadline dl)
{
	php_io_op_init(op, PHP_IO_OP_RECV, handle, fd, PHP_POLL_READ, dl);
	op->u.io.buf = buf;
	op->u.io.len = len;
	op->u.io.offset = -1;
	op->u.io.flags = flags;
}

PHPAPI void php_io_op_send(php_io_op *op, zend_object *handle, php_socket_t fd, const void *buf, size_t len, int flags, php_deadline dl)
{
	php_io_op_init(op, PHP_IO_OP_SEND, handle, fd, PHP_POLL_WRITE, dl);
	op->u.io.buf = (void *) buf;
	op->u.io.len = len;
	op->u.io.offset = -1;
	op->u.io.flags = flags;
}

PHPAPI void php_io_op_accept(php_io_op *op, zend_object *handle, php_socket_t fd, struct sockaddr *addr, socklen_t *addrlen, php_deadline dl)
{
	php_io_op_init(op, PHP_IO_OP_ACCEPT, handle, fd, PHP_POLL_READ, dl);
	op->u.accept.addr = addr;
	op->u.accept.addrlen = addrlen;
}

PHPAPI void php_io_op_connect(php_io_op *op, zend_object *handle, php_socket_t fd, const struct sockaddr *addr, socklen_t addrlen, php_deadline dl)
{
	php_io_op_init(op, PHP_IO_OP_CONNECT, handle, fd, PHP_POLL_WRITE, dl);
	op->u.connect.addr = addr;
	op->u.connect.addrlen = addrlen;
}

PHPAPI void php_io_op_getaddrinfo(php_io_op *op, const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res, php_deadline dl)
{
	php_io_op_init(op, PHP_IO_OP_GETADDRINFO, NULL, SOCK_ERR, 0, dl);
	op->u.getaddrinfo.node = node;
	op->u.getaddrinfo.service = service;
	op->u.getaddrinfo.hints = hints;
	op->u.getaddrinfo.res = res;
}

PHPAPI void php_io_op_fsync(php_io_op *op, zend_object *handle, php_socket_t fd, bool data_only)
{
	php_io_op_init(op, PHP_IO_OP_FSYNC, handle, fd, 0, php_io_deadline_infinite());
	op->u.fsync.data_only = data_only;
}

PHPAPI void php_io_op_any(php_io_op *op, php_io_op **members, uint32_t n, php_io_op_result *results)
{
	php_io_op_init(op, PHP_IO_OP_ANY, NULL, SOCK_ERR, 0, php_io_deadline_infinite());
	op->u.any.ops = members;
	op->u.any.n = n;
	op->u.any.results = results;
	op->u.any.n_results = 0;
}

/* Persistent operations */

struct _php_io_persistent_op {
	php_io_op op;
	bool registered;                    /* the provider's add hook ran */
	php_io_persistent_op *next_on_handle;
	php_io_persistent_op *prev;         /* FG(io_persistent_ops) */
	php_io_persistent_op *next;
};

static void php_io_op_detach_zobj(php_io_op *op);

static void php_io_persistent_register(php_io_persistent_op *p)
{
	php_io_hooks_state *state = FG(io_hooks);
	if (!p->registered && state) {
		p->registered = true;
		if (state->hooks.add) {
			state->hooks.add(state->data, &p->op);
		}
	}
}

PHPAPI php_io_op *php_io_op_persistent(zend_object *handle_obj, uint32_t events)
{
	php_poll_handle_object *handle = PHP_POLL_HANDLE_OBJ_FROM_ZOBJ(handle_obj);
	php_io_persistent_op *p;

	for (p = handle->persistent; p; p = p->next_on_handle) {
		if (p->op.u.poll.events == events) {
			php_io_persistent_register(p);
			return &p->op;
		}
	}

	p = ecalloc(1, sizeof(*p));
	php_io_op_poll(&p->op, handle_obj, php_poll_handle_get_fd(handle), events, php_io_deadline_infinite());
	p->op.flags |= PHP_IO_OP_F_PERSISTENT;
	GC_ADDREF(handle_obj);

	p->next_on_handle = handle->persistent;
	handle->persistent = p;
	p->next = FG(io_persistent_ops);
	if (p->next) {
		p->next->prev = p;
	}
	FG(io_persistent_ops) = p;

	php_io_persistent_register(p);
	return &p->op;
}

static void php_io_persistent_free(php_io_persistent_op *p)
{
	php_poll_handle_object *handle = PHP_POLL_HANDLE_OBJ_FROM_ZOBJ(p->op.handle);
	php_io_hooks_state *state = FG(io_hooks);

	if (p->registered && state && state->hooks.remove) {
		state->hooks.remove(state->data, &p->op);
	}
	p->registered = false;
	if (p->op.queue) {
		p->op.queue->ops->orphan(p->op.queue, &p->op);
	}
	php_io_op_detach_zobj(&p->op);

	php_io_persistent_op **link = &handle->persistent;
	while (*link != p) {
		link = &(*link)->next_on_handle;
	}
	*link = p->next_on_handle;

	if (p->prev) {
		p->prev->next = p->next;
	} else {
		FG(io_persistent_ops) = p->next;
	}
	if (p->next) {
		p->next->prev = p->prev;
	}

	OBJ_RELEASE(&handle->std);
	efree(p);
}

PHPAPI void php_io_op_persistent_release(zend_object *handle_obj, uint32_t events)
{
	php_poll_handle_object *handle = PHP_POLL_HANDLE_OBJ_FROM_ZOBJ(handle_obj);
	for (php_io_persistent_op *p = handle->persistent; p; p = p->next_on_handle) {
		if (p->op.u.poll.events == events) {
			php_io_persistent_free(p);
			return;
		}
	}
}

PHPAPI void php_io_handle_release_ops(zend_object *handle_obj)
{
	php_poll_handle_object *handle = PHP_POLL_HANDLE_OBJ_FROM_ZOBJ(handle_obj);
	while (handle->persistent) {
		php_io_persistent_free(handle->persistent);
	}
}

/* Registration */

PHPAPI zend_result php_io_hooks_register(const php_io_hooks *hooks, size_t size, void *data)
{
	php_io_hooks_state *state = FG(io_hooks);

	if (hooks == NULL) {
		if (state) {
			FG(io_hooks) = NULL;
			if (state->hooks.dtor) {
				state->hooks.dtor(state->data);
			}
			efree(state);
			/* The outgoing provider dropped its registrations; the next one
			 * sees every persistent op as new */
			for (php_io_persistent_op *p = FG(io_persistent_ops); p; p = p->next) {
				p->registered = false;
			}
		}
		return SUCCESS;
	}

	if (state) {
		return FAILURE;
	}

	ZEND_ASSERT(size <= sizeof(php_io_hooks));
	state = ecalloc(1, sizeof(*state));
	memcpy(&state->hooks, hooks, size);
	state->data = data;
	FG(io_hooks) = state;

	return SUCCESS;
}

PHPAPI const php_io_hooks *php_io_hooks_current(void **data)
{
	php_io_hooks_state *state = FG(io_hooks);
	if (!state) {
		return NULL;
	}
	if (data) {
		*data = state->data;
	}
	return &state->hooks;
}

PHPAPI bool php_io_hooks_active(void)
{
	return FG(io_hooks) != NULL;
}

PHPAPI void php_io_hooks_request_shutdown(void)
{
	php_io_hooks_register(NULL, 0, NULL);
	if (FG(io_queue)) {
		php_io_queue *q = FG(io_queue);
		FG(io_queue) = NULL;
		q->ops->destroy(q);
	}
	if (FG(io_orphans)) {
		zend_hash_destroy(FG(io_orphans));
		efree(FG(io_orphans));
		FG(io_orphans) = NULL;
	}
}

PHPAPI uint32_t php_io_ops_in_flight(void)
{
	return FG(io_ops_in_flight);
}

/* Orphans */

static zend_always_inline zend_ulong php_io_stream_key(php_stream *stream)
{
	zend_ulong key = (zend_ulong) (uintptr_t) stream;
	return (key >> 3) | (key << ((sizeof(key) * 8) - 3));
}

typedef struct {
	php_io_queue *queue;
	bool freeing;               /* php_stream_free() is draining it: the resource is closing under us */
} php_io_orphan;

static void php_io_orphan_dtor(zval *zv)
{
	efree(Z_PTR_P(zv));
}

PHPAPI void php_io_stream_orphan(php_stream *stream, php_io_queue *queue)
{
	if (!FG(io_orphans)) {
		FG(io_orphans) = emalloc(sizeof(HashTable));
		zend_hash_init(FG(io_orphans), 4, NULL, php_io_orphan_dtor, 0);
	}
	if (!zend_hash_index_exists(FG(io_orphans), php_io_stream_key(stream))) {
		php_io_orphan *o = emalloc(sizeof(*o));
		o->queue = queue;
		o->freeing = false;
		/* The buffer belongs to the backend until the completion arrives */
		GC_ADDREF(stream->res);
		zend_hash_index_add_new_ptr(FG(io_orphans), php_io_stream_key(stream), o);
	}
}

PHPAPI void php_io_stream_unfreeze(php_stream *stream)
{
	if (!FG(io_orphans)) {
		return;
	}
	php_io_orphan *o = zend_hash_index_find_ptr(FG(io_orphans), php_io_stream_key(stream));
	if (!o) {
		return;
	}
	bool freeing = o->freeing;
	zend_hash_index_del(FG(io_orphans), php_io_stream_key(stream));
	stream->flags &= ~PHP_STREAM_FLAG_IN_USE;
	if (!freeing) {
		zend_list_delete(stream->res);
	}
}

/* Called from php_stream_free() with the stream still frozen */
PHPAPI void php_io_stream_drain(php_stream *stream)
{
	if (!FG(io_orphans)) {
		return;
	}
	php_io_orphan *o = zend_hash_index_find_ptr(FG(io_orphans), php_io_stream_key(stream));
	if (!o) {
		return;
	}
	o->freeing = true;
	if (o->queue->ops->drain) {
		o->queue->ops->drain(o->queue, stream);
	}
	php_io_stream_unfreeze(stream);
}

/* Entry point */

static void php_io_op_detach_zobj(php_io_op *op)
{
	if (op->zobj) {
		zend_object *zobj = op->zobj;
		op->zobj = NULL;
		if (php_io_op_zobj_detach) {
			php_io_op_zobj_detach(zobj);
		}
		OBJ_RELEASE(zobj);
	}
}

/* The op is over from the caller's point of view: make sure no queue still
 * references it and invalidate every userland wrapper. */
static void php_io_op_finish(php_io_op *op)
{
	if (op->queue) {
		op->queue->ops->orphan(op->queue, op);
	}
	if (op->type == PHP_IO_OP_ANY) {
		for (uint32_t i = 0; i < op->u.any.n; i++) {
			php_io_op *m = op->u.any.ops[i];
			ZEND_ASSERT(!m->queue);
			if (!(m->flags & PHP_IO_OP_F_PERSISTENT)) {
				php_io_op_detach_zobj(m);
			}
		}
	}
	if (!(op->flags & PHP_IO_OP_F_PERSISTENT)) {
		php_io_op_detach_zobj(op);
	}
}

/* A provider installed after a persistent op was created has not seen it */
static void php_io_op_register_persistent(php_io_op *op)
{
	if (op->flags & PHP_IO_OP_F_PERSISTENT) {
		php_io_persistent_register((php_io_persistent_op *) op);
	}
	if (op->type == PHP_IO_OP_ANY) {
		for (uint32_t i = 0; i < op->u.any.n; i++) {
			if (op->u.any.ops[i]->flags & PHP_IO_OP_F_PERSISTENT) {
				php_io_persistent_register((php_io_persistent_op *) op->u.any.ops[i]);
			}
		}
	}
}

static php_io_queue *php_io_core_queue(void)
{
	if (!FG(io_queue)) {
		/* poll(2) handles regular files and needs no registration syscalls for
		 * the one-shot waits of the synchronous path */
		FG(io_queue) = php_io_queue_create_poll(PHP_POLL_BACKEND_POLL);
		if (!FG(io_queue)) {
			FG(io_queue) = php_io_queue_create_poll(PHP_POLL_BACKEND_AUTO);
		}
	}
	return FG(io_queue);
}

static zend_result php_io_run_sync_timer(php_io_op *op, php_io_op_result *result)
{
	result->index = 0;
	result->res = 0;
	result->error = 0;

	if (php_deadline_is_infinite(&op->deadline)) {
		/* A timer that never fires has no synchronous meaning */
		result->status = PHP_IO_UNSUPPORTED;
		return SUCCESS;
	}

	for (;;) {
		zend_hrtime_t remaining = php_io_deadline_remaining(&op->deadline, zend_hrtime());
		if (remaining == 0) {
			break;
		}
#ifdef PHP_WIN32
		Sleep((DWORD) ((remaining + 999999) / 1000000));
		break;
#else
		struct timespec ts = {
			.tv_sec = remaining / ZEND_NANO_IN_SEC,
			.tv_nsec = remaining % ZEND_NANO_IN_SEC,
		};
		if (nanosleep(&ts, NULL) == 0 || errno != EINTR) {
			break;
		}
#endif
	}

	result->status = PHP_IO_DONE;
	return SUCCESS;
}

static zend_result php_io_run_sync(php_io_op *op, php_io_op_result *result)
{
	if (op->type == PHP_IO_OP_TIMER) {
		return php_io_run_sync_timer(op, result);
	}

	php_io_queue *q = php_io_core_queue();

	result->index = 0;
	result->res = -1;
	result->error = 0;

	if (!q) {
		result->status = PHP_IO_DONE;
		result->error = ENOMEM;
		return SUCCESS;
	}

	if (q->ops->submit(q, op, NULL) == FAILURE) {
		result->status = PHP_IO_DONE;
		result->error = errno ? errno : EINVAL;
		return SUCCESS;
	}

	php_io_queue_completion c;
	int n;
	do {
		n = q->ops->wait(q, &c, 1, NULL);
	} while (n == 0);

	if (n < 0) {
		int err = errno;
		if (op->queue) {
			q->ops->orphan(q, op);
		}
		result->status = PHP_IO_DONE;
		result->error = err ? err : EIO;
		return SUCCESS;
	}

	ZEND_ASSERT(c.op == op);
	*result = c.result;
	return SUCCESS;
}

static zend_result php_io_run_ex(php_io_op *op, php_io_op_result *result);

PHPAPI zend_result php_io_run(php_io_op *op, php_io_op_result *result)
{
	FG(io_ops_in_flight)++;
	zend_result rc = php_io_run_ex(op, result);
	FG(io_ops_in_flight)--;
	return rc;
}

static zend_result php_io_run_ex(php_io_op *op, php_io_op_result *result)
{
	php_io_hooks_state *state = FG(io_hooks);

	if (state) {
		zend_object *pending = EG(exception);
		php_io_op_register_persistent(op);
		if (EG(exception) != pending) {
			result->status = PHP_IO_CANCELLED;
			result->res = -1;
			result->error = ECANCELED;
			return FAILURE;
		}
		zend_result rc = state->hooks.run(state->data, op, result);
		php_io_op_finish(op);

		if (rc == FAILURE) {
			result->status = PHP_IO_CANCELLED;
			result->res = -1;
			result->error = ECANCELED;
			return FAILURE;
		}
		if (result->status != PHP_IO_UNSUPPORTED) {
			return SUCCESS;
		}
	}

	zend_result rc = php_io_run_sync(op, result);
	php_io_op_finish(op);
	return rc;
}

/* Wrappers */

static int php_io_poll_result_to_revents(const php_io_op_result *result, uint32_t events)
{
	switch (result->status) {
		case PHP_IO_DONE:
		case PHP_IO_READY:
			if (result->error) {
				errno = result->error;
				return -1;
			}
			return result->res ? (int) result->res : (int) events;
		case PHP_IO_TIMEOUT:
			errno = ETIMEDOUT;
			return 0;
		case PHP_IO_INTERRUPTED:
			errno = EINTR;
			return -1;
		case PHP_IO_CANCELLED:
			errno = ECANCELED;
			return -1;
		case PHP_IO_UNSUPPORTED:
		default:
			errno = ENOTSUP;
			return -1;
	}
}

/* The stream is frozen for the duration; a queue keeping the op past this
 * frame (the ring on an abnormal exit) keeps it frozen until the op settled */
typedef struct {
	php_stream *stream;
	zend_object *handle;
} php_io_frame;

static void php_io_frame_begin(php_io_frame *f, php_stream *stream)
{
	f->stream = stream;
	f->handle = NULL;
	if (stream) {
		if (FG(io_hooks)) {
			zval handle_zv;
			php_stream_poll_weak_handle_from_stream(&handle_zv, stream);
			f->handle = Z_OBJ(handle_zv);
		}
		ZEND_ASSERT(!(stream->flags & PHP_STREAM_FLAG_IN_USE));
		stream->flags |= PHP_STREAM_FLAG_IN_USE;
	}
}

static void php_io_frame_end(php_io_frame *f, php_io_op *op)
{
	if (f->stream && !(op && op->in_flight)) {
		f->stream->flags &= ~PHP_STREAM_FLAG_IN_USE;
	}
	if (f->handle) {
		OBJ_RELEASE(f->handle);
	}
}

static zend_always_inline uint32_t php_io_hook_flags(void)
{
	php_io_hooks_state *state = FG(io_hooks);
	return state ? state->hooks.flags : 0;
}

PHPAPI int php_io_poll(php_stream *stream, php_socket_t fd, uint32_t events, php_deadline *dl)
{
	php_io_op op;
	php_io_op_result result;
	php_io_frame f;

	php_io_frame_begin(&f, stream);
	php_io_op_poll(&op, f.handle, fd, events, *dl);
	op.stream = stream;
	zend_result rc = php_io_run(&op, &result);
	php_io_frame_end(&f, &op);

	if (rc == FAILURE) {
		errno = ECANCELED;
		return -1;
	}
	return php_io_poll_result_to_revents(&result, events);
}

/* Status to a syscall-like return for a data op; true when the caller is done */
static bool php_io_data_result(const php_io_op_result *result, ssize_t *ret)
{
	switch (result->status) {
		case PHP_IO_DONE:
			if (result->error) {
				errno = result->error;
				*ret = -1;
			} else {
				*ret = (ssize_t) result->res;
			}
			return true;
		case PHP_IO_TIMEOUT:
			errno = ETIMEDOUT;
			*ret = -1;
			return true;
		case PHP_IO_INTERRUPTED:
			errno = EINTR;
			*ret = -1;
			return true;
		case PHP_IO_CANCELLED:
			errno = ECANCELED;
			*ret = -1;
			return true;
		case PHP_IO_READY:
		case PHP_IO_UNSUPPORTED:
		default:
			return false;
	}
}

/* The descriptor ladder of section 5.5: the syscall first and the op on
 * EAGAIN, or the op first with F_DIRECT; Ready means retry the syscall,
 * Unsupported means syscall first from now on. */
#define PHP_IO_DESCRIPTOR_OP(stream, fd, dl, SYSCALL, PREP) \
	do { \
		php_io_frame f; \
		php_io_op op; \
		php_io_op_result result; \
		ssize_t ret; \
		bool direct = (php_io_hook_flags() & PHP_IO_HOOKS_F_DIRECT) != 0; \
		bool waited = false; \
		php_io_frame_begin(&f, stream); \
		memset(&op, 0, sizeof(op)); \
		for (;;) { \
			if (!direct) { \
				ret = (SYSCALL); \
				if (ret >= 0 || !PHP_IS_TRANSIENT_ERROR(errno)) { \
					break; \
				} \
				if (waited && (dl)->hrtime == 0) { \
					/* A non-blocking deadline gets one readiness check */ \
					break; \
				} \
			} \
			PREP; \
			op.stream = (stream); \
			if (php_io_run(&op, &result) == FAILURE) { \
				errno = ECANCELED; \
				ret = -1; \
				break; \
			} \
			if (php_io_data_result(&result, &ret)) { \
				break; \
			} \
			waited = true; \
			if (result.status == PHP_IO_UNSUPPORTED && !direct) { \
				errno = ENOTSUP; \
				ret = -1; \
				break; \
			} \
			direct = false; \
		} \
		php_io_frame_end(&f, &op); \
		return ret; \
	} while (0)

PHPAPI ssize_t php_io_recv(php_stream *stream, php_socket_t fd, void *buf, size_t len, int flags, php_deadline *dl)
{
	PHP_IO_DESCRIPTOR_OP(stream, fd, dl,
			recv(fd, buf, len, flags),
			php_io_op_recv(&op, f.handle, fd, buf, len, flags, *dl));
}

PHPAPI ssize_t php_io_send(php_stream *stream, php_socket_t fd, const void *buf, size_t len, int flags, php_deadline *dl)
{
	PHP_IO_DESCRIPTOR_OP(stream, fd, dl,
			send(fd, buf, len, flags),
			php_io_op_send(&op, f.handle, fd, buf, len, flags, *dl));
}

PHPAPI php_socket_t php_io_accept(php_stream *stream, php_socket_t fd, struct sockaddr *addr, socklen_t *addrlen, php_deadline *dl)
{
	PHP_IO_DESCRIPTOR_OP(stream, fd, dl,
			(ssize_t) accept(fd, addr, addrlen),
			php_io_op_accept(&op, f.handle, fd, addr, addrlen, *dl));
}

/* The connect is started once; the wait completes as Ready and the result
 * is read from SO_ERROR, or as Done when the provider connected itself */
PHPAPI int php_io_connect(php_stream *stream, php_socket_t fd, const struct sockaddr *addr, socklen_t addrlen, php_deadline *dl)
{
	php_io_frame f;
	php_io_op op;
	php_io_op_result result;
	int ret = 0;
	bool direct = (php_io_hook_flags() & PHP_IO_HOOKS_F_DIRECT) != 0;

	php_io_frame_begin(&f, stream);
	memset(&op, 0, sizeof(op));

	if (!direct) {
		if (connect(fd, addr, addrlen) == 0) {
			goto out;
		}
		if (errno != EINPROGRESS && errno != EAGAIN && errno != EWOULDBLOCK) {
			ret = -1;
			goto out;
		}
	}

	for (;;) {
		php_io_op_connect(&op, f.handle, fd, addr, addrlen, *dl);
		op.stream = stream;
		if (php_io_run(&op, &result) == FAILURE) {
			errno = ECANCELED;
			ret = -1;
			break;
		}
		if (result.status == PHP_IO_READY) {
			int error = 0;
			socklen_t len = sizeof(error);
			if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *) &error, &len) != 0) {
				ret = -1;
			} else if (error) {
				errno = error;
				ret = -1;
			}
			break;
		}
		if (result.status == PHP_IO_UNSUPPORTED && direct) {
			/* Start it ourselves and wait for writability instead */
			direct = false;
			if (connect(fd, addr, addrlen) == 0) {
				break;
			}
			if (errno != EINPROGRESS && errno != EAGAIN && errno != EWOULDBLOCK) {
				ret = -1;
				break;
			}
			php_io_op_poll(&op, f.handle, fd, PHP_POLL_WRITE, *dl);
			continue;
		}
		ssize_t r;
		if (php_io_data_result(&result, &r)) {
			ret = r < 0 ? -1 : 0;
			break;
		}
		errno = ENOTSUP;
		ret = -1;
		break;
	}

out:
	php_io_frame_end(&f, &op);
	return ret;
}

/* Regular files have no readiness form: without F_FILES the call is
 * synchronous. Pipes and character devices keep blocking descriptors, so
 * they wait for readiness first unless the provider performs the op. */
static ssize_t php_io_file_op(php_stream *stream, int fd, php_deadline *dl, bool regular,
		ssize_t (*syscall_fn)(int, void *, size_t), void *buf, size_t len,
		void (*prep)(php_io_op *, zend_object *, php_socket_t, void *, size_t, int64_t, php_deadline))
{
	uint32_t flags = php_io_hook_flags();
	php_io_frame f;
	php_io_op op;
	php_io_op_result result;
	ssize_t ret;

	php_io_frame_begin(&f, stream);
	memset(&op, 0, sizeof(op));

	bool offload = FG(io_hooks) && (regular ? (flags & PHP_IO_HOOKS_F_FILES) : (flags & (PHP_IO_HOOKS_F_FILES | PHP_IO_HOOKS_F_DIRECT)));
	bool ready = false;
	for (;;) {
		if (offload) {
			prep(&op, f.handle, fd, buf, len, -1, *dl);
			op.stream = stream;
			if (php_io_run(&op, &result) == FAILURE) {
				errno = ECANCELED;
				ret = -1;
				break;
			}
			if (php_io_data_result(&result, &ret)) {
				break;
			}
			/* Ready or Unsupported: perform it here */
			ready = result.status == PHP_IO_READY;
			offload = false;
		}
		if (!regular && FG(io_hooks) && !ready) {
			/* A blocking descriptor: wait for readiness before the syscall */
			int n = php_io_poll(NULL, fd, prep == php_io_op_read ? PHP_POLL_READ : PHP_POLL_WRITE, dl);
			if (n < 0) {
				ret = -1;
				break;
			}
			if (n == 0) {
				errno = ETIMEDOUT;
				ret = -1;
				break;
			}
		}
		do {
			ret = syscall_fn(fd, buf, len);
		} while (ret < 0 && errno == EINTR);
		break;
	}

	php_io_frame_end(&f, &op);
	return ret;
}

static ssize_t php_io_read_syscall(int fd, void *buf, size_t len)
{
	return read(fd, buf, len);
}

static ssize_t php_io_write_syscall(int fd, void *buf, size_t len)
{
	return write(fd, buf, len);
}

PHPAPI ssize_t php_io_read(php_stream *stream, int fd, void *buf, size_t len, php_deadline *dl)
{
	bool regular = !stream || !(stream->flags & PHP_STREAM_FLAG_NO_SEEK);
	return php_io_file_op(stream, fd, dl, regular, php_io_read_syscall, buf, len, php_io_op_read);
}

PHPAPI ssize_t php_io_write(php_stream *stream, int fd, const void *buf, size_t len, php_deadline *dl)
{
	bool regular = !stream || !(stream->flags & PHP_STREAM_FLAG_NO_SEEK);
	return php_io_file_op(stream, fd, dl, regular, php_io_write_syscall, (void *) buf, len,
			(void (*)(php_io_op *, zend_object *, php_socket_t, void *, size_t, int64_t, php_deadline)) php_io_op_write);
}

PHPAPI int php_io_fsync(php_stream *stream, int fd, bool data_only)
{
	if (FG(io_hooks) && (php_io_hook_flags() & PHP_IO_HOOKS_F_FILES)) {
		php_io_frame f;
		php_io_op op;
		php_io_op_result result;
		ssize_t ret;

		php_io_frame_begin(&f, stream);
		php_io_op_fsync(&op, f.handle, fd, data_only);
		op.stream = stream;
		zend_result rc = php_io_run(&op, &result);
		php_io_frame_end(&f, &op);
		if (rc == FAILURE) {
			errno = ECANCELED;
			return -1;
		}
		if (php_io_data_result(&result, &ret)) {
			return ret < 0 ? -1 : 0;
		}
	}
#ifdef HAVE_FDATASYNC
	return data_only ? fdatasync(fd) : fsync(fd);
#else
	return fsync(fd);
#endif
}

PHPAPI zend_result php_io_sleep(php_deadline dl)
{
	php_io_op op;
	php_io_op_result result;

	php_io_op_timer(&op, dl);
	return php_io_run(&op, &result);
}
