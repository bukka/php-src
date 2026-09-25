--TEST--
Io\Poll\SignalHandle is refused in thread-safe builds
--EXTENSIONS--
pcntl
--SKIPIF--
<?php
if (!PHP_ZTS) die("skip thread-safe builds only");
?>
--FILE--
<?php
try {
    new Io\Poll\SignalHandle([SIGUSR1]);
} catch (Io\Poll\PollException $e) {
    echo get_class($e), ": ", $e->getMessage(), "\n";
}
?>
--EXPECT--
Io\Poll\PollException: Io\Poll\SignalHandle is not available in thread-safe builds
