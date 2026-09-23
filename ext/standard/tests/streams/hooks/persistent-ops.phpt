--TEST--
IO hooks: curl's sockets are persistent Poll operations bracketed by add() and remove()
--EXTENSIONS--
curl
--FILE--
<?php

include __DIR__ . '/scheduler.inc';

final class Tracing extends Scheduler
{
    public array $added = [];
    public array $removed = [];
    public array $runs = [];
    public bool $membersOk = true;
    public bool $addsOk = true;
    public bool $removesOk = true;

    public function run(\Io\Operation $op): \Io\Completion
    {
        if ($op instanceof \Io\Operation\Any) {
            foreach ($op->getOperations() as $m) {
                if ($m instanceof \Io\Operation\Poll) {
                    $this->runs[spl_object_id($m)] = ($this->runs[spl_object_id($m)] ?? 0) + 1;
                    $this->membersOk = $this->membersOk && $m->isPersistent()
                        && isset($this->added[spl_object_id($m)]) && $this->added[spl_object_id($m)] === $m;
                }
            }
        }
        return parent::run($op);
    }

    public function add(\Io\Operation $op): void
    {
        $this->added[spl_object_id($op)] = $op;
        $this->addsOk = $this->addsOk && $op instanceof \Io\Operation\Poll && $op->isPersistent()
            && $op->getHandle() instanceof \Io\Poll\WeakHandle && $op->getTimeout() === null
            && $op->getEvents() !== [];
        parent::add($op);
    }

    public function remove(\Io\Operation $op): void
    {
        $this->removed[spl_object_id($op)] = true;
        $this->removesOk = $this->removesOk && isset($this->added[spl_object_id($op)]) && $op->isValid();
        parent::remove($op);
    }
}

$scheduler = new Tracing();
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
    fwrite($conn, "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok");
    fclose($conn);
});

$scheduler->spawn(function () use ($addr) {
    $ch = curl_init("http://$addr/");
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    var_dump(curl_exec($ch));
});

$scheduler->loop();

// Every registration was added once, run with the same object, and removed
var_dump(count($scheduler->added) >= 1);
var_dump(array_keys($scheduler->removed) === array_keys($scheduler->added));
var_dump($scheduler->addsOk, $scheduler->removesOk, $scheduler->membersOk);
$ran = true;
$invalid = true;
foreach ($scheduler->added as $id => $op) {
    $ran = $ran && ($scheduler->runs[$id] ?? 0) >= 1;
    $invalid = $invalid && !$op->isValid();
}
var_dump($ran, $invalid);
Io\Hooks\set_hooks(null);
?>
--EXPECT--
string(2) "ok"
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
