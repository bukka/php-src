--TEST--
Io\Curl\SocketWeakHandle cannot be constructed or cloned, and is invalid once libcurl drops the socket
--EXTENSIONS--
curl
--FILE--
<?php

include __DIR__ . '/../../standard/tests/streams/hooks/scheduler.inc';

class CapturingScheduler extends Scheduler
{
    public ?Io\Curl\SocketWeakHandle $handle = null;

    public function run(\Io\Operation $op): \Io\Completion
    {
        $ops = $op instanceof Io\Operation\Any ? $op->getOperations() : [$op];
        foreach ($ops as $member) {
            if ($member->getHandle() instanceof Io\Curl\SocketWeakHandle) {
                $this->handle = $member->getHandle();
            }
        }
        return parent::run($op);
    }
}

try {
    new Io\Curl\SocketWeakHandle();
} catch (Error $e) {
    echo $e->getMessage(), "\n";
}

$scheduler = new CapturingScheduler();
Io\Hooks\set_hooks($scheduler);

$server = stream_socket_server('tcp://127.0.0.1:0');
$addr = stream_socket_get_name($server, false);

$scheduler->spawn(function () use ($server) {
    $conn = stream_socket_accept($server, 5);
    $request = '';
    while (!str_ends_with($request, "\r\n\r\n")) {
        $chunk = fread($conn, 1024);
        if ($chunk === false || $chunk === '') break;
        $request .= $chunk;
    }
    fwrite($conn, "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nhi");
    fclose($conn);
});

$scheduler->spawn(function () use ($addr) {
    $ch = curl_init("http://$addr/");
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    var_dump(curl_exec($ch));
});

$scheduler->loop();
Io\Hooks\set_hooks(null);

$handle = $scheduler->handle;
var_dump($handle instanceof Io\Curl\SocketWeakHandle);
try {
    clone $handle;
} catch (Error $e) {
    echo $e->getMessage(), "\n";
}
try {
    (new Io\Poll\Context())->add($handle, [Io\Poll\Event::Read]);
} catch (Io\Poll\InvalidHandleException $e) {
    echo $e->getMessage(), "\n";
}
?>
--EXPECT--
Call to private Io\Curl\SocketWeakHandle::__construct() from global scope
string(2) "hi"
bool(true)
Trying to clone an uncloneable object of class Io\Curl\SocketWeakHandle
Invalid handle for polling
