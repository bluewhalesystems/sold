#!/bin/bash
. $(dirname $0)/common.inc

mkdir -p $t/Foo.framework

cat <<EOF | cc -o $t/Foo.framework/Foo -shared -xc -
#include <stdio.h>
void hello() {
  printf("Hello world\n");
}
EOF

cat <<EOF | cc -o $t/a.o -c -xc -
#include <stdio.h>
int main() {
  printf("Hello world\n");
}
EOF

cc --ld-path=./ld64 -o $t/exe $t/a.o -Wl,-F$t -Wl,-framework,Foo
otool -l $t/exe | grep -A3 'cmd LC_LOAD_DYLIB' | grep -Fq Foo.framework/Foo

cc --ld-path=./ld64 -o $t/exe $t/a.o -Wl,-F$t -Wl,-framework,Foo \
  -Wl,-dead_strip_dylibs
otool -l $t/exe | grep -A3 'cmd LC_LOAD_DYLIB' >& $t/log
! grep -Fq Foo.framework/Foo $t/log || false
