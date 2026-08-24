# Third-party notices

CopyToPoints, MultiplyCf, CopyToPointsUSD, ParticlesToUSD and VolumeToUSD are
MIT (see `LICENSE`). They are **plugins**: compiled against, and loaded by,
software this repository does not contain and does not redistribute.

## The binary release ships no third-party code either

Unlike a renderer, these nodes need no libraries of their own - they use the USD
that is already inside Nuke. So the release zip
(`CopyToPoints-<version>-Nuke...-win64.zip`) holds only this project's own DLLs,
its Python and its icons: everything below is true of the zip exactly as it is
true of the repository.

## What this repository does NOT contain

No third-party source or binary is vendored here. In particular there are **no
Nuke headers, libraries or DLLs**: the build links against the Nuke install on
the machine doing the building, and nothing from Foundry is copied into this
repository or into a release of it. A plugin needs a licensed Nuke to build and
to run.

## Build and runtime dependencies

| Component | Licence | How it is used |
|---|---|---|
| **Nuke NDK** (DDImage, Ndk, FdkBase, and the `usg` USD API) | Foundry proprietary, per your Nuke licence | Linked at build time; supplied by your Nuke install. |
| **OpenUSD (pxr)**, via Nuke's `usg` wrapper | Modified Apache-2.0 (Pixar) | The USD nodes author prims through Nuke's own USD; this does not link pxr directly. |
| **OpenVDB** | Mozilla Public License 2.0 | **Not linked and not used.** Nuke ships `openvdb.dll` but no headers, so `VolumeToUSD` reads the `.vdb` header itself with plain file I/O to list grid names and bounds, and otherwise only names the file and grids in the USD stage for a renderer to open. No OpenVDB code is compiled in. |

Being MIT does not grant you any right to Nuke or to any component above; each
remains under its own terms.

## Platform code

The 3D viewer brush polls the mouse through platform APIs - Win32 on Windows,
X11 on Linux, CoreGraphics on macOS - using the systems' own headers. No
third-party wrapper is bundled.

## Icons

`nuke/icons/*.png` are drawn by `tools/make_icons.py` in the InstanceRender
repository. They are stylistically consistent with Nuke's own icon set - a grey
body, dark interior lines, one colour accent - but no Foundry artwork is copied,
traced or included.
