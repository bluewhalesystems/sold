# -*- mode: sh -*-

# Make sure all commands print out messages in English
export LC_ALL=C

if [ "$ARCH" = "x86_64" ]; then
    exit 0
fi

CC="xcrun --sdk iphoneos cc --target=${ARCH}-apple-ios"
CXX="xcrun --sdk iphoneos c++ --target=${ARCH}-apple-ios"

# Common functions
test_cflags() {
  echo 'int main() {}' | $CC "$@" -o /dev/null -xc - >& /dev/null
}

skip() {
  echo skipped
  trap - EXIT
  exit 0
}

on_error() {
  code=$?
  echo "command failed: $1: $BASH_COMMAND"
  trap - EXIT
  exit $code
}

on_exit() {
  echo OK
  exit 0
}

trap 'on_error $LINENO' ERR
trap on_exit EXIT

# Print out the startup message
testname=$(basename "$0" .sh)
echo -n "Testing $testname ... "
t=out/test/macho/$ARCH/$testname
mkdir -p $t
