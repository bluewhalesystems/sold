#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | cc -o $t/a.o -c -xc -
int foo();
int main() { foo(); }
EOF

cc --ld-path=./ld64 -o $t/exe $t/a.o -Wl,-U,_foo
nm $t/exe | grep -q 'U _foo$'
