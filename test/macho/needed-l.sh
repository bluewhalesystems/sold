#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | cc -o $t/libfoo.dylib -shared -xc -
#include <stdio.h>
void hello() {
  printf("Hello world\n");
}
EOF

cat <<EOF | cc -o $t/a.o -c -xc -
int main() {}
EOF

cc --ld-path=./ld64 -o $t/exe $t/a.o -L$t -Wl,-needed-lfoo
$t/exe

otool -l $t/exe | grep -A3 LOAD_DY | grep -q libfoo.dylib
