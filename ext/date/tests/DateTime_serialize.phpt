--TEST--
Test serialization of DateTime objects
--FILE--
<?php
//Set the default time zone 
date_default_timezone_set("Europe/London");

$date1 = new DateTime("2005-07-14 22:30:41");
var_dump($date1);
$serialized = serialize($date1);
var_dump($serialized); 

$date2 = unserialize($serialized);
var_dump($date2);
// Try to use unserialzied object 
var_dump( $date2->format( "F j, Y, g:i a") ); 

$date3 = new DateTime("2013-09-22 16:43:23");
$date3->name_day = "Darina";
$date3->value = 23;
var_dump($date3);
$serialized = serialize($date3);
var_dump($serialized);

$date4 = unserialize($serialized);
var_dump($date4);
// Try to use unserialzied object
var_dump( $date4->format( "F j, Y, g:i a") );

?>
===DONE=== 
--EXPECTF--
object(DateTime)#%d (3) {
  ["date"]=>
  string(19) "2005-07-14 22:30:41"
  ["timezone_type"]=>
  int(3)
  ["timezone"]=>
  string(13) "Europe/London"
}
string(118) "O:8:"DateTime":3:{s:4:"date";s:19:"2005-07-14 22:30:41";s:13:"timezone_type";i:3;s:8:"timezone";s:13:"Europe/London";}"
object(DateTime)#%d (3) {
  ["date"]=>
  string(19) "2005-07-14 22:30:41"
  ["timezone_type"]=>
  int(3)
  ["timezone"]=>
  string(13) "Europe/London"
}
string(23) "July 14, 2005, 10:30 pm"
object(DateTime)#%d (5) {
  ["date"]=>
  string(19) "2013-09-22 16:43:23"
  ["timezone_type"]=>
  int(3)
  ["timezone"]=>
  string(13) "Europe/London"
  ["name_day"]=>
  string(6) "Darina"
  ["value"]=>
  int(23)
}
string(163) "O:8:"DateTime":5:{s:4:"date";s:19:"2013-09-22 16:43:23";s:13:"timezone_type";i:3;s:8:"timezone";s:13:"Europe/London";s:8:"name_day";s:6:"Darina";s:5:"value";i:23;}"
object(DateTime)#%d (5) {
  ["date"]=>
  string(19) "2013-09-22 16:43:23"
  ["timezone_type"]=>
  int(3)
  ["timezone"]=>
  string(13) "Europe/London"
  ["name_day"]=>
  string(6) "Darina"
  ["value"]=>
  int(23)
}
string(27) "September 22, 2013, 4:43 pm"
===DONE===