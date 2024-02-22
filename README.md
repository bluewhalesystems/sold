# sold: The commercial version of the mold linker

_sold_ is a fork of [mold](https://github.com/rui314/mold) that supports
not only Linux but also macOS/iOS. Originally, _sold_ was available under
a commercial license. Now, it has been relicensed under the MIT license.

## How to build

The build instruction for sold is the same as for mold except that you check
out `https://github.com/bluewhalesystems/sold.git` instead of
`https://github.com/rui314/mold.git`. For the details, please see
https://github.com/rui314/mold#how-to-build.

## How to use

sold is installed under several different executable names as follows:

1. `ld.sold`: GNU ld-compatible linker for Linux and other ELF-based systems.
   The instructions to use sold on Linux is the same as the mold linker.
   For the details, see the [mold's README](https://github.com/rui314/mold).

2. `ld64.sold`: Apple's ld-compatible mode for macOS/iOS. You can use the
   linker on macOS by adding `--ld-path=path/to/ld64.sold` to the linker flags.

3. `ld.mold` and `ld64.mold`: They are just aliases for `ld.sold` and
   `ld64.sold`, respectively, so that you don't have to change the linker name
   in your build system from `mold` to `sold` after purchasing a commercial
   license.
