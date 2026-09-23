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

#ifndef PHP_IO_HOOKS_H
#define PHP_IO_HOOKS_H

#include "main/hooks/io_hooks.h"

BEGIN_EXTERN_C()

/* The Io\Operation wrapper of an op, created on first use. No reference is
 * added: the op holds the one that keeps it alive. */
PHPAPI zend_object *php_io_operation_get_zobj(php_io_op *op);

PHP_MINIT_FUNCTION(io_hooks);

END_EXTERN_C()

#endif /* PHP_IO_HOOKS_H */
