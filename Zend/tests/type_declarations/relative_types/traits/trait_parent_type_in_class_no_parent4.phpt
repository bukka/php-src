--TEST--
Cannot use a trait which references parent as a type in a class with no parent, DNF type
--FILE--
<?php
trait TraitExample {
    public function bar(): (X&Y)|parent { return parent::class; }
}

class A {
    use TraitExample;
}
?>
DONE
--EXPECT--
DONE
