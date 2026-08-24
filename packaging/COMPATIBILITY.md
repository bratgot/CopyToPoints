# Compatibility

Windows x64. Nuke 14.1 through 17.1.

## The build must match the Nuke minor version exactly

There is one build per Nuke **minor** version and they are not interchangeable:

| Nuke | folders in this zip |
|---|---|
| 14.1 | `classic\nuke14.1`, `usd\nuke14.1` |
| 15.2 | `classic\nuke15.2`, `usd\nuke15.2` |
| 16.0 | `classic\nuke16.0`, `usd\nuke16.0` |
| 16.1 | `classic\nuke16.1`, `usd\nuke16.1` |
| 17.0 | `classic\nuke17.0`, `usd\nuke17.0` |
| 17.1 | `classic\nuke17.1`, `usd\nuke17.1` |

Nuke's plugin ABI changes between minor versions: a 16.0 build will not load in
16.1, and a 17.0 build will not load in 17.1. Nuke reports *"The specified
procedure could not be found"* - and in **17.1 that failure aborts start-up
entirely**, which is worth knowing before you copy a folder by hand.

The plugin's `init.py` prefers the exact `nuke<major>.<minor>` folder and falls
back to a `nuke<major>` one if that is all there is, so older layouts keep
working.

**Patch versions do not matter.** `Nuke17.1v1`, `v2` and `v3` all use the
`nuke17.1` build.

## Which nodes exist in which version

| | 14.1 | 15.2 | 16.0 | 16.1 | 17.0 | 17.1 |
|---|:--:|:--:|:--:|:--:|:--:|:--:|
| **CopyToPoints** (classic 3D) | yes | yes | yes | yes | yes | yes |
| **MultiplyCf** (classic 3D) | yes | yes | yes | yes | yes | yes |
| **CopyToPointsUSD** | preview | yes | yes | yes | yes | yes |
| **ParticlesToUSD** | yes | yes | yes | yes | yes | yes |
| **VolumeToUSD** | no | see below | see below | yes | yes | yes |

**CopyToPointsUSD on 14.1** works against that version's *preview* of the new 3D
system. It is usable, but 14.1's USD support is itself a preview and the classic
`CopyToPoints` is the better choice there.

**VolumeToUSD needs `usdVol`**, the USD schema for volume prims, which comes
from Nuke's own USD rather than from this plugin:

* **Nuke 14.1 has no `usdVol` at all** - neither headers nor library. The node
  loads but there is no volume schema for it to author into, so it cannot work.
  It is shipped in that folder only because it is built from the same source;
  treat 14.1 as unsupported for this node.
* **15.2 and 16.0 do ship `usdVol`**, so the schema is there. The node is
  documented as supported from **16.1**, which is the version it was developed
  and tested against - it may well work earlier, but that is not a claim this
  release makes.
* **16.1, 17.0 and 17.1** are the tested versions.

## The classic 3D system in newer Nuke

`CopyToPoints` and `MultiplyCf` use Nuke's original 3D system, which still
exists in 16 and 17 alongside the USD one, and they are built and shipped for
every version here. If your graph is USD-based, `CopyToPointsUSD` is the node
you want - it emits a `UsdGeomPointInstancer` that Hydra and ScanlineRender2
instance natively.

## Particles

Nuke's particle nodes are classic 3D and **will not connect to a `GeoScene`**.
That is a limit of Nuke, not of this plugin. `ParticlesToUSD` is the bridge: it
turns particles into a `UsdGeomPoints` prim carrying ids, widths, velocities,
colour and birth positions, which the USD graph can then use.

## Operating system

**Windows x64 only, in this zip.** The source builds on Linux (gcc 9 for Nuke
14, gcc 11 for 15 to 17) and the CMake is set up for it, but no Linux binary is
shipped and none has been run - build from source if you need one:
<https://github.com/bratgot/CopyToPoints>

## Third-party libraries

**None are shipped.** These nodes link against the USD that is already inside
your Nuke, which is exactly why the build has to match the Nuke version so
closely. See `THIRD_PARTY_NOTICES.md`.
