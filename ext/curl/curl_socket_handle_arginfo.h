/* This is a generated file, edit curl_socket_handle.stub.php instead.
 * Stub hash: 98a4d5089436846e90791c003944cd20c0294971 */

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_Io_Curl_SocketWeakHandle___construct, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_METHOD(Io_Curl_SocketWeakHandle, __construct);

static const zend_function_entry class_Io_Curl_SocketWeakHandle_methods[] = {
	ZEND_ME(Io_Curl_SocketWeakHandle, __construct, arginfo_class_Io_Curl_SocketWeakHandle___construct, ZEND_ACC_PRIVATE)
	ZEND_FE_END
};

static zend_class_entry *register_class_Io_Curl_SocketWeakHandle(zend_class_entry *class_entry_Io_Poll_WeakHandle)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "Io\\Curl", "SocketWeakHandle", class_Io_Curl_SocketWeakHandle_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL|ZEND_ACC_NO_DYNAMIC_PROPERTIES|ZEND_ACC_NOT_SERIALIZABLE);
	zend_class_implements(class_entry, 1, class_entry_Io_Poll_WeakHandle);

	return class_entry;
}
