#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF > $t/a.c
int main() {}
EOF

$CC -o $t/a.o -c -g $t/a.c

$CC --ld-path=./ld64 -o $t/exe $t/a.o -g
nm -a $t/exe | grep -Eq 'OSO .*debuginfo-sourcename/a.o'
