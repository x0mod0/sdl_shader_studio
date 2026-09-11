# Application icon

Two renders of the same mark. Which one a platform gets depends on who draws
the rounding:

- `icon-16.png` … `icon-1024.png` — rounded (macOS squircle / freedesktop),
  transparent corners. Used for the macOS `.icns` and the Linux hicolor theme,
  both of which show the image as-is.
- `square/icon-*.png` — full bleed, no rounding. Used for `app.ico`, because
  Windows masks the icon itself in the taskbar and title bar.

`app.ico` is generated and committed so a Windows build needs no image tooling.
Regenerate it whenever `square/` changes:

```sh
python3 ../../tools/make_ico.py
```

The macOS `.icns` is *not* committed — `cmake/AppIcon.cmake` builds it from the
rounded PNGs on every build, so editing the artwork is enough. `app.rc` is the
Windows resource script that links `app.ico` into the executable.
