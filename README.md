# sold: A commercial version of the mold linker

The "sold" linker is a commercial version of the
[mold](https://github.com/rui314/mold) linker. Unlike the mold linker whose
license is AGPL, the sold linker is available under an ordinary, per-user,
per-month/year commercial license. The sold linker is the best option for
organizations who don't want to use AGPL'ed programs.

Except for the product names and their licenses, sold and mold are the same
program.

Please see the [End User Agreement License](LICENSE.md) for the details about
licensing terms. You can purchase a subscription or obtain an evaluation
license at [our website](https://bluewhale.systems).

## How to build

sold is written in C++20, so if you build sold yourself, you need a
recent version of a C++ compiler and a C++ standard library. GCC 10.2
or Clang 12.0.0 (or later) as well as libstdc++ 10 or libc++ 7 (or
later) are recommended.

### Install dependencies

To install build dependencies, run `./install-build-deps.sh` in this
directory. It recognizes your Linux distribution and tries to install
necessary packages. You may want to run it as root.

### Compile sold

```shell
git clone https://github.com/bluewhalesystems/sold.git
mkdir sold/build
cd sold/build
git checkout v1.7.0
../install-build-deps.sh
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=c++ ..
cmake --build . -j $(nproc)
sudo cmake --install .
```

You may need to pass a C++20 compiler command name to `cmake`.
In the above case, `c++` is passed. If it doesn't work for you,
try a specific version of a compiler such as `g++-10` or `clang++-12`.

By default, `sold` is installed to `/usr/local/bin`. You can change
that by passing `-DCMAKE_INSTALL_PREFIX=<directory>`. For other cmake
options, see the comments in `CMakeLists.txt`.

## How to use

The sold linker is installed as `ld.sold` as well as `ld.mold`. Therefore,
the instructions to use sold is the same as the mold linker. For the details,
refer the [mold's README](https://github.com/rui314/mold).
