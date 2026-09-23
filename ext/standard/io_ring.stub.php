<?php

/**
 * @generate-class-entries
 * @generate-c-enums
 */

namespace Io\Ring {

    /** Which backend ior chose. Informational: there is no way to request one. */
    enum Backend {
        case IoUring;
        case Iocp;
        case Threads;
    }

    /**
     * The Ring: an operation queue that executes every operation itself,
     * on io_uring, IOCP or ior's thread pool.
     * @strict-properties
     * @not-serializable
     */
    final class Engine implements \Io\OperationQueue
    {
        /** Submission queue depth; 0 is the depth the core's own ring queue uses. */
        public function __construct(int $entries = 0) {}

        public function getBackend(): Backend {}

        /**
         * Raised for every posted completion, so a loop that keeps its own
         * Poll context can embed a ring: add it with Event::Notify and, when it
         * fires, call waitCompletions() with a zero timeout until it returns an
         * empty array.
         */
        public function getHandle(): \Io\Poll\NotifyHandle {}

        /** @implementation-alias Io\Poll\OperationQueue::submit */
        public function submit(\Io\Operation $op, mixed $data = null): void {}

        /** @implementation-alias Io\Poll\OperationQueue::cancel */
        public function cancel(\Io\Operation $op): void {}

        /** @implementation-alias Io\Poll\OperationQueue::add */
        public function add(\Io\Operation $op): void {}

        /** @implementation-alias Io\Poll\OperationQueue::remove */
        public function remove(\Io\Operation $op): void {}

        /**
         * @return list<\Io\Completion>
         * @implementation-alias Io\Poll\OperationQueue::waitCompletions
         */
        public function waitCompletions(?\Time\Duration $timeout = null, ?int $max = null): array {}

        /** @implementation-alias Io\Poll\OperationQueue::countPending */
        public function countPending(): int {}

        /**
         * @return list<\Io\Hooks\Capability>
         * @implementation-alias Io\Poll\OperationQueue::getHookCapabilities
         */
        public function getHookCapabilities(): array {}
    }

    class RingException extends \Io\IoException {}

    /** Submit, cancel and wait failures, the errno in the code */
    class FailedRingOperationException extends RingException {}
}
