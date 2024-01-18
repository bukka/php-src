--TEST--
FPM: bug75712 - getenv should not copy $_ENV and $_SERVER
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
[unconfined]
listen = {{ADDR}}
pm = static
pm.max_children = 1
env[TEST] = test
EOT;

$code = <<<EOT
<?php

var_dump(getenv());
var_dump(isset(getenv()['argv']));

function notcalled()
{
    \$_SERVER['argv'];
}
EOT;

$tester = new FPM\Tester($cfg, $code);
$tester->start();
$tester->expectLogStartNotices();
$tester->request()->expectBody('bool(false)');
$tester->terminate();
$tester->close();

?>
Done
--EXPECT--
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
