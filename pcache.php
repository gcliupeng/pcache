<?php
$re=pcache_add('abc','abc',20);
var_dump($re);
$re=pcache_get('abc');
var_dump($re);
$re=pcache_info();
echo $re."\r\n";
$re=pcache_del('abc');
echo pcache_info()."\r\n";
exit;
?>
