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

cc --ld-path=./ld64 -o $t/exe $t/a.o -L$t -Wl,-lfoo
otool -l $t/exe | grep -A3 LOAD_DY | grep -q libfoo.dylib

cc --ld-path=./ld64 -o $t/exe $t/a.o -L$t -Wl,-lfoo -Wl,-dead_strip_dylibs
otool -l $t/exe | grep -A3 LOAD_DY > $t/log
! grep -q libfoo.dylib $t/log || false
