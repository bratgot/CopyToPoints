# CopyToPoints plugin folder menu.py -- toolbar registration (only for the
# builds that exist for this Nuke version, see init.py).
import os
import nuke

_here = os.path.dirname(os.path.abspath(__file__))
_major = nuke.NUKE_VERSION_MAJOR
_minor = nuke.NUKE_VERSION_MINOR
_toolbar = nuke.menu("Nodes")
_geo_menu = _toolbar.findItem("3D") or _toolbar.addMenu("3D")


def _have(kind):
    """Is there a build of this flavour for THIS Nuke?

    The same rule init.py uses, and it has to BE the same rule: init.py adds the
    plugin path for nuke<major>.<minor> and falls back to nuke<major>. A menu that
    only looked for the major-only folder would leave the node loadable but
    missing from the toolbar the moment a per-minor folder appeared - which is
    what happens for every version past 14.
    """
    for name in ("nuke%d.%d" % (_major, _minor), "nuke%d" % _major):
        if os.path.isdir(os.path.join(_here, kind, name)):
            return True
    return False


if _have("classic"):
    _modify = _geo_menu.findItem("Modify") or _geo_menu.addMenu("Modify")
    _modify.addCommand("CopyToPoints", "nuke.createNode('CopyToPoints')",
                       icon="CopyToPoints.png",
                       tooltip="Copy / instance geometry onto every point of a point cloud "
                               "(e.g. the output of ParticleToGeo). Classic 3D system.")
    _shader = _geo_menu.findItem("Shader") or _geo_menu.addMenu("Shader")
    _shader.addCommand("MultiplyCf", "nuke.createNode('MultiplyCf')",
                       icon="MultiplyCf.png",
                       tooltip="Multiply a texture/material by the geometry Cf colour so per-copy "
                               "colour shows in ScanlineRender.")

if _have("usd"):
    _modify = _geo_menu.findItem("Modify") or _geo_menu.addMenu("Modify")
    _modify.addCommand("ParticlesToUSD", "nuke.createNode('ParticlesToUSD')",
                       icon="ParticlesToUSD.png",
                       tooltip="Classic Nuke particles (or any classic point cloud) as a USD "
                               "Points prim, so CopyToPointsUSD and the rest of the new 3D "
                               "system can use them. The two systems will not connect "
                               "otherwise: every particle node is classic 3D.")
    _modify.addCommand("CopyToPointsUSD", "nuke.createNode('CopyToPointsUSD')",
                       icon="CopyToPointsUSD.png",
                       tooltip="USD PointInstancer copy-to-points for the new 3D system "
                               "(instances prototypes onto points / scattered faces).")
    _modify.addCommand("VolumeToUSD", "nuke.createNode('VolumeToUSD')",
                       icon="VolumeToUSD.png",
                       tooltip="A FieldVolume's .vdb as a USD Volume prim, so the new 3D system "
                               "and InstanceRender can see it. Nuke's field graph does not reach "
                               "the USD stage otherwise: GeoFieldMesh gives an isosurface and "
                               "GeoFieldSet leaves the stage unchanged.")
