<?php

/* The minimal provider: a fiber scheduler that is a complete provider on any
 * Io\OperationQueue. The Hooks methods are called by the core from inside
 * blocking functions, on the fiber that runs them; spawn() and loop() are
 * what the script calls. */
final class Scheduler implements \Io\Hooks\Hooks
{
    /** Fibers to resume, with the value: null to start, a Completion to continue. */
    private array $ready = [];
    /** Submitted operations whose fiber is suspended in run(). */
    private int $inFlight = 0;
    private \Io\OperationQueue $queue;

    public function __construct(?\Io\OperationQueue $queue = null)
    {
        $this->queue = $queue ?? new \Io\Poll\OperationQueue();
    }

    /** Schedule a task; it starts on the next pass of loop(). */
    public function spawn(callable $fn): void
    {
        $this->ready[] = [new \Fiber($fn), null];
    }

    /* The Hooks interface, called by the core. */

    public function getCapabilities(): array
    {
        return $this->queue->getHookCapabilities();
    }

    public function run(\Io\Operation $op): \Io\Completion
    {
        $this->queue->submit($op, \Fiber::getCurrent());
        $this->inFlight++;
        return \Fiber::suspend();
    }

    public function add(\Io\Operation $op): void
    {
        $this->queue->add($op);
    }

    public function remove(\Io\Operation $op): void
    {
        $this->queue->remove($op);
    }

    /* The event loop, called by the script on the main fiber. */
    public function loop(): void
    {
        while ($this->ready || $this->inFlight > 0) {
            while ($this->ready) {
                [$fiber, $value] = array_shift($this->ready);
                $fiber->isStarted() ? $fiber->resume($value) : $fiber->start();
            }
            if ($this->inFlight === 0) {
                break;
            }
            foreach ($this->queue->waitCompletions() as $c) {
                $this->inFlight--;
                $this->ready[] = [$c->getData(), $c];
            }
        }
    }

    /* Test helper: start a task and let it run before continuing the caller. */
    public function go(callable $fn): void
    {
        $this->ready[] = [new \Fiber($fn), null];
        $fiber = \Fiber::getCurrent();
        if ($fiber === null) {
            return;
        }
        $this->ready[] = [$fiber, null];
        \Fiber::suspend();
    }
}
