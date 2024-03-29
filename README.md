# sold linker

_sold_ is a fork of [mold](https://github.com/rui314/mold) that supports
not only Linux but also macOS/iOS. Originally, _sold_ was available under
a commercial license. Now, it has been relicensed under the MIT license.

## How to build

### macOS

```shell
git clone --branch stable https://github.com/bluewhalesystems/sold.git
cd sold
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=c++ -B build
cmake --build build -j$(nproc)
sudo cmake --build build --target install
```

### Linux

Building on Linux is mostly the same as on macOS, with the added step of running the `install-build-deps.sh` script.

```shell
git clone --branch stable https://github.com/bluewhalesystems/sold.git
cd sold
./install-build-deps.sh
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=c++ -B build
cmake --build build -j$(nproc)
sudo cmake --build build --target install
```

For more detailed information, please see the [mold build instructions](https://.github.com/rui314/mold#how-to-build).
## How to use

sold is installed under several different executable names as follows:

1. `ld.sold`: GNU ld-compatible linker for Linux and other ELF-based systems.
   The instructions to use sold on Linux is the same as the mold linker.
   For the details, see the [mold's README](https://github.com/rui314/mold).

2. `ld64.sold`: Apple's ld-compatible mode for macOS/iOS. You can use the
   linker on macOS by adding `--ld-path=path/to/ld64.sold` to the linker flags.

3. `ld.mold` and `ld64.mold`: They are just aliases for `ld.sold` and
   `ld64.sold`, respectively.
