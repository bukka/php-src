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
#include "php_network.h"
#include "main/php_io_ring.h"

#ifdef HAVE_IOR

#include <ior.h>
#include <errno.h>
#ifndef PHP_WIN32
# include <netdb.h>
# include <unistd.h>
# include <sys/wait.h>
#endif

/* The op layer's signal types are handed to ior as they are */
ZEND_STATIC_ASSERT(sizeof(php_sigset_t) == sizeof(ior_sigset_t), "php_sigset_t must match ior_sigset_t");
ZEND_STATIC_ASSERT(sizeof(php_siginfo_t) == sizeof(ior_siginfo_t), "php_siginfo_t must match ior_siginfo_t");

/* One submitted op. The main submission and its linked timeout each
 * produce a cqe, and the record lives until both were reaped, however the
 * op ended: a buffer stays in use until the main cqe arrived. What the
 * backend reads or writes besides a stream's buffer lives in the record
 * and reaches the op only when its completion is delivered. */
typedef struct _php_io_ring_req php_io_ring_req;

struct _php_io_ring_req {
	php_io_op *op;
	void *data;
	php_io_op_type type;
	php_io_op_result result;
	int32_t main_res;               /* the main cqe's result, -1 until then */
	php_io_ring_req *group;         /* member: the Any's request */
	uint32_t index;                 /* member: position in the Any */
	bool has_lt;                    /* a linked timeout was submitted */
	bool main_done;                 /* the main cqe was reaped */
	bool lt_done;                   /* the linked timeout's cqe was reaped */
	int32_t lt_res;
	bool cancelled;                 /* cancel() was called */
	bool orphaned;                  /* nobody wants the completion */
	bool delivered;                 /* the output went to the op */
	php_stream *orphan_stream;      /* frozen until the record settled */
	bool ready;                     /* top-level: completion to deliver */
	bool fired;                     /* group: in the fired list */
	bool group_done;                /* member: the group folded already */
	php_io_ring_req **members;      /* group */
	uint32_t n_members;
	uint32_t n_settled;             /* group: members whose cqes all arrived */
	php_io_ring_req *prev;          /* live list */
	php_io_ring_req *next;
	union {
		struct { char *node; char *service; struct addrinfo hints; bool has_hints;
		         struct addrinfo *res; } gai;
		struct { struct sockaddr *addr; socklen_t addrlen; int flags;
		         char *host; size_t hostlen; char *service; size_t servicelen; } gni;
		struct { struct sockaddr *addr; socklen_t addrlen; socklen_t cap; } sock;
		struct { php_socket_t fd; bool data_only; } fsync;
		struct { int status; } waitpid;
		struct { php_sigset_t set; php_siginfo_t info; } sigwait;
	} u;
};

struct php_io_ring {
	ior_ctx *ctx;
	uint32_t features;
	bool fd_nonblock;
	pid_t owner_pid;                /* a child inherits the ring but must not touch it */
	php_io_ring_req *live;          /* every record with a cqe outstanding or a completion to deliver */
	uint32_t pending;               /* submitted ops not yet delivered, orphans included */
	php_io_ring_req **ready;
	uint32_t n_ready;
	uint32_t ready_cap;
	php_io_ring_req **fired;
	uint32_t n_fired;
	uint32_t fired_cap;
	ior_cqe **cqes;
	uint32_t cqes_cap;
	bool notify_created;
};

/* Tags in the cqe user data: the linked timeout's cqe and a cancel's cqe
 * carry the record pointer with a low bit set */
#define PHP_IO_RING_TAG_LT     ((uintptr_t) 1)
#define PHP_IO_RING_TAG_CANCEL ((uintptr_t) 2)
#define PHP_IO_RING_TAG_MASK   ((uintptr_t) 3)

/* Every cqe of the record arrived */
static zend_always_inline bool php_io_ring_req_settled(php_io_ring_req *req)
{
	return req->main_done && (!req->has_lt || req->lt_done);
}

static void php_io_ring_list_push(php_io_ring_req ***list, uint32_t *n, uint32_t *cap, php_io_ring_req *req)
{
	if (*n == *cap) {
		*cap = *cap ? *cap * 2 : 16;
		*list = safe_erealloc(*list, *cap, sizeof(**list), 0);
	}
	(*list)[(*n)++] = req;
}

static void php_io_ring_list_remove(php_io_ring_req **list, uint32_t *n, php_io_ring_req *req)
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

/* Inherited across fork: the kernel ring is shared with the parent and the
 * worker threads do not exist here, so it is left alone and leaked */
static zend_always_inline bool php_io_ring_foreign(php_io_ring *ring)
{
	return ring->owner_pid != getpid();
}

PHPAPI php_io_ring *php_io_ring_create(uint32_t entries, bool fd_nonblock)
{
	ior_params params;
	memset(&params, 0, sizeof(params));
	params.backend = IOR_BACKEND_AUTO;
	if (fd_nonblock) {
		params.flags |= IOR_SETUP_FD_NONBLOCK;
	}

	ior_ctx *ctx;
	int rc = ior_queue_init_params(entries ? entries : PHP_IO_RING_DEFAULT_ENTRIES, &ctx, &params);
	if (rc < 0) {
		errno = -rc;
		return NULL;
	}

	php_io_ring *ring = ecalloc(1, sizeof(*ring));
	ring->ctx = ctx;
	ring->features = params.features;
	ring->owner_pid = getpid();
	ring->fd_nonblock = fd_nonblock;
	ring->cqes_cap = 64;
	ring->cqes = safe_emalloc(ring->cqes_cap, sizeof(*ring->cqes), 0);
	return ring;
}

PHPAPI uint32_t php_io_ring_features(php_io_ring *ring)
{
	return ring->features;
}

PHPAPI php_io_ring_backend_type php_io_ring_get_backend_type(php_io_ring *ring)
{
	switch (ior_get_backend_type(ring->ctx)) {
		case IOR_BACKEND_IOURING: return PHP_IO_RING_BACKEND_IO_URING;
		case IOR_BACKEND_IOCP: return PHP_IO_RING_BACKEND_IOCP;
		default: return PHP_IO_RING_BACKEND_THREADS;
	}
}

PHPAPI const char *php_io_ring_backend_name(php_io_ring *ring)
{
	return ior_get_backend_name(ring->ctx);
}

PHPAPI uint32_t php_io_ring_hook_flags(php_io_ring *ring)
{
	uint32_t flags = PHP_IO_HOOKS_F_FILES;
	if (ring->features & IOR_FEAT_NATIVE_ASYNC) {
		flags |= PHP_IO_HOOKS_F_DIRECT;
	}
	return flags;
}

PHPAPI php_socket_t php_io_ring_notify_fd(php_io_ring *ring)
{
	ior_fd_t fd = ior_notify_fd(ring->ctx);
	if (fd == IOR_INVALID_FD) {
		return SOCK_ERR;
	}
	ring->notify_created = true;
	return (php_socket_t) fd;
}

PHPAPI void php_io_ring_notify_clear(php_io_ring *ring)
{
	if (ring->notify_created) {
		ior_notify_clear(ring->ctx);
	}
}

PHPAPI uint32_t php_io_ring_count_pending(php_io_ring *ring)
{
	/* Orphans included: a loop must keep reaping until they settled */
	uint32_t n = ring->pending;
	for (php_io_ring_req *r = ring->live; r; r = r->next) {
		if (r->orphaned && !r->ready && !php_io_ring_req_settled(r)) {
			n++;
		}
	}
	return n;
}

static uint32_t php_io_ring_reap(php_io_ring *ring);

/* Records */

/* The inputs are copied and the outputs get slots of their own, so that a
 * record outliving the op's frame never touches it */
static void php_io_ring_req_capture(php_io_ring_req *req, php_io_op *op)
{
	switch (op->type) {
		case PHP_IO_OP_GETADDRINFO:
			req->u.gai.node = op->u.getaddrinfo.node ? estrdup(op->u.getaddrinfo.node) : NULL;
			req->u.gai.service = op->u.getaddrinfo.service ? estrdup(op->u.getaddrinfo.service) : NULL;
			if (op->u.getaddrinfo.hints) {
				req->u.gai.hints.ai_flags = op->u.getaddrinfo.hints->ai_flags;
				req->u.gai.hints.ai_family = op->u.getaddrinfo.hints->ai_family;
				req->u.gai.hints.ai_socktype = op->u.getaddrinfo.hints->ai_socktype;
				req->u.gai.hints.ai_protocol = op->u.getaddrinfo.hints->ai_protocol;
				req->u.gai.has_hints = true;
			}
			break;
		case PHP_IO_OP_GETNAMEINFO:
			req->u.gni.addrlen = op->u.getnameinfo.addrlen;
			req->u.gni.addr = emalloc(MAX(req->u.gni.addrlen, 1));
			memcpy(req->u.gni.addr, op->u.getnameinfo.addr, req->u.gni.addrlen);
			req->u.gni.flags = op->u.getnameinfo.flags;
			if (op->u.getnameinfo.host && op->u.getnameinfo.hostlen) {
				req->u.gni.hostlen = op->u.getnameinfo.hostlen;
				req->u.gni.host = emalloc(req->u.gni.hostlen);
			}
			if (op->u.getnameinfo.service && op->u.getnameinfo.servicelen) {
				req->u.gni.servicelen = op->u.getnameinfo.servicelen;
				req->u.gni.service = emalloc(req->u.gni.servicelen);
			}
			break;
		case PHP_IO_OP_ACCEPT:
			if (op->u.accept.addr && op->u.accept.addrlen) {
				req->u.sock.cap = *op->u.accept.addrlen;
				req->u.sock.addrlen = req->u.sock.cap;
				req->u.sock.addr = emalloc(MAX(req->u.sock.cap, 1));
			}
			break;
		case PHP_IO_OP_CONNECT:
			req->u.sock.addrlen = op->u.connect.addrlen;
			req->u.sock.addr = emalloc(MAX(req->u.sock.addrlen, 1));
			memcpy(req->u.sock.addr, op->u.connect.addr, req->u.sock.addrlen);
			break;
		case PHP_IO_OP_FSYNC:
			req->u.fsync.fd = op->fd;
			req->u.fsync.data_only = op->u.fsync.data_only;
			break;
		case PHP_IO_OP_SIGWAIT:
			req->u.sigwait.set = *op->u.sigwait.set;
			break;
		default:
			break;
	}
}

/* The completion is delivered: the op gets what the record collected */
static void php_io_ring_req_output(php_io_ring_req *req, php_io_op *op)
{
	int32_t res = req->main_res;

	req->delivered = true;
	switch (req->type) {
		case PHP_IO_OP_GETADDRINFO:
			if (op->u.getaddrinfo.res) {
				*op->u.getaddrinfo.res = req->u.gai.res;
				req->u.gai.res = NULL;
			}
			break;
		case PHP_IO_OP_GETNAMEINFO:
			if (res == 0) {
				if (req->u.gni.host) {
					memcpy(op->u.getnameinfo.host, req->u.gni.host, req->u.gni.hostlen);
				}
				if (req->u.gni.service) {
					memcpy(op->u.getnameinfo.service, req->u.gni.service, req->u.gni.servicelen);
				}
			}
			break;
		case PHP_IO_OP_ACCEPT:
			if (res >= 0 && req->u.sock.addr) {
				memcpy(op->u.accept.addr, req->u.sock.addr, MIN(req->u.sock.addrlen, req->u.sock.cap));
				*op->u.accept.addrlen = req->u.sock.addrlen;
			}
			break;
		case PHP_IO_OP_WAITPID:
			if (res > 0 && op->u.waitpid.status) {
				*op->u.waitpid.status = req->u.waitpid.status;
			}
			break;
		case PHP_IO_OP_SIGWAIT:
			if (res > 0 && op->u.sigwait.info) {
				*op->u.sigwait.info = req->u.sigwait.info;
			}
			break;
		default:
			break;
	}
}

/* The record goes away: what the op produced and nobody took is released */
static void php_io_ring_req_discard(php_io_ring_req *req)
{
	bool unclaimed = req->main_done && !req->delivered;
	int32_t res = req->main_res;

	switch (req->type) {
		case PHP_IO_OP_GETADDRINFO:
			if (req->u.gai.res) {
				freeaddrinfo(req->u.gai.res);
			}
			if (req->u.gai.node) {
				efree(req->u.gai.node);
			}
			if (req->u.gai.service) {
				efree(req->u.gai.service);
			}
			break;
		case PHP_IO_OP_GETNAMEINFO:
			efree(req->u.gni.addr);
			if (req->u.gni.host) {
				efree(req->u.gni.host);
			}
			if (req->u.gni.service) {
				efree(req->u.gni.service);
			}
			break;
		case PHP_IO_OP_ACCEPT:
			if (unclaimed && res >= 0) {
				closesocket((php_socket_t) res);
			}
			ZEND_FALLTHROUGH;
		case PHP_IO_OP_CONNECT:
			if (req->u.sock.addr) {
				efree(req->u.sock.addr);
			}
			break;
		case PHP_IO_OP_WAITPID:
			/* ior reaped the child: its status goes to the next wait for it */
#ifdef PHP_WIN32
			if (unclaimed && res > 0) {
#else
			if (unclaimed && res > 0 && (WIFEXITED(req->u.waitpid.status) || WIFSIGNALED(req->u.waitpid.status))) {
#endif
				php_io_child_reaped((pid_t) res, req->u.waitpid.status);
			}
			break;
		default:
			break;
	}
}

static php_io_ring_req *php_io_ring_req_create(php_io_ring *ring, php_io_op *op, void *data)
{
	php_io_ring_req *req = ecalloc(1, sizeof(*req));
	req->op = op;
	req->data = data;
	req->type = op->type;
	req->main_res = -1;
	php_io_ring_req_capture(req, op);
	req->next = ring->live;
	if (ring->live) {
		ring->live->prev = req;
	}
	ring->live = req;
	return req;
}

static void php_io_ring_req_free(php_io_ring *ring, php_io_ring_req *req)
{
	if (req->orphan_stream) {
		php_io_stream_unfreeze(req->orphan_stream);
		req->orphan_stream = NULL;
	}
	if (req->prev) {
		req->prev->next = req->next;
	} else {
		ring->live = req->next;
	}
	if (req->next) {
		req->next->prev = req->prev;
	}
	php_io_ring_req_discard(req);
	if (req->members) {
		efree(req->members);
	}
	efree(req);
}

/* Read and Write ops carry a file descriptor: on Windows a CRT one, whose
 * handle is what IOCP works on (an overlapped one, the plain wrapper's
 * promise for the files it hands over) */
static zend_always_inline ior_fd_t php_io_ring_file_fd(php_io_op *op)
{
#ifdef PHP_WIN32
	return (ior_fd_t) _get_osfhandle((int) op->fd);
#else
	return (ior_fd_t) op->fd;
#endif
}

/* Work callbacks: they run on a worker and see only the record */

static int32_t php_io_ring_work_getaddrinfo(ior_work_token *token, void *arg)
{
	php_io_ring_req *req = arg;
	return getaddrinfo(req->u.gai.node, req->u.gai.service,
			req->u.gai.has_hints ? &req->u.gai.hints : NULL, &req->u.gai.res);
}

static int32_t php_io_ring_work_getnameinfo(ior_work_token *token, void *arg)
{
	php_io_ring_req *req = arg;
	return getnameinfo(req->u.gni.addr, req->u.gni.addrlen,
			req->u.gni.host, req->u.gni.hostlen,
			req->u.gni.service, req->u.gni.servicelen, req->u.gni.flags);
}

static int32_t php_io_ring_work_fsync(ior_work_token *token, void *arg)
{
	php_io_ring_req *req = arg;
#ifdef HAVE_FDATASYNC
	int rc = req->u.fsync.data_only ? fdatasync((int) req->u.fsync.fd) : fsync((int) req->u.fsync.fd);
#else
	int rc = fsync((int) req->u.fsync.fd);
#endif
	return rc == 0 ? 0 : -errno;
}

static uint32_t php_io_ring_poll_mask_to_ior(uint32_t events)
{
	uint32_t mask = 0;
	if (events & PHP_POLL_READ) {
		mask |= IOR_POLL_IN;
	}
	if (events & PHP_POLL_WRITE) {
		mask |= IOR_POLL_OUT;
	}
	return mask;
}

static uint32_t php_io_ring_poll_mask_from_ior(uint32_t mask)
{
	uint32_t events = 0;
	if (mask & IOR_POLL_IN) {
		events |= PHP_POLL_READ;
	}
	if (mask & IOR_POLL_OUT) {
		events |= PHP_POLL_WRITE;
	}
	if (mask & (IOR_POLL_ERR | IOR_POLL_NVAL)) {
		events |= PHP_POLL_ERROR;
	}
	if (mask & IOR_POLL_HUP) {
		events |= PHP_POLL_HUP;
	}
	return events;
}

/* Relative, because zend_hrtime() runs on CLOCK_MONOTONIC_RAW where it
 * exists and ior's absolute deadlines offer the monotonic, boot-time and
 * wall clocks; ior copies the timespec at submit */
static void php_io_ring_deadline_to_ts(const php_deadline *dl, ior_timespec *ts)
{
	zend_hrtime_t remaining = php_io_deadline_remaining(dl, zend_hrtime());
	ts->tv_sec = (int64_t) (remaining / ZEND_NANO_IN_SEC);
	ts->tv_nsec = (long long) (remaining % ZEND_NANO_IN_SEC);
}

/* Preps and submits one op (a member of an Any included). Returns FAILURE
 * with errno set when the submission queue is full or the op has no ring
 * form; the caller decides what that means for the op. */
/* An entry taken for a prep that failed: a nop whose completion carries no
 * record, so the next submit issues nothing stale */
static void php_io_ring_sqe_void(ior_ctx *ctx, ior_sqe *sqe)
{
	ior_prep_nop(ctx, sqe);
	ior_sqe_set_data(ctx, sqe, NULL);
}

static zend_result php_io_ring_submit_one(php_io_ring *ring, php_io_ring_req *req)
{
	php_io_op *op = req->op;
	ior_ctx *ctx = ring->ctx;
	bool link_deadline = !php_deadline_is_infinite(&op->deadline) && op->type != PHP_IO_OP_TIMER;
	ior_timespec ts;   /* read by ior_submit() below */

	/* What has no ring form is refused before an entry is taken: a taken
	 * entry cannot be given back, and the next submit would issue it with
	 * whatever it still holds */
	switch (op->type) {
		case PHP_IO_OP_POLL:
			if (!(ring->features & IOR_FEAT_POLL_ADD)) {
				errno = ENOTSUP;
				return FAILURE;
			}
			break;
		case PHP_IO_OP_TIMER:
			if (php_deadline_is_infinite(&op->deadline)) {
				/* Never fires: ior_prep_nop would end it at once */
				errno = ENOTSUP;
				return FAILURE;
			}
			break;
		case PHP_IO_OP_READ:
		case PHP_IO_OP_WRITE:
		case PHP_IO_OP_RECV:
		case PHP_IO_OP_SEND:
		case PHP_IO_OP_ACCEPT:
		case PHP_IO_OP_CONNECT:
		case PHP_IO_OP_GETADDRINFO:
		case PHP_IO_OP_GETNAMEINFO:
		case PHP_IO_OP_FSYNC:
		case PHP_IO_OP_WAITPID:
		case PHP_IO_OP_SIGWAIT:
			break;
		default:
			errno = ENOTSUP;
			return FAILURE;
	}

	ior_sqe *sqe = ior_get_sqe(ctx);
	if (!sqe) {
		errno = EBUSY;
		return FAILURE;
	}

	switch (op->type) {
		case PHP_IO_OP_POLL:
			ior_prep_poll_add(ctx, sqe, (ior_fd_t) op->fd, php_io_ring_poll_mask_to_ior(op->u.poll.events));
			break;
		case PHP_IO_OP_TIMER:
			php_io_ring_deadline_to_ts(&op->deadline, &ts);
			ior_prep_timeout(ctx, sqe, &ts, 0, 0);
			break;
		case PHP_IO_OP_READ:
			ior_prep_read(ctx, sqe, php_io_ring_file_fd(op), op->u.io.buf, (unsigned) MIN(op->u.io.len, UINT32_MAX),
					op->u.io.offset < 0 ? IOR_OFF_NONE : (uint64_t) op->u.io.offset);
			op->in_flight = true;
			break;
		case PHP_IO_OP_WRITE:
			ior_prep_write(ctx, sqe, php_io_ring_file_fd(op), op->u.io.buf, (unsigned) MIN(op->u.io.len, UINT32_MAX),
					op->u.io.offset < 0 ? IOR_OFF_NONE : (uint64_t) op->u.io.offset);
			op->in_flight = true;
			break;
		case PHP_IO_OP_RECV:
			ior_prep_recv(ctx, sqe, (ior_fd_t) op->fd, op->u.io.buf, (unsigned) MIN(op->u.io.len, UINT32_MAX), op->u.io.flags);
			op->in_flight = true;
			break;
		case PHP_IO_OP_SEND:
			ior_prep_send(ctx, sqe, (ior_fd_t) op->fd, op->u.io.buf, (unsigned) MIN(op->u.io.len, UINT32_MAX), op->u.io.flags);
			op->in_flight = true;
			break;
		case PHP_IO_OP_ACCEPT:
			ior_prep_accept(ctx, sqe, (ior_fd_t) op->fd, req->u.sock.addr, req->u.sock.addr ? &req->u.sock.addrlen : NULL,
					IOR_ACCEPT_CLOEXEC | (ring->fd_nonblock ? IOR_ACCEPT_NONBLOCK : 0));
			op->in_flight = true;
			break;
		case PHP_IO_OP_CONNECT:
			ior_prep_connect(ctx, sqe, (ior_fd_t) op->fd, req->u.sock.addr, req->u.sock.addrlen);
			op->in_flight = true;
			break;
		case PHP_IO_OP_GETADDRINFO:
			if (ior_prep_work(ctx, sqe, php_io_ring_work_getaddrinfo, req) < 0) {
				php_io_ring_sqe_void(ctx, sqe);
				errno = ENOTSUP;
				return FAILURE;
			}
			break;
		case PHP_IO_OP_GETNAMEINFO:
			if (ior_prep_work(ctx, sqe, php_io_ring_work_getnameinfo, req) < 0) {
				php_io_ring_sqe_void(ctx, sqe);
				errno = ENOTSUP;
				return FAILURE;
			}
			break;
		case PHP_IO_OP_FSYNC:
			if (ior_prep_work(ctx, sqe, php_io_ring_work_fsync, req) < 0) {
				php_io_ring_sqe_void(ctx, sqe);
				errno = ENOTSUP;
				return FAILURE;
			}
			op->in_flight = true;
			break;
		case PHP_IO_OP_WAITPID:
			if (ior_prep_waitpid(ctx, sqe, (ior_pid_t) op->u.waitpid.pid, &req->u.waitpid.status, op->u.waitpid.options) < 0) {
				php_io_ring_sqe_void(ctx, sqe);
				errno = ENOTSUP;
				return FAILURE;
			}
			break;
		case PHP_IO_OP_SIGWAIT: {
			int rc = ior_prep_sigwait(ctx, sqe, (const ior_sigset_t *) &req->u.sigwait.set, (ior_siginfo_t *) &req->u.sigwait.info);
			if (rc < 0) {
				php_io_ring_sqe_void(ctx, sqe);
				errno = -rc;
				return FAILURE;
			}
			break;
		}
		default:
			ZEND_UNREACHABLE();
	}

	ior_sqe_set_data(ctx, sqe, req);

	if (link_deadline) {
		ior_sqe *lt = ior_get_sqe(ctx);
		if (!lt) {
			/* The prepared entry cannot be taken back: submit it unbounded */
			ior_submit(ctx);
		} else {
			ior_sqe_set_flags(ctx, sqe, IOR_SQE_IO_LINK);
			php_io_ring_deadline_to_ts(&op->deadline, &ts);
			ior_prep_link_timeout(ctx, lt, &ts, 0);
			ior_sqe_set_data(ctx, lt, (void *) ((uintptr_t) req | PHP_IO_RING_TAG_LT));
			req->has_lt = true;
		}
	}

	int rc = ior_submit(ctx);
	if (rc < 0) {
		/* The entries stay queued in ior and are retried by the next submit */
		errno = -rc;
	}
	return SUCCESS;
}

static void php_io_ring_submit_cancel(php_io_ring *ring, php_io_ring_req *req)
{
	ior_sqe *sqe = ior_get_sqe(ring->ctx);
	if (!sqe) {
		ior_submit(ring->ctx);
		sqe = ior_get_sqe(ring->ctx);
		if (!sqe) {
			return;
		}
	}
	ior_prep_cancel(ring->ctx, sqe, req);
	ior_sqe_set_data(ring->ctx, sqe, (void *) ((uintptr_t) req | PHP_IO_RING_TAG_CANCEL));
	ior_submit(ring->ctx);
}

PHPAPI zend_result php_io_ring_submit_op(php_io_ring *ring, php_io_op *op, void *data)
{
	if (php_io_ring_foreign(ring)) {
		errno = EPERM;
		return FAILURE;
	}
	if (op->queue_data) {
		errno = EALREADY;
		return FAILURE;
	}

	php_io_ring_req *req = php_io_ring_req_create(ring, op, data);
	op->queue_data = req;
	ring->pending++;

	if (op->type == PHP_IO_OP_ANY) {
		uint32_t n = op->u.any.n;
		op->u.any.n_results = 0;
		req->n_members = n;
		req->members = n ? safe_emalloc(n, sizeof(*req->members), 0) : NULL;
		req->main_done = true;              /* the Any itself has no cqe */
		for (uint32_t i = 0; i < n; i++) {
			php_io_ring_req *m = php_io_ring_req_create(ring, op->u.any.ops[i], NULL);
			m->group = req;
			m->index = i;
			op->u.any.ops[i]->queue_data = m;
			req->members[i] = m;
		}
		bool any_fired = false;
		for (uint32_t i = 0; i < n; i++) {
			php_io_ring_req *m = req->members[i];
			if (php_io_ring_submit_one(ring, m) != SUCCESS) {
				/* A member without a ring form completes at once */
				m->main_done = true;
				m->result.status = errno == ENOTSUP ? PHP_IO_UNSUPPORTED : PHP_IO_DONE;
				m->result.index = i;
				m->result.res = -1;
				m->result.error = errno == ENOTSUP ? 0 : errno;
				any_fired = true;
			}
		}
		if (any_fired || n == 0) {
			req->fired = true;
			php_io_ring_list_push(&ring->fired, &ring->n_fired, &ring->fired_cap, req);
		}
		return SUCCESS;
	}

	if (php_io_ring_submit_one(ring, req) != SUCCESS) {
		req->main_done = true;
		req->ready = true;
		req->result.status = errno == ENOTSUP ? PHP_IO_UNSUPPORTED : PHP_IO_DONE;
		req->result.index = 0;
		req->result.res = -1;
		req->result.error = errno == ENOTSUP ? 0 : errno;
		php_io_ring_list_push(&ring->ready, &ring->n_ready, &ring->ready_cap, req);
	}
	return SUCCESS;
}

/* Cancel every cqe-producing submission of a record */
static void php_io_ring_req_cancel(php_io_ring *ring, php_io_ring_req *req)
{
	if (!req->main_done && !req->cancelled) {
		req->cancelled = true;
		php_io_ring_submit_cancel(ring, req);
	}
}

/* The record is no longer wanted: drop it now if settled, or let the reap
 * that settles it drop it */
static void php_io_ring_req_release(php_io_ring *ring, php_io_ring_req *req)
{
	req->orphaned = true;
	if (php_io_ring_req_settled(req)) {
		/* Nothing outstanding: no freeze to keep */
		req->orphan_stream = NULL;
	}
	if (req->ready) {
		php_io_ring_list_remove(ring->ready, &ring->n_ready, req);
		req->ready = false;
	}
	if (req->op) {
		req->op->queue_data = NULL;
		req->op->queue = NULL;
		req->op->in_flight = false;
		req->op = NULL;
	}
	if (php_io_ring_req_settled(req)) {
		php_io_ring_req_free(ring, req);
	}
}

/* In a child the record can neither be cancelled nor complete: it is
 * settled here so that the release frees it */
static void php_io_ring_req_cancel_or_forget(php_io_ring *ring, php_io_ring_req *req)
{
	if (php_io_ring_foreign(ring)) {
		req->main_done = true;
		req->lt_done = true;
		req->delivered = true;
		req->orphan_stream = NULL;
		return;
	}
	php_io_ring_req_cancel(ring, req);
}

PHPAPI zend_result php_io_ring_cancel(php_io_ring *ring, php_io_op *op)
{
	php_io_ring_req *req = op->queue_data;
	if (!req) {
		errno = ENOENT;
		return FAILURE;
	}
	if (req->group) {
		errno = EINVAL;
		return FAILURE;
	}

	ring->pending--;
	if (op->type == PHP_IO_OP_ANY) {
		if (req->fired) {
			php_io_ring_list_remove(ring->fired, &ring->n_fired, req);
			req->fired = false;
		}
		for (uint32_t i = 0; i < req->n_members; i++) {
			php_io_ring_req *m = req->members[i];
			m->group = NULL;
			php_io_ring_req_cancel_or_forget(ring, m);
			php_io_ring_req_release(ring, m);
		}
		req->n_members = 0;
	} else {
		php_io_ring_req_cancel_or_forget(ring, req);
	}
	php_io_ring_req_release(ring, req);
	return SUCCESS;
}

PHPAPI bool php_io_ring_orphan(php_io_ring *ring, php_io_op *op)
{
	/* The caller's frame is going away; a record still in flight keeps the
	 * stream frozen and finishes silently in a later wait. In a child
	 * nothing completes, so nothing is kept. */
	php_io_ring_req *req = op->queue_data;
	bool keep = req && op->in_flight && op->stream && !req->group
			&& op->type != PHP_IO_OP_ANY && !php_io_ring_req_settled(req)
			&& !php_io_ring_foreign(ring);
	if (keep) {
		req->orphan_stream = op->stream;
	}
	php_io_ring_cancel(ring, op);
	if (keep) {
		op->in_flight = true;
	}
	return keep;
}

/* Wait until every orphaned op on the stream settled; other completions
 * stay queued for delivery */
PHPAPI void php_io_ring_drain(php_io_ring *ring, php_stream *stream)
{
	if (php_io_ring_foreign(ring)) {
		return;
	}
	for (;;) {
		bool pending = false;
		for (php_io_ring_req *r = ring->live; r; r = r->next) {
			if (r->orphan_stream == stream) {
				pending = true;
				break;
			}
		}
		if (!pending) {
			return;
		}
		ior_cqe *cqe;
		int rc = ior_wait_cqe(ring->ctx, &cqe);
		if (rc < 0 && rc != -EINTR) {
			return;
		}
		php_io_ring_reap(ring);
	}
}

/* Completion processing */

static void php_io_ring_result_from_cqe(php_io_ring_req *req, int32_t res)
{
	php_io_op_result *r = &req->result;
	bool dns = req->type == PHP_IO_OP_GETADDRINFO || req->type == PHP_IO_OP_GETNAMEINFO;

	r->index = req->index;
	r->error = 0;
	r->res = res;

	if (res >= 0) {
		r->status = PHP_IO_DONE;
		if (req->type == PHP_IO_OP_POLL) {
			r->res = php_io_ring_poll_mask_from_ior((uint32_t) res);
		} else if (dns && res != 0) {
			/* EAI_* codes are the work result as they are */
			r->error = res;
			r->res = -1;
		}
		return;
	}

	switch (res) {
		case -ETIME:
			r->status = req->type == PHP_IO_OP_TIMER ? PHP_IO_DONE : PHP_IO_TIMEOUT;
			r->res = 0;
			break;
		case -ECANCELED:
			/* A fired linked timeout cancels the guarded op */
			r->status = req->cancelled ? PHP_IO_CANCELLED : (req->has_lt ? PHP_IO_TIMEOUT : PHP_IO_CANCELLED);
			r->res = -1;
			break;
		case -EOPNOTSUPP:
		case -ENOSYS:
#if defined(ENOTSUP) && ENOTSUP != EOPNOTSUPP
		case -ENOTSUP:
#endif
		case -ENOTSOCK:
			r->status = PHP_IO_UNSUPPORTED;
			r->res = -1;
			break;
		default:
			r->status = PHP_IO_DONE;
			/* A negative EAI_* code for the DNS ops */
			r->error = dns ? res : -res;
			r->res = -1;
			break;
	}
}

/* The main cqe of a record arrived */
static void php_io_ring_req_main_cqe(php_io_ring *ring, php_io_ring_req *req, int32_t res)
{
	req->main_done = true;
	req->main_res = res;
	if (req->op) {
		req->op->in_flight = false;
	}

	if (req->orphaned) {
		if (php_io_ring_req_settled(req)) {
			php_io_ring_req_free(ring, req);
		}
		return;
	}

	php_io_ring_result_from_cqe(req, res);

	if (req->group) {
		if (!req->group->group_done && !req->group->fired) {
			req->group->fired = true;
			php_io_ring_list_push(&ring->fired, &ring->n_fired, &ring->fired_cap, req->group);
		}
		return;
	}

	req->ready = true;
	php_io_ring_list_push(&ring->ready, &ring->n_ready, &ring->ready_cap, req);
}

static void php_io_ring_req_lt_cqe(php_io_ring *ring, php_io_ring_req *req, int32_t res)
{
	req->lt_done = true;
	req->lt_res = res;
	if (req->orphaned && php_io_ring_req_settled(req)) {
		php_io_ring_req_free(ring, req);
	}
}

/* The Any completes with the members that completed by now; the rest are
 * cancelled and their cqes consumed silently */
static void php_io_ring_group_fold(php_io_ring *ring, php_io_ring_req *req)
{
	php_io_op *op = req->op;
	uint32_t n_results = 0;

	req->fired = false;
	req->group_done = true;

	for (uint32_t i = 0; i < req->n_members; i++) {
		php_io_ring_req *m = req->members[i];
		if (m->main_done) {
			if (op->u.any.results) {
				op->u.any.results[n_results] = m->result;
			}
			php_io_ring_req_output(m, m->op);
			n_results++;
		} else {
			php_io_ring_req_cancel(ring, m);
		}
		m->group = NULL;
		php_io_ring_req_release(ring, m);
	}
	req->n_members = 0;
	op->u.any.n_results = n_results;

	req->ready = true;
	req->result.status = PHP_IO_DONE;
	req->result.index = 0;
	req->result.res = 0;
	req->result.error = 0;
	php_io_ring_list_push(&ring->ready, &ring->n_ready, &ring->ready_cap, req);
}

static void php_io_ring_fold_all(php_io_ring *ring)
{
	while (ring->n_fired) {
		php_io_ring_req *req = ring->fired[0];
		php_io_ring_list_remove(ring->fired, &ring->n_fired, req);
		php_io_ring_group_fold(ring, req);
	}
}

static void php_io_ring_process_cqe(php_io_ring *ring, ior_cqe *cqe)
{
	uintptr_t data = (uintptr_t) ior_cqe_get_data(ring->ctx, cqe);
	int32_t res = ior_cqe_get_res(ring->ctx, cqe);
	php_io_ring_req *req = (php_io_ring_req *) (data & ~PHP_IO_RING_TAG_MASK);

	if (!req) {
		return;
	}
	switch (data & PHP_IO_RING_TAG_MASK) {
		case PHP_IO_RING_TAG_LT:
			php_io_ring_req_lt_cqe(ring, req, res);
			break;
		case PHP_IO_RING_TAG_CANCEL:
			/* The target completes on its own, whatever the cancel reported */
			break;
		default:
			php_io_ring_req_main_cqe(ring, req, res);
			break;
	}
}

/* Take everything the ring has */
static uint32_t php_io_ring_reap(php_io_ring *ring)
{
	uint32_t total = 0;
	for (;;) {
		unsigned n = ior_peek_batch_cqe(ring->ctx, ring->cqes, ring->cqes_cap);
		if (n == 0) {
			break;
		}
		for (unsigned i = 0; i < n; i++) {
			php_io_ring_process_cqe(ring, ring->cqes[i]);
		}
		ior_cq_advance(ring->ctx, n);
		total += n;
		if (n < ring->cqes_cap) {
			break;
		}
	}
	php_io_ring_fold_all(ring);
	return total;
}

static uint32_t php_io_ring_deliver(php_io_ring *ring, php_io_queue_completion *out, uint32_t max)
{
	uint32_t n = MIN(max, ring->n_ready);

	for (uint32_t i = 0; i < n; i++) {
		php_io_ring_req *req = ring->ready[i];
		if (req->type != PHP_IO_OP_ANY) {
			php_io_ring_req_output(req, req->op);
		}
		out[i].op = req->op;
		out[i].data = req->data;
		out[i].result = req->result;
		req->ready = false;
		ring->pending--;
		req->op->queue_data = NULL;
		req->op->queue = NULL;
		req->op = NULL;
		req->orphaned = true;
		if (php_io_ring_req_settled(req)) {
			php_io_ring_req_free(ring, req);
		}
	}
	memmove(ring->ready, &ring->ready[n], (ring->n_ready - n) * sizeof(*ring->ready));
	ring->n_ready -= n;
	return n;
}

PHPAPI int php_io_ring_wait(php_io_ring *ring, php_io_queue_completion *out, uint32_t max, const struct timespec *timeout)
{
	if (php_io_ring_foreign(ring)) {
		errno = EPERM;
		return -1;
	}
	zend_hrtime_t limit = ZEND_HRTIME_T_MAX;
	/* Only orphans: once they settled there is nothing to report, as
	 * count_pending() told */
	bool orphans_only = ring->pending == 0 && ring->live;

	if (max == 0) {
		return 0;
	}
	if (timeout) {
		zend_hrtime_t now = zend_hrtime();
		zend_hrtime_t rel = (zend_hrtime_t) timeout->tv_sec * ZEND_NANO_IN_SEC + timeout->tv_nsec;
		limit = rel < ZEND_HRTIME_T_MAX - now ? now + rel : ZEND_HRTIME_T_MAX;
		if (rel == 0) {
			/* A loop woken by the notification descriptor: clear, then reap until empty */
			php_io_ring_notify_clear(ring);
		}
	}

	for (;;) {
		php_io_ring_reap(ring);
		if (ring->n_ready) {
			return (int) php_io_ring_deliver(ring, out, max);
		}
		if (limit != ZEND_HRTIME_T_MAX && zend_hrtime() >= limit) {
			return 0;
		}
		if (ring->pending == 0) {
			if (ring->live) {
				orphans_only = true;
			} else if (orphans_only) {
				return 0;
			} else if (limit == ZEND_HRTIME_T_MAX) {
				errno = EDEADLK;
				return -1;
			}
		}

		ior_cqe *cqe;
		int rc;
		if (limit == ZEND_HRTIME_T_MAX) {
			rc = ior_wait_cqe(ring->ctx, &cqe);
		} else {
			zend_hrtime_t now = zend_hrtime();
			zend_hrtime_t remaining = limit > now ? limit - now : 0;
			ior_timespec ts = { .tv_sec = (int64_t) (remaining / ZEND_NANO_IN_SEC), .tv_nsec = (long long) (remaining % ZEND_NANO_IN_SEC) };
			rc = ior_wait_cqe_timeout(ring->ctx, &cqe, &ts);
		}
		if (rc == -ETIME) {
			php_io_ring_reap(ring);
			return (int) php_io_ring_deliver(ring, out, max);
		}
		if (rc == -EINTR) {
			continue;
		}
		if (rc < 0) {
			errno = -rc;
			return -1;
		}
		/* The next reap consumes it */
	}
}

PHPAPI void php_io_ring_destroy(php_io_ring *ring)
{
	if (php_io_ring_foreign(ring)) {
		while (ring->live) {
			php_io_ring_req *req = ring->live;
			req->orphaned = false;
			req->delivered = true;
			if (req->ready) {
				php_io_ring_list_remove(ring->ready, &ring->n_ready, req);
			}
			php_io_ring_req_free(ring, req);
		}
		if (ring->ready) {
			efree(ring->ready);
		}
		if (ring->fired) {
			efree(ring->fired);
		}
		efree(ring->cqes);
		efree(ring);
		return;
	}

	/* Cancel everything in flight and drain until each has completed */
	for (php_io_ring_req *req = ring->live; req; req = req->next) {
		php_io_ring_req_cancel(ring, req);
	}
	while (ring->live) {
		php_io_ring_req *req = ring->live;
		if (php_io_ring_req_settled(req)) {
			if (req->ready) {
				php_io_ring_list_remove(ring->ready, &ring->n_ready, req);
			}
			php_io_ring_req_free(ring, req);
			continue;
		}
		ior_cqe *cqe;
		if (ior_wait_cqe(ring->ctx, &cqe) < 0) {
			break;
		}
		php_io_ring_reap(ring);
		/* Settled records that were still wanted are dropped on the next pass */
		for (php_io_ring_req *r = ring->live; r; r = r->next) {
			r->orphaned = true;
		}
	}
	ior_queue_exit(ring->ctx);
	if (ring->ready) {
		efree(ring->ready);
	}
	if (ring->fired) {
		efree(ring->fired);
	}
	efree(ring->cqes);
	efree(ring);
}

/* The ring as an operation queue */

typedef struct {
	php_io_queue base;
	php_io_ring *ring;
} php_io_ring_queue;

static zend_result php_io_ring_queue_submit(php_io_queue *base, php_io_op *op, void *data)
{
	php_io_ring_queue *q = (php_io_ring_queue *) base;
	if (op->queue) {
		errno = EALREADY;
		return FAILURE;
	}
	op->queue = base;
	if (op->type == PHP_IO_OP_ANY) {
		for (uint32_t i = 0; i < op->u.any.n; i++) {
			op->u.any.ops[i]->queue = base;
		}
	}
	if (php_io_ring_submit_op(q->ring, op, data) != SUCCESS) {
		op->queue = NULL;
		return FAILURE;
	}
	return SUCCESS;
}

static zend_result php_io_ring_queue_cancel(php_io_queue *base, php_io_op *op)
{
	php_io_ring_queue *q = (php_io_ring_queue *) base;
	if (op->queue != base) {
		errno = ENOENT;
		return FAILURE;
	}
	return php_io_ring_cancel(q->ring, op);
}

static zend_result php_io_ring_queue_add(php_io_queue *base, php_io_op *op)
{
	/* Each run is a fresh single-shot poll; nothing to retain yet */
	return SUCCESS;
}

static void php_io_ring_queue_remove(php_io_queue *base, php_io_op *op)
{
}

static int php_io_ring_queue_wait(php_io_queue *base, php_io_queue_completion *out, uint32_t max, const struct timespec *timeout)
{
	return php_io_ring_wait(((php_io_ring_queue *) base)->ring, out, max, timeout);
}

static void php_io_ring_queue_orphan(php_io_queue *base, php_io_op *op)
{
	if (op->queue == base && php_io_ring_orphan(((php_io_ring_queue *) base)->ring, op)) {
		php_io_stream_orphan(op->stream, base);
	}
}

static void php_io_ring_queue_drain(php_io_queue *base, php_stream *stream)
{
	php_io_ring_drain(((php_io_ring_queue *) base)->ring, stream);
}

static uint32_t php_io_ring_queue_count_pending(php_io_queue *base)
{
	return php_io_ring_count_pending(((php_io_ring_queue *) base)->ring);
}

static uint32_t php_io_ring_queue_hook_flags(php_io_queue *base)
{
	return php_io_ring_hook_flags(((php_io_ring_queue *) base)->ring);
}

static void php_io_ring_queue_destroy(php_io_queue *base)
{
	php_io_ring_queue *q = (php_io_ring_queue *) base;
	php_io_ring_destroy(q->ring);
	efree(q);
}

static const php_io_queue_ops php_io_ring_queue_ops = {
	.submit = php_io_ring_queue_submit,
	.cancel = php_io_ring_queue_cancel,
	.add = php_io_ring_queue_add,
	.remove = php_io_ring_queue_remove,
	.wait = php_io_ring_queue_wait,
	.orphan = php_io_ring_queue_orphan,
	.drain = php_io_ring_queue_drain,
	.count_pending = php_io_ring_queue_count_pending,
	.hook_flags = php_io_ring_queue_hook_flags,
	.destroy = php_io_ring_queue_destroy,
};

PHPAPI php_io_ring *php_io_queue_ring(php_io_queue *q)
{
	ZEND_ASSERT(q->ops == &php_io_ring_queue_ops);
	return ((php_io_ring_queue *) q)->ring;
}

PHPAPI php_io_queue *php_io_queue_create_ring(uint32_t entries)
{
	/* No IOR_SETUP_FD_NONBLOCK: a userland Io\Ring\Engine may be handed a
	 * blocking stream, which the thread backend then makes non-blocking
	 * itself so that the op waits on its poller and stays cancellable */
	php_io_ring *ring = php_io_ring_create(entries, false);
	if (!ring) {
		return NULL;
	}
	php_io_ring_queue *q = ecalloc(1, sizeof(*q));
	q->base.ops = &php_io_ring_queue_ops;
	q->ring = ring;
	return &q->base;
}

#endif /* HAVE_IOR */
