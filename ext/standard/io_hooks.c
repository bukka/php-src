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

/* The userland bridge of the IO hooks: Io\Operation wrappers over the C
 * ops, Io\Completion, Io\Poll\OperationQueue over the C poll queue, and the
 * Io\Hooks\Hooks adapter that set_hooks() installs as the C provider. */

#include "php.h"
#include "zend_enum.h"
#include "zend_exceptions.h"
#include "zend_interfaces.h"
#include "ext/standard/file.h"
#include "ext/standard/io_poll.h"
#include "ext/standard/io_hooks.h"
#include "ext/date/php_time.h"
#include "io_hooks_arginfo.h"
#include "io_hooks_decl.h"

#include <errno.h>

static zend_class_entry *php_io_completion_status_ce;
static zend_class_entry *php_io_operation_ce;
static zend_class_entry *php_io_operation_poll_ce;
static zend_class_entry *php_io_operation_timer_ce;
static zend_class_entry *php_io_operation_any_ce;
static zend_class_entry *php_io_completion_ce;
static zend_class_entry *php_io_invalid_operation_exception_ce;
static zend_class_entry *php_io_operation_queue_ce;
static zend_class_entry *php_io_poll_operation_queue_ce;
static zend_class_entry *php_io_hooks_ce;
static zend_class_entry *php_io_hooks_capability_ce;

static zend_object_handlers php_io_operation_handlers;
static zend_object_handlers php_io_completion_handlers;
static zend_object_handlers php_io_poll_operation_queue_handlers;

typedef struct {
	php_io_op *op;              /* NULL once the operation ended */
	zend_object std;
} php_io_operation_obj;

typedef struct {
	zend_object *operation;
	php_io_status status;
	int64_t res;
	int error;
	uint32_t events;            /* readiness mask, for Poll operations and Ready completions */
	zval data;
	zval completions;           /* array for an Any, undef otherwise */
	zend_object std;
} php_io_completion_obj;

typedef struct _php_io_opqueue_sub php_io_opqueue_sub;

typedef struct {
	php_io_queue *queue;
	php_io_opqueue_sub *subs;   /* submissions not delivered yet */
	zend_object std;
} php_io_opqueue_obj;

/* One submission: what comes back as the completion's operation and data */
struct _php_io_opqueue_sub {
	zend_object *operation;
	zval data;
	php_io_opqueue_sub *prev;
	php_io_opqueue_sub *next;
};

#define PHP_IO_OPERATION_FROM_ZOBJ(o) ZEND_CONTAINER_OF(o, php_io_operation_obj, std)
#define PHP_IO_COMPLETION_FROM_ZOBJ(o) ZEND_CONTAINER_OF(o, php_io_completion_obj, std)
#define PHP_IO_OPQUEUE_FROM_ZOBJ(o) ZEND_CONTAINER_OF(o, php_io_opqueue_obj, std)

/* Completion status enum */

/* The enum cases are declared in the order of php_io_status */
ZEND_STATIC_ASSERT(ZEND_ENUM_Io_CompletionStatus_Unsupported - ZEND_ENUM_Io_CompletionStatus_Done == PHP_IO_UNSUPPORTED - PHP_IO_DONE,
		"Io\\CompletionStatus must mirror php_io_status");

static zend_object *php_io_status_case(php_io_status status)
{
	ZEND_ASSERT(status <= PHP_IO_UNSUPPORTED);
	return zend_enum_get_case_by_id(php_io_completion_status_ce, ZEND_ENUM_Io_CompletionStatus_Done + status);
}

static php_io_status php_io_status_from_case(zend_object *case_obj)
{
	return (php_io_status) (zend_enum_fetch_case_id(case_obj) - ZEND_ENUM_Io_CompletionStatus_Done);
}

/* Operation objects */

static zend_object *php_io_operation_create_object(zend_class_entry *ce)
{
	php_io_operation_obj *intern = zend_object_alloc(sizeof(php_io_operation_obj), ce);
	zend_object_std_init(&intern->std, ce);
	object_properties_init(&intern->std, ce);
	intern->op = NULL;
	return &intern->std;
}

static zend_class_entry *php_io_operation_ce_for(php_io_op_type type)
{
	switch (type) {
		case PHP_IO_OP_POLL: return php_io_operation_poll_ce;
		case PHP_IO_OP_TIMER: return php_io_operation_timer_ce;
		case PHP_IO_OP_ANY: return php_io_operation_any_ce;
		default: return php_io_operation_ce;
	}
}

PHPAPI zend_object *php_io_operation_get_zobj(php_io_op *op)
{
	if (!op->zobj) {
		/* Through the handler directly: the base class is abstract and the
		 * constructors are private, the core is the only creator */
		zend_object *zobj = php_io_operation_create_object(php_io_operation_ce_for(op->type));
		PHP_IO_OPERATION_FROM_ZOBJ(zobj)->op = op;
		op->zobj = zobj;
	}
	return op->zobj;
}

static void php_io_operation_detach(zend_object *zobj)
{
	PHP_IO_OPERATION_FROM_ZOBJ(zobj)->op = NULL;
}

static php_io_op *php_io_operation_fetch(zval *zv)
{
	php_io_operation_obj *intern = PHP_IO_OPERATION_FROM_ZOBJ(Z_OBJ_P(zv));
	if (!intern->op) {
		zend_throw_exception(php_io_invalid_operation_exception_ce, "The operation has ended", 0);
	}
	return intern->op;
}

static uint32_t php_io_op_events(php_io_op *op)
{
	return op->type == PHP_IO_OP_POLL ? op->u.poll.events : op->ready_events;
}

/* Completion objects */

static zend_object *php_io_completion_create_object(zend_class_entry *ce)
{
	php_io_completion_obj *intern = zend_object_alloc(sizeof(php_io_completion_obj), ce);
	zend_object_std_init(&intern->std, ce);
	object_properties_init(&intern->std, ce);
	intern->operation = NULL;
	ZVAL_NULL(&intern->data);
	ZVAL_UNDEF(&intern->completions);
	return &intern->std;
}

static void php_io_completion_free_object(zend_object *obj)
{
	php_io_completion_obj *intern = PHP_IO_COMPLETION_FROM_ZOBJ(obj);
	if (intern->operation) {
		OBJ_RELEASE(intern->operation);
	}
	zval_ptr_dtor(&intern->data);
	zval_ptr_dtor(&intern->completions);
	zend_object_std_dtor(&intern->std);
}

static HashTable *php_io_completion_get_gc(zend_object *obj, zval **table, int *n)
{
	php_io_completion_obj *intern = PHP_IO_COMPLETION_FROM_ZOBJ(obj);
	zend_get_gc_buffer *gc_buffer = zend_get_gc_buffer_create();
	if (intern->operation) {
		zend_get_gc_buffer_add_obj(gc_buffer, intern->operation);
	}
	zend_get_gc_buffer_add_zval(gc_buffer, &intern->data);
	zend_get_gc_buffer_add_zval(gc_buffer, &intern->completions);
	zend_get_gc_buffer_use(gc_buffer, table, n);
	return NULL;
}

/* operation: reference added here; data and completions: copied, may be NULL */
static php_io_completion_obj *php_io_completion_create(zval *rv, php_io_op *op, zend_object *operation,
		php_io_status status, int64_t res, int error, zval *data, zval *completions)
{
	object_init_ex(rv, php_io_completion_ce);
	php_io_completion_obj *c = PHP_IO_COMPLETION_FROM_ZOBJ(Z_OBJ_P(rv));

	GC_ADDREF(operation);
	c->operation = operation;
	c->status = status;
	c->res = res;
	c->error = error;
	c->events = (op && (op->type == PHP_IO_OP_POLL || status == PHP_IO_READY)) ? (uint32_t) res : 0;
	if (data) {
		ZVAL_COPY(&c->data, data);
	}
	if (completions) {
		ZVAL_COPY(&c->completions, completions);
	}
	return c;
}

/* Io\Operation */

PHP_METHOD(Io_Operation, __construct)
{
	zend_throw_error(NULL, "Operations are created by the engine");
}

PHP_METHOD(Io_Operation, getHandle)
{
	ZEND_PARSE_PARAMETERS_NONE();

	php_io_op *op = php_io_operation_fetch(ZEND_THIS);
	if (!op) {
		RETURN_THROWS();
	}
	if (!op->handle) {
		RETURN_NULL();
	}
	RETURN_OBJ_COPY(op->handle);
}

PHP_METHOD(Io_Operation, getEvents)
{
	ZEND_PARSE_PARAMETERS_NONE();

	php_io_op *op = php_io_operation_fetch(ZEND_THIS);
	if (!op) {
		RETURN_THROWS();
	}
	php_io_poll_events_to_event_enums(php_io_op_events(op), return_value);
}

PHP_METHOD(Io_Operation, getTimeout)
{
	ZEND_PARSE_PARAMETERS_NONE();

	php_io_op *op = php_io_operation_fetch(ZEND_THIS);
	if (!op) {
		RETURN_THROWS();
	}
	if (php_deadline_is_infinite(&op->deadline)) {
		RETURN_NULL();
	}

	zend_hrtime_t remaining = php_io_deadline_remaining(&op->deadline, zend_hrtime());
	zval ns;
	ZVAL_LONG(&ns, (zend_long) MIN(remaining, (zend_hrtime_t) ZEND_LONG_MAX));
	zend_call_method_with_1_params(NULL, php_date_ce_time_duration, NULL, "fromnanoseconds", return_value, &ns);
}

PHP_METHOD(Io_Operation, isValid)
{
	ZEND_PARSE_PARAMETERS_NONE();

	RETURN_BOOL(PHP_IO_OPERATION_FROM_ZOBJ(Z_OBJ_P(ZEND_THIS))->op != NULL);
}

PHP_METHOD(Io_Operation, complete)
{
	zend_object *status_obj;
	zend_long res = 0, error = 0;

	ZEND_PARSE_PARAMETERS_START(1, 3)
		Z_PARAM_OBJ_OF_CLASS(status_obj, php_io_completion_status_ce)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(res)
		Z_PARAM_LONG(error)
	ZEND_PARSE_PARAMETERS_END();

	php_io_op *op = php_io_operation_fetch(ZEND_THIS);
	if (!op) {
		RETURN_THROWS();
	}
	php_io_completion_create(return_value, op, Z_OBJ_P(ZEND_THIS),
			php_io_status_from_case(status_obj), res, (int) error, NULL, NULL);
}

PHP_METHOD(Io_Operation, completeReady)
{
	zval *events_zv;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ARRAY(events_zv)
	ZEND_PARSE_PARAMETERS_END();

	php_io_op *op = php_io_operation_fetch(ZEND_THIS);
	if (!op) {
		RETURN_THROWS();
	}

	uint32_t events = php_io_poll_event_enums_to_events(events_zv);
	if (!events) {
		zend_argument_type_error(1, "must be a non-empty array of Io\\Poll\\Event enums");
		RETURN_THROWS();
	}

	php_io_status status = (op->type == PHP_IO_OP_POLL || op->type == PHP_IO_OP_TIMER)
			? PHP_IO_DONE : PHP_IO_READY;
	php_io_completion_create(return_value, op, Z_OBJ_P(ZEND_THIS), status, events, 0, NULL, NULL);
}

/* Io\Operation\Poll */

PHP_METHOD(Io_Operation_Poll, isPersistent)
{
	ZEND_PARSE_PARAMETERS_NONE();

	php_io_op *op = php_io_operation_fetch(ZEND_THIS);
	if (!op) {
		RETURN_THROWS();
	}
	RETURN_BOOL(op->flags & PHP_IO_OP_F_PERSISTENT);
}

/* Io\Operation\Any */

PHP_METHOD(Io_Operation_Any, getOperations)
{
	ZEND_PARSE_PARAMETERS_NONE();

	php_io_op *op = php_io_operation_fetch(ZEND_THIS);
	if (!op) {
		RETURN_THROWS();
	}

	array_init_size(return_value, op->u.any.n);
	for (uint32_t i = 0; i < op->u.any.n; i++) {
		zval member;
		ZVAL_OBJ_COPY(&member, php_io_operation_get_zobj(&op->u.any.ops[i]));
		zend_hash_next_index_insert_new(Z_ARRVAL_P(return_value), &member);
	}
}

/* The index of the member an operation object wraps, or -1 */
static int32_t php_io_any_member_index(php_io_op *any, zend_object *operation)
{
	for (uint32_t i = 0; i < any->u.any.n; i++) {
		if (any->u.any.ops[i].zobj == operation) {
			return (int32_t) i;
		}
	}
	return -1;
}

PHP_METHOD(Io_Operation_Any, completeWith)
{
	zval *completions;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ARRAY(completions)
	ZEND_PARSE_PARAMETERS_END();

	php_io_op *op = php_io_operation_fetch(ZEND_THIS);
	if (!op) {
		RETURN_THROWS();
	}

	if (zend_hash_num_elements(Z_ARRVAL_P(completions)) == 0) {
		zend_argument_value_error(1, "must not be empty");
		RETURN_THROWS();
	}

	zval *entry;
	ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(completions), entry) {
		if (Z_TYPE_P(entry) != IS_OBJECT || !instanceof_function(Z_OBJCE_P(entry), php_io_completion_ce)) {
			zend_argument_type_error(1, "must be a list of Io\\Completion objects");
			RETURN_THROWS();
		}
		php_io_completion_obj *c = PHP_IO_COMPLETION_FROM_ZOBJ(Z_OBJ_P(entry));
		if (php_io_any_member_index(op, c->operation) < 0) {
			zend_argument_value_error(1, "must only contain completions of the members of this operation");
			RETURN_THROWS();
		}
	} ZEND_HASH_FOREACH_END();

	php_io_completion_create(return_value, op, Z_OBJ_P(ZEND_THIS), PHP_IO_DONE, 0, 0, NULL, completions);
}

/* Io\Completion */

PHP_METHOD(Io_Completion, __construct)
{
	zend_throw_error(NULL, "Completions are created from operations");
}

PHP_METHOD(Io_Completion, getOperation)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_OBJ_COPY(PHP_IO_COMPLETION_FROM_ZOBJ(Z_OBJ_P(ZEND_THIS))->operation);
}

PHP_METHOD(Io_Completion, getStatus)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_OBJ_COPY(php_io_status_case(PHP_IO_COMPLETION_FROM_ZOBJ(Z_OBJ_P(ZEND_THIS))->status));
}

PHP_METHOD(Io_Completion, getResult)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_LONG((zend_long) PHP_IO_COMPLETION_FROM_ZOBJ(Z_OBJ_P(ZEND_THIS))->res);
}

PHP_METHOD(Io_Completion, getEvents)
{
	ZEND_PARSE_PARAMETERS_NONE();
	php_io_poll_events_to_event_enums(PHP_IO_COMPLETION_FROM_ZOBJ(Z_OBJ_P(ZEND_THIS))->events, return_value);
}

PHP_METHOD(Io_Completion, getError)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_LONG(PHP_IO_COMPLETION_FROM_ZOBJ(Z_OBJ_P(ZEND_THIS))->error);
}

PHP_METHOD(Io_Completion, getData)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_COPY(&PHP_IO_COMPLETION_FROM_ZOBJ(Z_OBJ_P(ZEND_THIS))->data);
}

PHP_METHOD(Io_Completion, getCompletions)
{
	ZEND_PARSE_PARAMETERS_NONE();

	php_io_completion_obj *c = PHP_IO_COMPLETION_FROM_ZOBJ(Z_OBJ_P(ZEND_THIS));
	if (Z_TYPE(c->completions) == IS_ARRAY) {
		RETURN_COPY(&c->completions);
	}
	RETURN_EMPTY_ARRAY();
}

/* Io\Poll\OperationQueue */

static zend_object *php_io_poll_operation_queue_create_object(zend_class_entry *ce)
{
	php_io_opqueue_obj *intern = zend_object_alloc(sizeof(php_io_opqueue_obj), ce);
	zend_object_std_init(&intern->std, ce);
	object_properties_init(&intern->std, ce);
	intern->queue = NULL;
	intern->subs = NULL;
	return &intern->std;
}

static void php_io_opqueue_sub_unlink(php_io_opqueue_obj *q, php_io_opqueue_sub *sub)
{
	if (sub->prev) {
		sub->prev->next = sub->next;
	} else {
		q->subs = sub->next;
	}
	if (sub->next) {
		sub->next->prev = sub->prev;
	}
}

static void php_io_opqueue_sub_free(php_io_opqueue_sub *sub)
{
	OBJ_RELEASE(sub->operation);
	zval_ptr_dtor(&sub->data);
	efree(sub);
}

static void php_io_poll_operation_queue_free_object(zend_object *obj)
{
	php_io_opqueue_obj *intern = PHP_IO_OPQUEUE_FROM_ZOBJ(obj);

	if (intern->queue) {
		/* Cancels and withdraws everything still submitted */
		intern->queue->ops->destroy(intern->queue);
		intern->queue = NULL;
	}
	while (intern->subs) {
		php_io_opqueue_sub *sub = intern->subs;
		php_io_opqueue_sub_unlink(intern, sub);
		php_io_opqueue_sub_free(sub);
	}
	zend_object_std_dtor(&intern->std);
}

static HashTable *php_io_poll_operation_queue_get_gc(zend_object *obj, zval **table, int *n)
{
	php_io_opqueue_obj *intern = PHP_IO_OPQUEUE_FROM_ZOBJ(obj);
	zend_get_gc_buffer *gc_buffer = zend_get_gc_buffer_create();
	for (php_io_opqueue_sub *sub = intern->subs; sub; sub = sub->next) {
		zend_get_gc_buffer_add_obj(gc_buffer, sub->operation);
		zend_get_gc_buffer_add_zval(gc_buffer, &sub->data);
	}
	zend_get_gc_buffer_use(gc_buffer, table, n);
	return NULL;
}

static php_io_opqueue_obj *php_io_opqueue_fetch(zval *zv)
{
	php_io_opqueue_obj *intern = PHP_IO_OPQUEUE_FROM_ZOBJ(Z_OBJ_P(zv));
	if (!intern->queue) {
		zend_throw_error(NULL, "Io\\Poll\\OperationQueue object is not constructed");
	}
	return intern;
}

PHP_METHOD(Io_Poll_OperationQueue, __construct)
{
	zval *context = NULL;

	ZEND_PARSE_PARAMETERS_START(0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_OBJECT_OR_NULL(context)
	ZEND_PARSE_PARAMETERS_END();

	php_io_opqueue_obj *intern = PHP_IO_OPQUEUE_FROM_ZOBJ(Z_OBJ_P(ZEND_THIS));

	if (intern->queue) {
		zend_throw_error(NULL, "Io\\Poll\\OperationQueue object is already constructed");
		RETURN_THROWS();
	}
	if (context) {
		zend_throw_error(NULL, "Sharing an Io\\Poll\\Context is not supported yet");
		RETURN_THROWS();
	}

	intern->queue = php_io_queue_create_poll(PHP_POLL_BACKEND_AUTO);
	if (!intern->queue) {
		zend_throw_exception(php_io_exception_class_entry, "Failed to create the poll queue", 0);
		RETURN_THROWS();
	}
}

PHP_METHOD(Io_Poll_OperationQueue, getContext)
{
	ZEND_PARSE_PARAMETERS_NONE();
	zend_throw_error(NULL, "Io\\Poll\\OperationQueue::getContext() is not supported yet");
}

PHP_METHOD(Io_Poll_OperationQueue, submit)
{
	zval *op_zv, *data = NULL;

	ZEND_PARSE_PARAMETERS_START(1, 2)
		Z_PARAM_OBJECT_OF_CLASS(op_zv, php_io_operation_ce)
		Z_PARAM_OPTIONAL
		Z_PARAM_ZVAL(data)
	ZEND_PARSE_PARAMETERS_END();

	php_io_opqueue_obj *intern = php_io_opqueue_fetch(ZEND_THIS);
	if (!intern) {
		RETURN_THROWS();
	}
	php_io_op *op = php_io_operation_fetch(op_zv);
	if (!op) {
		RETURN_THROWS();
	}
	if (op->queue) {
		zend_throw_error(NULL, "The operation is already submitted");
		RETURN_THROWS();
	}

	php_io_opqueue_sub *sub = emalloc(sizeof(*sub));
	sub->operation = Z_OBJ_P(op_zv);
	GC_ADDREF(sub->operation);
	if (data) {
		ZVAL_COPY(&sub->data, data);
	} else {
		ZVAL_NULL(&sub->data);
	}
	sub->prev = NULL;
	sub->next = intern->subs;
	if (intern->subs) {
		intern->subs->prev = sub;
	}
	intern->subs = sub;
	op->provider_data = sub;

	if (intern->queue->ops->submit(intern->queue, op, sub) == FAILURE) {
		op->provider_data = NULL;
		php_io_opqueue_sub_unlink(intern, sub);
		php_io_opqueue_sub_free(sub);
		zend_throw_exception_ex(php_io_exception_class_entry, errno,
				"Failed to submit the operation: %s", strerror(errno));
		RETURN_THROWS();
	}
}

PHP_METHOD(Io_Poll_OperationQueue, cancel)
{
	zval *op_zv;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_OBJECT_OF_CLASS(op_zv, php_io_operation_ce)
	ZEND_PARSE_PARAMETERS_END();

	php_io_opqueue_obj *intern = php_io_opqueue_fetch(ZEND_THIS);
	if (!intern) {
		RETURN_THROWS();
	}
	php_io_op *op = php_io_operation_fetch(op_zv);
	if (!op) {
		RETURN_THROWS();
	}
	if (op->queue != intern->queue) {
		zend_throw_error(NULL, "The operation is not submitted to this queue");
		RETURN_THROWS();
	}

	php_io_opqueue_sub *sub = op->provider_data;
	intern->queue->ops->cancel(intern->queue, op);
	if (sub) {
		op->provider_data = NULL;
		php_io_opqueue_sub_unlink(intern, sub);
		php_io_opqueue_sub_free(sub);
	}
}

PHP_METHOD(Io_Poll_OperationQueue, add)
{
	zval *op_zv;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_OBJECT_OF_CLASS(op_zv, php_io_operation_ce)
	ZEND_PARSE_PARAMETERS_END();

	php_io_opqueue_obj *intern = php_io_opqueue_fetch(ZEND_THIS);
	if (!intern) {
		RETURN_THROWS();
	}
	php_io_op *op = php_io_operation_fetch(op_zv);
	if (!op) {
		RETURN_THROWS();
	}
	intern->queue->ops->add(intern->queue, op);
}

PHP_METHOD(Io_Poll_OperationQueue, remove)
{
	zval *op_zv;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_OBJECT_OF_CLASS(op_zv, php_io_operation_ce)
	ZEND_PARSE_PARAMETERS_END();

	php_io_opqueue_obj *intern = php_io_opqueue_fetch(ZEND_THIS);
	if (!intern) {
		RETURN_THROWS();
	}
	php_io_op *op = php_io_operation_fetch(op_zv);
	if (!op) {
		RETURN_THROWS();
	}
	intern->queue->ops->remove(intern->queue, op);
}

/* Build the Completion for one delivered completion, members of an Any included */
static void php_io_opqueue_completion_to_zval(zval *rv, php_io_queue_completion *c)
{
	php_io_op *op = c->op;
	php_io_opqueue_sub *sub = c->data;
	zval members;

	ZVAL_UNDEF(&members);
	if (op->type == PHP_IO_OP_ANY) {
		array_init_size(&members, op->u.any.n_results);
		for (uint32_t i = 0; i < op->u.any.n_results; i++) {
			php_io_op_result *r = &op->u.any.results[i];
			php_io_op *member = &op->u.any.ops[r->index];
			zval member_zv;
			php_io_completion_create(&member_zv, member, php_io_operation_get_zobj(member),
					r->status, r->res, r->error, NULL, NULL);
			zend_hash_next_index_insert_new(Z_ARRVAL(members), &member_zv);
		}
	}

	op->provider_data = NULL;
	php_io_completion_create(rv, op, sub->operation, c->result.status, c->result.res,
			c->result.error, &sub->data, Z_TYPE(members) == IS_ARRAY ? &members : NULL);
	zval_ptr_dtor(&members);
}

PHP_METHOD(Io_Poll_OperationQueue, waitCompletions)
{
	php_date_time_duration *timeout = NULL;
	zend_long max = 0;
	bool max_is_null = true;

	ZEND_PARSE_PARAMETERS_START(0, 2)
		Z_PARAM_OPTIONAL
		Z_PARAM_DATE_TIME_DURATION_OR_NULL(timeout)
		Z_PARAM_LONG_OR_NULL(max, max_is_null)
	ZEND_PARSE_PARAMETERS_END();

	php_io_opqueue_obj *intern = php_io_opqueue_fetch(ZEND_THIS);
	if (!intern) {
		RETURN_THROWS();
	}

	struct timespec timeout_ts;
	if (timeout) {
		if (timeout->duration.negative) {
			zend_argument_value_error(1, "must not be negative");
			RETURN_THROWS();
		}
		timeout_ts.tv_sec = timeout->duration.seconds;
		timeout_ts.tv_nsec = timeout->duration.nanoseconds;
	}

	if (max_is_null) {
		max = 64;
	} else if (max <= 0) {
		zend_argument_value_error(2, "must be greater than 0");
		RETURN_THROWS();
	} else if (max > 4096) {
		max = 4096;
	}

	php_io_queue_completion *completions = safe_emalloc((size_t) max, sizeof(*completions), 0);
	int n = intern->queue->ops->wait(intern->queue, completions, (uint32_t) max, timeout ? &timeout_ts : NULL);
	if (n < 0) {
		int err = errno;
		efree(completions);
		if (err == EDEADLK) {
			zend_throw_exception(php_io_exception_class_entry,
					"No operation can complete: nothing pending has a descriptor or a deadline", err);
		} else {
			zend_throw_exception_ex(php_io_exception_class_entry, err,
					"Failed to wait for completions: %s", strerror(err));
		}
		RETURN_THROWS();
	}

	array_init_size(return_value, n);
	for (int i = 0; i < n; i++) {
		php_io_opqueue_sub *sub = completions[i].data;
		zval completion;
		php_io_opqueue_completion_to_zval(&completion, &completions[i]);
		zend_hash_next_index_insert_new(Z_ARRVAL_P(return_value), &completion);
		php_io_opqueue_sub_unlink(intern, sub);
		php_io_opqueue_sub_free(sub);
	}
	efree(completions);
}

PHP_METHOD(Io_Poll_OperationQueue, countPending)
{
	ZEND_PARSE_PARAMETERS_NONE();

	php_io_opqueue_obj *intern = php_io_opqueue_fetch(ZEND_THIS);
	if (!intern) {
		RETURN_THROWS();
	}
	RETURN_LONG(intern->queue->ops->count_pending(intern->queue));
}

static void php_io_hook_flags_to_capabilities(uint32_t flags, zval *rv)
{
	array_init(rv);
	if (flags & PHP_IO_HOOKS_F_FILES) {
		zval c;
		ZVAL_OBJ_COPY(&c, zend_enum_get_case_by_id(php_io_hooks_capability_ce, ZEND_ENUM_Io_Hooks_Capability_Files));
		zend_hash_next_index_insert_new(Z_ARRVAL_P(rv), &c);
	}
	if (flags & PHP_IO_HOOKS_F_DIRECT) {
		zval c;
		ZVAL_OBJ_COPY(&c, zend_enum_get_case_by_id(php_io_hooks_capability_ce, ZEND_ENUM_Io_Hooks_Capability_Direct));
		zend_hash_next_index_insert_new(Z_ARRVAL_P(rv), &c);
	}
}

PHP_METHOD(Io_Poll_OperationQueue, getHookCapabilities)
{
	ZEND_PARSE_PARAMETERS_NONE();

	php_io_opqueue_obj *intern = php_io_opqueue_fetch(ZEND_THIS);
	if (!intern) {
		RETURN_THROWS();
	}
	php_io_hook_flags_to_capabilities(intern->queue->ops->hook_flags(intern->queue), return_value);
}

/* The userland provider adapter, installed as the C provider by set_hooks() */

typedef struct {
	zend_object *obj;
	zend_fcall_info_cache run_fcc;
	zend_fcall_info_cache add_fcc;
	zend_fcall_info_cache remove_fcc;
} php_io_hooks_php_data;

static void php_io_hooks_method_fcc(zend_object *obj, const char *name, zend_fcall_info_cache *fcc)
{
	zend_string *name_str = zend_string_init(name, strlen(name), false);
	zend_function *fn = obj->handlers->get_method(&obj, name_str, NULL);
	zend_string_release(name_str);
	ZEND_ASSERT(fn != NULL);

	*fcc = (zend_fcall_info_cache) {
		.function_handler = fn,
		.object = obj,
		.called_scope = obj->ce,
	};
	zend_fcc_addref(fcc);
}

static zend_result php_io_hooks_php_run(void *data, php_io_op *op, php_io_op_result *result)
{
	php_io_hooks_php_data *php_data = data;
	zend_object *zobj = php_io_operation_get_zobj(op);
	zval arg, retval;

	ZVAL_OBJ_COPY(&arg, zobj);
	ZVAL_UNDEF(&retval);
	zend_call_known_fcc(&php_data->run_fcc, &retval, 1, &arg, NULL);
	zval_ptr_dtor(&arg);

	if (EG(exception)) {
		zval_ptr_dtor(&retval);
		return FAILURE;
	}
	if (Z_TYPE(retval) != IS_OBJECT || !instanceof_function(Z_OBJCE(retval), php_io_completion_ce)) {
		zval_ptr_dtor(&retval);
		zend_throw_error(NULL, "Io\\Hooks\\Hooks::run() must return an Io\\Completion");
		return FAILURE;
	}

	php_io_completion_obj *c = PHP_IO_COMPLETION_FROM_ZOBJ(Z_OBJ(retval));
	if (c->operation != zobj) {
		zval_ptr_dtor(&retval);
		zend_throw_error(NULL, "Io\\Hooks\\Hooks::run() returned the completion of another operation");
		return FAILURE;
	}

	result->status = c->status;
	result->index = 0;
	result->res = c->res;
	result->error = c->error;

	if (op->type == PHP_IO_OP_ANY) {
		uint32_t n = 0;
		if (Z_TYPE(c->completions) == IS_ARRAY) {
			zval *entry;
			ZEND_HASH_FOREACH_VAL(Z_ARRVAL(c->completions), entry) {
				php_io_completion_obj *m = PHP_IO_COMPLETION_FROM_ZOBJ(Z_OBJ_P(entry));
				int32_t index = php_io_any_member_index(op, m->operation);
				if (index < 0 || n >= op->u.any.n) {
					continue;
				}
				op->u.any.results[n].status = m->status;
				op->u.any.results[n].index = (uint32_t) index;
				op->u.any.results[n].res = m->res;
				op->u.any.results[n].error = m->error;
				n++;
			} ZEND_HASH_FOREACH_END();
		}
		op->u.any.n_results = n;
	}

	zval_ptr_dtor(&retval);
	return SUCCESS;
}

static void php_io_hooks_php_call_void(zend_fcall_info_cache *fcc, php_io_op *op)
{
	zval arg;
	ZVAL_OBJ_COPY(&arg, php_io_operation_get_zobj(op));
	zend_call_known_fcc(fcc, NULL, 1, &arg, NULL);
	zval_ptr_dtor(&arg);
}

static void php_io_hooks_php_add(void *data, php_io_op *op)
{
	php_io_hooks_php_call_void(&((php_io_hooks_php_data *) data)->add_fcc, op);
}

static void php_io_hooks_php_remove(void *data, php_io_op *op)
{
	php_io_hooks_php_call_void(&((php_io_hooks_php_data *) data)->remove_fcc, op);
}

static void php_io_hooks_php_dtor(void *data)
{
	php_io_hooks_php_data *php_data = data;
	zend_fcc_dtor(&php_data->run_fcc);
	zend_fcc_dtor(&php_data->add_fcc);
	zend_fcc_dtor(&php_data->remove_fcc);
	OBJ_RELEASE(php_data->obj);
	efree(php_data);
}

static const php_io_hooks php_io_hooks_php_adapter = {
	.run = php_io_hooks_php_run,
	.add = php_io_hooks_php_add,
	.remove = php_io_hooks_php_remove,
	.dtor = php_io_hooks_php_dtor,
};

/* The installed userland provider object, NULL for none or a C provider */
static zend_object *php_io_hooks_php_current(void)
{
	void *data;
	const php_io_hooks *hooks = php_io_hooks_current(&data);
	if (!hooks || hooks->run != php_io_hooks_php_run) {
		return NULL;
	}
	return ((php_io_hooks_php_data *) data)->obj;
}

static uint32_t php_io_hooks_capabilities_to_flags(zval *capabilities)
{
	uint32_t flags = 0;
	zval *entry;

	ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(capabilities), entry) {
		if (Z_TYPE_P(entry) != IS_OBJECT || Z_OBJCE_P(entry) != php_io_hooks_capability_ce) {
			zend_throw_error(NULL, "Io\\Hooks\\Hooks::getCapabilities() must return a list of Io\\Hooks\\Capability");
			return 0;
		}
		switch (zend_enum_fetch_case_id(Z_OBJ_P(entry))) {
			case ZEND_ENUM_Io_Hooks_Capability_Files:
				flags |= PHP_IO_HOOKS_F_FILES;
				break;
			case ZEND_ENUM_Io_Hooks_Capability_Direct:
				flags |= PHP_IO_HOOKS_F_DIRECT;
				break;
		}
	} ZEND_HASH_FOREACH_END();

	return flags;
}

PHP_FUNCTION(Io_Hooks_set_hooks)
{
	zend_object *hooks_obj = NULL;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_OBJ_OF_CLASS_OR_NULL(hooks_obj, php_io_hooks_ce)
	ZEND_PARSE_PARAMETERS_END();

	zend_object *previous = php_io_hooks_php_current();
	if (php_io_hooks_active() && !previous) {
		zend_throw_error(NULL, "IO hooks are owned by an internal provider");
		RETURN_THROWS();
	}

	php_io_hooks hooks = php_io_hooks_php_adapter;
	php_io_hooks_php_data *php_data = NULL;

	if (hooks_obj) {
		zval capabilities;
		zend_fcall_info_cache caps_fcc;
		php_io_hooks_method_fcc(hooks_obj, "getCapabilities", &caps_fcc);
		ZVAL_UNDEF(&capabilities);
		zend_call_known_fcc(&caps_fcc, &capabilities, 0, NULL, NULL);
		zend_fcc_dtor(&caps_fcc);
		if (EG(exception)) {
			zval_ptr_dtor(&capabilities);
			RETURN_THROWS();
		}
		if (Z_TYPE(capabilities) != IS_ARRAY) {
			zval_ptr_dtor(&capabilities);
			zend_throw_error(NULL, "Io\\Hooks\\Hooks::getCapabilities() must return an array");
			RETURN_THROWS();
		}
		hooks.flags = php_io_hooks_capabilities_to_flags(&capabilities);
		zval_ptr_dtor(&capabilities);
		if (EG(exception)) {
			RETURN_THROWS();
		}

		php_data = emalloc(sizeof(*php_data));
		php_data->obj = hooks_obj;
		GC_ADDREF(hooks_obj);
		php_io_hooks_method_fcc(hooks_obj, "run", &php_data->run_fcc);
		php_io_hooks_method_fcc(hooks_obj, "add", &php_data->add_fcc);
		php_io_hooks_method_fcc(hooks_obj, "remove", &php_data->remove_fcc);
	}

	/* The previous provider is returned, so it survives its dtor */
	if (previous) {
		GC_ADDREF(previous);
	}
	php_io_hooks_register(NULL, 0, NULL);

	if (php_data) {
		zend_result rc = php_io_hooks_register(&hooks, sizeof(hooks), php_data);
		ZEND_ASSERT(rc == SUCCESS);
	}

	if (previous) {
		RETURN_OBJ(previous);
	}
	RETURN_NULL();
}

PHP_FUNCTION(Io_Hooks_get_hooks)
{
	ZEND_PARSE_PARAMETERS_NONE();

	zend_object *current = php_io_hooks_php_current();
	if (!current) {
		RETURN_NULL();
	}
	RETURN_OBJ_COPY(current);
}

PHP_FUNCTION(Io_Hooks_is_active)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_BOOL(php_io_hooks_active());
}

PHP_MINIT_FUNCTION(io_hooks)
{
	php_io_completion_status_ce = register_class_Io_CompletionStatus();

	php_io_operation_ce = register_class_Io_Operation();
	php_io_operation_ce->create_object = php_io_operation_create_object;
	memcpy(&php_io_operation_handlers, &std_object_handlers, sizeof(zend_object_handlers));
	php_io_operation_handlers.offset = offsetof(php_io_operation_obj, std);
	php_io_operation_handlers.clone_obj = NULL;
	php_io_operation_ce->default_object_handlers = &php_io_operation_handlers;

	php_io_operation_poll_ce = register_class_Io_Operation_Poll(php_io_operation_ce);
	php_io_operation_poll_ce->create_object = php_io_operation_create_object;
	php_io_operation_poll_ce->default_object_handlers = &php_io_operation_handlers;

	php_io_operation_timer_ce = register_class_Io_Operation_Timer(php_io_operation_ce);
	php_io_operation_timer_ce->create_object = php_io_operation_create_object;
	php_io_operation_timer_ce->default_object_handlers = &php_io_operation_handlers;

	php_io_operation_any_ce = register_class_Io_Operation_Any(php_io_operation_ce);
	php_io_operation_any_ce->create_object = php_io_operation_create_object;
	php_io_operation_any_ce->default_object_handlers = &php_io_operation_handlers;

	php_io_completion_ce = register_class_Io_Completion();
	php_io_completion_ce->create_object = php_io_completion_create_object;
	memcpy(&php_io_completion_handlers, &std_object_handlers, sizeof(zend_object_handlers));
	php_io_completion_handlers.offset = offsetof(php_io_completion_obj, std);
	php_io_completion_handlers.free_obj = php_io_completion_free_object;
	php_io_completion_handlers.get_gc = php_io_completion_get_gc;
	php_io_completion_handlers.clone_obj = NULL;
	php_io_completion_ce->default_object_handlers = &php_io_completion_handlers;

	php_io_invalid_operation_exception_ce
			= register_class_Io_InvalidOperationException(php_io_exception_class_entry);

	php_io_operation_queue_ce = register_class_Io_OperationQueue();

	php_io_poll_operation_queue_ce = register_class_Io_Poll_OperationQueue(php_io_operation_queue_ce);
	php_io_poll_operation_queue_ce->create_object = php_io_poll_operation_queue_create_object;
	memcpy(&php_io_poll_operation_queue_handlers, &std_object_handlers, sizeof(zend_object_handlers));
	php_io_poll_operation_queue_handlers.offset = offsetof(php_io_opqueue_obj, std);
	php_io_poll_operation_queue_handlers.free_obj = php_io_poll_operation_queue_free_object;
	php_io_poll_operation_queue_handlers.get_gc = php_io_poll_operation_queue_get_gc;
	php_io_poll_operation_queue_handlers.clone_obj = NULL;
	php_io_poll_operation_queue_ce->default_object_handlers = &php_io_poll_operation_queue_handlers;

	php_io_hooks_ce = register_class_Io_Hooks_Hooks();
	php_io_hooks_capability_ce = register_class_Io_Hooks_Capability();

	zend_register_functions(NULL, ext_functions, NULL, type);

	php_io_op_zobj_detach = php_io_operation_detach;

	return SUCCESS;
}
