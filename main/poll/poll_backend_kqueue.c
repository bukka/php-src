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
   | Authors: Jakub Zelenka <bukka@php.net>                               |
   +----------------------------------------------------------------------+
*/

#include "php_poll_internal.h"

#ifdef HAVE_KQUEUE

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

/* Per-FD state kept in fd_tracking (grouped mode only). The filter bits mirror what is
 * registered in the kqueue, so the backend never has to probe the kernel to find out. */
#define KQUEUE_FD_READ    (1 << 0) /* EVFILT_READ is registered */
#define KQUEUE_FD_WRITE   (1 << 1) /* EVFILT_WRITE is registered */
#define KQUEUE_FD_FILTERS (KQUEUE_FD_READ | KQUEUE_FD_WRITE)
#define KQUEUE_FD_ONESHOT (1 << 2) /* Filters are registered with EV_ONESHOT */
#define KQUEUE_FD_ET      (1 << 3) /* Filters are registered with EV_CLEAR */
#define KQUEUE_FD_FIRED   (1 << 4) /* A one-shot filter fired, the remaining filters are stale */

typedef struct kqueue_backend_data {
	int kqueue_fd;
	struct kevent *events;
	int events_capacity;
	int filter_count; /* Number of filters registered in the kqueue, sizes the wait buffer */
	HashTable *fd_tracking; /* FD -> KQUEUE_FD_* flags, NULL in raw events mode */
} kqueue_backend_data_t;

static zend_result kqueue_backend_init(php_poll_ctx *ctx)
{
	kqueue_backend_data_t *data
			= php_poll_calloc(1, sizeof(kqueue_backend_data_t), ctx->persistent);
	if (!data) {
		php_poll_set_error(ctx, PHP_POLL_ERR_NOMEM);
		return FAILURE;
	}

	data->kqueue_fd = kqueue();
	if (data->kqueue_fd == -1) {
		pefree(data, ctx->persistent);
		php_poll_set_error(ctx, PHP_POLL_ERR_SYSTEM);
		return FAILURE;
	}

	/* Use hint for initial allocation if provided, otherwise start with reasonable default */
	int initial_capacity = ctx->max_events_hint > 0 ? ctx->max_events_hint : 64;
	data->events = php_poll_calloc(initial_capacity, sizeof(struct kevent), ctx->persistent);
	if (!data->events) {
		close(data->kqueue_fd);
		pefree(data, ctx->persistent);
		php_poll_set_error(ctx, PHP_POLL_ERR_NOMEM);
		return FAILURE;
	}
	data->events_capacity = initial_capacity;
	data->filter_count = 0;

	/* Initialize single tracking hash table (only if not using raw events) */
	if (!ctx->raw_events) {
		data->fd_tracking = php_poll_malloc(sizeof(HashTable), ctx->persistent);
		zend_hash_init(data->fd_tracking, 8, NULL, NULL, ctx->persistent);
	} else {
		data->fd_tracking = NULL;
	}

	ctx->backend_data = data;
	return SUCCESS;
}

static void kqueue_backend_cleanup(php_poll_ctx *ctx)
{
	kqueue_backend_data_t *data = (kqueue_backend_data_t *) ctx->backend_data;
	if (data) {
		if (data->kqueue_fd >= 0) {
			close(data->kqueue_fd);
		}
		pefree(data->events, ctx->persistent);

		/* Cleanup tracking hash table if initialized */
		if (data->fd_tracking) {
			zend_hash_destroy(data->fd_tracking);
			pefree(data->fd_tracking, ctx->persistent);
		}

		pefree(data, ctx->persistent);
		ctx->backend_data = NULL;
	}
}

/* Convert poll events to the tracking flags describing their kqueue registration */
static inline zend_long kqueue_tracking_flags(uint32_t events)
{
	zend_long flags = 0;
	if (events & PHP_POLL_READ) {
		flags |= KQUEUE_FD_READ;
	}
	if (events & PHP_POLL_WRITE) {
		flags |= KQUEUE_FD_WRITE;
	}
	if (events & PHP_POLL_ONESHOT) {
		flags |= KQUEUE_FD_ONESHOT;
	}
	if (events & PHP_POLL_ET) {
		flags |= KQUEUE_FD_ET;
	}
	return flags;
}

/* Number of filters described by the tracking flags */
static inline int kqueue_filter_count(zend_long tracking_flags)
{
	return !!(tracking_flags & KQUEUE_FD_READ) + !!(tracking_flags & KQUEUE_FD_WRITE);
}

/* Append EV_DELETE changes for the filters set in tracking_flags, returns the new change count */
static int kqueue_set_deletes(struct kevent *changes, int count, int fd, zend_long filters)
{
	if (filters & KQUEUE_FD_READ) {
		EV_SET(&changes[count], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
		count++;
	}
	if (filters & KQUEUE_FD_WRITE) {
		EV_SET(&changes[count], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
		count++;
	}
	return count;
}

/* Append EV_ADD changes for the requested events, returns the new change count */
static int kqueue_set_adds(struct kevent *changes, int count, int fd, uint32_t events, void *data)
{
	uint16_t flags = EV_ADD | EV_ENABLE;
	if (events & PHP_POLL_ONESHOT) {
		flags |= EV_ONESHOT;
	}
	if (events & PHP_POLL_ET) {
		flags |= EV_CLEAR;
	}

	if (events & PHP_POLL_READ) {
		EV_SET(&changes[count], fd, EVFILT_READ, flags, 0, 0, data);
		count++;
	}
	if (events & PHP_POLL_WRITE) {
		EV_SET(&changes[count], fd, EVFILT_WRITE, flags, 0, 0, data);
		count++;
	}
	return count;
}

/* Delete the given filters one at a time so a filter that is already gone (ENOENT) does not
 * prevent the others from being deleted. Returns the number of filters that were registered,
 * or -1 with errno set on any other error. */
static int kqueue_delete_filters(kqueue_backend_data_t *backend_data, int fd, zend_long filters)
{
	struct kevent changes[2];
	int count = kqueue_set_deletes(changes, 0, fd, filters);
	int deleted = 0;

	for (int i = 0; i < count; i++) {
		if (kevent(backend_data->kqueue_fd, &changes[i], 1, NULL, 0, NULL) == 0) {
			deleted++;
		} else if (!php_poll_is_not_found_error()) {
			return -1;
		}
	}

	return deleted;
}

static zend_result kqueue_backend_add(php_poll_ctx *ctx, int fd, uint32_t events, void *data)
{
	kqueue_backend_data_t *backend_data = (kqueue_backend_data_t *) ctx->backend_data;

	/* Check for duplicate in non-raw mode */
	if (!ctx->raw_events && zend_hash_index_exists(backend_data->fd_tracking, fd)) {
		php_poll_set_error(ctx, PHP_POLL_ERR_EXISTS);
		return FAILURE;
	}

	struct kevent changes[2]; /* Max 2 changes: read + write */
	int change_count = kqueue_set_adds(changes, 0, fd, events, data);

	if (change_count > 0
			&& kevent(backend_data->kqueue_fd, changes, change_count, NULL, 0, NULL) == -1) {
		php_poll_set_current_errno_error(ctx);
		return FAILURE;
	}

	backend_data->filter_count += change_count;

	if (!ctx->raw_events) {
		zval tracking_zval;
		ZVAL_LONG(&tracking_zval, kqueue_tracking_flags(events));
		zend_hash_index_add_new(backend_data->fd_tracking, fd, &tracking_zval);
	}

	return SUCCESS;
}

static zend_result kqueue_backend_modify(php_poll_ctx *ctx, int fd, uint32_t events, void *data)
{
	kqueue_backend_data_t *backend_data = (kqueue_backend_data_t *) ctx->backend_data;
	struct kevent changes[4]; /* Max 4 changes: 2 deletes followed by 2 adds */
	int change_count;

	if (ctx->raw_events) {
		/* Nothing tracks which filters the FD has, so drop both to learn how many there were */
		int deleted = kqueue_delete_filters(backend_data, fd, KQUEUE_FD_FILTERS);
		if (deleted < 0) {
			php_poll_set_current_errno_error(ctx);
			return FAILURE;
		}

		change_count = kqueue_set_adds(changes, 0, fd, events, data);
		if (change_count > 0
				&& kevent(backend_data->kqueue_fd, changes, change_count, NULL, 0, NULL) == -1) {
			php_poll_set_current_errno_error(ctx);
			return FAILURE;
		}

		backend_data->filter_count += change_count - deleted;
		return SUCCESS;
	}

	zval *tracking = zend_hash_index_find(backend_data->fd_tracking, fd);
	zend_long old_flags = tracking ? Z_LVAL_P(tracking) : 0;
	zend_long new_flags = kqueue_tracking_flags(events);
	zend_long old_filters = old_flags & KQUEUE_FD_FILTERS;
	zend_long new_filters = new_flags & KQUEUE_FD_FILTERS;

	/* Drop the filters that are no longer wanted. EV_ADD on a registered filter only updates its
	 * udata and not EV_ONESHOT or EV_CLEAR, so when those change the kept filters are re-created. */
	zend_long stale_filters = old_filters & ~new_filters;
	if ((old_flags ^ new_flags) & (KQUEUE_FD_ONESHOT | KQUEUE_FD_ET)) {
		stale_filters |= old_filters & new_filters;
	}

	/* The deletes and adds are applied in order by a single kevent() call */
	change_count = kqueue_set_deletes(changes, 0, fd, stale_filters);
	change_count = kqueue_set_adds(changes, change_count, fd, events, data);
	if (change_count > 0
			&& kevent(backend_data->kqueue_fd, changes, change_count, NULL, 0, NULL) == -1) {
		php_poll_set_current_errno_error(ctx);
		return FAILURE;
	}

	backend_data->filter_count += kqueue_filter_count(new_flags) - kqueue_filter_count(old_flags);

	if (tracking) {
		Z_LVAL_P(tracking) = new_flags;
	} else {
		/* The FD was not added before, it is registered now */
		zval tracking_zval;
		ZVAL_LONG(&tracking_zval, new_flags);
		zend_hash_index_add_new(backend_data->fd_tracking, fd, &tracking_zval);
	}

	return SUCCESS;
}

static zend_result kqueue_backend_remove(php_poll_ctx *ctx, int fd)
{
	kqueue_backend_data_t *backend_data = (kqueue_backend_data_t *) ctx->backend_data;
	zend_long filters = KQUEUE_FD_FILTERS;
	zval *tracking = NULL;

	if (!ctx->raw_events) {
		tracking = zend_hash_index_find(backend_data->fd_tracking, fd);
		if (!tracking) {
			php_poll_set_error(ctx, PHP_POLL_ERR_NOTFOUND);
			return FAILURE;
		}
		filters = Z_LVAL_P(tracking) & KQUEUE_FD_FILTERS;
	}

	int deleted = kqueue_delete_filters(backend_data, fd, filters);
	if (deleted < 0) {
		php_poll_set_current_errno_error(ctx);
		return FAILURE;
	}

	if (tracking) {
		/* The tracked filters were all counted, even one the kernel dropped on its own */
		backend_data->filter_count -= kqueue_filter_count(filters);
		zend_hash_index_del(backend_data->fd_tracking, fd);
	} else {
		if (deleted == 0) {
			php_poll_set_error(ctx, PHP_POLL_ERR_NOTFOUND);
			return FAILURE;
		}
		backend_data->filter_count -= deleted;
	}

	return SUCCESS;
}

static int kqueue_backend_wait(
		php_poll_ctx *ctx, php_poll_event *events, int max_events,
		const struct timespec *timeout)
{
	kqueue_backend_data_t *backend_data = (kqueue_backend_data_t *) ctx->backend_data;

	/* Cap every kevent() request at the entries we can still deliver, like epoll's maxevents.
	 * Unretrieved events stay pending in the kqueue (EV_ONESHOT deletes and EV_CLEAR resets only
	 * at retrieval), so nothing is lost and the caller's buffer cannot overflow. */
	if (max_events > backend_data->events_capacity) {
		struct kevent *new_events = php_poll_realloc(
				backend_data->events, max_events * sizeof(struct kevent), ctx->persistent);
		if (!new_events) {
			php_poll_set_error(ctx, PHP_POLL_ERR_NOMEM);
			return -1;
		}
		backend_data->events = new_events;
		backend_data->events_capacity = max_events;
	}

	if (ctx->raw_events) {
		/* Raw events mode - direct 1:1 mapping, no grouping */
		int nfds = kevent(
				backend_data->kqueue_fd, NULL, 0, backend_data->events, max_events, timeout);

		if (nfds < 0) {
			php_poll_set_current_errno_error(ctx);
			return -1;
		}

		for (int i = 0; i < nfds; i++) {
			events[i].fd = (int) backend_data->events[i].ident;
			events[i].events = 0;
			events[i].revents = 0;
			events[i].data = backend_data->events[i].udata;

			/* Convert kqueue filter to poll event */
			if (backend_data->events[i].filter == EVFILT_READ) {
				events[i].revents |= PHP_POLL_READ;
			} else if (backend_data->events[i].filter == EVFILT_WRITE) {
				events[i].revents |= PHP_POLL_WRITE;
			}

			/* Convert kqueue flags to poll events */
			if (backend_data->events[i].flags & EV_EOF) {
				events[i].revents |= PHP_POLL_HUP;
			}
			if (backend_data->events[i].flags & EV_ERROR) {
				events[i].revents |= PHP_POLL_ERROR;
			}

			/* The kernel deletes a one-shot filter once it is retrieved */
			if (backend_data->events[i].flags & EV_ONESHOT) {
				backend_data->filter_count--;
			}
		}

		return nfds;
	}

	/* Grouped events mode with one-shot tracking.
	 *
	 * kqueue reports one event per (FD, filter) pair, so a READ+WRITE pair merges into a single
	 * entry and one round can fill fewer than max_events entries while more events are pending.
	 * Top up with zero-timeout rounds, each requesting only the remaining free entries. Both
	 * FreeBSD and XNU re-enqueue delivered level-triggered events at the tail of the active queue,
	 * so top-up rounds see not-yet-delivered events first, matching epoll's round-robin fill. */
	int unique_events = 0, fired_fds = 0;
	const struct timespec zero_timeout = {0, 0};
	const struct timespec *round_timeout = timeout;

	while (true) {
		int slots = max_events - unique_events;
		int nfds = kevent(
				backend_data->kqueue_fd, NULL, 0, backend_data->events, slots, round_timeout);

		if (nfds < 0) {
			if (unique_events > 0) {
				/* Deliver what was already gathered */
				break;
			}
			php_poll_set_current_errno_error(ctx);
			return -1;
		}

		int new_unique_events = 0;

		for (int i = 0; i < nfds; i++) {
			struct kevent *kev = &backend_data->events[i];
			int fd = (int) kev->ident;
			uint32_t revents = 0;

			/* Convert this event */
			if (kev->filter == EVFILT_READ) {
				revents |= PHP_POLL_READ;
			} else if (kev->filter == EVFILT_WRITE) {
				revents |= PHP_POLL_WRITE;
			}

			if (kev->flags & EV_EOF) {
				revents |= PHP_POLL_HUP;
			}
			if (kev->flags & EV_ERROR) {
				revents |= PHP_POLL_ERROR;
			}

			/* Look for existing event for this FD */
			bool found = false;
			for (int j = 0; j < unique_events; j++) {
				if (events[j].fd == fd) {
					/* Combine with existing event */
					events[j].revents |= revents;
					found = true;
					break;
				}
			}

			if (!found) {
				/* New FD, create new event */
				ZEND_ASSERT(unique_events < max_events);
				events[unique_events].fd = fd;
				events[unique_events].events = 0;
				events[unique_events].revents = revents;
				events[unique_events].data = kev->udata;
				unique_events++;
				new_unique_events++;
			}

			if (kev->flags & EV_ONESHOT) {
				/* The kernel deleted this filter when it was retrieved */
				backend_data->filter_count--;

				zval *tracking = zend_hash_index_find(backend_data->fd_tracking, fd);
				if (tracking) {
					zend_long flags = Z_LVAL_P(tracking);
					flags &= ~(kev->filter == EVFILT_READ ? KQUEUE_FD_READ : KQUEUE_FD_WRITE);
					if (flags & KQUEUE_FD_FILTERS) {
						/* The other filter of a read+write one-shot FD is still registered. It would
						 * fire as a second event for a watcher that already fired, so it is dropped
						 * once this call is done gathering events. */
						flags |= KQUEUE_FD_FIRED;
						fired_fds++;
					} else if (flags & KQUEUE_FD_FIRED) {
						/* Both filters fired in this call, nothing is left to drop */
						flags &= ~KQUEUE_FD_FIRED;
						fired_fds--;
					}
					Z_LVAL_P(tracking) = flags;
				}
			}
		}

		/* Stop on a partial round (drained or timed out), a full buffer, or a full round with no
		 * new FDs (kernel re-reported level-triggered events already merged above - don't spin) */
		if (nfds < slots || unique_events == max_events || new_unique_events == 0) {
			break;
		}
		round_timeout = &zero_timeout;
	}

	if (fired_fds > 0) {
		/* Drop the stale filters of fired one-shot FDs. The FDs stay tracked without filters until
		 * they are removed or re-armed by a modify. */
		ZEND_HASH_FOREACH_NUM_KEY_VAL(backend_data->fd_tracking, zend_ulong fd_key, zval *tracking)
		{
			zend_long flags = Z_LVAL_P(tracking);
			if (flags & KQUEUE_FD_FIRED) {
				zend_long filters = flags & KQUEUE_FD_FILTERS;
				/* A filter that fired meanwhile is already gone, which is fine */
				kqueue_delete_filters(backend_data, (int) fd_key, filters);
				backend_data->filter_count -= kqueue_filter_count(filters);
				Z_LVAL_P(tracking) = flags & ~(KQUEUE_FD_FILTERS | KQUEUE_FD_FIRED);
				if (--fired_fds == 0) {
					break;
				}
			}
		}
		ZEND_HASH_FOREACH_END();
	}

	return unique_events;
}

static bool kqueue_backend_is_available(void)
{
	int fd = kqueue();
	if (fd >= 0) {
		close(fd);
		return true;
	}
	return false;
}

static int kqueue_backend_get_suitable_max_events(php_poll_ctx *ctx)
{
	kqueue_backend_data_t *backend_data = (kqueue_backend_data_t *) ctx->backend_data;

	if (!backend_data) {
		return -1;
	}

	/* kqueue reports one event per filter even in grouped mode (events are merged into
	 * per-FD entries afterwards) and the wait request is capped at max_events, so size
	 * for the total filter count to allow retrieving all pending events in one call. */
	int active_filters = backend_data->filter_count;
	return active_filters == 0 ? 1 : active_filters;
}

const php_poll_backend_ops php_poll_backend_kqueue_ops = {
	.type = PHP_POLL_BACKEND_KQUEUE,
	.name = "kqueue",
	.init = kqueue_backend_init,
	.cleanup = kqueue_backend_cleanup,
	.add = kqueue_backend_add,
	.modify = kqueue_backend_modify,
	.remove = kqueue_backend_remove,
	.wait = kqueue_backend_wait,
	.is_available = kqueue_backend_is_available,
	.get_suitable_max_events = kqueue_backend_get_suitable_max_events,
	.supports_et = true
};


#endif /* HAVE_KQUEUE */
