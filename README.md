# sold: A commercial version of the mold linker

The "sold" linker is a commercial version of the
[mold](https://github.com/rui314/mold) linker. Unlike the mold linker whose
license is AGPL, the sold linker is available under an ordinary, per-user,
per-month/year commercial license. sold is a superset of mold; in addition
to all the mold's features, sold supports macOS/iOS.

Please see the [End User Agreement License](LICENSE.md) for the details about
licensing terms. You can purchase a subscription or obtain an evaluation
license at [our website](https://bluewhale.systems).

## How to build

The build instruction for sold is the same as for mold except that you check
out `https://github.com/bluewhalesystems/sold.git` instead of
`https://github.com/rui314/mold.git`. For the details, please see
https://github.com/rui314/mold#how-to-build.

## How to use

The sold linker is installed as `ld.sold` as well as `ld.mold`. Therefore,
the instructions to use sold is the same as the mold linker. For the details,
refer the [mold's README](https://github.com/rui314/mold).
