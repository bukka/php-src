--TEST--
Stack limit 005 - Internal stack limit check in zend_compile_expr()
--EXTENSIONS--
zend_test
--INI--
zend.max_allowed_stack_size=128K
--FILE--
<?php

$test
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
    ->f()->f()->f()->f()->f()->f()->f()->f()->f()->f()
;

--EXPECTF--
Fatal error: Maximum call stack size of %d bytes reached during compilation. Try splitting expression in %s on line %d