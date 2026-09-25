--TEST--
IO hooks: a retained registration nobody waits on does not report the peer's hangup
--FILE--
<?php
$q = new Io\Poll\OperationQueue();
[$a, $b] = stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, 0);
$h = new class($q, $b) implements Io\Hooks\Hooks {
    public int $runs = 0;
    public function __construct(public $q, public $b) {}
    public function getCapabilities(): array { return []; }
    public function add(Io\Operation $op): void {}
    public function remove(Io\Operation $op): void {}
    public function run(Io\Operation $op): Io\Completion {
        if ($this->runs++ == 0) {
            fwrite($this->b, "x");
        }
        $this->q->add($op);
        $this->q->submit($op);
        foreach ($this->q->waitCompletions() as $c) {}
        return $c;
    }
};
Io\Hooks\set_hooks($h);
var_dump(fread($a, 10));
Io\Hooks\set_hooks(null);
var_dump($h->runs > 0);
fclose($b);

$cpu = function () {
    $r = getrusage();
    return $r['ru_utime.tv_sec'] + $r['ru_utime.tv_usec'] / 1e6 + $r['ru_stime.tv_sec'] + $r['ru_stime.tv_usec'] / 1e6;
};
$before = $cpu();
var_dump(count($q->waitCompletions(Time\Duration::fromMilliseconds(500))));
// Waiting on a hangup nobody asked about spun for the whole wait
var_dump($cpu() - $before < 0.25);
?>
--EXPECT--
string(1) "x"
bool(true)
int(0)
bool(true)
