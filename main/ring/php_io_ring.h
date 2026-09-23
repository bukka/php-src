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

#ifndef PHP_RING_PHP_IO_RING_H
#define PHP_RING_PHP_IO_RING_H

#include "php.h"
#include "main/hooks/io_hooks.h"

#ifdef HAVE_IOR

BEGIN_EXTERN_C()

/* A thin PHPAPI wrapper over ior: translates php_io_op into submissions
 * and completions into statuses, and folds Any ops. */

typedef struct php_io_ring php_io_ring;

/* ior has no default depth, so the wrapper defines the one the core's queue
 * uses and Io\Ring\Engine takes for 0. */
#define PHP_IO_RING_DEFAULT_ENTRIES 256

/* Which backend ior chose. Keep in sync with Io\Ring\Backend. */
typedef enum php_io_ring_backend_type {
	PHP_IO_RING_BACKEND_IO_URING,
	PHP_IO_RING_BACKEND_IOCP,
	PHP_IO_RING_BACKEND_THREADS,
} php_io_ring_backend_type;

/* fd_nonblock promises that every submitted descriptor is non-blocking
 * (IOR_SETUP_FD_NONBLOCK). */
PHPAPI php_io_ring *php_io_ring_create(uint32_t entries, bool fd_nonblock);
PHPAPI void php_io_ring_destroy(php_io_ring *ring);

/* Preps and submits in one call; after it returns the op is either
 * completed or cancellable. A finite deadline adds a linked timeout. */
PHPAPI zend_result php_io_ring_submit_op(php_io_ring *ring, php_io_op *op, void *data);
PHPAPI zend_result php_io_ring_cancel(php_io_ring *ring, php_io_op *op);
/* Hand an in-flight op to the ring: cancelled and finished silently */
PHPAPI void php_io_ring_orphan(php_io_ring *ring, php_io_op *op);

/* With a zero timeout it first clears the notification descriptor. */
PHPAPI int php_io_ring_wait(php_io_ring *ring, php_io_queue_completion *out, uint32_t max, const struct timespec *timeout);
PHPAPI uint32_t php_io_ring_count_pending(php_io_ring *ring);

PHPAPI php_socket_t php_io_ring_notify_fd(php_io_ring *ring);
PHPAPI void php_io_ring_notify_clear(php_io_ring *ring);
PHPAPI uint32_t php_io_ring_features(php_io_ring *ring);
PHPAPI php_io_ring_backend_type php_io_ring_get_backend_type(php_io_ring *ring);
PHPAPI const char *php_io_ring_backend_name(php_io_ring *ring);
/* PHP_IO_HOOKS_F_* a provider on this ring should register with */
PHPAPI uint32_t php_io_ring_hook_flags(php_io_ring *ring);

/* The ring as an operation queue (section 5.6) */
PHPAPI php_io_queue *php_io_queue_create_ring(uint32_t entries);
/* The ring behind a queue created by php_io_queue_create_ring() */
PHPAPI php_io_ring *php_io_queue_ring(php_io_queue *q);

END_EXTERN_C()

#endif /* HAVE_IOR */

#endif /* PHP_RING_PHP_IO_RING_H */
