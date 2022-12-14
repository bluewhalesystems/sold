#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | cc -c -o $t/a.o -xc -
#include <stdio.h>

int foo() __attribute__((weak));

int foo() {
  return 3;
}

int main() {
  printf("%d\n", foo());
}
EOF

cat <<EOF | cc -c -o $t/b.o -xc -
int foo() { return 42; }
EOF

cc --ld-path=./ld64 -o $t/exe1 $t/a.o
$t/exe1 | grep -q '^3$'

cc --ld-path=./ld64 -o $t/exe1 $t/a.o $t/b.o
$t/exe1 | grep -q '^42$'
