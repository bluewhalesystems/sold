#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | cc -c -o $t/a.o -xc -
int foo() __attribute__((weak));

int foo() {
  return 3;
}
EOF

cc --ld-path=./ld64 -shared -o $t/b.dylib $t/a.o

cat <<EOF | cc -c -o $t/c.o -xc -
#include <stdio.h>

int foo() __attribute((weak));

int main() {
  printf("%d\n", foo ? foo() : 42);
}
EOF

cc --ld-path=./ld64 -o $t/exe $t/b.dylib $t/c.o
$t/exe | grep -q '^3$'

cc -c -o $t/d.o -xc /dev/null
cc --ld-path=./ld64 -shared -o $t/b.dylib $t/d.o
$t/exe | grep -q '^42$'
