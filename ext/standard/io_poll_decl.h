/* This is a generated file, edit io_poll.stub.php instead.
 * Stub hash: 75e85eb1edfdea3d55fe0e61db98442801e3d088 */

#ifndef ZEND_IO_POLL_DECL_75e85eb1edfdea3d55fe0e61db98442801e3d088_H
#define ZEND_IO_POLL_DECL_75e85eb1edfdea3d55fe0e61db98442801e3d088_H

typedef enum zend_enum_Io_Poll_Backend {
	ZEND_ENUM_Io_Poll_Backend_Auto = 1,
	ZEND_ENUM_Io_Poll_Backend_Poll = 2,
	ZEND_ENUM_Io_Poll_Backend_Epoll = 3,
	ZEND_ENUM_Io_Poll_Backend_Kqueue = 4,
	ZEND_ENUM_Io_Poll_Backend_EventPorts = 5,
	ZEND_ENUM_Io_Poll_Backend_WSAPoll = 6,
} zend_enum_Io_Poll_Backend;

typedef enum zend_enum_Io_Poll_Event {
	ZEND_ENUM_Io_Poll_Event_Read = 1,
	ZEND_ENUM_Io_Poll_Event_Write = 2,
	ZEND_ENUM_Io_Poll_Event_Error = 3,
	ZEND_ENUM_Io_Poll_Event_HangUp = 4,
	ZEND_ENUM_Io_Poll_Event_ReadHangUp = 5,
	ZEND_ENUM_Io_Poll_Event_OneShot = 6,
	ZEND_ENUM_Io_Poll_Event_EdgeTriggered = 7,
	ZEND_ENUM_Io_Poll_Event_Priority = 8,
	ZEND_ENUM_Io_Poll_Event_Timer = 9,
	ZEND_ENUM_Io_Poll_Event_Notify = 10,
} zend_enum_Io_Poll_Event;

#endif /* ZEND_IO_POLL_DECL_75e85eb1edfdea3d55fe0e61db98442801e3d088_H */
