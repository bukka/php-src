--TEST--
GH-9261: Problem with enabling crypto on stream socket connection
--EXTENSIONS--
openssl
--SKIPIF--
<?php
if (!function_exists("proc_open")) die("skip no proc_open");
?>
--FILE--
<?php
$certFile = __DIR__ . DIRECTORY_SEPARATOR . 'gh9261.pem.tmp';

$serverCode = <<<'CODE'
    $context = stream_context_create(['ssl' => ['local_cert' => 'x']]);

    $flags = STREAM_SERVER_BIND|STREAM_SERVER_LISTEN;
    $fp = stream_socket_server("ssl://127.0.0.1:0", $errornum, $errorstr, $flags, $context);
    phpt_notify_server_start($fp);
    $conn = stream_socket_accept($fp);
    fclose($conn);
    phpt_wait();
CODE;
//$serverCode = sprintf($serverCode, $certFile);

$peerName = 'gh9261';
$clientCode = <<<'CODE'
    $context = stream_context_create(['ssl' => ['verify_peer' => false, 'peer_name' => '%s']]);

    $fp = stream_socket_client("ssl://{{ ADDR }}", $errornum, $errorstr, 3000, STREAM_CLIENT_CONNECT, $context);
    var_dump(fread($fp, 8192));
    phpt_notify();
    echo "done";
CODE;
$clientCode = sprintf($clientCode, $peerName);

include 'CertificateGenerator.inc';
$certificateGenerator = new CertificateGenerator();
$certificateGenerator->saveNewCertAsFileWithKey($peerName, $certFile);

include 'ServerClientTestCase.inc';
ServerClientTestCase::getInstance()->run($clientCode, $serverCode);
?>
--CLEAN--
<?php
@unlink(__DIR__ . DIRECTORY_SEPARATOR . 'gh9261.pem.tmp');
?>
--EXPECTF--
PHP Warning:  stream_socket_accept(): Path for local_cert in ssl stream context option must not contain any null bytes in %s
PHP Warning:  stream_socket_accept(): Unable to get real path of certificate file `%scert.crt' in %s
PHP Warning:  stream_socket_accept(): Failed to enable crypto in %s
PHP Warning:  stream_socket_accept(): Accept failed: Success%s
