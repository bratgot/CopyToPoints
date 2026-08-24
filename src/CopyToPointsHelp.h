// CopyToPointsHelp.h
//
// Popup help for the classic CopyToPoints node: a Python snippet (run by the
// per-tab "help" PyScript buttons) that opens a modeless tabbed dialog with
// HTML help.  Falls back to nuke.message() when PySide is unavailable.
// Strict ASCII.  The C++ side appends "_ctp_help(<tab index>)".

#pragma once

#include <string>

namespace ctp {

const char* const kHelpPy = R"PY(import nuke
def _ctp_help(idx):
    css = "<style>body{font-family:sans-serif;font-size:12px} h2{color:#e8b04a;margin-bottom:2px} h3{color:#9ecbff;margin-bottom:0px} td{padding:2px 8px 2px 0px;vertical-align:top} .k{color:#f4d58d;font-weight:bold}</style>"
    pages = []
    pages.append(("Overview", css + '''
<h2>CopyToPoints</h2><i>Houdini-style copy-to-points for Nuke's classic 3D system - created by Marten Blumen.</i>
<h3>What it does</h3>
Copies (or instances) the geometry connected to the <span class=k>geo</span> inputs onto every point of the
<span class=k>points</span> input: a ParticleToGeo point cloud, any point cloud, or the vertices of a mesh /
Card / terrain. Points can also be <b>scattered</b> across the faces of the points input.
<h3>Typical setups</h3>
<table>
<tr><td class=k>particles</td><td>ParticleEmitter ... ParticleToGeo -&gt; points. Rocks on geo1..geoN. Align = direction (vel), multiply by size, random rotation, copy colour, MultiplyCf on the rock material so Cf shows.</td></tr>
<tr><td class=k>terrain</td><td>Terrain mesh -&gt; points. Copy tab: scatter = add/replace, count, weighting (valleys / peaks / flat / steep), separation. Paint tab: paint density / scale / colour / scatter layers.</td></tr>
<tr><td class=k>guide first</td><td>Turn on 'show guide' (or 'guide only') to see where the copies land before connecting heavy prototypes.</td></tr>
</table>
<h3>Inputs</h3>
<table>
<tr><td class=k>points</td><td>copy targets: point cloud, particle system (ParticleToGeo) or mesh.</td></tr>
<tr><td class=k>geo1..geo32</td><td>prototype geometry. Every object on every geo input is a variant; connect a Scene to bring several objects in on one input.</td></tr>
</table>
<h3>Instances vs bake</h3>
<b>instances</b>: one lightweight object per copy that shares the prototype's point/primitive data through Nuke's
ref-counted caches (memory ~ one prototype + one matrix per copy). <b>bake</b>: real point/primitive copies in one
object per prototype (exportable through WriteGeo). Colour: instances get an object <b>Cf</b>, bake gets point Cf.
<h3>Guard</h3>
If the points input offers more targets than <span class=k>max source points</span> nothing is built and a warning
is shown - protects against accidentally connecting geometry that was meant to be a prototype (every vertex would get a copy).
'''))
    pages.append(("Copy", css + '''
<h2>Copy tab</h2>
<table>
<tr><td class=k>mode</td><td>instances (shared, lightweight) or bake (real copies).</td></tr>
<tr><td class=k>points source</td><td>points: every vertex of the points input is a target. objects: every object of the points input is one target (its origin), e.g. a Scene of nulls / cards.</td></tr>
<tr><td class=k>keep source points</td><td>pass the points input through to the output as well.</td></tr>
<tr><td class=k>max copies</td><td>safety cap on the number of copies (0 = unlimited).</td></tr>
<tr><td class=k>max source points</td><td>guard: more targets than this = nothing is built + warning.</td></tr>
<tr><td class=k>density</td><td>probability that a point receives a copy (seeded, stable per id).</td></tr>
</table>
<h3>Guide geometry</h3>
<span class=k>show guide</span> draws every copy position as a point cloud (or points + axes) with <span class=k>guide size</span>;
it is display-only (render mode off) and never renders. <span class=k>guide only</span> hides the copies so you only see the guide.
<span class=k>guide shows the painted layer</span> colours the guide points with the heat map of the current paint layer at every copy
(scattered points get the interpolated weight), otherwise cyan = vertex copies, orange = scattered.
<h3>Scatter (like Houdini Scatter)</h3>
<table>
<tr><td class=k>scatter</td><td>off / add (vertices + scattered points) / replace (scattered points only).</td></tr>
<tr><td class=k>count, seed</td><td>number of scattered points; stable while the topology is unchanged (points stick to their faces).</td></tr>
<tr><td class=k>weighting</td><td>uniform (by area), prefer flat areas, prefer steep slopes, prefer peaks (high), prefer valleys (low) - judged per face relative to the up vector (Transform tab).</td></tr>
<tr><td class=k>bias</td><td>exponent on the feature: 1 = linear, higher = more concentrated.</td></tr>
<tr><td class=k>multiply by painted scatter layer</td><td>scatter density x (1 + painted 'scatter' weight): -1 = no points there.</td></tr>
<tr><td class=k>separation</td><td>minimum distance between scattered points (Poisson-disc style, 0 = off). If the requested count does not fit the node warns and keeps as many as fit.</td></tr>
<tr><td class=k>deforming geometry</td><td>seeded scatter is stable on a static mesh; on a deforming mesh the area-weighted choice reshuffles. USD node: 'stick to the surface (reference frame shape)' selects on the shape at the reference frame and lets the points ride the deformation; 'topology only' picks faces uniformly (stable, denser on small faces). Classic node: 'stick to deforming geometry (uniform per face)'.</td></tr>
</table>
<h3>Variants</h3>
<span class=k>pick variant</span>: sequential (round robin), random (variant seed) or attribute (an int/float point attribute such as the particle id).
The 'variant' paint layer overrides this when enabled on the Paint tab.
'''))
    pages.append(("Transform", css + '''
<h2>Transform tab</h2>
<h3>Rotation</h3>
<table>
<tr><td class=k>align</td><td>none / direction attribute (vel by default: forward axis of the copy points along it, using the up vector) / quaternion attribute / euler attribute / particle orientation (needs 'read particle system').</td></tr>
<tr><td class=k>rotate</td><td>extra rotation (degrees XYZ) applied to every copy.</td></tr>
<tr><td class=k>random rotate, min/max</td><td>per-copy random rotation range per axis (seeded, stable per point id).</td></tr>
<tr><td class=k>rotation variance</td><td>simple jitter of +/- degrees on every axis.</td></tr>
<tr><td class=k>spin</td><td>roll along velocity: rotate about (up x velocity) by roll rate x distance travelled - tumbling rocks that follow their motion, e.g. after a bounce. Use ParticleBounce 'new channels' + roll channels mask to roll only impacted particles.</td></tr>
</table>
<h3>Scale</h3>
<table>
<tr><td class=k>uniform scale, scale xyz</td><td>base scale of every copy.</td></tr>
<tr><td class=k>multiply by size attribute</td><td>float point attribute (ParticleToGeo: size) multiplied into the scale.</td></tr>
<tr><td class=k>scale attribute (vec3)</td><td>optional per-axis Vector3 attribute.</td></tr>
<tr><td class=k>random scale, min/max</td><td>per-copy random uniform multiplier.</td></tr>
</table>
<h3>Local offset</h3>
<table>
<tr><td class=k>local offset</td><td>offset in the prototype's local space (moves the copy along its own axes, so it follows the alignment).</td></tr>
<tr><td class=k>random offset, min/max</td><td>per-copy random offset range per axis (seeded, stable per id) - e.g. y 0..0.2 sinks/raises rocks a bit differently.</td></tr>
<tr><td class=k>offset variance</td><td>simple +/- jitter per axis on top of the offset.</td></tr>
</table>
<h3>Randomness</h3>
<span class=k>seed</span> drives every random choice; <span class=k>id attribute</span> (default: id) keeps the choices attached to the same particle across frames.
'''))
    pages.append(("Attributes", css + '''
<h2>Attributes tab</h2>
<table>
<tr><td class=k>copy colour, colour attribute</td><td>the source point colour (Cf) is written onto every copy: object attribute in instances mode, point attribute in bake mode. Use <b>MultiplyCf</b> (3D &gt; Shader) between the texture and the prototype so ScanlineRender shows it.</td></tr>
<tr><td class=k>colour variance hue / saturation / value</td><td>per-copy random colour jitter (works from white without a source colour).</td></tr>
<tr><td class=k>copy attributes</td><td>comma separated extra point attributes copied from the source point onto the copy.</td></tr>
<tr><td class=k>read particle system</td><td>when the points input is a particle node, read the particle system directly: id, size, colour, velocity, orientation, initial position, channels ... ParticleToGeo itself only exports id, Cf and size.</td></tr>
<tr><td class=k>dump attributes / refresh list</td><td>writes the attribute layout of the points input to %TEMP%/CopyToPoints_attributes.txt and shows it in the panel.</td></tr>
</table>
'''))
)PY" R"PY(
    pages.append(("Paint", css + '''
<h2>Paint tab - paint weights and colour in the 3D viewer</h2>
Enable painting, keep this panel open, hover the geometry of the <b>points</b> input in a 3D viewer:
<b>LMB drag</b> paints the current layer, <b>Shift+LMB drag</b> resizes the brush, Alt/MMB/RMB navigate as usual.
Weights are stored in the node (saved with the script), keyed by source point index, so they stay valid as long
as the source topology does not change.
<h3>Layers</h3>
<table>
<tr><td colspan=2><i>Every layer is a signed offset from the node's current values (0 = as the knobs say): enable a layer and it starts
from the current value; add raises it, subtract lowers it, set drives it towards 'value', smooth averages.</i></td></tr>
<tr><td class=k>density</td><td>copy probability = density + weight (0..1): density 0 + paint +1 = copies only where painted; density 1 + paint -1 = holes.</td></tr>
<tr><td class=k>scale</td><td>scale x (1 + weight x 'scale per unit weight'): +1 doubles, -0.5 halves.</td></tr>
<tr><td class=k>rotation</td><td>extra rotation of weight x degrees about a local axis.</td></tr>
<tr><td class=k>variant</td><td>shifts the picked prototype by round(weight): +1 = next, -1 = previous (wraps).</td></tr>
<tr><td class=k>scatter</td><td>Copy tab 'multiply by painted scatter layer'. 'painted scatter' = add and remove (default): negative paint thins the base count out, positive paint adds points on top - 'paint adds' = points a +1 stroke over the whole surface adds (scaled by painted area and weight, independent of count, so painting from zero works); remove only: fixed count, the layer can only thin out.</td></tr>
<tr><td class=k>colour</td><td>paints a colour (Cd) with the <b>brush colour</b>: the copies' Cf becomes the painted colour (replace) or source x painted (multiply), blended by the painted coverage. add/set paint the colour, subtract erases, smooth blurs. Optionally also written as Cf onto the kept source points.</td></tr>
</table>
<h3>Brush</h3>
<table>
<tr><td class=k>mode</td><td>add / subtract / set (towards value) / smooth (average).</td></tr>
<tr><td class=k>radius, hardness, opacity, value</td><td>brush size (world units), edge softness, stroke strength, and the amount the brush works with (layers hold -8 .. +8).</td></tr>
<tr><td class=k>occlusion test</td><td>only paint points visible from the camera (nothing behind other faces or on the far side). Off = the whole brush sphere.</td></tr>
<tr><td class=k>brush colour</td><td>colour used by the colour layer (and by flood fill on that layer).</td></tr>
<tr><td class=k>flood fill layer</td><td>sets the whole current layer to 'value' (colour layer: the brush colour with full coverage). Works for every layer, no viewer needed.</td></tr>
<tr><td class=k>clear layer / clear all</td><td>reset the current / every layer.</td></tr>
<tr><td class=k>show weights</td><td>heat map of the current layer: magenta = negative, dark = 0, blue ... red = positive up to heat max (0 = auto); the colour layer is shown in its own colours.</td></tr>
</table>
'''))
    pages.append(("USD node", css + '''
<h2>CopyToPointsUSD (Nuke 15 / 16 / 17, new 3D system)</h2>
Same controls as the classic node, on USD stages. Inputs: <span class=k>points</span> = a stage with Mesh / Points
prims (GeoCard, terrain, GeoImport geometry, point clouds); <span class=k>geo1..geoN</span> = stages whose root
geometry prims become the prototypes (variants). Materials bound upstream (GeoBindMaterial) travel with the
prototypes: the /materials scope is copied to the output at its original path so the bindings resolve.
<h3>mode</h3>
<table>
<tr><td class=k>instances</td><td>one UsdGeomPointInstancer under /&lt;node&gt;/instancer: prototypes stored once, every copy is a
position / orientation / scale entry - the lightest output. Per-copy colour becomes an instance-rate displayColor primvar,
which ScanlineRender2 does not shade (Hydra viewers may).</td></tr>
<tr><td class=k>copies</td><td>every copy is a prim /&lt;node&gt;/copies/copy_N that <i>references</i> the prototype and carries its own
displayColor - source colour, colour variance and the painted colour render everywhere. 'copies share geometry (instanceable)' =
USD scenegraph instancing (packed): one prototype in memory, but per-copy colour is then instance-rate (not shaded by ScanlineRender2).</td></tr>
<tr><td class=k>max copy points</td><td>memory guard: copies x prototype points (millions). ScanlineRender2 un-instances everything it renders
(~350 bytes per point per copy, every mode) - 3000 copies of a 40k-point mesh = 40 GB. Above the limit nothing is built.</td></tr>
</table>
<h3>What is different</h3>
<ul>
<li>guide = Points prim (and BasisCurves for the up axes). <b>guide purpose</b>: <i>default</i> is always visible in the Viewer
but ScanlineRender2 renders it too (turn the guide off before rendering); <i>guide</i> / <i>proxy</i> are hidden by Nuke's Viewer
until 'display guides' / 'display proxy' is on in the Viewer settings (button 'show guides in the viewer').</li>
<li>colours: per-copy colour (source displayColor, colour variance, painted colour) shows in mode <i>copies</i> when the
prototype has <b>no material bound</b> - a UsdPreviewSurface ignores displayColor unless it reads the primvar. In mode
<i>instances</i> the colour is an instance-rate primvar that ScanlineRender2 does not shade.</li>
<li>Attributes tab: 'refresh list' shows the prims and attributes seen on the points input (like the classic node).</li>
<li>hide source geometry authors visibility = invisible on the source prims.</li>
<li>Paint tab: identical brush and layers (density, scale, rotation, variant, scatter, colour, flood fill); the painted colour can
also be written as primvars:displayColor onto the source prims.</li>
<li>no particle-system reading (no classic particles) and no MultiplyCf (displayColor is a first-class primvar).</li>
</ul>
'''))
    try:
        try:
            from PySide6 import QtWidgets, QtCore
        except ImportError:
            from PySide2 import QtWidgets, QtCore
        if not nuke.GUI or not isinstance(QtWidgets.QApplication.instance(), QtWidgets.QApplication):
            raise RuntimeError("no GUI")
        parent = QtWidgets.QApplication.activeWindow()
        dlg = QtWidgets.QDialog(parent)
        dlg.setWindowTitle("CopyToPoints help")
        dlg.resize(780, 660)
        lay = QtWidgets.QVBoxLayout(dlg)
        tabs = QtWidgets.QTabWidget()
        for title, body in pages:
            tb = QtWidgets.QTextBrowser()
            tb.setOpenExternalLinks(True)
            tb.setHtml(body)
            tabs.addTab(tb, title)
        if 0 <= idx < len(pages):
            tabs.setCurrentIndex(idx)
        lay.addWidget(tabs)
        _sb = getattr(QtWidgets.QDialogButtonBox, "StandardButton", QtWidgets.QDialogButtonBox)
        bb = QtWidgets.QDialogButtonBox(_sb.Close)
        bb.rejected.connect(dlg.reject)
        bb.accepted.connect(dlg.accept)
        lay.addWidget(bb)
        _wa = getattr(QtCore.Qt, "WidgetAttribute", QtCore.Qt)
        dlg.setAttribute(_wa.WA_DeleteOnClose)
        dlg.show()
        nuke._ctpHelpDlg = dlg
    except Exception:
        import re
        txt = re.sub(r"<[^>]+>", "", pages[idx][1] if 0 <= idx < len(pages) else pages[0][1])
        nuke.message(txt)
)PY";

// One PyScript command per help page: the snippet above plus a call opening that page
inline const char* helpScript(int page)
{
  static std::string cmds[6];
  static bool built = false;
  if (!built) {
    for (int i = 0; i < 6; ++i) cmds[i] = std::string(kHelpPy) + "\n_ctp_help(" + std::to_string(i) + ")\n";
    built = true;
  }
  return cmds[(page < 0 || page > 5) ? 0 : page].c_str();
}

} // namespace ctp
