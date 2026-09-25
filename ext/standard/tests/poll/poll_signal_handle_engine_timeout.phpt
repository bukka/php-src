--TEST--
Io\Poll\SignalHandle refuses the signal the execution timeout runs on
--EXTENSIONS--
pcntl
--SKIPIF--
<?php
if (PHP_ZTS) die("skip SignalHandle is refused in thread-safe builds");
if (PHP_OS_FAMILY !== 'Linux' || php_uname('m') === 'aarch64') die("skip SIGPROF timer platforms only");
?>
--FILE--
<?php
try {
    new Io\Poll\SignalHandle([SIGUSR1, SIGPROF]);
} catch (ValueError $e) {
    echo $e->getMessage() === "Io\\Poll\\SignalHandle::__construct(): Argument #1 (\$signals) must not contain signal " . SIGPROF . ", which the execution timeout uses" ? "refused\n" : $e->getMessage() . "\n";
}

// The timeout still fires with a handle on other signals
$h = new Io\Poll\SignalHandle([SIGUSR1]);
set_time_limit(1);
while (true) {}
?>
--EXPECTF--
refused

Fatal error: Maximum execution time of 1 second exceeded in %s on line %d
