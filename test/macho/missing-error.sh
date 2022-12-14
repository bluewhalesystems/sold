#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | cc -o $t/a.o -c -xc -
int foo();

int main() {
  foo();
}
EOF

! cc --ld-path=./ld64 -o $t/exe $t/a.o 2> $t/log || false
grep -q 'undefined symbol: .*\.o: _foo' $t/log
