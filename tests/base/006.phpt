--TEST--
Check parallel args
--SKIPIF--
<?php
if (!extension_loaded('parallel')) {
	echo 'skip';
}
?>
--FILE--
<?php
try {
	$parallel = new parallel\Runtime(4,2);
} catch (Throwable $t) {
	var_dump($t->getMessage());
}
?>
--EXPECT--
string(58) "bootstrap or bootstrap and optional configuration expected"


