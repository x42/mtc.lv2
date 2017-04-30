mtc.lv2 - Midi Timecode Generator
=================================

mtc.lv2 is MIDI Timecode generator.

Install
-------

Compiling mtc.lv2 requires the LV2 SDK, gnu-make, and a c-compiler.

```bash
  git clone git://github.com/x42/mtc.lv2.git
  cd mtc.lv2
  make
  sudo make install PREFIX=/usr
```

Note to packagers: The Makefile honors `PREFIX` and `DESTDIR` variables as well
as `CFLAGS`, `LDFLAGS` and `OPTIMIZATIONS` (additions to `CFLAGS`), also
see the first 10 lines of the Makefile.
