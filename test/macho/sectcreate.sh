#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | cc -o $t/a.o -c -xc -
int main() {}
EOF

echo 'foobar' > $t/contents

cc --ld-path=./ld64 -o $t/exe $t/a.o -Wl,-sectcreate,__TEXT,__foo,$t/contents

otool -l $t/exe | grep -A3 'sectname __foo' > $t/log
grep -q 'segname __TEXT' $t/log
grep -q 'segname __TEXT' $t/log
grep -q 'size 0x0*7$' $t/log
