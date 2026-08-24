# CopyToPoints for Nuke 14.1 / 15 / 16 / 17 — Installation

Package contents:

```
CopyToPoints-<version>-Nuke14.1-win64/
|-- INSTALL.md              this file
|-- install.bat             double-click installer (runs install.ps1)
|-- install.ps1             PowerShell installer / uninstaller
`-- CopyToPoints/           the plugin folder that goes into ~/.nuke
    |-- classic/nuke14/     CopyToPoints.dll + MultiplyCf.dll  (Nuke 14.1, classic 3D)
    |-- usd/nuke15/         CopyToPointsUSD.dll               (Nuke 15.2, USD PointInstancer)
    |-- usd/nuke16/         CopyToPointsUSD.dll               (Nuke 16.0)
    |-- usd/nuke17/         CopyToPointsUSD.dll               (Nuke 17.0)
    |-- init.py, menu.py    plugin-folder scripts: pick the build for the running Nuke, toolbar entries
    |-- README.md           full node documentation
    |-- VERSION.txt
    `-- examples/CopyToPoints_example.nk
```

Requirements: **Windows x64**, Nuke 14.1 (classic node) and/or Nuke 15.2 /
16.0 / 17.0 (USD node). Each DLL is compiled against its own major version's
NDK; `init.py` only loads the matching one, so installing all is safe.

## Automatic install (recommended)

1. Close Nuke (it locks loaded plugin DLLs).
2. Unzip the package anywhere.
3. Double-click **install.bat** (or run `.\install.ps1` in PowerShell).

The installer copies `CopyToPoints\` to `%USERPROFILE%\.nuke\CopyToPoints\`
and appends this block to `%USERPROFILE%\.nuke\init.py` (only once):

```python
# --- CopyToPoints (auto-added by install.ps1) ---
import nuke
nuke.pluginAddPath('./CopyToPoints')
# --- end CopyToPoints ---
```

Start Nuke. In 14.1 the nodes are under **3D > Modify > CopyToPoints** and
**3D > Shader > MultiplyCf**; in 15/16/17 under **3D > Modify >
CopyToPointsUSD** (or press Tab and type the name). Open `~/.nuke/CopyToPoints/examples/CopyToPoints_example.nk` for a
working setup.

Uninstall: `.\install.ps1 -Uninstall` (removes the folder and the init.py
block), or delete the folder and the block by hand.

Install into a different plugin location (studio share, per-project .nuke):
`.\install.ps1 -NukeHome D:\path\to\nuke_plugins` — or copy the
`CopyToPoints` folder anywhere and add `nuke.pluginAddPath('<path>/CopyToPoints')`
to any `init.py` on your NUKE_PATH.

## Manual install

1. Copy the `CopyToPoints` folder into `%USERPROFILE%\.nuke\`.
2. Add `nuke.pluginAddPath('./CopyToPoints')` to `%USERPROFILE%\.nuke\init.py`
   (create the file if it does not exist).
3. Restart Nuke.

## Quick start

```
ParticleEmitter -> ParticleGravity -> ParticleToGeo ----------------.
                                                                     v
CheckerBoard -> MultiplyCf -> Sphere ---- geo1 ---> CopyToPoints -> ScanlineRender
                          `-> Cube   ---- geo2 --->     ^
                                                        `-- points input 0
```

* `points` = the point cloud (ParticleToGeo, or a particle node directly, or
  any geometry); `geo1..geoN` = prototype geometry, every object = a variant.
* Rotation from the particles: on the ParticleEmitter set **rotation
  velocity**, on CopyToPoints set **align = particle orientation**. Extra
  rolling: **spin = roll along velocity**.
* Per-particle colour on the copies: put **MultiplyCf** between the texture
  and the prototype geometry.
* Look at the "attributes found on the inputs" list (Attributes tab) to see
  every attribute the particle system offers.
* Terrain: enable painting on the **Paint** tab and paint density / scale /
  rotation / variant weights straight onto the mesh in the 3D viewer.

Full documentation of every knob, the two output modes (instances / bake),
particle attributes and rotation recipes: `CopyToPoints/README.md`.

## Troubleshooting

* "plugin did not define CopyToPoints" / node not found: the folder is not on
  the plugin path — check the init.py block, restart Nuke.
* Copies render but are all white/grey although particles are coloured: use
  MultiplyCf as the material (ScanlineRender ignores Cf with plain textures).
* Nothing renders: connect at least one geo input; check the node's info (i)
  or the attribute list for "no prototype geometry".
* Viewer sluggish with very many copies in `instances` mode: set the node's
  `display` to bounding box, or switch `mode` to `bake`.
* Rebuilding/overwriting the DLLs fails: close Nuke first (file lock).

## Debug log / crash reports - one folder for everything

Open `~/.nuke/CopyToPoints/init.py` and set the folder at the top:

```python
CTP_DIAG_DIR = r"C:/temp/nuke_diag"        # any folder you like; "" = off
```

Restart Nuke. From then on that folder collects

* `CopyToPoints_log.txt` - a flushed trace of every build of both nodes (engine
  steps, guards, brush strokes); after a crash the last line shows where it died,
* `crashdumps/` - Nuke's own crash dumps (`*.dmp` / `*.crash`) copied out of the
  temp folder (`%TEMP%` and `%TEMP%\nuke`) at every Nuke start, so they are not
  lost when the temp folder is cleaned,
* `sessions.txt` - one line per Nuke start (version, time, pid).

Nuke itself keeps writing its dumps to `%TEMP%\nuke` (only the environment
variable `NUKE_TEMP_DIR`, set before Nuke starts, moves them). Alternatively set
just the trace log through the environment: `setx CTP_LOG C:\temp\ctp.log`
(Windows) or `export CTP_LOG=~/ctp.log` (Linux/macOS) - `CTP_DIAG_DIR` overrides
it. After a crash send: the tail of `CopyToPoints_log.txt`, the newest file in
`crashdumps/`, and the `.nk` (or its `.nk.autosave`). The log grows across
sessions - delete it when it gets big.
