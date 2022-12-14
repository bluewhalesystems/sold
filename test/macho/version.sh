#!/bin/bash
. $(dirname $0)/common.inc

./ld64 -v | grep -q '[ms]old'

cat <<EOF | cc -o $t/a.o -c -xc -
#include <stdio.h>

int main() {
  printf("Hello world\n");
}
EOF

cc --ld-path=./ld64 -Wl,-v -o $t/exe $t/a.o | grep -q '[ms]old'
$t/exe | grep -q 'Hello world'
