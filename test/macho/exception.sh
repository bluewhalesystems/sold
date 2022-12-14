#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | c++ -c -o $t/a.o -xc++ -
int main() {
  try {
    throw 0;
  } catch (int x) {
    return x;
  }
  return 1;
}
EOF

c++ --ld-path=./ld64 -o $t/exe $t/a.o
$t/exe
