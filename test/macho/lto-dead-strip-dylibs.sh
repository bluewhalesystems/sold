#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | c++ -o $t/a.o -c -xc++ - -flto
#include <iostream>
int main() {
  std::cout << "Hello world\n";
}
EOF

c++ --ld-path=./ld64 -o $t/exe $t/a.o -flto -dead_strip_dylibs
$t/exe | grep -q 'Hello world'
