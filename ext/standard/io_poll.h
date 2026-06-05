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
   | Author: Jakub Zelenka <bukka@php.net>                                |
   +----------------------------------------------------------------------+
*/

#ifndef PHP_IO_POLL_H
#define PHP_IO_POLL_H

#include "php_streams.h"

BEGIN_EXTERN_C()

PHPAPI void php_io_poll_stream_notify_close(php_stream *stream);

PHPAPI extern zend_class_entry *php_io_poll_event_class_entry;
PHPAPI extern zend_class_entry *php_stream_poll_handle_class_entry;

PHPAPI zend_result php_io_poll_events_to_event_enums(uint32_t events, zval *event_enums);

PHPAPI void php_stream_poll_handle_from_stream(zval *dest, php_stream *stream);

END_EXTERN_C()

#endif /* PHP_IO_POLL_H */
