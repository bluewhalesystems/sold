#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | cc -o $t/a.o -c -xc - -flto
#include <stdio.h>
int main() {
  printf("Hello world\n");
}
EOF

cc --ld-path=./ld64 -o $t/exe $t/a.o -flto -Wl,-object_path_lto,$t/obj
$t/exe | grep -q 'Hello world'
otool -l $t/obj > /dev/null
