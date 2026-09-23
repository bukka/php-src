--TEST--
IO hooks: a provider completing Timer, Poll and Any operations itself
--EXTENSIONS--
curl
--FILE--
<?php

final class Recorder implements Io\Hooks\Hooks
{
    public array $seen = [];

    public function getCapabilities(): array { return []; }

    public function run(Io\Operation $op): Io\Completion
    {
        $this->seen[] = $op::class;
        if ($op instanceof Io\Operation\Timer) {
            var_dump($op->getHandle(), $op->getEvents(), $op->getTimeout() instanceof Time\Duration);
            return $op->complete(Io\CompletionStatus::Done);
        }
        if ($op instanceof Io\Operation\Any) {
            $members = $op->getOperations();
            $timer = null;
            foreach ($members as $m) {
                if ($m instanceof Io\Operation\Timer) {
                    $timer = $m;
                } else {
                    var_dump($m instanceof Io\Operation\Poll, $m->getHandle() instanceof Io\Poll\Handle, $m->getTimeout());
                }
            }
            /* Report every member ready: libcurl copes with spurious readiness */
            $completions = [];
            foreach ($members as $m) {
                $completions[] = $m === $timer ? $m->complete(Io\CompletionStatus::Done) : $m->completeReady($m->getEvents());
            }
            $c = $op->completeWith($completions);
            var_dump(count($c->getCompletions()) === count($members));
            return $c;
        }
        return $op->completeReady($op->getEvents());
    }

    public function add(Io\Operation $op): void {}
    public function remove(Io\Operation $op): void {}
}

$recorder = new Recorder();
Io\Hooks\set_hooks($recorder);

usleep(1);

$ch = curl_init("http://127.0.0.1:1/");
curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt($ch, CURLOPT_CONNECTTIMEOUT_MS, 200);
var_dump(curl_exec($ch));
var_dump(curl_errno($ch) !== 0);

Io\Hooks\set_hooks(null);
var_dump(array_unique($recorder->seen));

/* Operations end when run() returns */
$last = null;
Io\Hooks\set_hooks(new class($last) implements Io\Hooks\Hooks {
    public function __construct(private &$last) {}
    public function getCapabilities(): array { return []; }
    public function run(Io\Operation $op): Io\Completion {
        $this->last = $op;
        return $op->complete(Io\CompletionStatus::Done);
    }
    public function add(Io\Operation $op): void {}
    public function remove(Io\Operation $op): void {}
});
usleep(1);
var_dump($last instanceof Io\Operation\Timer, $last->isValid());
try {
    $last->getTimeout();
} catch (Io\InvalidOperationException $e) {
    echo $e->getMessage(), "\n";
}
Io\Hooks\set_hooks(null);
?>
--EXPECTF--
NULL
array(0) {
}
bool(true)
%A
bool(false)
bool(true)
array(%d) {
%A
}
bool(true)
bool(false)
The operation has ended
