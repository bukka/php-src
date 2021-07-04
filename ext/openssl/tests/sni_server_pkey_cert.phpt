--TEST--
sni_server with separate pkeys and certs variants
--EXTENSIONS--
openssl
--SKIPIF--
<?php
if (!function_exists("proc_open")) die("skip no proc_open");
?>
--FILE--
<?php
$serverCode = <<<'CODE'
    $flags = STREAM_SERVER_BIND|STREAM_SERVER_LISTEN;
    $us_cert = openssl_x509_read("file://" .  __DIR__ . "/sni_server_us_cert.pem");
    $us_pkey = openssl_pkey_get_private("file://" . __DIR__ . "/sni_server_us_key.pem");
    $ctx = stream_context_create(['ssl' => [
        'local_cert' => __DIR__ . '/domain1.pem',
        'SNI_server_certs' => [
            "cs.php.net" => [
                'local_cert' => 'file://' . __DIR__ . "/sni_server_cs_cert.pem",
                'local_pk' => 'file://' . __DIR__ . "/sni_server_cs_key.pem",
            ],
            "uk.php.net" => [
                'local_cert' => file_get_contents(__DIR__ . "/sni_server_uk_cert.pem"),
                'local_pk' => file_get_contents(__DIR__ . "/sni_server_uk_key.pem"),
            ],
            "us.php.net" => [
                'local_cert' => $us_cert,
                'local_pk' => $us_pkey,
            ],
        ]
    ]]);

    $server = stream_socket_server('tls://127.0.0.1:64321', $errno, $errstr, $flags, $ctx);
    phpt_notify();

    for ($i=0; $i < 3; $i++) {
        @stream_socket_accept($server, 3);
    }
CODE;

$clientCode = <<<'CODE'
    $flags = STREAM_CLIENT_CONNECT;
    $ctxArr = [
        'cafile' => __DIR__ . '/sni_server_ca.pem',
        'capture_peer_cert' => true
    ];

    phpt_wait();

    $ctxArr['peer_name'] = 'cs.php.net';
    $ctx = stream_context_create(['ssl' => $ctxArr]);
    $client = stream_socket_client("tls://127.0.0.1:64321", $errno, $errstr, 1, $flags, $ctx);
    $cert = stream_context_get_options($ctx)['ssl']['peer_certificate'];
    var_dump(openssl_x509_parse($cert)['subject']['CN']);

    $ctxArr['peer_name'] = 'uk.php.net';
    $ctx = stream_context_create(['ssl' => $ctxArr]);
    $client = @stream_socket_client("tls://127.0.0.1:64321", $errno, $errstr, 1, $flags, $ctx);
    $cert = stream_context_get_options($ctx)['ssl']['peer_certificate'];
    var_dump(openssl_x509_parse($cert)['subject']['CN']);

    $ctxArr['peer_name'] = 'us.php.net';
    $ctx = stream_context_create(['ssl' => $ctxArr]);
    $client = @stream_socket_client("tls://127.0.0.1:64321", $errno, $errstr, 1, $flags, $ctx);
    $cert = stream_context_get_options($ctx)['ssl']['peer_certificate'];
    var_dump(openssl_x509_parse($cert)['subject']['CN']);
CODE;

include 'ServerClientTestCase.inc';
ServerClientTestCase::getInstance()->run($clientCode, $serverCode);
?>
--EXPECTF--
string(%d) "cs.php.net"
string(%d) "uk.php.net"
string(%d) "us.php.net"
