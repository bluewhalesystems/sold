# sold: A commercial version of the mold linker

The "Sold" linker is a commercial version of the
[mold](https://github.com/rui314/mold) linker. Unlike the mold linker whose
license is AGPL, the Sold linker is available under an ordinary, per-user,
per-month/year commercial license. Sold is a superset of mold; in addition
to all the mold's features, Sold supports macOS/iOS.

Please see the [End User Agreement License](LICENSE.md) for the details about
licensing terms. You can purchase a subscription or obtain an evaluation
license at [our website](https://bluewhale.systems).

## How to build

The build instruction for Sold is the same as for mold except that you check
out `https://github.com/bluewhalesystems/sold.git` instead of
`https://github.com/rui314/mold.git`. For the details, please see
https://github.com/rui314/mold#how-to-build.

## How to use

Sold is installed under several different executable names as follows:

1. `ld.sold`: GNU ld-compatible linker for Linux and other ELF-based systems.
   The instructions to use Sold on Linux is the same as the mold linker.
   For the details, see the [mold's README](https://github.com/rui314/mold).

2. `ld64.sold`: Apple's ld-compatible mode for macOS/iOS. You can use the
   linker on macOS by adding `--ld-path=path/to/ld64.sold` to the linker flags.

3. `ld.mold` and `ld64.mold`: They are just aliases for `ld.sold` and
   `ld64.sold`, respectively, so that you don't have to change the linker name
   in your build system from `mold` to `sold` after purchasing a commercial
   license.
