--TEST--
GMP serialization and unserialization
--SKIPIF--
<?php if (!extension_loaded("gmp")) print "skip"; ?>
--FILE--
<?php

var_dump($n = gmp_init(42));
var_dump($s = serialize($n));
var_dump(unserialize($s));

$n = gmp_init(13);
$n->foo = "bar";
var_dump($s = serialize($n));
var_dump(unserialize($s));

?>
--EXPECTF--	
object(GMP)#%d (1) {
  ["num"]=>
  string(2) "42"
}
string(33) "O:3:"GMP":1:{s:3:"num";s:2:"42";}"
object(GMP)#%d (1) {
  ["num"]=>
  string(2) "42"
}
string(53) "O:3:"GMP":2:{s:3:"num";s:2:"13";s:3:"foo";s:3:"bar";}"
object(GMP)#%d (2) {
  ["fo"]=>
  string(3) "bar"
  ["num"]=>
  string(2) "13"
}