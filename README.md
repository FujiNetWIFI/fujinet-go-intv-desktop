# fujinet-go-intv-desktop

A self-contained Mattel Intellivision emulator with a built-in FujiNet, based
on [jzIntv](http://spatula-city.org/~im14u2c/intv/) patched with a FujiNet
mailbox peripheral (BoIP -- Bus Over IP -- to a real `fujinet-firmware`
instance bundled with the app). Part of the FujiNet Go Desktop family
alongside `fujinet-go-adam-desktop`, `fujinet-go-apple2-desktop`,
`fujinet-go-coco-desktop`, and `fujinet-go-msx-desktop`.

**Status: early implementation.** See `TODO` for exactly what is built and
verified so far (the headless, SDL-free jzIntv core and ROM-embedding
compliance switch) versus what is still to come (the session library,
debugger, and all four native frontends). There is no working GUI build yet.

## Building the core (what exists today)

```sh
cmake -B build -DFRONTEND=none -DWITH_FUJINET=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

This builds `libjzintv_core.a` -- the vendored, FujiNet-patched jzIntv source
compiled headless (no SDL, no `main()`) -- and a small link-proof test.
`-DWITH_INTV_ROMS=OFF` (the default for anything meant to be redistributed)
builds without embedding Mattel's EXEC/GROM firmware; see `COMPLIANCE.md`.

## License

GPL-3.0-or-later. See `LICENSE` and `COMPLIANCE.md`.
