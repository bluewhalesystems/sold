#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF > $t/a.c
#include <stdio.h>
extern char *msg;
void hello() { printf("Hello world\n"); }
EOF

$CC -o $t/a.o -c -g $t/a.c

cat <<EOF > $t/b.c
char *msg = "Hello world\n";
void hello();
int main() { hello(); }
EOF

$CC -o $t/b.o -c -g $t/b.c

rm -f $t/c.a
ar cru $t/c.a $t/b.o

$CC --ld-path=./ld64 -o $t/exe $t/a.o $t/c.a -g

$t/exe | grep -q 'Hello world'

lldb --batch -o 'b main' -o run -o list $t/exe | \
  grep -Eq '^-> 3\s+int main\(\) { hello\(\); }'

lldb --batch -o 'b hello' -o run -o list $t/exe | \
  grep -Eq '^-> 3\s+void hello\(\) { printf\("Hello world\\n"\); }'
