#!/bin/bash
. $(dirname $0)/common.inc

mkdir -p $t/foo/bar

cat <<EOF | cc -shared -o $t/foo/bar/libbaz.dylib -xc -
void foo() {}
EOF

cat <<EOF | cc -o $t/a.o -c -xc -
void foo();
void bar() { foo(); }
EOF

cc --ld-path=./ld64 -shared -o $t/b.dylib $t/a.o -nodefaultlibs \
  -L/foo/bar -isysroot $t -lbaz
