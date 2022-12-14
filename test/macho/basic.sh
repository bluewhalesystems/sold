#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | cc -o $t/a.o -c -xc -
int main() {
  return 0;
}
EOF

cc --ld-path=./ld64 -o $t/exe $t/a.o
$t/exe
