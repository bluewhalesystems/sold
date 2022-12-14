#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | cc -o $t/a.o -c -xc - -fmodules
#include <zlib.h>
int main() {}
EOF

cc --ld-path=./ld64 -o $t/exe $t/a.o
