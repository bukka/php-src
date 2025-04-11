--TEST--
dns_get_record() basic usage
--SKIPIF--
<?php
if (!function_exists('dns_get_record')) {
    die("skip dns_get_record not available");
}
// Check if the domain resolves using dig
$domain = 'www.basic.dnstest.php.net';
$dig = `dig +short $domain @127.0.0.1`;
if (trim($dig) !== '192.0.2.1') {
    die("skip local BIND not resolving test domain ($domain)");
}
?>
--FILE--
<?php
$domain = 'www.basic.dnstest.php.net';

$result = dns_get_record($domain, DNS_A);
var_dump($result);
?>
--EXPECTF--
array(%d) {
  [0]=>
  array(%d) {
    ["host"]=>
    string(%d) "www.basic.dnstest.php.net"
    ["class"]=>
    string(2) "IN"
    ["ttl"]=>
    int(%d)
    ["type"]=>
    string(1) "A"
    ["ip"]=>
    string(%d) "192.0.2.1"
  }
}
