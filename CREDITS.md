# Credits

## Original emulator and backend

- **nukeykt** is the original developer of [Nuked-SC55](https://github.com/nukeykt/Nuked-SC55),
  the SC-55 emulator on which this project is based. Thanks also to the original
  project's contributors for their emulation and hardware-research work.
- **J.C. Moyer / jcmoyer** maintains the [backend fork](https://github.com/jcmoyer/Nuked-SC55)
  used by VSC-55, including reusable-library and optimization work. VSC-55 pins
  release 0.7.0, commit `02f6e3d7bad89af33514bd48211bb950f8ad0e6b`.

## Faceplate and GUI lineage

- **linoshkmalayil / Nuked-SC55-GUI-Float** is the direct source of VSC-55's faceplate:
  [`data/sc55_background.bmp`](https://github.com/linoshkmalayil/Nuked-SC55-GUI-Float/blob/bf4b8319d120814dfbc3a467a578d8ea16a90d08/data/sc55_background.bmp),
  copied at commit `bf4b8319d120814dfbc3a467a578d8ea16a90d08` to
  `assets/frontpanel-float/PANEL.BMP`. The LCD contrast reference also comes from this project.
- **kebufu / mckuhei** is credited for the earlier GUI code in
  [mckuhei/Nuked-SC55](https://github.com/mckuhei/Nuked-SC55), as explicitly
  acknowledged by GUI-Float. That contribution is part of the upstream GUI lineage.
- **Grieferus** is credited for GUI design in GUI-Float's
  [pinned README](https://github.com/linoshkmalayil/Nuked-SC55-GUI-Float/blob/bf4b8319d120814dfbc3a467a578d8ea16a90d08/README.md).

The faceplate comes from the upstream projects credited above. Its separate
license is retained in [LICENSES/Faceplate.txt](LICENSES/Faceplate.txt).

## Tools

- **MinGW Lite**, **GCC**, **mingw-w64**, and **Open Watcom** contributors - build toolchains.

Upstream author notices are retained in the submodule and relevant asset/license files.
