# CopyToPoints for Nuke

Houdini's *Copy to Points*, for Nuke. Five nodes: two that copy geometry onto
every point of a point cloud, one shader that makes per-copy colour visible, and
two that carry Nuke's own particles and volumes across into its USD 3D system.

Built for the output of **ParticleToGeo** - thousands of rocks driven by
particles, each with its own position, velocity alignment, size, colour and
variant - and for terrain: scattering across faces with painted weights.

This zip is a **compiled Windows x64 release**. Nothing needs building.

---

## Install

Double-click **`install.bat`**, or from PowerShell:

```powershell
.\install.ps1                 # every Nuke version in this zip
.\install.ps1 -Versions 17.1  # just the one you use
```

Then start Nuke. `INSTALL.md` covers custom locations, studio installs and what
to do when a node does not appear.

## The five nodes

| Node | Where it appears | What it does |
|---|---|---|
| **CopyToPoints** | 3D toolbar | Copies geometry onto points. Input 0 is the points, inputs 1-32 are prototypes, and every object on a connected input is a variant. Copies share the prototype's geometry rather than duplicating it, so memory does not grow with how complex the prototype is. |
| **MultiplyCf** | 3D toolbar | Multiplies a material by the geometry's `Cf` colour, because ScanlineRender otherwise ignores per-copy colour. Put it between a texture and the prototype. |
| **CopyToPointsUSD** | USD 3D toolbar | The same idea on Nuke's USD system: one `UsdGeomPointInstancer`, so Hydra and ScanlineRender2 instance it natively. |
| **ParticlesToUSD** | USD 3D toolbar | Nuke's particles as a `UsdGeomPoints` prim with ids, widths, velocities, colour and each particle's birth position. Particle nodes will not connect to a `GeoScene` at all - this is the only way they reach the USD system. |
| **VolumeToUSD** | USD 3D toolbar | A `FieldVolume`'s `.vdb` as a USD `Volume` prim with density and up to two emissive grids. Nuke's field graph authors no Volume prim of its own. |

## Which Nuke

**14.1, 15.2, 16.0, 16.1, 17.0 and 17.1** - Windows x64.

The build has to match the Nuke **minor** version exactly; `COMPATIBILITY.md`
has the matrix, and which nodes exist in which version.

## What you need

* **Nuke** 14.1 or newer, Windows x64.
* **Nothing else.** These nodes use the USD that ships inside Nuke, so there is
  no library to install and nothing to put on your PATH.

## What is in the box

| | |
|---|---|
| `CopyToPoints/` | the plugin: `classic/` and `usd/` builds per Nuke version, icons, and the scripts that load them |
| `examples/` | a ready-made demo graph |
| `INSTALL.md` | installing, uninstalling, and what to do when a node does not appear |
| `COMPATIBILITY.md` | which build for which Nuke, and which nodes each version has |

## Worth knowing

* **Variants**: connect several prototypes and choose between them by
  `sequential`, `random`, or an attribute - the particle `id`, for instance.
* **Painting**: both CopyToPoints nodes have a Paint tab - a viewer brush over
  nine weight layers (density, scale, rotation, variant, scatter, colour) with
  flood fill, occlusion masking and an add/remove-scatter mode.
* **Alignment**: copies can follow point normals, particle velocity or an
  attribute, with per-copy spin and roll.
* **Scattering**: scatter across faces with weighting and separation, seeded so
  a deforming input does not re-scatter underneath you.

Every knob has rollover help, and each tab has its own help popup.

## Licence

CopyToPoints is **MIT** - see `LICENSE`. Use it commercially, modify it, ship it
inside a pipeline; keep the copyright notice.

It is a plugin: it does not contain Nuke and gives you no rights to it. No
third-party binaries are shipped here at all - see `THIRD_PARTY_NOTICES.md`.

## Source

<https://github.com/bratgot/CopyToPoints>
