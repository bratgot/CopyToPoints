# Building CopyToPoints

## Windows (tested)
`build.ps1` — Nuke 14 / 15 use the Visual Studio 2019 toolset, 16 / 17 VS 2022:
```powershell
.\build.ps1                    # classic Nuke 14.1 plugins   -> build\Release
.\build.ps1 -Usd               # + CopyToPointsUSD for every Nuke 15/16/17 found (build-usd15|16|17)
.\build.ps1 -Usd -Install      # + install into %USERPROFILE%\.nuke\CopyToPoints
.\package.ps1                  # dist\CopyToPoints-<version>-Nuke14-17-win64.zip
```

## Linux / macOS (compiles from the same sources; not run by the author)
```bash
./build.sh --nuke /usr/local/Nuke14.1v8 --install          # classic
./build.sh --usd /usr/local/Nuke17.0v3 --install           # USD node
```
* Use Foundry's documented compiler for the Nuke version (Linux: gcc 9 for Nuke 14,
  gcc 11 for 15–17; macOS: the Xcode clang of that release; universal / arm64 builds
  need the matching Nuke build).
* Platform-specific code is confined to `src/PaintBrushKnob.h` (the viewer brush polls
  the mouse: Win32 on Windows, X11 `XQueryPointer` on the current GLX drawable on Linux,
  CoreGraphics button/modifier state + Nuke's viewer mouse coordinates on macOS) and to
  `ctpLogPath()`. Everything else is portable C++17 + the NDK.
* Linux links X11 and GL (found by CMake), macOS the OpenGL and ApplicationServices
  frameworks. Wayland sessions: XQueryPointer needs XWayland (Nuke runs under XWayland).
* Install layout is the same as on Windows: `~/.nuke/CopyToPoints/{classic/nuke14, usd/nuke15|16|17}`
  with `init.py` picking the build; on Linux the plugin files are `.so`, on macOS `.dylib`
  (`add_nuke_plugin` sets the right suffix, `init.py` only adds directories).
* Test scripts run the same way: `Nuke14.1 -t -i test/copytopoints_test.py`,
  `Nuke17.0 -t -i test/copytopoints_usd_test.py <plugin_dir> <out_dir>`.

If the brush does not paint on Linux/macOS, check `CTP_LOG=<file>` output for
`brush:state lmb=... inside=... hit=...` lines: `inside=0` means the mouse coordinates
are wrong for that platform, `lmb=0` while pressing means the button poll failed.
