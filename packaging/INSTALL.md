# Installing CopyToPoints

Windows x64. Nuke 14.1, 15.2, 16.0, 16.1, 17.0 or 17.1.

## The quick way

Unpack the zip anywhere and double-click **`install.bat`**.

It copies the plugin to `%USERPROFILE%\.nuke\CopyToPoints` and adds one line to
`%USERPROFILE%\.nuke\init.py` so Nuke looks in that folder. Start Nuke:
CopyToPoints and MultiplyCf are on the **3D** toolbar, CopyToPointsUSD,
ParticlesToUSD and VolumeToUSD on the **USD/3D** one.

## From PowerShell, with options

```powershell
.\install.ps1                              # every build in the zip
.\install.ps1 -Versions 17.1               # only Nuke 17.1
.\install.ps1 -Versions 16.1,17.1          # two of them
.\install.ps1 -Prefix D:\studio\nuke       # somewhere other than ~/.nuke
```

The whole plugin is a few megabytes, so installing all six versions costs
almost nothing - `-Versions` is there for tidiness rather than space.

**`-Prefix`** installs somewhere else, for a shared or per-project location.
That folder must be on Nuke's plugin path: set `NUKE_PATH` to it, or add
`nuke.pluginAddPath('...')` to an `init.py` Nuke already reads.

## Installing by hand

1. Copy the `CopyToPoints` folder from this zip into `%USERPROFILE%\.nuke\`.
2. Add this to `%USERPROFILE%\.nuke\init.py` (create it if it is not there):

   ```python
   import nuke
   nuke.pluginAddPath('./CopyToPoints')
   ```

There is nothing else to place - no libraries, no PATH entries.

## Uninstalling

```powershell
.\uninstall.ps1
```

It removes the plugin folder and takes out only its own block from `init.py`;
anything else in that file is left alone.

## When a node does not appear

Nuke prints the reason. Open the **Script Editor**, or start Nuke from a console
and read the output as it starts up.

**Nothing is printed at all**
Nuke is not reading the folder. Check that `%USERPROFILE%\.nuke\init.py`
contains the `pluginAddPath` line above, and that `%USERPROFILE%\.nuke` is where
Nuke actually looks - if `NUKE_PATH` is set it may be reading elsewhere.

**"The specified procedure could not be found"**
A build for the wrong Nuke minor version. In Nuke 17.1 this failure aborts
start-up entirely, so it is worth being precise: install the matching build with
`.\install.ps1 -Versions <your version>`. See `COMPATIBILITY.md`.

**The USD nodes are missing but the classic ones are there (or the reverse)**
The two sets are installed separately, under `classic\` and `usd\`. Check both
folders exist for your version inside `%USERPROFILE%\.nuke\CopyToPoints`.

**Particles will not connect to CopyToPointsUSD**
They cannot - Nuke's particles are classic 3D and will not connect to a
`GeoScene` at all. That is what **ParticlesToUSD** is for: put it between the
particles and the USD graph.

**VolumeToUSD produces no volume**
Check `COMPATIBILITY.md` - Nuke 14.1's USD has no `usdVol` schema at all, so
there is nothing for it to author there.

## Diagnostics

The plugin's `init.py` has a `CTP_DIAG_DIR` setting near the top, empty by
default. Point it at a folder and the plugin writes a trace log there and copies
any Nuke crash dumps out of `%TEMP%` beside it - useful when reporting a problem,
since Nuke otherwise leaves those where they are hard to find.

## Studio install

Put the plugin folder on a share, point `NUKE_PATH` at its parent:

```
set NUKE_PATH=\share\nuke_plugins
```

laid out as `\share\nuke_plugins\CopyToPoints\...`. The plugin's own `init.py`
picks the build matching whichever Nuke starts, so one install serves every
version at once.
