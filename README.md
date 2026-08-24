# CopyToPoints for Nuke (14.1 classic 3D, 15+ USD)

Five nodes: two that copy geometry onto points, one shader, and two that carry
Nuke's own particles and fields across into the USD 3D system.

Native NDK plugins that copy / instance geometry onto every point of a point
cloud — Houdini's *Copy to Points* for Nuke. Built for the output of
**ParticleToGeo** (particles drive thousands of rocks with per-particle
position, velocity alignment, size, colour and variant selection) and for
terrain (scatter across faces, painted weights).

| Node | Nuke | What it is |
|---|---|---|
| **CopyToPoints** | 14.1 (classic 3D) | `GeoOp`. Input 0 = points, inputs 1..32 = prototype geometry (variants). Instances share the prototype geometry through Nuke's ref-counted caches; bake mode makes real copies. Viewer painting of weight layers. |
| **MultiplyCf**   | 14.1 (classic 3D) | `Material`. Multiplies a texture/material by the geometry `Cf` colour so per-copy colour is visible in ScanlineRender (the built-in renderer otherwise ignores `Cf`). |
| **CopyToPointsUSD** | 14.1 (new-3D preview), 15.2 / 16.0 / 16.1 / 17.0 / 17.1 | `GeomOp`. Same idea on the new 3D system: emits one **UsdGeomPointInstancer** — the prototypes are copied once under the instancer, every copy is a position/orientation/scale entry, and Hydra / ScanlineRender2 instance natively. |
| **ParticlesToUSD** | 14.1+ | `GeomOp`. Nuke's particles as a `UsdGeomPoints` prim, carrying ids, widths, velocities, colour, and each particle's birth position and channels. Particle nodes will not connect to a `GeoScene` at all, so this is the only way they reach the USD system. |
| **VolumeToUSD** | 16.1+ (needs `usdVol`) | `GeomOp`. A `FieldVolume`'s `.vdb` as a USD `Volume` prim, with the density and up to two emissive grids. Nuke's field graph authors no Volume prim of its own — `GeoFieldMesh` gives an isosurface and `GeoFieldSet` leaves the stage unchanged. |

Built and runtime-tested against **Nuke 14.1v8, 15.2v9, 16.0v8, 16.1v1, 17.0v4
and 17.1v1 on Windows** (VS 2019 for 14/15, VS 2022 for 16/17). One build per
Nuke MINOR version, because the NDK is not compatible across them; the plugin
folder's `init.py` picks the one matching the running Nuke.

---

## Quick start

```
ParticleEmitter -> (forces) -> ParticleToGeo ---------------------.
                                                                   v
CheckerBoard -> MultiplyCf -> Sphere  ---- geo1 --->  CopyToPoints  -> ScanlineRender
                          `-> Cube    ---- geo2 --->      ^
                                                          `-- points input 0
```

1. `3D > Modify > CopyToPoints`. Connect the ParticleToGeo (or any geometry
   whose points you want to use) to **points**.
2. Connect one or more prototype geometries to **geo1, geo2, ...**. Every
   object on every connected geo input becomes a *variant* (a Scene node
   with several objects also works as one input).
3. Pick how variants are chosen (`sequential`, `random`, `attribute` e.g. the
   particle `id`), how copies are oriented (`align`) and scaled.
4. If you want the particle colour on the copies, put `MultiplyCf` between the
   texture and the prototype geometry (or use ApplyMaterial downstream).

`examples/CopyToPoints_example.nk` is a ready-made demo graph.

## Output modes

* **instances (shared, lightweight)** — default. One output object per point.
  Each object *references* the prototype's point list, primitive list and
  attributes (Nuke's ref-counted geometry caches); it only carries its own
  4x4 matrix. Memory does not grow with the prototype's complexity, and
  materials assigned upstream of the geo inputs are kept per copy. The source
  point colour is attached as an **object** attribute `Cf`; extra source
  attributes (`copy attributes`) become object attributes too.
* **bake (real copies)** — one output object per prototype containing real
  point/primitive copies for all its instances (points, vertex/point/primitive
  attributes replicated, normals rotated). Heavier, but it is what you want for
  WriteGeo export or if a downstream node struggles with tens of thousands of
  objects. Colour becomes a **point** attribute `Cf`.

Both modes render identically (verified pixel-for-pixel in the tests).

## Knobs

The panel is organised in tabs: **Copy** (output + variants), **Transform**
(rotation, scale, randomness), **Attributes** (colour, particle system, the
attribute list), **Paint** (viewer painting) and **About**.

**output** — `mode`; `copy onto`: *auto* (every point of point clouds and
meshes — terrain, cards, ReadGeo — except when the points input is a particle
node whose particles are geometry: then each particle mesh counts as ONE
target at its centre, which protects against copying onto every vertex of
every particle), *every point* (always all points), *one per object*; `keep source points`; `max instances` (0 = unlimited); `max source
points` (guard, default 250k: above it nothing is built and the node shows a
warning — Nuke's per-object cost grows faster than linearly, ~100k copies take
10 s, 300k minutes; prefer bake mode for huge counts); `density`.

**guide geometry (viewer only)** — `guide`: *off* / *copy positions (points)* /
*positions + up axis lines*; `axis length`; `hide the copies (guide only)`.
Shows where the copies go as a point cloud (orange = scattered points, cyan =
source vertices/particles) plus optional lines along each copy's local up axis
(orientation and scale at a glance). Guide objects have render mode off, so
ScanlineRender never sees them; with *hide the copies* you can judge a scatter
or a paint job without any prototype connected.

**scatter points on the geometry** — `scatter`: *off* / *add to the points* /
*replace the points*; `count`, `scatter seed`. Like Houdini's Scatter node:
copy targets are generated anywhere on the faces of the points input
(area-weighted, random position per face) instead of only on the vertices —
a much more natural distribution on terrain. Scattered points stick to their
faces (stable per seed), carry the **face normal** (use `align = direction
attribute` with attribute `N` to stand copies up on the surface), the
interpolated source colour, and the **interpolated painted weights**, and
they count towards `max source points`.
`weighting`: *uniform (by area)*, *prefer flat areas* / *prefer steep slopes*
(by the surface gradient relative to the up vector), *prefer peaks (high)* /
*prefer valleys (low)* (by height along up), shaped by `bias` (exponent).
`multiply by painted 'scatter' layer`: paint a **scatter** layer on the Paint
tab and the scattered density follows it (rejection sampled per point, so it
is smooth inside big faces). `separation`: minimum distance between scattered
points (Poisson-disc style non-collision, deterministic); when the surface
cannot hold the count at that distance fewer points are made — the node info
and the attribute dump report `scatter: N of M points, ... rejected`.
Deforming geometry: the scatter is seeded, so a *static* mesh never changes;
with a *deforming* mesh (same topology, moving points) the area-weighted choice
reshuffles as face areas change. USD node `deforming geometry`: *recompute*
(default) / *stick to the surface (reference frame shape)* — face choice,
barycentrics, weighting and separation come from the shape at `reference
frame` (read from the input's time samples, e.g. a GeoImport cache) and the
points then ride the deforming surface / *topology only* — every face equally
likely, stable under any deformation without a reference frame (denser where
faces are small). Classic node: `stick to deforming geometry (uniform per
face)` = the topology-only variant.

**variants** — `pick variant`: sequential (index mod count), random (seeded),
attribute (integer point attribute mod count, default `id`); `variant seed`,
`variant attribute`.

**rotation** — `align`: none / direction attribute (points `forward axis`
along a Vector3 attribute — default `vel`, the particle velocity — using
`up vector`) / quaternion attribute (Vector4 x,y,z,w) / euler attribute
(Vector3 degrees, XYZ) / **particle orientation** (the particle system's own
quaternion, which integrates the emitter's *rotation velocity* — real
tumbling). `spin`: `roll along velocity` adds a stateless roll about the axis
(up × velocity) by `roll rate` degrees per unit of distance travelled from the
particle's birth position; `roll channels mask` restricts it to particles in
given Nuke particle channels (a=1, b=2, c=4 ...), e.g. only the ones
ParticleBounce moved to a new channel on impact. `rotate` = extra XYZ rotation
applied to every copy in its local frame (fix a model's rest orientation).
`random rotation` with per-axis `min`/`max` degrees; `rotation variance` = a
simple ± degrees jitter on every axis (the quick counterpart of random scale).

**scale** — `uniform scale`, `scale xyz`, `multiply by size attribute`
(default on, `size` = ParticleToGeo particle size), optional `scale attribute`
(any Vector3 or float point attribute, multiplied per axis — ParticleToGeo
itself only provides the scalar `size`), `random scale` min/max.

**local offset** — `local offset` (translation in the prototype's local space:
it follows the copy's rotation and scale, so it works as a pivot offset),
`random offset` with per-axis `min`/`max` (a seeded per-copy random offset in
that range, stable per id — e.g. y 0..0.2 sinks every rock a little
differently) and `offset variance` (± jitter per axis on top).

**randomness** — `seed`, `id attribute` (default `id`): random rotation /
scale / variant / density are keyed on this integer point attribute so they
stay attached to the same particle from frame to frame. Without it the point
index is used (fine for static clouds, flickery for particles that die).

**attributes** — `copy colour` + `colour attribute` (default `Cf`); `colour
variance hue / saturation / value` = per-copy random jitter of the copy colour
(hue as a fraction of the hue circle, value as ± brightness) — works from white
when the source has no colour, so with **MultiplyCf** every rock gets a
slightly different tint; `copy attributes` (comma list of extra point
attributes copied per instance),
`dump attributes to file` (writes `%TEMP%/CopyToPoints_attributes.txt` with
the full attribute layout incl. first values and copy counts).

**attributes found on the inputs** — a read-only list of every attribute seen
on the points input and on each prototype during the last geometry build
(`name : group / type / count`). It fills in while the panel is open once the
node has been viewed or rendered; `refresh list` forces an update. The same
list is printed in the node's info box (the `i` button). This is where you
look up what a particle system actually provides (e.g. `id`, `Cf`, `size`,
`vel`) before typing a name into the attribute knobs.

Transform per copy: `T(point) * R(align) * R(random) * R(variance) * R(rotate) * R(paint) * S * T(offset) * prototype_matrix`.

## Painting weights in the 3D viewer (Paint tab)

For terrain / mesh sources you can paint five weight layers and a colour
directly on the source geometry and let them drive the copies (this replaces a
projected mask and needs no camera):

1. Connect the terrain to `points`, the rocks to `geo1..`, open the CopyToPoints
   panel and a **3D viewer** looking at the terrain.
2. Tick **enable painting**. A brush circle follows the mouse over the surface.
   **LMB drag** paints the selected `layer`, **Shift+LMB drag** resizes the
   brush; Alt / MMB / RMB navigate as usual (painting pauses while navigating).
   `mode` = add / subtract / set / smooth, with `radius`, `hardness`, `opacity`
   and `value` — the amount the brush works with. **Every layer is a signed
   offset from the node's current values** (0 = "as the knobs say", stored as
   −8 … +8 in 16-bit fixed point): *add* raises the layer, *subtract* lowers
   it, *set* drives it towards `value`, *smooth* averages it. `occlusion test`
   (default on) only paints points visible from the camera — nothing on the
   far side of a sphere or behind another face. `show weights (heat map)`
   draws the source points with a diverging heat map of the current layer
   (magenta = negative, dark = 0, blue → red = positive up to `heat max`; 0 =
   auto-range to the largest |weight|), with `point size`.
3. Enable a layer and it takes the node's current value as its baseline; the
   painting then adds to / subtracts from it:
   * **density layer** — copy probability = `density` + w (0..1): density 0 +
     paint +1 = copies only where painted; density 1 + paint −1 = holes.
   * **scale layer** — scale × (1 + w × `scale per unit weight`): +1 doubles,
     −0.5 halves.
   * **rotation layer** — extra `w × degrees` about a local axis.
   * **variant layer** — shifts the picked prototype by round(w) (+1 = next,
     −1 = previous, wraps).
   * **scatter layer** — scatter density × (1 + w) (Copy tab, `multiply by
     painted 'scatter' layer`). `painted scatter` mode: *add and remove* (default)
     — negative paint thins the base `count` out (density × (1 + w)), positive
     paint **adds points on top**: `paint adds` (default 1000) is the number of
     points a +1 stroke over the whole surface adds, scaled by the painted area
     and weight and independent of `count` — so you can start from a few or
     zero points and paint the rest in; painting into an area the weighting
     left empty adds points there. *remove only* — the count stays fixed and
     the layer can only thin out.
   * **colour layer** — paints a colour (Cd) with the `brush colour`: the
     copies' `Cf` becomes the painted colour (*replace*) or the source colour
     × painted colour (*multiply*), blended by the painted coverage — so a
     points input without any colour attribute gets one created. `add`/`set`
     paint the colour, `subtract` erases, `smooth` blurs. `also write Cf onto
     kept source points` writes it as a `Cf` point attribute onto the
     passed-through source geometry (`keep source points`) as well. The heat
     map shows this layer in its own colours.
4. `flood fill layer` sets the whole current layer to `value` (colour layer:
   the brush colour with full coverage) — works on every layer without the
   viewer, e.g. fill density with 1 and subtract holes, or fill a base tint and
   paint highlights. `clear layer` / `clear all layers`; strokes are undoable
   (Ctrl+Z).

The weights are stored in the node (saved with the script as compact signed
16-bit run-length/base64 text, format `v3`; `v2` (1.x, unsigned) and `v1`
scripts still load — note that their layers now read as offsets) keyed by source point index, so they stay valid as
long as the source topology does not change; if the point count changes the
layers are resized (existing weights keep their index). Painting rebuilds the
copies live (`update copies while painting`) or on mouse release.

The brush works on the geometry cached during the last build of the node —
view or render the node once after connecting a new terrain. Implementation
note: like the AttributePainter plugin, mouse and buttons are polled while the
viewer redraws (Windows), so Nuke's own viewer navigation is never intercepted.

## Particles: what you get, and how to get rotation

ParticleToGeo's point cloud only carries `id` (int), `Cf` (Vector4) and
`size` (float) — no velocity, no orientation. So with **read particle system**
on (default) CopyToPoints reads the particle system behind the points input
directly (works with ParticleToGeo *or* any particle node — ParticleEmitter,
ParticleBounce, ... can be plugged straight into `points`) and provides these
virtual point attributes, matched to points by `id`:

| name | type | meaning |
|---|---|---|
| `vel`, `speed` | Vector3, float | velocity |
| `orient` | Vector4 (x,y,z,w) | quaternion orientation (integrated from the emitter's rotation velocity) |
| `rotaxis`, `rotangle`, `rotvel` | Vector3, float, float | rotation axis / current angle / angular speed (deg) |
| `age`, `life`, `mass`, `psize` | float, float, float, Vector3 | age in frames, lifetime, mass, per-axis size |
| `initialP`, `lastP` | Vector3 | birth position, previous-frame position |
| `channels` | int mask | particle channels (a=1, b=2, c=4, ...) |
| `bounce` | int flags | ParticleBounce collision flags for this frame |

They show up in the "attributes found" list as `… : particle system / …` and
can be used anywhere a name is asked (align, scale, variant, id, copy attrs).

Rotation recipes:

* **Tumbling in flight** — ParticleEmitter: set `rotation velocity` (and
  `rotation velocity variation`) — Nuke integrates it into the orientation
  quaternion. CopyToPoints: `align = particle orientation`. Every rock spins
  about its own random axis, continuously through bounces.
* **Extra spin / rolling that follows the motion** — `spin = roll along
  velocity`; angle grows with distance travelled, so a rock that skids after
  landing keeps rolling in its travel direction; combine with the above.
* **Different behaviour after impact** — Nuke's ParticleBounce doesn't change
  spin, but it can move bounced particles into a channel (`out/in new
  channels`, e.g. `b`). Then either set `roll channels mask = 2` so only
  impacted rocks roll, or split the system with two ParticleToGeo nodes
  (their `channels` knob) → two CopyToPoints with different settings.
* Nuke ParticleBounce note: `object = plane` only reacts to particles crossing
  the XY plane at its axis; use `object = input` with your own ground geometry
  for a floor.

## Shading and colour

* Materials: whatever is connected upstream of a prototype (texture,
  Phong/BasicMaterial, ApplyMaterial ...) is used for every copy of that
  prototype in both modes.
* Per-particle colour: ScanlineRender's default shading samples the texture
  and ignores `Cf`. **MultiplyCf** (`3D > Shader > MultiplyCf`) multiplies its
  input material by `Cf` (point, vertex or object colour) and undoes the
  renderer's homogeneous-W division on the colour channels so the result is
  exact. `mix` blends the effect, `multiply alpha` also scales alpha.
* No material at all on the prototype = Nuke's solid shader, which shows `Cf`
  flat-coloured.
* Geometry that has no `Cf` at all passes through MultiplyCf unchanged (the
  renderer would otherwise hand it its default 18 % grey vertex colour).

## Help

Every tab has a **help...** button that opens a popup (tabbed, HTML) with an
overview, every knob explained and the typical particle / terrain setups; the
node's `?` help and the knob tooltips cover the same ground.

## Debug log / crash reports

Set one folder at the top of `~/.nuke/CopyToPoints/init.py`
(`CTP_DIAG_DIR = r"C:/temp/nuke_diag"`) and restart Nuke: it collects the
plugin trace log (`CopyToPoints_log.txt`, a flushed line per step of both
nodes — after a crash the last line tells where it died), copies of Nuke's own
crash dumps rescued from `%TEMP%`/`%TEMP%\nuke` at every start (`crashdumps/`)
and a `sessions.txt`. Without it, `CTP_LOG=<file>` in the environment enables
just the trace log. Details in `dist-src/INSTALL.md`.

## Distribution package

`.\package.ps1` (after `.\build.ps1`) produces
`dist\CopyToPoints-<version>-Nuke14.1-win64.zip` containing the plugin folder,
`INSTALL.md`, and an `install.bat` / `install.ps1` that copies the folder to
`~/.nuke/CopyToPoints` and registers it in `~/.nuke/init.py` (idempotent;
`install.ps1 -Uninstall` reverses it). Hand that zip to artists.

## CopyToPointsUSD (Nuke 14.1 preview, 15+)

Nuke 14.1 already ships the USD 3D system as a preview (GeoCard, GeoImport,
ScanlineRender2 ...) and its NDK carries the `usg` API, so the same node builds
against 14.1 (`build-usd14.1`, installed as `usd/nuke14.1`); the whole USD test
suite passes there too. Two small API shims (`authorDisplayColor`,
`setSingleReference`) cover the 14.1 differences.

`3D > Modify > CopyToPointsUSD`. Inputs: `points` = a stage with Mesh / Points
prims (terrain, GeoCard, GeoImport geometry, point clouds); `geo1..geoN` =
stages whose **root geometry prims** become the prototypes (variants).
Non-geometry roots — the `/materials` scope written by GeoBindMaterial, lights —
are copied to the output **at their original path**, so `material:binding`
relationships inside the prototypes keep resolving (materials bound upstream of
a geo input travel with the copies).

Two output **modes**:
* **instances** — one `PointInstancer` under `/<node>/instancer` with
  `positions`, `orientations` (quath), `scales`, `protoIndices`, `ids`,
  `extent`; the prototypes live once under `instancer/Prototypes/proto_N`
  (flattened input stage → `copySpec`, so the geo inputs are *not* merged into
  the output and the originals never show up). Per-copy colour is authored as
  an instance-rate `primvars:displayColor` — which **ScanlineRender2 does not
  shade** (Hydra viewers may); the node warns when that is the case.
* **copies** — every copy is a prim `/<node>/copies/copy_N` (an `Xform` with the
  copy's matrix) whose child `geo` *references* the prototype (`proto_N` is an
  `Xform` wrapper around the copied root prim). Source colour, colour variance
  and the painted colour are authored as constant `displayColor` overrides on
  the copy's gprims and render everywhere; materials are kept.
  `copies share geometry (instanceable)` marks every copy `instanceable` — USD
  scenegraph instancing, Houdini's *packed* idea: one prototype in memory, the
  copies are transforms — but then per-copy colour is an instance-rate primvar
  that ScanlineRender2 does not shade.

**Memory — read this before scattering a heavy GeoImport.** In the Hydra
viewer both modes instance. **ScanlineRender2 un-instances everything it
renders**: measured on Nuke 17, ~350 bytes per prototype point per copy in
*every* mode (100 copies of a 40k-point sphere = +1.5 GB, 3000 copies = 40 GB
→ out of memory). The node therefore has a **`max copy points (M)`** guard
(default 20 M = copies × points of the chosen prototypes): above it nothing is
built and the node warns. Fix the scene rather than the limit: use a proxy /
decimated prototype for the render, fewer copies, or keep the heavy version
for the viewer only.

Also emitted: `guide` — a `Points` prim (and `guide_axes`, a `BasisCurves` prim
with one line per copy along its up axis); `guide only` hides the copies; `guide shows the painted layer` colours the
guide points with the heat map of the current paint layer interpolated at every
copy — scattered points included — so you see the painted weights where the
copies actually land (same in the classic node).
`guide purpose`: *default* is always visible in the Viewer but ScanlineRender2
renders it too (turn the guide off before rendering); *guide* / *proxy* are
hidden by Nuke's Viewer until 'display guides' / 'display proxy' is on in the
Viewer settings (`show guides in the viewer` button) and are dropped by
ScanlineRender2 only when its 'prim purpose filter mode' is default/render.

Colours: per-copy colour (source displayColor, colour variance, painted colour)
shows in mode *copies* when the prototype has **no material bound** — a
UsdPreviewSurface ignores `displayColor` unless it reads the primvar (the node
warns). In mode *instances* the colour is an instance-rate primvar that
ScanlineRender2 does not shade.

Knobs mirror the classic node: **Copy** (mode, copy onto every point / one per
prim, `hide source geometry` — authors `visibility = invisible` on the source
prims, guard/density, guide, **scatter** with weighting / bias / separation /
painted scatter layer), **Transform** (align to normals with forward axis + up — normals come from the
`normals` attribute, a `primvars:normals` primvar (vertex or faceVarying,
averaged per point, e.g. after GeoNormals) or, when a mesh has none, are
computed from its faces —
rotate, random rotation, rotation variance, scale, point widths, random scale,
local offset + random offset + variance), **Attributes** (copy displayColor,
colour variance), **Paint** — the same viewer brush as the classic node
(density / scale / rotation / variant / scatter / colour layers, flood fill,
heat map; the painted colour can also be written as vertex `displayColor` onto
the source prims), an **attribute list** of the points input (Attributes tab),
per-tab **help...** buttons. Not in the USD node:
particle-system reading (no classic particles there) and MultiplyCf (not
needed — displayColor is a first-class primvar).

Verified headless on 15.2/16.0/17.0:
instancer contents through GeoExport (81 copies for a 9×9 GeoCard, both
prototypes referenced, scatter 300 / separation, guide purpose, hidden source,
displayColor primvar, painted density / scale / variant / colour / scatter
layers, flood fill, guide axes / guide only, copies mode, materials at their
path with resolving bindings) and ScanlineRender2 renders (Camera4): copies
visible, *hide source* honoured, copies with a bound material still render,
copies mode shows per-copy colours (17: with a GeoDistantLight).

### Named attributes, and spin

USD has schemas for `widths` and `displayColor` and nothing at all for "the
float I want to scale by", so a pipeline carrying its own names needs to be able
to say them. Six knobs take one, each looked up plain and then under
`primvars:` because the same value is authored either way depending on what
wrote it:

| knob | reads | used for |
|---|---|---|
| `use size attr` / `size attr` | float | multiplied into the scale, separately from `use widths` |
| `scale attr` | vec3 (or float) | per-axis scale, so a squashed particle stays squashed |
| `id attr` | int | the id every random choice hangs off |
| `variant attr` | int / float | which prototype, in `attribute` mode |
| `color attr` | colour | read instead of `primvars:displayColor` |
| `copy attrs` | comma separated | carried onto every copy as instance-rate primvars |

`id attr` is the one that matters most and is easiest to miss. Randomisation
otherwise hangs off the point INDEX, which is right for a static mesh and wrong
for a particle stream: one particle dying renumbers every point after it, so
every copy downstream changes its rotation and size in the same frame.

**spin** turns each copy about `(up x velocity)` by `roll rate` degrees for every
unit it has travelled since it was born - so a rock rolls rather than slides. It
is worked out from the DISTANCE rather than accumulated, so scrubbing backwards
gives the same answer. It needs velocities and a `primvars:initialP`, which
`ParticlesToUSD` authors; `roll channels` is a bitmask read from
`primvars:channel`, so only the particles that bounced start rolling.

Five knobs the classic node has are deliberately NOT here, because the USD node
already covers them: `scatter_uniform_faces` (`scatter stick` does this and
more), `keep_points` (the source prims stay in the stage either way - what was
wanted from it is `hide source`), `dump_attributes` (`refresh attrs` and the
attribute list replaced it), and `read_particles` (not representable - particle
nodes will not connect to a `GeoScene`, which is why `ParticlesToUSD` exists).

Two Nuke-16/17-specific traps for anyone building GeomOps with VS 2022: define
`_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR` (Nuke ships an older `msvcp140.dll`,
`std::mutex` built with toolset ≥14.40 crashes otherwise) and, on 17, register
with `GeomOp::Description` (the op must be owned by a `GeomOpNode`).

## ParticlesToUSD

`3D > Modify > ParticlesToUSD`. A classic Nuke particle system (or any classic
point cloud) as a `UsdGeomPoints` prim, so the USD 3D system can use it. Nothing
else gets them across: every particle node is classic 3D and will not connect to
a `GeoScene`.

It carries what a renderer downstream actually needs:

* **positions, ids and widths** - the ids matter, because they are what lets a
  renderer pair a particle with itself across the shutter.
* **velocities**, authored in USD's units - per **second**, not per frame - which
  is what makes motion blur work on a stream whose particle count changes.
* **displayColor** per particle.
* **`primvars:initialP`** (where each particle was born) and
  **`primvars:channel`** (which channels it is on). Neither is needed to draw a
  point; they are there so `CopyToPointsUSD`'s spin can roll a copy by the
  distance it has travelled, and roll only the particles that bounced.

## VolumeToUSD

`3D > Modify > VolumeToUSD`. A `FieldVolume`'s `.vdb` as a USD `Volume` prim.
Connect a `FieldVolume` to it, or type a path into `vdb file`; the grid
pulldowns are read from the file itself, and **reload grids** re-reads it when
the file has been rewritten under the same name.

* **density grid** - the smoke a renderer absorbs and scatters with, times
  `density scale`.
* **temperature** and **emission** grids - two emissive slots, summed, because a
  simulation usually carries heat and flames separately and they read
  differently. Each has a tint and a scale, and `emission file` covers the case
  where the emissive grids live in a different `.vdb` from the density.
* **read as** - the important one. A simulation's `temperature` grid holds
  **KELVIN**: measured on an aerial explosion, density peaks at 0.89 and flames
  at 7.3, but temperature peaks at 8336. Read as **blackbody**, that number
  picks the COLOUR and the scale picks the brightness. Read as **intensity** it
  is multiplied in directly, which is right for a `flames` or `fuel` grid and
  turns a temperature grid into 46361 in the viewer. `temperature` defaults to
  blackbody, `emission` to intensity.
* **K min / K max** - only for a blackbody grid normalised to 0..1: the range it
  is stretched into. Left at zero the grid is taken as Kelvin already.
* **viewport display** - a Volume prim draws nothing on its own, so without this
  the node looks empty and there is nothing to frame on. The box is the grid's
  own bounds from the file header; the fog points fill it evenly and are NOT
  shaped by the density, because the voxels are compressed and cannot be decoded
  here.

The path is authored **unresolved** - a sequence stays `explosion_%04d.vdb` -
and the renderer fills the frame in, because a path resolved once renders the
whole shot as one frame.

Every value is authored twice: as a plain `ir:` attribute and as a constant
`primvars:ir:` primvar. A Hydra render delegate can only ask for primvars, so
without the second copy a volume shown through `GeoRender` arrives with every
shading knob at its default.

## Building

Windows is the tested platform. Linux / macOS build from the same sources
(`build.sh`, see `BUILDING.md`): the only platform-specific code is the viewer
brush's mouse polling (Win32 / X11 / CoreGraphics).

How far the Linux side is proven, precisely: `CopyCore.h` - the maths both copy
nodes share - compiles clean under g++ 11.5 on AlmaLinux 9 against Nuke's own
headers, which is a second compiler's
opinion of the shared header rather than a build. Nothing has been linked or run
against a Linux Nuke, because there is not one on the machine this was written
on. Use the compiler Foundry documents for the version: gcc 9 for Nuke 14,
gcc 11 for 15 to 17.

Requirements: Nuke 14.1 (tested 14.1v8), CMake >= 3.15, Visual Studio 2019
build tools (Nuke 14 = VS 2019 toolchain).

```powershell
.\build.ps1                    # configure + build Release  (build\Release\*.dll)   [classic, Nuke 14.1]
.\build.ps1 -Usd               # + the USD nodes for every Nuke found (build-usd<major>.<minor>/)
.\build.ps1 -Usd -Install      # + cmake --install into %USERPROFILE%\.nuke\CopyToPoints (all builds)
.\build.ps1 -Clean             # wipe build dirs first
# or by hand:
cmake -G "Visual Studio 16 2019" -A x64 -DNuke_DIR="C:/Program Files/Nuke14.1v8/cmake" -B build
cmake --build build --config Release
cmake --install build --config Release --prefix "$env:USERPROFILE\.nuke"
```

`build.bat` wraps `build.ps1` with the execution policy bypassed.
`Nuke_DIR` must point at the `cmake/` sub-folder of the Nuke install (the
directory containing `NukeConfig.cmake`); the registry lookup is disabled so
other installed Nuke versions can't hijack the configure step.

Install layout: `~/.nuke/CopyToPoints/{init.py, menu.py, README.md,
classic/nuke<ver>/{CopyToPoints.dll, MultiplyCf.dll},
usd/nuke<ver>/{CopyToPointsUSD.dll, ParticlesToUSD.dll, VolumeToUSD.dll}, icons/}`
and an idempotent `nuke.pluginAddPath('./CopyToPoints')` block appended to
`~/.nuke/init.py`. The plugin folder's `init.py` adds the folders named for the
running Nuke's exact `<major>.<minor>` to the plugin path, falling back to the
older major-only ones - the NDK is not compatible across minor versions, so a
16.0 build refuses to load in 16.1. One install serves every Nuke version. Restart Nuke afterwards (or "Update All Plugins").

## Testing

The test suite is developer scaffolding and is not shipped with the source - it
needs a Nuke licence in terminal mode, several gigabytes of render output and
paths from the machine it runs on. What it covers is worth stating, because it
is what the claims above rest on.

**The classic node**, end to end through ScanlineRender: copies are produced,
both output modes render and agree, instances survive re-evaluation of the same
frame and scrubbing away and back, per-copy colour reaches the render through
MultiplyCf, and `keep points`, the no-prototype case and WriteGeo/obj export all
behave. A mesh as the points source, the attribute / density / cap knobs, motion
blur in both modes, and a 30k-copy performance run - instances about 10 s
against bake's 19 s for build and render at 320x240.

**The USD nodes**, checked against the authored stage rather than a picture, so
a wrong value cannot hide behind a plausible render:

* the named attributes and spin, from a `.usda` whose primvars could not arise
  any other way - a size rising 0.25 to 2.0 drives the scales 0.25 to 2.0 and
  falls back to a flat 1.0 with the switch off, a 0..70 attribute comes out as
  0..70 on the instancer, and a red/blue tint splits the copies 4/4 on a source
  carrying no `displayColor` at all
* ids, widths and velocities surviving the trip to `UsdGeomPoints`, per frame
* `VolumeToUSD` finding EVERY grid in a `.vdb` - the descriptors are spread
  through the file rather than packed at the front, so a scanner reading only
  the first few megabytes reports `density` alone and misses the two that matter
  for fire
* the scale bias / shape curve, and the reference-frame scatter
* `CopyCore.h` through g++ on Linux (see Building)

**In the 3D viewer**, by GUI probe: the brush paints, the layers take, and the
colours reach the copies - none of which a headless test can see.

Two things worth knowing if you write your own: headless Nuke only licenses with
`-t -i`, and Nuke 16+ segfaults on a script path longer than about 100
characters.

## Limitations / notes

* `CopyToPoints` and `MultiplyCf` are classic-3D only; the USD 3D system is
  served by `CopyToPointsUSD`, `ParticlesToUSD` and `VolumeToUSD` instead.
  The limitations below are the classic node's.
* Instances mode creates one GeoInfo per copy. The 3D viewer compiles a GL
  display list per object; with very high counts the viewer gets sluggish —
  set the node's `display` to bounding box / wireframe while working, the
  render is unaffected. Bake mode is friendlier to the viewer.
* Instance data sharing relies on Nuke's ref-counted geometry caches
  (`PointList` / `PrimitiveList` / `Attribute` are `RefCountedObject`s); the
  plugin writes the shared references into both its own cache entries and the
  output GeoInfos. Downstream modifiers (ModifyGeo etc.) copy-on-write, so
  they never touch the prototype.
* Motion blur in ScanlineRender works (multiple scene samples); particle
  birth/death between samples behaves exactly like ParticleToGeo itself.
* `spin = roll` is stateless (distance from birth position), so it is exact
  for straight-line travel and an approximation for curved paths — but it
  never pops when scrubbing or rendering frames out of order.
* Sources are strict ASCII. `windows.h`/`GL` are included only for the viewer
  brush (mouse polling + overlay), after every DDImage header.
* Viewer painting has been exercised headless only for its data path (storage,
  serialisation, effects); the interactive brush itself needs the GUI.

## Files

```
CMakeLists.txt, build.ps1, build.bat     build / install
cmake/install_user_menu.cmake            idempotent ~/.nuke/init.py registration
src/CopyCore.h                           shared core (paint layers, scatter, processSample, helpers)
src/CopyToPoints.cpp                     the classic GeoOp (Nuke 14.1)
src/CopyToPointsUSD.cpp                  the USD GeomOp (Nuke 15+)
src/MultiplyCf.cpp                       the Material (Nuke 14.1)
src/ParticlesToUSD.cpp                   Nuke particles as a UsdGeomPoints prim
src/VolumeToUSD.cpp                      a FieldVolume's .vdb as a USD Volume prim
nuke/menu.py, nuke/init.py               plugin-folder scripts (installed)
nuke/icons/                              node and menu icons, 24px + @2x
examples/CopyToPoints_example.nk         demo graph
src/CopyToPointsHelp.h                   popup help (Python/PySide2 dialog run by the help buttons, both nodes)
src/PaintBrushKnob.h                     the shared 3D-viewer brush knob (PaintHost interface implemented by both ops)
build.sh, BUILDING.md                    Linux / macOS build (compiles, never linked or run)
package.ps1, dist-src/                   zip packaging (INSTALL.md, install.ps1)
```

## License

MIT - see `LICENSE`.

This is a **plugin**: it is built against, and loaded by, Nuke, which is
Foundry's and is not included here. Nothing third-party is vendored in this
repository - no Nuke headers or libraries, no Embree, USD, CUDA or OptiX
binaries; all come from your own installs. `THIRD_PARTY_NOTICES.md` lists every
dependency, its licence, and the algorithms implemented here from published
descriptions. Being MIT does not grant you rights to any of them.
