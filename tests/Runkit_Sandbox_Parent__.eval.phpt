--TEST--
Runkit_Sandbox_Parent Class -- Eval
--SKIPIF--
<?php if(!extension_loaded("runkit")) print "skip";
	  /* May not be available due to lack of TSRM interpreter support */
	  if(!function_exists("runkit_lint")) print "skip"; ?>
--FILE--
<?php
$php = new Runkit_Sandbox();
$php['parent_access'] = true;
$php['parent_eval'] = true;
$php->ini_set('html_errors',false);
$php->ini_set('display_errors',true);
$php->error_reporting(E_ALL);
$php->eval('$PARENT = new Runkit_Sandbox_Parent;
			$PARENT->eval(\'var_dump($php);\');');
--EXPECT--
object(Runkit_Sandbox)#1 (0) {
}
