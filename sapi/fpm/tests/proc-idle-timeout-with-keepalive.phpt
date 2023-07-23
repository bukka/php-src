--TEST--
FPM: Process manager config pm.process_idle_timeout with keep alive set
--SKIPIF--
<?php
include "skipif.inc";
if (!getenv("FPM_RUN_RESOURCE_HEAVY_TESTS")) die("skip resource heavy test");
if (getenv("SKIP_SLOW_TESTS")) die("skip slow test");
?>
--FILE--
<?php

require_once "tester.inc";

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
log_level = debug
[unconfined]
listen = {{ADDR}}
pm = ondemand
pm.max_children = 3
pm.min_spare_servers = 1
pm.process_idle_timeout = 1
pm.status_path = /status
;request_terminate_timeout = 2
EOT;

$code = <<<EOT
<?php
usleep(300000);
EOT;

$tester = new FPM\Tester($cfg, $code, clientTransport: 'stream');
$tester->start();
$tester->expectLogStartNotices();
//$tester->multiRequest(2, connKeepAlive: true);
$tester->multiRequest(2, connKeepAlive: true, writeDelay: 3000, readTimeout: 5000);
$tester->status([
    'total processes' => 2,
]);
// wait for process idle timeout
sleep(2);
$tester->status([
    'total processes' => 2,
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
