#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | cc -o $t/a.o -c -xc -
#include <stdio.h>

int main() {
  printf("Hello");
  puts(" world");
}
EOF

cc --ld-path=./ld64 -o $t/exe $t/a.o
$t/exe | grep -q 'Hello world'
