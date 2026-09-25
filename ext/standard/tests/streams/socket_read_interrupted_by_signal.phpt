--TEST--
A blocking socket read returns early for a signal PHP has a handler for
--EXTENSIONS--
pcntl
--SKIPIF--
<?php
if (!function_exists('pcntl_alarm')) die('skip pcntl_alarm() not available');
?>
--FILE--
<?php
[$a, $b] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, 0);
/* Only the signal ends the first reads */
stream_set_timeout($a, 3600);

pcntl_async_signals(true);
pcntl_signal(SIGALRM, function () { echo "handler\n"; });
pcntl_alarm(1);
var_dump(fread($a, 10));
$meta = stream_get_meta_data($a);
var_dump($meta['timed_out'], $meta['eof']);

pcntl_async_signals(false);
pcntl_alarm(1);
var_dump(fgets($a));
echo "dispatch\n";
pcntl_signal_dispatch();

fwrite($b, "data\n");
var_dump(fgets($a));
?>
--EXPECT--
handler
bool(false)
bool(false)
bool(false)
bool(false)
dispatch
handler
string(5) "data
"
