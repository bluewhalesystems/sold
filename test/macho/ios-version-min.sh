#!/bin/bash
. $(dirname $0)/ios.inc

cat <<EOF | $CC -o $t/a.o -c -xc -
int main() {}
EOF

$CC --ld-path=./ld64 --target=$ARCH-apple-ios -o $t/exe1 $t/a.o -mios-version-min=11.2
otool -l $t/exe1 > $t/log
grep -q 'platform 2' $t/log
grep -q 'minos 11.2' $t/log
