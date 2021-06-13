--TEST--
FPM: Process manager config pm.process_idle_timeout
--SKIPIF--
<?php
include "skipif.inc";
?>
--FILE--
<?php

require_once "tester.inc";

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
[unconfined]
listen = {{ADDR}}
pm = ondemand
pm.max_children = 3
pm.process_idle_timeout = 1
pm.status_path = /status
EOT;

$code = <<<EOT
<?php
usleep(200);
EOT;

$tester = new FPM\Tester($cfg, $code);
$tester->start();
$tester->expectLogStartNotices();
// TODO create async requst - already possibly in fcgi.php
// or alternatively replace with $tester->multiRequest
$r1 = $tester->asyncRequest();
$r2 = $tester->asyncRequest();
$r1->wait();
$r2->wait();
// or alternatively replace with $tester->multiRequest
$tester->multiRequest(2);
$this->status([
    'active processes' => 2,
]);
// wait for process idle timeout
sleep(2);
$this->status([
    'active processes' => 1,
]);
$tester->terminate();
$tester->expectLogTerminatingNotices();
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
