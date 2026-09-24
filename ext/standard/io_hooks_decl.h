/* This is a generated file, edit io_hooks.stub.php instead.
 * Stub hash: 017285c42f4a672145ede1a3a5f18b5ec1526b82 */

#ifndef ZEND_IO_HOOKS_DECL_017285c42f4a672145ede1a3a5f18b5ec1526b82_H
#define ZEND_IO_HOOKS_DECL_017285c42f4a672145ede1a3a5f18b5ec1526b82_H

typedef enum zend_enum_Io_CompletionStatus {
	ZEND_ENUM_Io_CompletionStatus_Done = 1,
	ZEND_ENUM_Io_CompletionStatus_Ready = 2,
	ZEND_ENUM_Io_CompletionStatus_Timeout = 3,
	ZEND_ENUM_Io_CompletionStatus_Interrupted = 4,
	ZEND_ENUM_Io_CompletionStatus_Cancelled = 5,
	ZEND_ENUM_Io_CompletionStatus_Unsupported = 6,
} zend_enum_Io_CompletionStatus;

typedef enum zend_enum_Io_Hooks_Capability {
	ZEND_ENUM_Io_Hooks_Capability_Files = 1,
	ZEND_ENUM_Io_Hooks_Capability_Direct = 2,
} zend_enum_Io_Hooks_Capability;

#endif /* ZEND_IO_HOOKS_DECL_017285c42f4a672145ede1a3a5f18b5ec1526b82_H */
