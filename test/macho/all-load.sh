#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | cc -o $t/a.o -c -xc -
int foo = 3;
EOF

cat <<EOF | cc -o $t/b.o -c -xc -
int bar = 5;
EOF

rm -f $t/c.a
ar rc $t/c.a $t/a.o $t/b.o

cat <<EOF | cc -o $t/d.o -c -xc -
int main() {}
EOF

cc --ld-path=./ld64 -o $t/exe $t/d.o -Wl,-all_load $t/c.a
nm $t/exe | grep -q 'D _foo$'
nm $t/exe | grep -q 'D _bar$'
