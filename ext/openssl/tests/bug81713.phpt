--TEST--
Bug #81713 (OpenSSL functions null byte injection)
--SKIPIF--
<?php
if (!extension_loaded("openssl")) die("skip openssl not loaded");
?>
--FILE--
<?php
$crt_file = __DIR__ . '/cert.crt';
$csr_file = __DIR__ . '/cert.csr';
$tests = [
    ["openssl_pkey_get_public", "file://$crt_file"],
    ["openssl_pkey_get_public", "file://$crt_file\x00foo"],
    ["openssl_csr_get_subject", "file://$csr_file"],
    ["openssl_csr_get_subject", "file://$csr_file\x00foo"],
    ["openssl_x509_fingerprint", "file://$crt_file"],
    ["openssl_x509_fingerprint", "file://$crt_file\x00foo"],
];
foreach ($tests as $test) {
    try {
        $key = $test[0]($test[1]);
        var_dump($key);
    }
    catch (ValueError $e) {
        echo $e->getMessage() . PHP_EOL;
    }
}
?>
--EXPECTF--
object(OpenSSLAsymmetricKey)#1 (0) {
}
openssl_pkey_get_public(): Argument #1 ($public_key) must not contain any null bytes
array(6) {
  ["C"]=>
  string(2) "NL"
  ["ST"]=>
  string(13) "Noord Brabant"
  ["L"]=>
  string(4) "Uden"
  ["O"]=>
  string(10) "Triconnect"
  ["OU"]=>
  string(10) "Triconnect"
  ["CN"]=>
  string(15) "*.triconnect.nl"
}
openssl_csr_get_subject(): Argument #1 ($csr) must not contain any null bytes
string(40) "6e6fd1ea10a5a23071d61c728ee9b40df6dbc33c"

Warning: openssl_x509_fingerprint(): X.509 Certificate cannot be retrieved in %s on line %d
openssl_x509_fingerprint(): Argument #1 ($certificate) must not contain any null bytes
