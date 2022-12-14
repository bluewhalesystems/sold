#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | cc -o $t/a.o -c -xc -
int main() {}
EOF

! cc --ld-path=./ld64 -o $t/exe $t/a.o -Wl,-Z > $t/log 2>&1
grep -q 'library not found: -lSystem' $t/log
