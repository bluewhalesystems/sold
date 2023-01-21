#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | cc -o $t/a.o -c -xc -
#include <stdio.h>
int main() { printf("Hello world\n"); }
EOF

cc --ld-path=./ld64 -o $t/exe $t/a.o
objdump --macho --indirect-symbols $t/exe | grep -q _printf
