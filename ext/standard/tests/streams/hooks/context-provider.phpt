--TEST--
IO hooks: a provider on its own Poll context, timers included (section 6.3)
--FILE--
<?php

final class ContextProvider implements \Io\Hooks\Hooks
{
    private \Io\Poll\Context $context;
    private array $ready = [];
    private int $waiting = 0;

    public function __construct()
    {
        $this->context = new \Io\Poll\Context();
    }

    public function getCapabilities(): array { return []; }
    public function add(\Io\Operation $op): void {}
    public function remove(\Io\Operation $op): void {}

    public function spawn(callable $fn): void
    {
        $this->ready[] = [new \Fiber($fn), null];
    }

    private function await(array $pairs): array
    {
        $fiber = \Fiber::getCurrent();
        $watchers = [];
        foreach ($pairs as $i => [$handle, $events]) {
            $watchers[$i] = $this->context->add($handle, [...$events, \Io\Poll\Event::OneShot], [$fiber, $i]);
        }
        $this->waiting++;
        [$i, $triggered] = \Fiber::suspend();
        foreach ($watchers as $j => $w) {
            if ($j !== $i && $w->isActive()) {
                $w->remove();
            }
        }
        return [$i, $triggered];
    }

    public function run(\Io\Operation $op): \Io\Completion
    {
        if ($op instanceof \Io\Operation\Any) {
            $members = $op->getOperations();
            $pairs = array_map(fn ($m) => [$m->getHandle(), $m->getEvents()], $members);
            [$i, $triggered] = $this->await($pairs);
            return $op->completeWith([$members[$i]->completeReady($triggered)]);
        }
        if ($op->getHandle() === null) {
            return $op->complete(\Io\CompletionStatus::Unsupported);
        }
        $pairs = [[$op->getHandle(), $op->getEvents()]];
        if (!$op instanceof \Io\Operation\Timer && $op->getTimeout() !== null) {
            $pairs[] = [new \Io\Poll\TimerHandle($op->getTimeout()), [\Io\Poll\Event::Timer]];
        }
        [$i, $triggered] = $this->await($pairs);
        return $i === 0
            ? $op->completeReady($triggered)
            : $op->complete(\Io\CompletionStatus::Timeout);
    }

    public function loop(): void
    {
        while ($this->ready || $this->waiting > 0) {
            while ($this->ready) {
                [$fiber, $value] = array_shift($this->ready);
                $fiber->isStarted() ? $fiber->resume($value) : $fiber->start();
            }
            if ($this->waiting === 0) {
                break;
            }
            $resumed = [];
            foreach ($this->context->wait() as $w) {
                [$fiber, $i] = $w->getData();
                if (isset($resumed[spl_object_id($fiber)])) {
                    continue;
                }
                $resumed[spl_object_id($fiber)] = true;
                $this->waiting--;
                $this->ready[] = [$fiber, [$i, $w->getTriggeredEvents()]];
            }
        }
    }
}

$provider = new ContextProvider();
\Io\Hooks\set_hooks($provider);

$server = stream_socket_server('tcp://127.0.0.1:0');
$addr = stream_socket_get_name($server, false);

$provider->spawn(function () use ($server) {
    $conn = stream_socket_accept($server, 5);
    usleep(20000);
    fwrite($conn, "hello\n");
    fclose($conn);
    echo "server done\n";
});
$provider->spawn(function () use ($addr) {
    $c = stream_socket_client("tcp://$addr");
    $line = trim(fgets($c));
    echo "client got: $line\n";
});
$provider->spawn(function () {
    usleep(5000);
    echo "slept\n";
});
$provider->spawn(function () use ($addr) {
    // A read that times out is a Timeout completion from the timer pair
    $c = stream_socket_client("tcp://$addr");
    stream_set_timeout($c, 0, 50000);
    var_dump(fgets($c));
    var_dump(stream_get_meta_data($c)['timed_out']);
});

$provider->loop();
echo "done\n";
?>
--EXPECT--
slept
server done
client got: hello
bool(false)
bool(true)
done
