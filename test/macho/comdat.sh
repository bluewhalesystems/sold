#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | cc -o $t/a.o -c -xc++ -
#include <iostream>
struct T {
  T() { std::cout << "foo "; }
};
T x;
EOF

cat <<EOF | cc -o $t/b.o -c -xc++ -
#include <iostream>
struct T {
  T() { std::cout << "foo "; }
};
T y;
EOF

cat <<EOF | cc -o $t/c.o -c -xc++ -
#include <iostream>
int main() {
  std::cout << "bar\n";
}
EOF

c++ --ld-path=./ld64 -o $t/exe $t/a.o $t/b.o $t/c.o
$t/exe | grep -q '^foo foo bar$'
