# Compliance

## Licensing

This project links jzIntv (GPL-2.0-or-later, Copyright (c) 1998-2020 Joseph
Zbiciak and contributors) into a combined work distributed under
GPL-3.0-or-later. See `LICENSE`, and `tools/jzintv/jzintv-fujinet.patch`'s
header for provenance of the FujiNet mailbox peripheral patch applied on top
(itself GPL, from the FujiNet project).

jzIntv's own upstream: <http://spatula-city.org/~im14u2c/intv/>. This
repository vendors the `20200712` source release by URL + SHA256
(`cmake/Dependencies.cmake`); see `cmake/StageJzIntv.cmake` for how the
FujiNet patch is applied on top of the pristine download.

## EXEC and GROM

`exec.bin` (the Intellivision's system ROM / EXEC) and `grom.bin` (the
Graphics ROM / GROM) are Mattel Electronics copyrighted firmware. They are
**not** freely licensed and this project does not have redistribution rights
to them.

- `-DWITH_INTV_ROMS=ON` (the default for a local development build) embeds
  both images from `tools/jzintv/roms/` into the binary via
  `tools/jzintv/embed-roms.py`, so a checkout boots straight to a live
  Intellivision (or, with no cartridge, the FujiNet config ROM) without any
  extra setup. **A binary built this way must not be redistributed.**
- `-DWITH_INTV_ROMS=OFF` (what every artifact this repository publishes is
  built with) embeds neither image. The app reads them from the user's own
  ROM directory at run time, provisioned through the "Import System ROMs..."
  path once a milestone implementing it lands. `core/tests/no_embedded_roms.py`
  is registered as a ctest against every built frontend executable (plus
  `boot_smoke`, once it exists) and fails the build if a distinctive slice of
  either image is found in the shipped binary -- see that script's own header
  for how the check works and why it scans the whole file rather than a fixed
  offset.

If you build with `WITH_INTV_ROMS=ON` for your own local testing, do not
distribute the resulting binary, package, `.deb`/`.rpm`, flatpak, `.app`
bundle, or installer to anyone else.

## FujiNet runtime

The bundled FujiNet runtime (`cmake/FujiNetRuntime.cmake`, not yet
implemented -- see the project plan's M3 milestone) is built from
[fujinet-firmware](https://github.com/FujiNetWIFI/fujinet-firmware)
(GPL-3.0-or-later), pinned by commit in `cmake/Dependencies.cmake`.
