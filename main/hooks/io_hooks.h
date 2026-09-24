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

#ifndef PHP_HOOKS_IO_HOOKS_H
#define PHP_HOOKS_IO_HOOKS_H

#include "php.h"
#include "php_network.h"
#include "php_streams.h"
#include "php_deadline.h"
#include "main/php_poll.h"

BEGIN_EXTERN_C()

/* Operations */

typedef enum {
	PHP_IO_OP_POLL,
	PHP_IO_OP_TIMER,
	PHP_IO_OP_READ,
	PHP_IO_OP_WRITE,
	PHP_IO_OP_RECV,
	PHP_IO_OP_SEND,
	PHP_IO_OP_ACCEPT,
	PHP_IO_OP_CONNECT,
	PHP_IO_OP_GETADDRINFO,
	PHP_IO_OP_GETNAMEINFO,
	PHP_IO_OP_FSYNC,
	PHP_IO_OP_WAITPID,
	PHP_IO_OP_SIGWAIT,
	PHP_IO_OP_ANY,
} php_io_op_type;

#define PHP_IO_OP_F_PERSISTENT 0x01   /* POLL only: a registration that outlives one run */

typedef enum {
	PHP_IO_DONE,
	PHP_IO_READY,
	PHP_IO_TIMEOUT,
	PHP_IO_INTERRUPTED,
	PHP_IO_CANCELLED,
	PHP_IO_UNSUPPORTED,
} php_io_status;

typedef struct _php_io_op_result {
	php_io_status status;
	uint32_t index;             /* member index, in the results of an ANY op */
	int64_t res;                /* bytes, accepted fd, PHP_POLL_* revents */
	int error;                  /* errno, or EAI_* for GETADDRINFO */
} php_io_op_result;

typedef struct _php_io_op php_io_op;
typedef struct _php_io_queue php_io_queue;

struct _php_io_op {
	php_io_op_type type;
	uint32_t flags;             /* PHP_IO_OP_F_* */
	zend_object *handle;        /* Io\Poll\Handle identity; NULL for TIMER, DNS ops and ANY */
	uint32_t ready_events;      /* PHP_POLL_* that let the op proceed */
	php_socket_t fd;
	php_deadline deadline;      /* absolute monotonic */
	union {
		struct { uint32_t events; } poll;
		struct { void *buf; size_t len; int64_t offset; int flags; } io;
		struct { struct sockaddr *addr; socklen_t *addrlen; } accept;
		struct { const struct sockaddr *addr; socklen_t addrlen; } connect;
		struct { const char *node; const char *service;
		         const struct addrinfo *hints; struct addrinfo **res; } getaddrinfo;
		struct { bool data_only; } fsync;
		struct { php_io_op **ops; uint32_t n;                          /* members, caller owned */
		         php_io_op_result *results; uint32_t n_results; } any;  /* filled on completion */
	} u;
	php_stream *stream;         /* the stream frozen for the op, NULL for other descriptor owners */
	zend_object *zobj;          /* Io\Operation wrapper, created lazily for userland hooks */
	void *provider_data;        /* provider scratch, never read by the core */
	php_io_queue *queue;        /* set by the queue at submit, cleared at completion */
	void *queue_data;           /* the queue's request record, never read by the core */
	bool in_flight;             /* the queue's backend references buf or addr */
};

PHPAPI void php_io_op_poll(php_io_op *op, zend_object *handle, php_socket_t fd, uint32_t events, php_deadline dl);
PHPAPI void php_io_op_timer(php_io_op *op, php_deadline dl);
PHPAPI void php_io_op_read(php_io_op *op, zend_object *handle, php_socket_t fd, void *buf, size_t len, int64_t off, php_deadline dl);
PHPAPI void php_io_op_write(php_io_op *op, zend_object *handle, php_socket_t fd, const void *buf, size_t len, int64_t off, php_deadline dl);
PHPAPI void php_io_op_recv(php_io_op *op, zend_object *handle, php_socket_t fd, void *buf, size_t len, int flags, php_deadline dl);
PHPAPI void php_io_op_send(php_io_op *op, zend_object *handle, php_socket_t fd, const void *buf, size_t len, int flags, php_deadline dl);
PHPAPI void php_io_op_accept(php_io_op *op, zend_object *handle, php_socket_t fd, struct sockaddr *addr, socklen_t *addrlen, php_deadline dl);
PHPAPI void php_io_op_connect(php_io_op *op, zend_object *handle, php_socket_t fd, const struct sockaddr *addr, socklen_t addrlen, php_deadline dl);
PHPAPI void php_io_op_getaddrinfo(php_io_op *op, const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res, php_deadline dl);
PHPAPI void php_io_op_fsync(php_io_op *op, zend_object *handle, php_socket_t fd, bool data_only);
PHPAPI void php_io_op_any(php_io_op *op, php_io_op **members, uint32_t n, php_io_op_result *results);

/* Persistent Poll ops: owned by the core, kept on the handle, the same
 * php_io_op for every run that asks for the same (handle, events) pair. The
 * provider's add hook runs at creation and its remove hook at release. */
typedef struct _php_io_persistent_op php_io_persistent_op;
PHPAPI php_io_op *php_io_op_persistent(zend_object *handle, uint32_t events);
PHPAPI void php_io_op_persistent_release(zend_object *handle, uint32_t events);
PHPAPI void php_io_handle_release_ops(zend_object *handle);

/* Hooks */

#define PHP_IO_HOOKS_F_FILES  0x01  /* send regular file ops and Fsync to the provider */
#define PHP_IO_HOOKS_F_DIRECT 0x02  /* submit descriptor ops without the syscall-first attempt */

typedef struct _php_io_hooks {
	uint32_t flags;             /* PHP_IO_HOOKS_F_* */

	/* Execute one operation to completion. May suspend the current flow.
	 * FAILURE means EG(exception) is set (provider error or cancellation). */
	zend_result (*run)(void *data, php_io_op *op, php_io_op_result *result);

	/* A persistent op was created. May be NULL. */
	void (*add)(void *data, php_io_op *op);

	/* A persistent op is going away. May be NULL. */
	void (*remove)(void *data, php_io_op *op);

	/* Called when hooks are unregistered or replaced. */
	void (*dtor)(void *data);
} php_io_hooks;

typedef struct _php_io_hooks_state {
	php_io_hooks hooks;
	void *data;
} php_io_hooks_state;

/* Install the provider for this request, NULL to uninstall. size allows
 * growing the struct. Fails if a provider is already installed. */
PHPAPI zend_result php_io_hooks_register(const php_io_hooks *hooks, size_t size, void *data);
PHPAPI const php_io_hooks *php_io_hooks_current(void **data);
PHPAPI bool php_io_hooks_active(void);
PHPAPI void php_io_hooks_request_shutdown(void);

/* Set by the userland bridge: invalidates the Io\Operation wrapper of an
 * op that ended, so a provider keeping the object cannot reach the op. */
PHPAPI extern void (*php_io_op_zobj_detach)(zend_object *zobj);

/* The one call stream code makes. Wraps the hook with the UNSUPPORTED
 * fallback; without a provider it waits itself. */
PHPAPI zend_result php_io_run(php_io_op *op, php_io_op_result *result);

/* Convenience wrappers. They return like the syscall they replace, with
 * errno set (ETIMEDOUT when the deadline passed, ECANCELED when the
 * provider cancelled). stream may be NULL for descriptors that are not
 * streams; a stream is frozen for the duration. */
PHPAPI int php_io_poll(php_stream *stream, php_socket_t fd, uint32_t events, php_deadline *dl);  /* revents, 0 on timeout, -1 on error */
PHPAPI ssize_t php_io_recv(php_stream *stream, php_socket_t fd, void *buf, size_t len, int flags, php_deadline *dl);
PHPAPI ssize_t php_io_send(php_stream *stream, php_socket_t fd, const void *buf, size_t len, int flags, php_deadline *dl);
PHPAPI ssize_t php_io_read(php_stream *stream, int fd, void *buf, size_t len, php_deadline *dl);
PHPAPI ssize_t php_io_write(php_stream *stream, int fd, const void *buf, size_t len, php_deadline *dl);
PHPAPI php_socket_t php_io_accept(php_stream *stream, php_socket_t fd, struct sockaddr *addr, socklen_t *addrlen, php_deadline *dl);
PHPAPI int php_io_connect(php_stream *stream, php_socket_t fd, const struct sockaddr *addr, socklen_t addrlen, php_deadline *dl);
PHPAPI int php_io_fsync(php_stream *stream, int fd, bool data_only);
PHPAPI zend_result php_io_sleep(php_deadline dl);

/* Active php_io_run() frames; pcntl_fork() refuses while any is in flight */
PHPAPI uint32_t php_io_ops_in_flight(void);

/* Orphans: a queue that keeps an in-flight op after its frame went away
 * registers the stream here and unfreezes it when the op settled. A stream
 * freed meanwhile is drained first. */
PHPAPI void php_io_stream_orphan(php_stream *stream, php_io_queue *queue);
PHPAPI void php_io_stream_unfreeze(php_stream *stream);
PHPAPI void php_io_stream_drain(php_stream *stream);

/* Poll with a relative timeout; NULL or tv_sec == -1 means no timeout. */
static inline int php_io_poll_tv(php_stream *stream, php_socket_t fd, uint32_t events, const struct timeval *tv)
{
	php_deadline dl;
	php_deadline_init(&dl, (struct timeval *) tv);
	return php_io_poll(stream, fd, events, &dl);
}

/* Operation queues */

typedef struct _php_io_queue_completion {
	php_io_op *op;
	void *data;
	php_io_op_result result;
} php_io_queue_completion;

typedef struct _php_io_queue_ops {
	zend_result (*submit)(php_io_queue *q, php_io_op *op, void *data);
	zend_result (*cancel)(php_io_queue *q, php_io_op *op);
	zend_result (*add)(php_io_queue *q, php_io_op *op);
	void (*remove)(php_io_queue *q, php_io_op *op);
	int (*wait)(php_io_queue *q, php_io_queue_completion *out, uint32_t max, const struct timespec *timeout);
	void (*orphan)(php_io_queue *q, php_io_op *op);
	/* Wait until every orphaned op on the stream settled, keeping other
	 * completions for delivery. May be NULL when orphan() never keeps one. */
	void (*drain)(php_io_queue *q, php_stream *stream);
	uint32_t (*count_pending)(php_io_queue *q);
	uint32_t (*hook_flags)(php_io_queue *q);
	void (*destroy)(php_io_queue *q);
} php_io_queue_ops;

struct _php_io_queue {
	const php_io_queue_ops *ops;
};

PHPAPI php_io_queue *php_io_queue_create_poll(php_poll_backend_type backend);

/* Deadline helpers */

static inline php_deadline php_io_deadline_from_timeval(const struct timeval *tv)
{
	php_deadline dl;
	php_deadline_init(&dl, (struct timeval *) tv);
	return dl;
}

static inline php_deadline php_io_deadline_from_ms(zend_long ms)
{
	php_deadline dl;
	if (ms < 0) {
		php_deadline_init_infinite(&dl);
	} else {
		struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
		php_deadline_init(&dl, &tv);
	}
	return dl;
}

static inline php_deadline php_io_deadline_from_ns(zend_hrtime_t ns)
{
	php_deadline dl;
	dl.hrtime = zend_hrtime();
	if (ns >= ZEND_HRTIME_T_MAX - dl.hrtime) {
		php_deadline_init_infinite(&dl);
	} else {
		dl.hrtime += ns;
	}
	return dl;
}

static inline php_deadline php_io_deadline_infinite(void)
{
	php_deadline dl;
	php_deadline_init_infinite(&dl);
	return dl;
}

/* Remaining time in nanoseconds, 0 when passed; only for finite deadlines. */
static inline zend_hrtime_t php_io_deadline_remaining(const php_deadline *dl, zend_hrtime_t now)
{
	return dl->hrtime > now ? dl->hrtime - now : 0;
}

END_EXTERN_C()

#endif /* PHP_HOOKS_IO_HOOKS_H */
