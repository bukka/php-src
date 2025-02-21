--TEST--
Bug #67609: TLS encryption fails behind HTTP proxy
--EXTENSIONS--
openssl
--SKIPIF--
<?php
if (!function_exists("proc_open")) die("skip no proc_open");
?>
--FILE--
<?php
$caCertFile = __DIR__ . DIRECTORY_SEPARATOR . 'bug67609-ca.pem.tmp';
$clientCertFile = __DIR__ . DIRECTORY_SEPARATOR . 'bug67609-client.pem.tmp';
$serverCertFile = __DIR__ . DIRECTORY_SEPARATOR . 'bug67609-server.pem.tmp';

$serverCode = <<<'CODE'
    $serverUri = "tls://127.0.0.1:11110";
    $serverFlags = STREAM_SERVER_BIND | STREAM_SERVER_LISTEN;
    $serverCtx = stream_context_create(['ssl' => [
        'local_cert' => '%s',
        'cafile' => '%s',
    ]]);

    $server = stream_socket_server($serverUri, $errno, $errstr, $serverFlags, $serverCtx);
    phpt_notify_server_start($server);

    $conn = stream_socket_accept($server, 30);
    fwrite($conn, "HTTP/1.0 200 OK\r\n\r\nHello from server");
    phpt_wait();
    fclose($conn);

CODE;
$serverCode = sprintf($serverCode, $serverCertFile, $caCertFile);

$proxyCode = <<<'CODE'
    $upstream = stream_socket_client("tcp://{{ ADDR }}", $errornum, $errorstr, 30, STREAM_CLIENT_CONNECT);
    stream_set_blocking($upstream, false);

    $flags = STREAM_SERVER_BIND | STREAM_SERVER_LISTEN;
    $server = stream_socket_server("tcp://127.0.0.1:11111", $errornum, $errorstr, $flags);
    phpt_notify_server_start($server);

    $conn = stream_socket_accept($server);
    stream_set_blocking($conn, false);

    while (!feof($conn) && !feof($upstream)) {
        $clientData = fread($conn, 8192);
        if ($clientData !== false && $clientData !== '') {
            fwrite($upstream, $clientData);
        }

        $serverData = fread($upstream, 8192);
        if ($serverData !== false && $serverData !== '') {
            fwrite($conn, $serverData);
        }
    }

    phpt_wait();

    fclose($conn);
    fclose($upstream);
CODE;

$clientCode = <<<'CODE'
    $clientCtx = stream_context_create([
        'ssl' => [
            'local_cert' => '%s',
            'cafile' => '%s',
            'verify_peer' => true,
            'verify_peer_name' => true,
        ],
        "http" => [
            "proxy" => "tcp://{{ ADDR }}"
        ],
    ]);

    // First HTTPS request
    var_dump(file_get_contents("https://bug67609-server/", false, $clientCtx));

    var_dump(stream_context_get_options($clientCtx));

    phpt_notify('server');
    phpt_notify('proxy');
CODE;
$clientCode = sprintf($clientCode, $serverCertFile, $caCertFile);

include 'CertificateGenerator.inc';
$certificateGenerator = new CertificateGenerator();
$certificateGenerator->saveCaCert($caCertFile);
$certificateGenerator->saveNewCertAsFileWithKey('bug67609-client', $clientCertFile);
$certificateGenerator->saveNewCertAsFileWithKey('bug67609-server', $serverCertFile);

include 'ServerClientTestCase.inc';
ServerClientTestCase::getInstance()->run($clientCode, [
    'server' => $serverCode,
    'proxy' => $proxyCode,
]);
?>
--CLEAN--
<?php
@unlink(__DIR__ . DIRECTORY_SEPARATOR . 'bug67609-ca.pem.tmp');
@unlink(__DIR__ . DIRECTORY_SEPARATOR . 'bug67609-client.pem.tmp');
@unlink(__DIR__ . DIRECTORY_SEPARATOR . 'bug67609-server.pem.tmp');
?>
--EXPECTF--
resource(%d) of type (stream)
