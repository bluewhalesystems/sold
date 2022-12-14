#!/bin/bash
. $(dirname $0)/common.inc

[ "`uname -p`" = arm ] && { echo skipped; exit; }

cat <<EOF | cc -o $t/a.o -c -xc -
int main() {}
EOF

cc --ld-path=./ld64 -o $t/exe $t/a.o -Wl,-headerpad,0
cc --ld-path=./ld64 -o $t/exe $t/a.o -Wl,-headerpad,0x10000
