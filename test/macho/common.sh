#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | cc -o $t/a.o -fcommon -c -xc -
int foo;
int bar;
EOF

cat <<EOF | cc -o $t/b.o -fcommon -c -xc -
int foo;
int bar = 5;
EOF

cat <<EOF | cc -o $t/c.o -c -xc -
#include <stdio.h>

extern int foo;
extern int bar;
static int baz[10000];

int main() {
  printf("%d %d %d\n", foo, bar, baz[0]);
}
EOF

cc --ld-path=./ld64 -o $t/exe $t/a.o $t/b.o $t/c.o
$t/exe | grep -q '^0 5 0$'
