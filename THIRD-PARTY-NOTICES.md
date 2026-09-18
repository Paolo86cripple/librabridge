# Third-party notices

## librashader C headers (`ags-patch-files/Engine/gfx/librashader/`)

`librashader.h` and `librashader_ld.h` are vendored unmodified from
https://github.com/SnowflakePowered/librashader (`include/`).

```
SPDX-License-Identifier: MIT
Copyright 2022 chyyran
```

These two header files are MIT-licensed specifically so they can be used in
permissively- or GPL-licensed projects like this one, even though the rest
of librashader (the reflection library and runtimes, i.e. the code that
gets compiled into `librashader.so` by `build_librashader.sh`) is licensed
under the Mozilla Public License 2.0 / GNU GPLv3 (dual-licensed) — see
https://github.com/SnowflakePowered/librashader#license. The GPLv3 option
is compatible with this repo's own GPLv3 license.

## AGS (Adventure Game Studio) engine

`librashader-integration.patch` is a diff against
https://github.com/adventuregamestudio/ags and is meant to be applied to a
clone of that repository. AGS's own license terms apply to the engine
source itself; this repo does not redistribute AGS, only a patch against
it.
