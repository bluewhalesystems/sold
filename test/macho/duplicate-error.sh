#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | cc -o $t/a.o -c -xc -
void hello() {}
EOF

cat <<EOF | cc -o $t/b.o -c -xc -
void hello() {}
int main() {}
EOF

! cc --ld-path=./ld64 -o $t/exe $t/a.o $t/b.o 2> $t/log || false
grep -q 'duplicate symbol: .*/b.o: .*/a.o: _hello' $t/log
