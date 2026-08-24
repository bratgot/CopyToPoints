// CopyToPoints.cpp
//
// Nuke 14.1 NDK plugin (classic 3D system).
//
// Copies / instances geometry onto every point of an input point cloud, in
// the spirit of Houdini's "Copy to Points" SOP.  The intended source is the
// output of Nuke's ParticleToGeo node (a point cloud carrying per-particle
// attributes such as "Cf", "vel", "size"), but any classic-3D geometry works:
// every point of every object on the "points" input receives one copy.
//
// Inputs
//   0        points  : GeoOp whose points are the copy targets
//   1..N     geo1..N : GeoOp prototypes.  Every object on every connected geo
//                      input becomes one "variant"; a variant is picked per
//                      point (sequential / random / by attribute).
//
// Output modes
//   instances : one output object per point that SHARES the prototype's point
//               and primitive lists (ref-counted) and only carries its own
//               transform matrix.  Lightweight: memory does not grow with the
//               prototype size.  Shading is inherited from the prototype
//               (material pointer) and per-instance colour is written as an
//               object attribute "Cf".
//   bake      : one output object per prototype containing real copies of the
//               points/primitives for every instance (like GeoScatter).
//               Heavier, but exportable via WriteGeo and it is the fallback if
//               a downstream node does not like thousands of tiny objects.
//
// Sources are strict ASCII.  No windows.h is included.

#include "DDImage/GeoOp.h"
#include "DDImage/GeoInfo.h"
#include "DDImage/GeometryList.h"
#include "DDImage/Scene.h"
#include "DDImage/Primitive.h"
#include "DDImage/Particles.h"
#include "DDImage/Polygon.h"
#include "DDImage/Attribute.h"
#include "DDImage/Knobs.h"
#include "DDImage/Knob.h"
#include "DDImage/Hash.h"
#include "DDImage/Matrix4.h"
#include "DDImage/Vector3.h"
#include "DDImage/Vector4.h"
#include "DDImage/ParticleOp.h"
#include "DDImage/ParticleRender.h"
#include "DDImage/ViewerContext.h"
#include "CopyCore.h"
#include "CopyToPointsHelp.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Viewer painting (shared brush knob; pulls in windows.h/GL, so it comes last)
#include "PaintBrushKnob.h"

using namespace DD::Image;
using namespace ctp;

namespace {

#define CTP_VERSION "1.9.1"
const char* const kClass = "CopyToPoints";
const char* const kHelp =
  "@b;CopyToPoints@n; copies geometry onto every point of the @b;points@n; input "
  "(for example the point cloud produced by ParticleToGeo).\n\n"
  "Connect one or more prototype geometries to the @b;geo@n; inputs. Every object "
  "on every geo input is a variant; a variant is chosen per point sequentially, "
  "randomly, or from an integer point attribute (e.g. the particle id).\n\n"
  "@b;instances@n; mode outputs one lightweight object per point that shares the "
  "prototype's point/primitive data and only carries its own transform. Materials "
  "assigned upstream of the geo inputs are kept, and the source point colour (Cf) "
  "can be written per instance so shaders can use it.\n"
  "@b;bake@n; mode writes real point/primitive copies into one object per prototype "
  "(exportable through WriteGeo, robust with any downstream node).\n\n"
  "Rotation can be aligned to a direction attribute (default: particle velocity "
  "'vel'), taken from a quaternion or euler attribute, and randomised. Scale can "
  "follow the 'size' attribute and be randomised.\n\n"
  "Tip: enable 'dump attributes' to write the attribute layout of the points input "
  "to %TEMP%/CopyToPoints_attributes.txt.";

// Output modes
enum { kModeInstances = 0, kModeBake = 1 };
const char* const kModeNames[] = { "instances (shared, lightweight)", "bake (real copies)", nullptr };

// Variant selection
const char* const kVariantNames[] = { "sequential", "random", "attribute", nullptr };

// Orientation source
const char* const kAlignNames[] = {
  "none",
  "direction attribute (e.g. vel / N)",
  "quaternion attribute (x y z w)",
  "euler attribute (degrees, XYZ)",
  "particle orientation (from the particle system)",
  nullptr };

// Names of the virtual point attributes synthesised from the particle system
// when the points input is a ParticleToGeo / particle node.
const char* const kPsVel      = "vel";
const char* const kPsOrient   = "orient";
const char* const kPsRotVel   = "rotvel";
const char* const kPsRotAxis  = "rotaxis";
const char* const kPsRotAngle = "rotangle";
const char* const kPsAge      = "age";
const char* const kPsLife     = "life";
const char* const kPsMass     = "mass";
const char* const kPsSize3    = "psize";
const char* const kPsBounce   = "bounce";
const char* const kPsLastP    = "lastP";
const char* const kPsInitP    = "initialP";
const char* const kPsChannels = "channels";
const char* const kPsSpeed    = "speed";

// Where copies come from
enum { kSourceAuto = 0, kSourceEveryPoint = 1, kSourceObjects = 2 };
const char* const kSourceNames[] = {
  "auto (points of point clouds, one per mesh object)",
  "every point (also mesh vertices)",
  "one per object (object centre)",
  nullptr };

// Does this object look like a mesh (faces) rather than a point cloud?
bool objectIsMesh(const GeoInfo& gi)
{
  const unsigned n = gi.primitives();
  for (unsigned q = 0; q < n && q < 8; ++q) {
    const Primitive* pr = gi.primitive(q);
    if (pr && pr->faces() > 0) return true;
  }
  return false;
}

// Attribute names must outlive the geometry that references them: Nuke
// stores raw const char* in AttribContext.  User-typed names are interned
// here for the lifetime of the process.
const char* internName(const std::string& s)
{
  static std::set<std::string>* names = new std::set<std::string>();
  return names->insert(s).first->c_str();
}

// Point-group attribute lookup helpers ---------------------------------------
const AttribContext* findPointAttrib(const GeoInfo& info, const char* name)
{
  if (!name || !*name) return nullptr;
  const AttribContext* ac = info.get_group_attribcontext(Group_Points, name);
  if (ac && !ac->empty()) return ac;
  return nullptr;
}

const AttribContext* findAttribInList(const AttribContextList& list, const char* name)
{
  if (!name || !*name) return nullptr;
  for (size_t i = 0; i < list.size(); ++i) {
    if (list[i].name && std::strcmp(list[i].name, name) == 0 && !list[i].empty()) return &list[i];
  }
  return nullptr;
}

bool attribFloatAt(const AttribContext* ac, unsigned i, float& out)
{
  if (!ac || !ac->attribute || i >= ac->attribute->size()) return false;
  switch (ac->type) {
    case FLOAT_ATTRIB:   out = ac->attribute->flt(i); return true;
    case INT_ATTRIB:     out = float(ac->attribute->integer(i)); return true;
    case VECTOR2_ATTRIB: out = ac->attribute->vector2(i).x; return true;
    case VECTOR3_ATTRIB: out = ac->attribute->vector3(i).x; return true;
    case NORMAL_ATTRIB:  out = ac->attribute->normal(i).x; return true;
    case VECTOR4_ATTRIB: out = ac->attribute->vector4(i).x; return true;
    default: return false;
  }
}

bool attribIntAt(const AttribContext* ac, unsigned i, int& out)
{
  if (!ac || !ac->attribute || i >= ac->attribute->size()) return false;
  if (ac->type == INT_ATTRIB) { out = ac->attribute->integer(i); return true; }
  float f;
  if (attribFloatAt(ac, i, f)) { out = int(std::floor(f + 0.5f)); return true; }
  return false;
}

bool attribVec3At(const AttribContext* ac, unsigned i, Vector3& out)
{
  if (!ac || !ac->attribute || i >= ac->attribute->size()) return false;
  switch (ac->type) {
    case VECTOR3_ATTRIB: out = ac->attribute->vector3(i); return true;
    case NORMAL_ATTRIB:  out = ac->attribute->normal(i); return true;
    case VECTOR4_ATTRIB: { const Vector4& v = ac->attribute->vector4(i); out = Vector3(v.x, v.y, v.z); return true; }
    case FLOAT_ATTRIB:   { const float f = ac->attribute->flt(i); out = Vector3(f, f, f); return true; }
    default: return false;
  }
}

bool attribVec4At(const AttribContext* ac, unsigned i, Vector4& out)
{
  if (!ac || !ac->attribute || i >= ac->attribute->size()) return false;
  switch (ac->type) {
    case VECTOR4_ATTRIB: out = ac->attribute->vector4(i); return true;
    case VECTOR3_ATTRIB: { const Vector3& v = ac->attribute->vector3(i); out = Vector4(v.x, v.y, v.z, 1.0f); return true; }
    case NORMAL_ATTRIB:  { const Vector3& v = ac->attribute->normal(i); out = Vector4(v.x, v.y, v.z, 1.0f); return true; }
    case FLOAT_ATTRIB:   { const float f = ac->attribute->flt(i); out = Vector4(f, f, f, 1.0f); return true; }
    default: return false;
  }
}

// One AttribContext that references a freshly allocated attribute.
AttribContext makeAttribContext(GroupType group, const char* name, AttribType type, size_t size)
{
  AttribContext ac;
  ac.group = group;
  ac.name = name;
  ac.type = type;
  ac.attribute = AttributePtr(new Attribute(name, type, size));
  ac.channel = 0;
  ac.varying = false;
  ac.recursive = false;
  return ac;
}

// Copy one element of any attribute type from src[si] to dst[di].
void copyAttribElement(AttribContext& dst, unsigned di, const AttribContext& src, unsigned si)
{
  if (!dst.attribute || !src.attribute) return;
  Attribute& d = *dst.attribute;
  d.copy(int(di), *src.attribute, int(si));
}

} // namespace

// ==========================================================================

class CopyToPoints : public GeoOp, public ctp::PaintHost
{
public:
  static const Description description;
  const char* Class() const override { return kClass; }
  const char* node_help() const override { return kHelp; }

  explicit CopyToPoints(Node* node);

  int minimum_inputs() const override { return 2; }
  int maximum_inputs() const override { return kMaxInputs; }
  bool test_input(int input, Op* op) const override;
  Op* default_input(int input) const override;
  const char* input_label(int input, char* buffer) const override;

  void knobs(Knob_Callback f) override;
  int knob_changed(Knob* k) override;
  void print_info(std::ostream& o) override;
  void build_handles(ViewerContext* ctx) override;

  // ---- ctp::PaintHost (the brush knob talks to the op through this) --------
  ctp::PaintBrushSettings paintSettings() const override
  {
    ctp::PaintBrushSettings st;
    st.enable = _paintEnable; st.layer = _paintLayer; st.mode = _paintMode;
    st.radius = _paintRadius; st.hardness = _paintHardness; st.opacity = _paintOpacity; st.value = _paintValue;
    for (int i = 0; i < 3; ++i) st.color[i] = _paintColor[i];
    st.show = _paintShow; st.heatMax = _paintHeatMax; st.pointSize = _paintPointSize; st.live = _paintLive;
    st.occlusion = _paintOcclusion;
    return st;
  }
  void setPaintRadius(double r) override
  {
    if (Knob* rk = knob("paint_radius")) rk->set_value(r);
    _paintRadius = r;
  }
  std::mutex& paintMutex() override { return _paintMutex; }
  const std::vector<Vector3>& paintPointsNoLock() const override { return _paintPts; }
  const std::vector<Vector3>& paintTrisNoLock() const override { return _paintTris; }
  void setPaintData(const PaintLayers& layers, unsigned version) override
  {
    std::lock_guard<std::mutex> lock(_paintMutex);
    _paint = layers;
    _paintVersion = version;
  }
  const char* paintUndoName() const override { return "CopyToPoints paint"; }

protected:
  void _validate(bool for_real) override;
  void get_geometry_hash() override;
  void geometry_engine(Scene& scene, GeometryList& out) override;
  void geometryEngineImpl(Scene& scene, GeometryList& out);

private:
  enum { kMaxInputs = 33 };  // points + 32 prototype inputs

  // Description of one prototype object (a GeoInfo on one of the geo inputs).
  struct ProtoRef {
    const GeoInfo* info;
    int            inputIndex;
    unsigned       objectIndex;
  };

  typedef ctp::CopyRec InstanceRec;   // one emitted copy (shared with the USD node)

  void gatherPrototypes(Scene& scene, std::vector<GeometryList>& protoLists, std::vector<ProtoRef>& protos);
  ctp::GatherParams makeGatherParams(size_t numVariants) const;
  // Drop the previous build's references held by cache entry 'obj' just before
  // it is re-added (never clears shared lists in place).
  void resetCacheEntry(unsigned obj)
  {
    if (obj < cache_list.size()) {
      GeoInfo::Cache& c = cache_list[obj];
      c.primitives = PrimitiveListPtr();
      c.points = PointListPtr();
      c.attributes.clear();
      c.vertices = 0;
    }
  }
  void gatherInstances(const GeometryList& pointsList, size_t numVariants, std::vector<InstanceRec>& out);
  void buildParticleAttribs(const GeometryList& pointsList);
  // real point attribute first, then the virtual particle-system attributes (source object 0)
  const AttribContext* srcAttrib(const GeoInfo& src, unsigned srcObject, const char* name) const
  {
    const AttribContext* ac = findPointAttrib(src, name);
    if (!ac && srcObject == 0) ac = findAttribInList(_psAttribs, name);
    return ac;
  }
  void emitInstances(GeometryList& out, unsigned& obj, const std::vector<ProtoRef>& protos,
                     const std::vector<InstanceRec>& inst, const GeometryList& pointsList);
  void emitGuide(GeometryList& out, unsigned& obj, const std::vector<InstanceRec>& inst);
  void emitBaked(GeometryList& out, unsigned& obj, const std::vector<ProtoRef>& protos,
                 const std::vector<InstanceRec>& inst, const GeometryList& pointsList);
  void dumpAttributes(const GeometryList& pointsList, const std::vector<ProtoRef>& protos) const;
  void appendKnobHash(Hash& h) const;

  // ---- knob storage ------------------------------------------------------
  int    _mode;
  int    _variantMode;
  int    _variantSeed;
  std::string _variantAttr;

  int    _alignMode;
  std::string _alignAttr;
  int    _forwardAxis;
  double _up[3];
  double _rotate[3];
  bool   _randomRotate;
  int    _spinMode;
  double _rollRate;
  int    _rollChannels;
  double _rotMin[3];
  double _rotMax[3];

  double _scale;
  double _scaleXYZ[3];
  bool   _useSizeAttr;
  std::string _sizeAttr;
  std::string _scaleAttr;
  bool   _randomScale;
  double _scaleMin;
  double _scaleMax;
  double _scaleBias;
  double _scaleShape;
  double _offset[3];
  bool   _randomOffset;
  double _offMin[3];
  double _offMax[3];
  double _offVariance[3];

  int    _seed;
  std::string _idAttr;
  double _density;
  int    _maxInstances;
  int    _pointsSource;
  int    _maxSourcePoints;
  double _maxCopyPoints;   // millions

  bool   _copyColor;
  std::string _colorAttr;
  std::string _copyAttrs;
  // the attributes each copy actually receives: the knob's list, plus velocity
  std::vector<std::string> copyAttrNames() const;
  bool   _keepPoints;
  bool   _dumpAttributes;
  bool   _readParticles;
  AttribContextList _psAttribs;   // virtual point attributes from the particle system
  std::string _psInfo;            // one-line description for the report
  std::string _sourceNote;        // note about mesh objects treated as single targets
  const char* _attrListText;   // storage for the read-only attribute list knob

  // ---- paint knobs ----------------------------------------------------------
  bool   _paintEnable;
  int    _paintLayer;
  double _paintRadius;
  double _paintHardness;
  double _paintOpacity;
  int    _paintMode;
  bool   _paintShow;
  double _paintValue;
  double _paintHeatMax;
  double _paintPointSize;
  bool   _paintLive;
  bool   _paintDensityEnable;
  bool   _paintOcclusion;
  bool   _paintScaleEnable;
  double _paintScaleAmount;
  bool   _paintRotEnable;
  int    _paintRotAxis;
  double _paintRotAmount;
  bool   _paintVariantEnable;
  float  _paintColor[3];
  bool   _paintColorEnable;
  int    _paintColorMode;
  bool   _paintColorSource;
  // paint data copied from the knob (store) for geometry_engine, plus the
  // world-space terrain cache the brush ray-casts against
  PaintLayers _paint;
  unsigned    _paintVersion;
  mutable std::mutex _paintMutex;
  std::vector<Vector3>  _paintTris;   // 3 entries per triangle, world space
  std::vector<unsigned> _meshTriIdx;  // 3 paint indices per triangle (into _paintPts)
  std::vector<unsigned> _meshTriObj;  // source object per triangle
  std::vector<Vector4>  _meshCf;      // per point colour (paint index order), valid where _meshHasCf[obj]
  std::vector<uint8_t>  _meshHasCf;   // per source object

  // scatter knobs + result of the last scatter (built in geometry_engine)
  int    _scatterMode;
  bool   _scatterUniformFaces;
  int    _scatterPaintMode;
  int    _scatterPaintCount;
  int    _guideMode;
  double _guideSize;
  bool   _guideHideCopies;
  bool   _guideHeat;
  int    _scatterCount;
  int    _scatterSeed;
  int    _scatterWeighting;
  double _scatterBias;
  bool   _scatterUsePaint;
  double _scatterSeparation;
  std::string _scatterInfo;
  double _rotVariance;
  double _colorVarHue, _colorVarSat, _colorVarVal;
  std::vector<ctp::ScatterPoint> _scatter;
  std::vector<Vector3>  _paintPts;    // every source point, world space, "all points" order
  std::vector<unsigned> _paintBase;   // first paint index of each source object
  bool _inputIsParticles;

  // ---- runtime stats (written in geometry_engine, read by print_info) -----
  unsigned _statInstances;
  unsigned _statVariants;
  unsigned _statPoints;
  std::string _lastError;
  std::string _attrReport;     // attribute report from the last geometry build
  std::mutex  _reportMutex;

  std::string buildAttrReport(const GeometryList& pointsList, const std::vector<ProtoRef>& protos) const;
  void refreshAttrListKnob();
};

// --------------------------------------------------------------------------
CopyToPoints::CopyToPoints(Node* node)
  : GeoOp(node)
  , _mode(kModeInstances)
  , _variantMode(kVariantSequential)
  , _variantSeed(1)
  , _variantAttr("id")
  , _alignMode(kAlignNone)
  , _alignAttr(kVelocityAttrName)
  , _forwardAxis(kAxisPZ)
  , _randomRotate(false)
  , _spinMode(kSpinNone)
  , _rollRate(200.0)
  , _rollChannels(0)
  , _scale(1.0)
  , _useSizeAttr(true)
  , _sizeAttr(kSizeAttrName)
  , _scaleAttr("")
  , _randomScale(false)
  , _scaleMin(0.5)
  , _scaleMax(1.5)
  , _scaleBias(0.0)
  , _scaleShape(0.0)
  , _seed(0)
  , _idAttr("id")
  , _density(1.0)
  , _maxInstances(0)
  , _pointsSource(kSourceAuto)
  , _maxSourcePoints(250000)
  , _maxCopyPoints(20.0)
  , _copyColor(true)
  , _colorAttr(kColorAttrName)
  , _copyAttrs("")
  , _keepPoints(false)
  , _dumpAttributes(false)
  , _readParticles(true)
  , _attrListText("")
  , _paintEnable(false)
  , _paintLayer(kPaintLayerDensity)
  , _paintRadius(1.0)
  , _paintHardness(0.5)
  , _paintOpacity(0.5)
  , _paintMode(kPaintModeAdd)
  , _paintShow(true)
  , _paintValue(1.0)
  , _paintHeatMax(1.0)
  , _paintPointSize(5.0)
  , _paintLive(true)
  , _paintDensityEnable(false)
  , _paintOcclusion(true)
  , _paintScaleEnable(false)
  , _paintScaleAmount(1.0)
  , _paintRotEnable(false)
  , _paintRotAxis(kPaintAxisY)
  , _paintRotAmount(90.0)
  , _paintVariantEnable(false)
  , _paintColorEnable(false)
  , _paintColorMode(kPaintColorReplace)
  , _paintColorSource(false)
  , _paintVersion(0)
  , _inputIsParticles(false)
  , _scatterMode(kScatterOff)
  , _scatterUniformFaces(false)
  , _scatterPaintMode(kScatterPaintAddRemove)
  , _scatterPaintCount(1000)
  , _guideMode(kGuideOff)
  , _guideSize(0.5)
  , _guideHideCopies(false)
  , _guideHeat(true)
  , _scatterCount(1000)
  , _scatterSeed(0)
  , _scatterWeighting(kScatterWUniform)
  , _scatterBias(2.0)
  , _scatterUsePaint(true)
  , _scatterSeparation(0.0)
  , _rotVariance(0.0)
  , _colorVarHue(0.0), _colorVarSat(0.0), _colorVarVal(0.0)
  , _statInstances(0)
  , _statVariants(0)
  , _statPoints(0)
{
  _up[0] = 0.0; _up[1] = 1.0; _up[2] = 0.0;
  _rotate[0] = _rotate[1] = _rotate[2] = 0.0;
  _rotMin[0] = _rotMin[1] = _rotMin[2] = 0.0;
  _rotMax[0] = 0.0; _rotMax[1] = 360.0; _rotMax[2] = 0.0;
  _scaleXYZ[0] = _scaleXYZ[1] = _scaleXYZ[2] = 1.0;
  _offset[0] = _offset[1] = _offset[2] = 0.0;
  _randomOffset = false;
  _paintColor[0] = 1.0f; _paintColor[1] = 0.25f; _paintColor[2] = 0.1f;
  for (int i = 0; i < 3; ++i) { _offMin[i] = 0.0; _offMax[i] = 0.0; _offVariance[i] = 0.0; }
}


// --------------------------------------------------------------------------
bool CopyToPoints::test_input(int input, Op* op) const
{
  (void)input;
  return dynamic_cast<GeoOp*>(op) != nullptr;
}

Op* CopyToPoints::default_input(int input) const
{
  if (input == 0) return GeoOp::default_input(input);
  return nullptr;   // prototype inputs are optional
}

const char* CopyToPoints::input_label(int input, char* buffer) const
{
  if (input == 0) return "points";
  std::snprintf(buffer, 32, "geo%d", input);
  return buffer;
}


// --------------------------------------------------------------------------
void CopyToPoints::build_handles(ViewerContext* ctx)
{
  ctpLog("build_handles", _paintEnable ? "paint on" : "paint off");
  GeoOp::build_handles(ctx);
  if (_paintEnable && ctx && ctx->viewer_mode() != VIEWER_2D) {
    if (Knob* k = knob("paint_data")) k->add_draw_handle(ctx);
  }
}

// --------------------------------------------------------------------------
void CopyToPoints::knobs(Knob_Callback f)
{
  GeoOp::knobs(f);   // display / selectable / render mode (first tab)

  // ---------------------------------------------------------------- Copy tab
  Tab_knob(f, "Copy");
  Named_Text_knob(f, "title", "", "<b><font size=+2>CopyToPoints</font></b>&nbsp;&nbsp;<font size=-1>v" CTP_VERSION "</font>");
  SetFlags(f, Knob::STARTLINE);
  Named_Text_knob(f, "subtitle", "", "<i>Copy / instance geometry onto points, particles and terrain</i>"
                                     "&nbsp;&nbsp;&nbsp;<font size=-1>Created by Marten Blumen</font>");
  SetFlags(f, Knob::STARTLINE);
  PyScript_knob(f, ctp::helpScript(1), "help_copy", "help...");
  Tooltip(f, "Open the popup help (overview, every tab explained, workflows).");
  Divider(f, "output");
  Enumeration_knob(f, &_mode, kModeNames, "mode", "mode");
  Tooltip(f, "instances: one lightweight object per point sharing the prototype geometry "
             "(only a transform per copy).\n"
             "bake: real point/primitive copies merged into one object per prototype "
             "(heavier, but works with WriteGeo and any downstream node).");
  Enumeration_knob(f, &_pointsSource, kSourceNames, "points_source", "copy onto");
  Tooltip(f, "What counts as a copy target on the points input.\n"
             "auto: every point of point clouds and meshes (terrain, cards, ReadGeo), except when the points input "
             "is a particle node whose particles are geometry: then every particle mesh counts as ONE target at "
             "its centre (copying onto every vertex of every particle would explode).\n"
             "every point: use all points, mesh vertices included (Houdini behaviour).\n"
             "one per object: every input object gives one copy at its bounding-box centre.");
  Bool_knob(f, &_keepPoints, "keep_points", "keep source points");
  Tooltip(f, "Also pass the input point cloud through to the output.");
  Int_knob(f, &_maxInstances, "max_instances", "max instances");
  Tooltip(f, "Safety cap on the number of copies (0 = unlimited).");
  Float_knob(f, &_maxCopyPoints, IRange(0.0, 200.0), "max_copy_points", "max copy points (M)");
  Tooltip(f, "Memory guard in millions of points: copies x points of the chosen prototypes. ScanlineRender builds render "
             "primitives per copy, so thousands of copies of a heavy mesh exhaust memory even in instances mode. Above "
             "the limit nothing is built and the node warns - use a lighter prototype or fewer copies. 0 = no limit.");
  Int_knob(f, &_maxSourcePoints, "max_source_points", "max source points");
  Tooltip(f, "Guard: if the points input offers more copy targets than this, nothing is built and the node "
             "shows an error instead of freezing Nuke (0 = no limit). Nuke's per-object cost grows faster than "
             "linearly: about 100k copies build+render in ~10 s, 300k in minutes. Raise the limit deliberately, "
             "and prefer 'bake' mode for very large counts.");
  Float_knob(f, &_density, IRange(0.0, 1.0), "density", "density");
  Tooltip(f, "Probability that a point receives a copy (1 = every point).");

  Divider(f, "guide geometry (viewer only, never renders)");
  Enumeration_knob(f, &_guideMode, kGuideNames, "guide_mode", "guide");
  Tooltip(f, "Show where the copies go as guide geometry in the 3D viewer: a point cloud of every copy position "
             "(orange = scattered points, cyan = source vertices / particles) and optionally a line per copy along "
             "its local up axis (shows the orientation and the scale). Guide objects have render mode off, so "
             "ScanlineRender never sees them.");
  Float_knob(f, &_guideSize, IRange(0.01, 5.0), "guide_size", "axis length");
  Tooltip(f, "How big the guide is drawn: the length of the up-axis line on each copy, in world units. "
             "It only changes the guide, never the copies.");
  Bool_knob(f, &_guideHideCopies, "guide_hide_copies", "hide the copies (guide only)");
  Tooltip(f, "Output only the guide geometry - handy to judge a scatter/paint before adding heavy prototypes. "
             "Turn off to get the copies back (the guide stays viewer-only either way).");
  Bool_knob(f, &_guideHeat, "guide_heat", "guide shows the painted layer (heat map)");
  Tooltip(f, "Colour the guide points with the heat map of the current paint layer (Paint tab: layer / heat max) "
             "interpolated at every copy - scattered points included - instead of cyan (vertex) / orange (scattered).");

  Divider(f, "scatter points on the geometry");
  Enumeration_knob(f, &_scatterMode, kScatterNames, "scatter_mode", "scatter");
  Tooltip(f, "Like Houdini's Scatter: generate copy targets anywhere on the faces of the points input "
             "(area weighted, random position on each face) instead of only on its vertices.\n"
             "add: scattered points plus the existing points.\n"
             "replace: only the scattered points.\n"
             "Scattered points get the face normal (use align = direction attribute, attribute 'N'), the "
             "interpolated source colour and the interpolated painted weights. They count towards 'max source points'.");
  Int_knob(f, &_scatterCount, IRange(0, 100000), "scatter_count", "count");
  Tooltip(f, "Number of scattered points (seeded, stable while the topology is unchanged; the points stick to their faces).");
  Int_knob(f, &_scatterSeed, "scatter_seed", "scatter seed");
  Tooltip(f, "Change this for a different random arrangement of the same number of scattered points. "
             "The points are stable while the seed and the topology stay put, so a scatter does not "
             "crawl from frame to frame.");
  Enumeration_knob(f, &_scatterWeighting, kScatterWeightNames, "scatter_weighting", "weighting");
  Tooltip(f, "Where scattered points prefer to land, judged per face relative to the 'up vector' (Transform tab):\n"
             "prefer flat / steep: by slope (gradient of the surface).\n"
             "prefer peaks / valleys: by height along up, normalised over the geometry's extent.");
  Float_knob(f, &_scatterBias, IRange(0.1, 8.0), "scatter_bias", "bias");
  Tooltip(f, "How strongly the weighting is applied (exponent on the feature): 1 = linear, higher = more concentrated.");
  Bool_knob(f, &_scatterUsePaint, "scatter_use_paint", "multiply by painted 'scatter' layer");
  Tooltip(f, "Paint the 'scatter' layer on the Paint tab; the scattered density is multiplied by it "
             "(painted 1 = full density, unpainted 0 = nothing). Ignored while that layer is completely empty.");
  Enumeration_knob(f, &_scatterPaintMode, kScatterPaintModeNames, "scatter_paint_mode", "painted scatter");
  Tooltip(f, "How the painted 'scatter' layer acts.\n"
             "remove only: candidates where the layer is negative are dropped (density x (1 + w), never above 1); the "
             "count stays 'count'.\n"
             "add and remove: negative paint thins the base 'count' out as above, positive paint ADDS points on top: "
             "'paint adds' points for a weight of +1 over the whole surface, scaled by the painted area and weight - "
             "independent of 'count', so you can start from a few (or zero) points and paint the rest in; painting "
             "into an area the weighting left empty adds points there. 'max copies' still caps the total.");
  Int_knob(f, &_scatterPaintCount, IRange(0, 100000), "scatter_paint_count", "paint adds");
  Tooltip(f, "add and remove mode: number of points a paint weight of +1 over the whole surface adds (a +1 stroke "
             "over 10% of the surface adds 10% of this, a +2 stroke twice that).");
  Bool_knob(f, &_scatterUniformFaces, "scatter_uniform_faces", "stick to deforming geometry (uniform per face)");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Every face is equally likely regardless of its area, so the scattered points keep their faces and "
             "barycentrics when the geometry deforms (same topology) instead of being reshuffled as face areas change. "
             "Denser where faces are small; off = area weighted (uniform in world space).");
  Float_knob(f, &_scatterSeparation, IRange(0.0, 5.0), "scatter_separation", "separation");
  Tooltip(f, "Minimum distance between scattered points (world units, 0 = off): a Poisson-disc style non-collision "
             "control. If the geometry cannot hold the requested count at this distance fewer points are made "
             "(the info shows how many).");

  Divider(f, "variants");
  Enumeration_knob(f, &_variantMode, kVariantNames, "variant_mode", "pick variant");
  Tooltip(f, "How the prototype is chosen per point when several objects are connected to the geo inputs.\n"
             "sequential: point index modulo variant count.\n"
             "random: seeded random choice.\n"
             "attribute: integer point attribute modulo variant count.");
  Int_knob(f, &_variantSeed, "variant_seed", "variant seed");
  Tooltip(f, "Change this to deal the prototypes out differently when pick variant is random. Only "
             "used by that mode.");
  String_knob(f, &_variantAttr, "variant_attr", "variant attribute");
  Tooltip(f, "Point attribute used when 'pick variant' is 'attribute' (int or float).");


  // ----------------------------------------------------------- Transform tab
  Tab_knob(f, "Transform");
  PyScript_knob(f, ctp::helpScript(2), "help_transform", "help...");
  Tooltip(f, "Open the popup help on the Transform page.");
  Divider(f, "rotation");
  Enumeration_knob(f, &_alignMode, kAlignNames, "align_mode", "align");
  Tooltip(f, "none: no per-point orientation.\n"
             "direction attribute: point the forward axis along a vector attribute (e.g. 'vel' velocity or 'N').\n"
             "quaternion attribute: Vector4 (x,y,z,w) rotation per point.\n"
             "euler attribute: Vector3 rotation in degrees (XYZ order) per point.\n"
             "particle orientation: the particle system's own quaternion (integrates the emitter's "
             "'rotation velocity', so particles tumble). Needs 'read particle system'.");
  String_knob(f, &_alignAttr, "align_attr", "align attribute");
  Tooltip(f, "Name of the point attribute used by 'align'. ParticleToGeo writes the velocity as 'vel'.");
  Enumeration_knob(f, &_forwardAxis, kAxisNames, "forward_axis", "forward axis");
  Tooltip(f, "Which local axis of the prototype is pointed along the direction attribute.");
  XYZ_knob(f, _up, "up", "up vector");
  Tooltip(f, "Up hint used to build the orientation frame in direction mode.");
  XYZ_knob(f, _rotate, "rotate", "rotate");
  Tooltip(f, "Additional rotation (degrees, XYZ order) applied to every copy.");
  Bool_knob(f, &_randomRotate, "random_rotate", "random rotation");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Give every copy its own random rotation, within the min/max range below. Seeded per "
             "point, so a copy keeps its angle from frame to frame instead of flickering.");
  XYZ_knob(f, _rotMin, "rot_min", "min");
  Tooltip(f, "The low end of the random rotation range, in degrees per axis. Set min and max to the "
             "same value for no randomness on that axis.");
  XYZ_knob(f, _rotMax, "rot_max", "max");
  Tooltip(f, "Per-copy random rotation range in degrees (seeded, stable per point id).");
  Float_knob(f, &_rotVariance, IRange(0.0, 180.0), "rot_variance", "rotation variance");
  Tooltip(f, "Simple jitter: every copy gets an extra random rotation of +/- this many degrees on each axis "
             "(applied after align and the random rotation range).");
  Enumeration_knob(f, &_spinMode, kSpinNames, "spin", "spin");
  Tooltip(f, "Extra rotation applied after 'align'.\n"
             "roll along velocity: rotate about the axis (up x velocity) by roll_rate degrees per unit of "
             "distance the particle has travelled from its birth position (stateless, so it works when "
             "scrubbing). Gives rocks a rolling/tumbling look that follows their motion, e.g. after a bounce. "
             "Needs 'read particle system' (uses vel, initialP; falls back to the align/velocity attribute).");
  Float_knob(f, &_rollRate, IRange(0.0, 1000.0), "roll_rate", "roll rate");
  Tooltip(f, "Degrees of roll per unit of distance travelled (about 57 / radius for a perfectly rolling sphere).");
  Int_knob(f, &_rollChannels, "roll_channels", "roll channels mask");
  Tooltip(f, "0 = roll every particle. Otherwise a bitmask of Nuke particle channels (a=1, b=2, c=4, d=8, ...); "
             "only particles in one of these channels roll. Use ParticleBounce's 'new channels' to move bounced "
             "particles into a channel so that only impacted particles roll.");

  Divider(f, "scale");
  Float_knob(f, &_scale, IRange(0.0, 10.0), "scale", "uniform scale");
  Tooltip(f, "Scales every copy by the same amount, on top of whatever size the prototype already is. "
             "1 leaves it alone.");
  XYZ_knob(f, _scaleXYZ, "scale_xyz", "scale xyz");
  Tooltip(f, "Per-axis scale on top of the uniform one, for squashing or stretching the copies. "
             "Multiplied with everything else that touches scale.");
  Bool_knob(f, &_useSizeAttr, "use_size_attr", "multiply by size attribute");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Take a per-point size from the points input and multiply it into the scale, so a "
             "particle system that already varies its sizes drives the copies. The attribute to read "
             "is named below.");
  String_knob(f, &_sizeAttr, "size_attr", "size attribute");
  Tooltip(f, "Float point attribute multiplied into the scale (ParticleToGeo: 'size').");
  String_knob(f, &_scaleAttr, "scale_attr", "scale attribute (vec3)");
  Tooltip(f, "Optional Vector3 (or float) point attribute multiplied into the per-axis scale. Leave empty to ignore.");
  Bool_knob(f, &_randomScale, "random_scale", "random scale");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Give every copy its own random size, within the min/max range below. Seeded per point, "
             "so a copy keeps its size rather than pulsing frame to frame.");
  Float_knob(f, &_scaleMin, IRange(0.0, 5.0), "scale_min", "min");
  Tooltip(f, "The low end of the random size range, as a multiplier. Set min and max to the same value "
             "to turn the randomness off while leaving it switched on.");
  Float_knob(f, &_scaleMax, IRange(0.0, 5.0), "scale_max", "max");
  Tooltip(f, "Per-copy random uniform scale multiplier range (seeded, stable per point id).");
  Float_knob(f, &_scaleBias, IRange(-1.0, 1.0), "scale_bias", "bias");
  Tooltip(f, "Slides the random sizes toward one end without changing the range: below zero gives more SMALL copies, above zero more LARGE ones. At 0 the sizes are spread evenly between min and max.");
  Float_knob(f, &_scaleShape, IRange(-1.0, 1.0), "scale_shape", "shape");
  Tooltip(f, "Gathers the random sizes toward the MIDDLE of the range below zero, or pushes them out to the two ENDS above it - at +1 most copies are near min or near max and few are in between. The average size stays where it was; only the spread changes.");
  Divider(f, "local offset");
  XYZ_knob(f, _offset, "offset", "local offset");
  Tooltip(f, "Offset applied in the prototype's local space (it follows the copy's rotation and scale).");
  Bool_knob(f, &_randomOffset, "random_offset", "random offset");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Add a per-copy random local offset in the min..max range per axis (seeded, stable per point id).");
  XYZ_knob(f, _offMin, "offset_min", "min");
  Tooltip(f, "The low end of the random offset range, per axis, in the copy's own local axes.");
  XYZ_knob(f, _offMax, "offset_max", "max");
  Tooltip(f, "Per-copy random local offset range (world units in the copy's local axes).");
  XYZ_knob(f, _offVariance, "offset_variance", "offset variance");
  Tooltip(f, "Simple jitter: every copy is moved by +/- this much (per axis, local space) on top of the offset above.");

  Divider(f, "randomness");
  Int_knob(f, &_seed, "seed", "seed");
  Tooltip(f, "The starting point for every random choice on this node - rotation, scale, offset, "
             "variant and colour. Change it to reshuffle them all at once while keeping the ranges.");
  String_knob(f, &_idAttr, "id_attr", "id attribute");
  Tooltip(f, "Integer point attribute that identifies a point stably across frames. When present, "
             "random rotation/scale/variant stay attached to the same particle as it moves. "
             "Falls back to the point index when the attribute does not exist.");


  // ---------------------------------------------------------- Attributes tab
  Tab_knob(f, "Attributes");
  PyScript_knob(f, ctp::helpScript(3), "help_attributes", "help...");
  Tooltip(f, "Open the popup help on the Attributes page.");
  Divider(f, "attributes");
  Bool_knob(f, &_copyColor, "copy_color", "copy colour");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Read a colour off each source point and write it onto the copy as Cf, so the copies "
             "inherit the colour of the points they sit on. The attribute to read is named below.");
  String_knob(f, &_colorAttr, "color_attr", "colour attribute");
  Tooltip(f, "Point colour attribute of the points input written onto every copy as 'Cf' "
             "(object attribute in instances mode, point attribute in bake mode) so shading can pick it up.");
  Float_knob(f, &_colorVarHue, IRange(0.0, 1.0), "color_var_hue", "colour variance hue");
  Tooltip(f, "Per-copy random hue shift (fraction of the hue circle) applied to the copy colour Cf. "
             "Works without a source colour too (starts from white). Needs MultiplyCf (or no material) to show.");
  Float_knob(f, &_colorVarSat, IRange(0.0, 1.0), "color_var_sat", "colour variance saturation");
  Tooltip(f, "Per-copy random saturation shift applied to the copy colour Cf, so the copies are not "
             "all equally vivid. 0 leaves the saturation alone.");
  Float_knob(f, &_colorVarVal, IRange(0.0, 1.0), "color_var_val", "colour variance value");
  Tooltip(f, "Per-copy random brightness variation (0 = none, 1 = up to +/-100%).");
  String_knob(f, &_copyAttrs, "copy_attrs", "copy attributes");
  Tooltip(f, "Comma separated list of extra point attributes to copy from the source point onto "
             "each copy as object attributes (e.g. 'id').\n\n"
             "The velocity ('vel') is always copied whether or not it is listed here: a renderer "
             "needs it to blur the copies, and cannot get it any other way, because Nuke's particle "
             "geometry carries no velocity at all.");
  Bool_knob(f, &_readParticles, "read_particles", "read particle system");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "When the points input is ParticleToGeo (or any particle node), read the particle system "
             "directly and provide these extra per-point attributes: vel (velocity), orient (quaternion), "
             "rotvel (rotation velocity), rotaxis, rotangle, age, life, mass, psize (Vector3 size), "
             "bounce (collision flags), lastP (previous position), initialP (birth position), speed, channels "
             "(particle channel mask). ParticleToGeo itself only exports id, Cf and size.");
  Bool_knob(f, &_dumpAttributes, "dump_attributes", "dump attributes to file");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Also write the full attribute layout (with first values) of the points input and the "
             "prototypes to %TEMP%/CopyToPoints_attributes.txt every time the geometry is rebuilt.");

  Divider(f, "attributes found on the inputs");
  Button(f, "refresh_attrs", "refresh list");
  Tooltip(f, "Show the attributes seen on the points input and the prototypes during the last "
             "geometry build (view or render the node first, then refresh). Also shown in the node's info (i).");
  Multiline_String_knob(f, &_attrListText, "attribute_list", "", 10);
  SetFlags(f, Knob::NO_ANIMATION | Knob::READ_ONLY | Knob::OUTPUT_ONLY | Knob::DO_NOT_WRITE | Knob::STARTLINE);
  Tooltip(f, "Point attributes of the points input (name : group / type / count) and the same for every "
             "prototype variant. Type these names into the attribute knobs above. Read-only.");

  // --------------------------------------------------------------- Paint tab
  Tab_knob(f, "Paint");
  PyScript_knob(f, ctp::helpScript(4), "help_paint", "help...");
  Tooltip(f, "Open the popup help on the Paint page.");
  Named_Text_knob(f, "paint_help", "",
    "<b>Paint weights on the source geometry in the 3D viewer</b> (terrain, cards, meshes).<br>"
    "Enable painting, open this panel, put the mouse over the geometry in a 3D viewer:<br>"
    "<b>LMB drag</b> paints the current layer, <b>Shift+LMB drag</b> resizes the brush, "
    "Alt/MMB/RMB navigate as usual. Each layer drives one thing (below).");
  SetFlags(f, Knob::STARTLINE);
  Divider(f, "brush");
  Bool_knob(f, &_paintEnable, "paint_enable", "enable painting");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Turns the viewer brush on. Weights are stored in the node (saved with the script), keyed by "
             "source point index, so they stay valid as long as the source topology does not change.");
  Enumeration_knob(f, &_paintLayer, kPaintLayerNames, "paint_layer", "layer");
  Tooltip(f, "Which weight layer the brush paints: density, scale, rotation or variant.");
  Enumeration_knob(f, &_paintMode, kPaintModeNames, "paint_mode", "mode");
  Tooltip(f, "Every layer is an offset from the node's current values (0 = as the knobs say). add / subtract move the "
             "layer up / down by opacity x value per sample, set drives it towards 'value', smooth averages it.");
  Float_knob(f, &_paintRadius, IRange(0.01, 20.0), "paint_radius", "radius");
  Tooltip(f, "Brush radius in world units (Shift+LMB drag in the viewer also changes it).");
  Float_knob(f, &_paintHardness, IRange(0.0, 1.0), "paint_hardness", "hardness");
  Tooltip(f, "0 = soft falloff over the whole radius, 1 = hard edge.");
  Float_knob(f, &_paintOpacity, IRange(0.0, 1.0), "paint_opacity", "opacity");
  Tooltip(f, "Strength of each stroke sample (0..1).");
  Float_knob(f, &_paintValue, IRange(-8.0, 8.0), "paint_value", "value");
  Tooltip(f, "The amount the brush works with (layers hold -8 .. +8). add/subtract: opacity x value per sample; "
             "set: the layer moves towards this value. Density: +1 = every point gets a copy, -1 = none; scale: "
             "x 'scale per unit'; rotation: x degrees; variant: +1 = next prototype; scatter: -1 = no scattered points.");
  Color_knob(f, _paintColor, "paint_color", "brush colour");
  Tooltip(f, "Colour painted by the 'colour' layer (and used by flood fill on that layer).");
  Bool_knob(f, &_paintShow, "paint_show", "show weights (heat map)");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Draw the source points as a heat map of the current layer: blue = 0, cyan, green, yellow, red = 'heat max'.");
  Float_knob(f, &_paintHeatMax, IRange(0.01, 16.0), "paint_heat_max", "heat max");
  Tooltip(f, "Weight value shown as red in the heat map (0 = auto: the layer's current maximum).");
  Float_knob(f, &_paintPointSize, IRange(1.0, 15.0), "paint_point_size", "point size");
  Tooltip(f, "How large the source points are drawn while painting. It only affects what you see in "
             "the Viewer - a bigger dot is easier to aim at on dense geometry.");
  Bool_knob(f, &_paintLive, "paint_live", "update copies while painting");
  Tooltip(f, "Rebuild the copies during the stroke (off = only when the mouse is released; faster on big scenes).");
  Bool_knob(f, &_paintOcclusion, "paint_occlusion", "occlusion test (only visible points)");
  Tooltip(f, "Only paint points that are visible from the camera: a point behind another face (or on the far side "
             "of the geometry) is skipped. Off = everything inside the brush sphere is painted, back faces included.");
  Button(f, "paint_fill_layer", "flood fill layer");
  Tooltip(f, "Set every point of the current layer to 'value' (colour layer: the brush colour with full coverage). "
             "Works on any layer without touching the viewer - e.g. fill density with 1 and then subtract holes, "
             "or fill the colour with a base tint and paint highlights.");
  SetFlags(f, Knob::KNOB_CHANGED_ALWAYS);   // buttons work from Python / with the panel closed
  Button(f, "paint_clear_layer", "clear layer");
  SetFlags(f, Knob::KNOB_CHANGED_ALWAYS);
  Tooltip(f, "Set every weight in the CURRENT layer back to zero, leaving the other layers "
             "alone. There is no undo for this.");
  Button(f, "paint_clear_all", "clear all layers");
  SetFlags(f, Knob::KNOB_CHANGED_ALWAYS);
  Tooltip(f, "Set every weight in EVERY layer back to zero - density, scale, rotation, "
             "variant, colour and scatter together. There is no undo for this.");
  Divider(f, "what the layers do");
  Bool_knob(f, &_paintDensityEnable, "paint_density_enable", "density layer");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Copy probability = 'density' (Copy tab) + painted weight, clamped to 0..1: unpainted areas keep the "
             "current density, add raises it (density 0 + paint 1 = copies only where painted), subtract lowers it "
             "(density 1 + paint -1 = holes).");
  Bool_knob(f, &_paintScaleEnable, "paint_scale_enable", "scale layer");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Let the painted scale layer change the size of the copies, by the amount per unit weight "
             "below. Off, the layer is kept but ignored.");
  Float_knob(f, &_paintScaleAmount, IRange(0.0, 4.0), "paint_scale_amount", "scale per unit weight");
  Tooltip(f, "The copy's scale is multiplied by (1 + weight x this): unpainted = current scale, +1 doubles it "
             "(with 1.0), -0.5 halves it.");
  Bool_knob(f, &_paintRotEnable, "paint_rot_enable", "rotation layer");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Let the painted rotation layer turn the copies about the axis chosen below. Off, the "
             "layer is kept but ignored.");
  Enumeration_knob(f, &_paintRotAxis, kPaintAxisNames, "paint_rot_axis", "axis");
  Tooltip(f, "Which of the copy's own local axes the painted rotation turns it about.");
  Float_knob(f, &_paintRotAmount, IRange(-360.0, 360.0), "paint_rot_amount", "degrees at full weight");
  Tooltip(f, "Extra rotation of weight x degrees about the chosen local axis of the copy.");
  Bool_knob(f, &_paintVariantEnable, "paint_variant_enable", "variant layer");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "The weight shifts the picked variant: +1 = next prototype, -1 = previous (wraps around); unpainted = as 'pick variant' says.");
  Bool_knob(f, &_paintColorEnable, "paint_color_enable", "colour layer");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "The painted colour drives the copies' Cf (creates the attribute when the source has none): "
             "where the colour layer has coverage the copy colour is replaced by / multiplied with the painted colour. "
             "Needs MultiplyCf (or no material) on the prototypes to show in ScanlineRender.");
  Enumeration_knob(f, &_paintColorMode, kPaintColorModeNames, "paint_color_mode", "");
  Tooltip(f, "Whether the painted colour REPLACES the copy's colour or is MULTIPLIED into it. "
             "Multiplying keeps the source colour's variation and tints it; replacing throws it away.");
  Bool_knob(f, &_paintColorSource, "paint_color_source", "also write Cf onto kept source points");
  Tooltip(f, "With 'keep source points' on, the painted colour is also written as a Cf point attribute onto the "
             "passed-through source geometry (creates Cf when missing).");
  Named_Text_knob(f, "paint_scatter_note", "", "<i>scatter layer</i>: scatter density x (1 + weight) when "
                  "'multiply by painted scatter layer' is on (Copy tab): -1 = no scattered points, +1 = twice as likely.");
  SetFlags(f, Knob::STARTLINE);
  CustomKnob1(PaintBrushKnob, f, this, "paint_data");
  SetFlags(f, Knob::NO_ANIMATION | Knob::INVISIBLE);

  // --------------------------------------------------------------- About tab
  Tab_knob(f, "About");
  Named_Text_knob(f, "about", "",
    "<b><font size=+2>CopyToPoints</font></b> v" CTP_VERSION "<br>"
    "<i>Houdini-style copy-to-points for Nuke's classic 3D system.</i><br><br>"
    "Copies or instances geometry onto every point of a point cloud, particle system or mesh, with variants, "
    "particle-driven rotation and scale, painted weights and shading via MultiplyCf.<br><br>"
    "<b>Created by Marten Blumen</b><br>"
    "Built for Nuke 14.1 (Windows x64). Companion node: MultiplyCf.");
  SetFlags(f, Knob::STARTLINE);
  PyScript_knob(f, ctp::helpScript(0), "help_about", "open help...");
  Tooltip(f, "Open the popup help: overview, every tab explained, typical setups.");
}

void CopyToPoints::refreshAttrListKnob()
{
  std::string text;
  {
    std::lock_guard<std::mutex> lock(_reportMutex);
    text = _attrReport;
  }
  if (text.empty())
    text = "(no geometry built yet: view the node in the 3D viewer or render it, then press 'refresh list')";
  if (Knob* kk = knob("attribute_list")) kk->set_text(text.c_str());
}

int CopyToPoints::knob_changed(Knob* k)
{
  try {
    if (!k) return GeoOp::knob_changed(k);
    if (k->is("refresh_attrs") || k->is("showPanel") || k->is("updateUI")) {
      refreshAttrListKnob();
      if (k->is("refresh_attrs") || k->is("updateUI")) return 1;
    }
    if (k->is("paint_clear_layer") || k->is("paint_clear_all") || k->is("paint_fill_layer")) {
      // read the brush knobs directly: with the panel closed (python / headless) the
      // members may not have been stored yet
      int layer = _paintLayer;
      double value = _paintValue;
      float col[3] = { _paintColor[0], _paintColor[1], _paintColor[2] };
      if (Knob* kk = knob("paint_layer")) layer = int(kk->get_value());
      if (Knob* kk = knob("paint_value")) value = kk->get_value();
      if (Knob* kk = knob("paint_color")) for (int i = 0; i < 3; ++i) col[i] = float(kk->get_value(i));
      if (PaintBrushKnob* pk = dynamic_cast<PaintBrushKnob*>(knob("paint_data"))) {
        if (k->is("paint_fill_layer")) {
          size_t npts = 0;
          { std::lock_guard<std::mutex> lock(_paintMutex); npts = _paintPts.size(); }
          if (npts == 0) warning("flood fill: no source points yet - view the node in a 3D viewer first");
          else pk->fillLayer(unsigned(npts), layer, float(value), col[0], col[1], col[2]);
        }
        else {
          pk->clearLayers(k->is("paint_clear_all") ? -1 : layer);
        }
      }
      return 1;
    }
    if (k->is("showPanel") || k->is("align_mode") || k->is("random_rotate") || k->is("spin") || k->is("paint_density_enable") ||
        k->is("random_scale") || k->is("use_size_attr") || k->is("variant_mode") ||
        k->is("copy_color") || k->is("paint_scale_enable") || k->is("paint_rot_enable") || k->is("scatter_weighting") ||
        k->is("random_offset") || k->is("paint_color_enable") || k->is("paint_layer")) {
      if (Knob* kk = knob("align_attr"))    kk->enable(_alignMode != kAlignNone);
      if (Knob* kk = knob("forward_axis"))  kk->enable(_alignMode == kAlignDirection);
      if (Knob* kk = knob("up"))            kk->enable(_alignMode == kAlignDirection);
      if (Knob* kk = knob("rot_min"))       kk->enable(_randomRotate);
      if (Knob* kk = knob("rot_max"))       kk->enable(_randomRotate);
      if (Knob* kk = knob("roll_rate"))     kk->enable(_spinMode == kSpinRoll);
      if (Knob* kk = knob("roll_channels")) kk->enable(_spinMode == kSpinRoll);
      if (Knob* kk = knob("scale_min"))     kk->enable(_randomScale);
      if (Knob* kk = knob("scale_max"))     kk->enable(_randomScale);
      if (Knob* kk = knob("scale_bias"))    kk->enable(_randomScale);
      if (Knob* kk = knob("scale_shape"))   kk->enable(_randomScale);
      if (Knob* kk = knob("size_attr"))     kk->enable(_useSizeAttr);
      if (Knob* kk = knob("variant_seed"))  kk->enable(_variantMode == kVariantRandom);
      if (Knob* kk = knob("variant_attr"))  kk->enable(_variantMode == kVariantAttribute);
      if (Knob* kk = knob("color_attr"))    kk->enable(_copyColor);
      if (Knob* kk = knob("paint_scale_amount")) kk->enable(_paintScaleEnable);
      if (Knob* kk = knob("scatter_bias"))       kk->enable(_scatterWeighting != kScatterWUniform);
      if (Knob* kk = knob("paint_rot_axis"))     kk->enable(_paintRotEnable);
      if (Knob* kk = knob("paint_rot_amount"))   kk->enable(_paintRotEnable);
      if (Knob* kk = knob("offset_min"))         kk->enable(_randomOffset);
      if (Knob* kk = knob("offset_max"))         kk->enable(_randomOffset);
      if (Knob* kk = knob("paint_color_mode"))   kk->enable(_paintColorEnable);
      if (Knob* kk = knob("paint_color_source")) kk->enable(_paintColorEnable);
      if (Knob* kk = knob("paint_color"))        kk->enable(_paintLayer == kPaintLayerColor);
      if (Knob* kk = knob("paint_heat_max"))     kk->enable(_paintLayer != kPaintLayerColor);
      return 1;
    }
  }
  catch (...) {
  }
  return GeoOp::knob_changed(k);
}

void CopyToPoints::print_info(std::ostream& o)
{
  GeoOp::print_info(o);
  o << "CopyToPoints: " << _statInstances << " copies from " << _statPoints
    << " points using " << _statVariants << " variant(s)";
  if (!_lastError.empty()) o << " [" << _lastError << "]";
  o << std::endl;
  std::lock_guard<std::mutex> lock(_reportMutex);
  if (!_attrReport.empty()) o << _attrReport << std::endl;
}

// --------------------------------------------------------------------------
void CopyToPoints::_validate(bool for_real)
{
  for (int i = 0; i < inputs(); ++i) {
    if (Op* op = Op::input(i)) op->validate(for_real);
  }
  GeoOp::_validate(for_real);
}

void CopyToPoints::appendKnobHash(Hash& h) const
{
  h.append(_mode);
  h.append(_variantMode);
  h.append(_variantSeed);
  h.append(_variantAttr);
  h.append(_alignMode);
  h.append(_alignAttr);
  h.append(_forwardAxis);
  for (int i = 0; i < 3; ++i) { h.append(_up[i]); h.append(_rotate[i]); h.append(_rotMin[i]); h.append(_rotMax[i]); }
  h.append(_randomRotate);
  h.append(_spinMode);
  h.append(_rollRate);
  h.append(_rollChannels);
  h.append(_scale);
  for (int i = 0; i < 3; ++i) { h.append(_scaleXYZ[i]); h.append(_offset[i]); h.append(_offMin[i]); h.append(_offMax[i]); h.append(_offVariance[i]); h.append(_paintColor[i]); }
  h.append(_randomOffset);
  h.append(_paintColorEnable);
  h.append(_paintColorMode);
  h.append(_paintColorSource);
  h.append(_useSizeAttr);
  h.append(_sizeAttr);
  h.append(_scaleAttr);
  h.append(_randomScale);
  h.append(_scaleMin);
  h.append(_scaleMax);
  h.append(_scaleBias);
  h.append(_scaleShape);
  h.append(_seed);
  h.append(_idAttr);
  h.append(_density);
  h.append(_maxInstances);
  h.append(_pointsSource);
  h.append(_maxSourcePoints); h.append(_maxCopyPoints);
  h.append(_copyColor);
  h.append(_colorAttr);
  h.append(_copyAttrs);
  h.append(_keepPoints);
  h.append(_dumpAttributes);
  h.append(_readParticles);
  h.append(_scatterMode);
  h.append(_guideMode);
  h.append(_guideSize);
  h.append(_guideHideCopies); h.append(_guideHeat);
  h.append(_scatterCount);
  h.append(_scatterSeed);
  h.append(_scatterWeighting);
  h.append(_scatterBias);
  h.append(_scatterUsePaint); h.append(_scatterUniformFaces); h.append(_scatterPaintMode); h.append(_scatterPaintCount);
  h.append(_scatterSeparation);
  h.append(_rotVariance);
  h.append(_colorVarHue);
  h.append(_colorVarSat);
  h.append(_colorVarVal);
  h.append(_paintEnable); h.append(_paintLayer); h.append(_paintHeatMax);
  h.append(_paintDensityEnable);
  h.append(_paintScaleEnable);
  h.append(_paintScaleAmount);
  h.append(_paintRotEnable);
  h.append(_paintRotAxis);
  h.append(_paintRotAmount);
  h.append(_paintVariantEnable);
  {
    std::lock_guard<std::mutex> lock(_paintMutex);
    h.append(_paintVersion);
    h.append(_paint.npoints);
  }
}

void CopyToPoints::get_geometry_hash()
{
  // Base: input0 hashes copied into geo_hash[]
  GeoOp::get_geometry_hash();

  Hash h;
  h.reset();
  // Prototype inputs: fold every group hash of every connected geo input.
  for (int i = 1; i < inputs(); ++i) {
    GeoOp* g = dynamic_cast<GeoOp*>(Op::input(i));
    if (!g) { h.append(0); continue; }
    g->validate(false);
    for (int grp = 0; grp < Group_Last; ++grp) h.append(g->hash(grp).getHash());
    h.append(i);
  }
  appendKnobHash(h);
  // The copies depend on the source points at THIS frame; make every group
  // depend on the frame and on the source point/matrix hashes so that a
  // full rebuild happens whenever the particles move.
  h.append(outputContext().frame());
  h.append(geo_hash[Group_Points].getHash());
  h.append(geo_hash[Group_Matrix].getHash());
  h.append(geo_hash[Group_Attributes].getHash());
  h.append(geo_hash[Group_Primitives].getHash());
  h.append(geo_hash[Group_Object].getHash());

  for (int grp = 0; grp < Group_Last; ++grp) geo_hash[grp].append(h);
}

// --------------------------------------------------------------------------
void CopyToPoints::gatherPrototypes(Scene& scene, std::vector<GeometryList>& protoLists, std::vector<ProtoRef>& protos)
{
  protoLists.clear();
  protoLists.resize(size_t(std::max(0, inputs())));
  protos.clear();
  for (int i = 1; i < inputs(); ++i) {
    GeoOp* g = dynamic_cast<GeoOp*>(Op::input(i));
    if (!g) continue;
    GeometryList& lst = protoLists[size_t(i)];
    g->get_geometry(scene, lst);
    for (unsigned o = 0; o < lst.objects(); ++o) {
      const GeoInfo& gi = lst[o];
      if (gi.points() == 0 && gi.primitives() == 0) continue;
      ProtoRef r;
      r.info = &gi;
      r.inputIndex = i;
      r.objectIndex = o;
      protos.push_back(r);
    }
  }
}

// One copy target, gathered from a source vertex or a scattered point.
// processSample() lives in CopyCore.h; this fills its parameter block from the knobs.
ctp::GatherParams CopyToPoints::makeGatherParams(size_t numVariants) const
{
  ctp::GatherParams g;
  g.seed = uint32_t(_seed);
  g.vseed = uint32_t(_variantSeed);
  g.nVar = int(numVariants);
  g.axisFix = axisToPlusZ(_forwardAxis);
  g.upVec = Vector3(static_cast<float>(_up[0]), static_cast<float>(_up[1]), static_cast<float>(_up[2]));
  g.userRot = eulerXYZ(float(_rotate[0]), float(_rotate[1]), float(_rotate[2]));
  g.hasUserRot = (_rotate[0] != 0.0 || _rotate[1] != 0.0 || _rotate[2] != 0.0);
  g.offset = Vector3(static_cast<float>(_offset[0]), static_cast<float>(_offset[1]), static_cast<float>(_offset[2]));
  g.hasOffset = (_offset[0] != 0.0 || _offset[1] != 0.0 || _offset[2] != 0.0);
  g.density = _density;
  g.paintDensityEnable = _paintDensityEnable;
  g.variantMode = _variantMode;
  g.paintVariantEnable = _paintVariantEnable;
  g.colorVarHue = _colorVarHue; g.colorVarSat = _colorVarSat; g.colorVarVal = _colorVarVal;
  g.alignMode = _alignMode;
  g.spinMode = _spinMode; g.rollRate = _rollRate;
  g.randomRotate = _randomRotate;
  for (int i = 0; i < 3; ++i) { g.rotMin[i] = _rotMin[i]; g.rotMax[i] = _rotMax[i]; g.scaleXYZ[i] = _scaleXYZ[i]; }
  g.rotVariance = _rotVariance;
  g.paintRotEnable = _paintRotEnable; g.paintRotAmount = _paintRotAmount; g.paintRotAxis = _paintRotAxis;
  g.scale = _scale;
  g.randomScale = _randomScale; g.scaleMin = _scaleMin; g.scaleMax = _scaleMax;
  g.scaleBias = _scaleBias; g.scaleShape = _scaleShape;
  g.paintScaleEnable = _paintScaleEnable; g.paintScaleAmount = _paintScaleAmount;
  g.paintColorEnable = _paintColorEnable; g.paintColorMode = _paintColorMode;
  g.randomOffset = _randomOffset;
  for (int i = 0; i < 3; ++i) { g.offMin[i] = _offMin[i]; g.offMax[i] = _offMax[i]; g.offVariance[i] = _offVariance[i]; }
  return g;
}


void CopyToPoints::gatherInstances(const GeometryList& pointsList, size_t numVariants, std::vector<InstanceRec>& out)
{
  out.clear();
  const ctp::GatherParams g = makeGatherParams(numVariants);
  const unsigned cap = (_maxInstances > 0) ? unsigned(_maxInstances) : 0xFFFFFFFFu;

  const bool useIdAttr = !trimCopy(_idAttr).empty();
  const std::string idName = trimCopy(_idAttr);
  const std::string alignName = trimCopy(_alignAttr);
  const std::string sizeName = trimCopy(_sizeAttr);
  const std::string scaleName = trimCopy(_scaleAttr);
  const std::string colorName = trimCopy(_colorAttr);
  const std::string variantName = trimCopy(_variantAttr);

  PaintLayers paint;
  {
    std::lock_guard<std::mutex> lock(_paintMutex);
    paint = _paint;
  }
  const bool usePaint = paint.npoints > 0;   // effects are gated individually; the guide shows the weights

  uint32_t running = 0;
  Vector3 centreStore;
  if (_scatterMode != kScatterReplace) {
    for (unsigned o = 0; o < pointsList.objects(); ++o) {
      const GeoInfo& src = pointsList[o];
      unsigned np = src.points();
      if (np == 0) continue;
      const Vector3* P = src.point_array();
      if (!P) continue;
      const Matrix4& srcM = src.matrix;
      const bool srcIdent = srcM.isIdentity();
      const size_t paintBase = (o < _paintBase.size()) ? _paintBase[o] : size_t(-1);

      const bool objectMode = (_pointsSource == kSourceObjects) ||
                              (_pointsSource == kSourceAuto && _inputIsParticles && objectIsMesh(src));
      if (objectMode) {
        Vector3 mn = P[0], mx = P[0];
        for (unsigned i = 1; i < np; ++i) {
          const Vector3& q = P[i];
          if (q.x < mn.x) mn.x = q.x; if (q.y < mn.y) mn.y = q.y; if (q.z < mn.z) mn.z = q.z;
          if (q.x > mx.x) mx.x = q.x; if (q.y > mx.y) mx.y = q.y; if (q.z > mx.z) mx.z = q.z;
        }
        centreStore = (mn + mx) * 0.5f;
        P = &centreStore;
        np = 1;
      }

      const AttribContext* idAc      = (useIdAttr && !objectMode) ? srcAttrib(src, o, idName.c_str()) : nullptr;
      const AttribContext* alignAc   = nullptr;
      if (objectMode)                        alignAc = nullptr;
      else if (_alignMode == kAlignParticle) alignAc = srcAttrib(src, o, kPsOrient);
      else if (_alignMode != kAlignNone)     alignAc = srcAttrib(src, o, alignName.c_str());
      const AttribContext* sizeAc    = (_useSizeAttr && !objectMode) ? srcAttrib(src, o, sizeName.c_str()) : nullptr;
      const AttribContext* rollVelAc = nullptr;
      const AttribContext* rollP0Ac  = nullptr;
      const AttribContext* rollChAc  = nullptr;
      if (_spinMode == kSpinRoll && !objectMode) {
        rollVelAc = srcAttrib(src, o, kPsVel);
        if (!rollVelAc && _alignMode == kAlignDirection) rollVelAc = alignAc;
        rollP0Ac = srcAttrib(src, o, kPsInitP);
        rollChAc = srcAttrib(src, o, kPsChannels);
      }
      const AttribContext* scaleAc   = (scaleName.empty() || objectMode) ? nullptr : srcAttrib(src, o, scaleName.c_str());
      const AttribContext* colorAc   = (_copyColor && !objectMode) ? srcAttrib(src, o, colorName.c_str()) : nullptr;
      const AttribContext* variantAc = (_variantMode == kVariantAttribute && !objectMode) ? srcAttrib(src, o, variantName.c_str()) : nullptr;
      if (objectMode && _copyColor) {
        const AttribContext* oc = src.get_group_attribcontext(Group_Object, colorName.c_str());
        if (oc && !oc->empty()) colorAc = oc;
      }

      for (unsigned p = 0; p < np; ++p, ++running) {
        if (out.size() >= cap) return;
        PointSample ps;
        ps.order = running;
        ps.id = running;
        int idv = 0;
        if (idAc && attribIntAt(idAc, p, idv)) ps.id = uint32_t(idv);
        ps.srcObject = o; ps.srcPoint = p;
        ps.Pw = srcIdent ? P[p] : srcM.transform(P[p]);
        if (usePaint && !objectMode && paintBase != size_t(-1) && (paintBase + p) < paint.npoints) {
          ps.hasPaint = true;
          for (int l = 0; l < kPaintLayerCount; ++l) ps.w[l] = paint.get(l, paintBase + p);
        }
        int va = 0;
        if (variantAc && attribIntAt(variantAc, p, va)) { ps.hasVariantAttr = true; ps.variantAttr = va; }
        Vector4 c;
        if (colorAc && attribVec4At(colorAc, p, c)) { ps.hasColor = true; ps.color = c; }
        if (alignAc) {
          if (_alignMode == kAlignDirection) {
            Vector3 d;
            if (attribVec3At(alignAc, p, d)) { if (!srcIdent) d = srcM.vtransform(d); ps.hasDir = true; ps.dir = d; }
          }
          else if (_alignMode == kAlignQuaternion || _alignMode == kAlignParticle) {
            Vector4 q;
            if (attribVec4At(alignAc, p, q)) { ps.hasQuat = true; ps.quat = q; }
          }
          else if (_alignMode == kAlignEuler) {
            Vector3 e;
            if (attribVec3At(alignAc, p, e)) { ps.hasEuler = true; ps.euler = e; }
          }
        }
        if (rollVelAc) {
          bool doRoll = true;
          if (_rollChannels != 0) { int ch = 0; doRoll = rollChAc && attribIntAt(rollChAc, p, ch) && ((ch & _rollChannels) != 0); }
          Vector3 v;
          if (doRoll && attribVec3At(rollVelAc, p, v)) {
            if (!srcIdent) v = srcM.vtransform(v);
            ps.rollOK = true; ps.rollVel = v;
            Vector3 p0;
            ps.rollDist = (rollP0Ac && attribVec3At(rollP0Ac, p, p0)) ? (P[p] - p0).length() : 0.0f;
          }
        }
        float sz;
        if (sizeAc && attribFloatAt(sizeAc, p, sz)) { ps.hasSize = true; ps.size = sz; }
        Vector3 sv;
        if (scaleAc && attribVec3At(scaleAc, p, sv)) { ps.hasScaleVec = true; ps.scaleVec = sv; }

        InstanceRec rec;
        if (processSample(ps, g, rec)) out.push_back(rec);
      }
    }
  }

  // scattered points (face samples)
  if (_scatterMode != kScatterOff && !_scatter.empty()) {
    const bool dirIsNormal = (_alignMode == kAlignDirection) && (alignName == "N" || alignName == "normal" || alignName.empty());
    for (size_t i = 0; i < _scatter.size(); ++i, ++running) {
      if (out.size() >= cap) return;
      const ScatterPoint& sp = _scatter[i];
      PointSample ps;
      ps.order = running;
      ps.id = 0x40000000u + uint32_t(i);
      ps.srcObject = sp.srcObject;
      ps.srcPoint = sp.i0;   // nearest source vertex for attribute copies
      ps.Pw = sp.P;
      if (dirIsNormal) { ps.hasDir = true; ps.dir = sp.N; }
      if (usePaint && sp.i2 < paint.npoints) {
        ps.hasPaint = true;
        for (int l = 0; l < kPaintLayerCount; ++l)
          ps.w[l] = paint.get(l, sp.i0) * sp.b0 + paint.get(l, sp.i1) * sp.b1 + paint.get(l, sp.i2) * sp.b2;
      }
      if (_copyColor && sp.srcObject < _meshHasCf.size() && _meshHasCf[sp.srcObject] && sp.i2 < _meshCf.size()) {
        ps.hasColor = true;
        ps.color = _meshCf[sp.i0] * sp.b0 + _meshCf[sp.i1] * sp.b1 + _meshCf[sp.i2] * sp.b2;
      }
      InstanceRec rec;
      if (processSample(ps, g, rec)) out.push_back(rec);
    }
  }
}

// --------------------------------------------------------------------------
// instances mode: one output object per copy sharing the prototype's data.
// --------------------------------------------------------------------------
void CopyToPoints::emitInstances(GeometryList& out, unsigned& obj, const std::vector<ProtoRef>& protos,
                                 const std::vector<InstanceRec>& inst, const GeometryList& pointsList)
{
  const std::vector<std::string> extraNames = copyAttrNames();

  for (size_t k = 0; k < inst.size(); ++k) {
    const InstanceRec& rec = inst[k];
    const GeoInfo& proto = *protos[size_t(rec.variant)].info;
    const GeoInfo::Cache* pc = proto.get_cache_pointer();

    resetCacheEntry(obj);
    out.add_object(int(obj));
    GeoInfo& gi = out[obj];
    // (looked up after add_object: with keep_points, pointsList aliases 'out'
    // and add_object may reallocate its storage)
    const GeoInfo& srcInfo = pointsList[rec.srcObject];

    // Build the attribute list: everything from the prototype, plus per-copy
    // object attributes.  AttribContext copies share the AttributePtr, so
    // this is cheap and does not duplicate the prototype's data.
    AttribContextList attrs;
    attrs.reserve(pc->attributes.size() + 2 + extraNames.size());
    for (size_t a = 0; a < pc->attributes.size(); ++a) {
      const AttribContext& ac = pc->attributes[a];
      if (rec.hasColor && ac.name && std::strcmp(ac.name, kColorAttrName) == 0) continue; // overridden below
      attrs.push_back(ac);
    }
    if (rec.hasColor) {
      AttribContext ac = makeAttribContext(Group_Object, kColorAttrName, VECTOR4_ATTRIB, 1);
      ac.attribute->vector4(0) = rec.color;
      attrs.push_back(ac);
    }
    for (size_t e = 0; e < extraNames.size(); ++e) {
      const AttribContext* sac = srcAttrib(srcInfo, rec.srcObject, extraNames[e].c_str());
      if (!sac || !sac->attribute || rec.srcPoint >= sac->attribute->size()) continue;
      AttribContext ac = makeAttribContext(Group_Object, internName(extraNames[e]), sac->type, 1);
      copyAttribElement(ac, 0, *sac, rec.srcPoint);
      // drop any same-named object attribute inherited from the prototype
      for (size_t a = 0; a < attrs.size(); ++a) {
        if (attrs[a].group == Group_Object && attrs[a].name && std::strcmp(attrs[a].name, ac.name) == 0) {
          attrs.erase(attrs.begin() + long(a));
          break;
        }
      }
      attrs.push_back(ac);
    }

    // Share the geometry: write the ref-counted pointers into both our own
    // cache entry (what Nuke re-synchronises the output from when nothing
    // changed) and the output GeoInfo (what the renderer/viewer read now).
    if (cache_list.size() <= obj) cache_list.resize(obj + 1);
    GeoInfo::Cache& mine = cache_list[obj];
    mine.type       = GeoInfo::Cache::REFERENCE;
    mine.primitives = pc->primitives;
    mine.points     = pc->points;
    mine.vertices   = pc->vertices;
    mine.attributes = attrs;
    mine.bbox       = pc->bbox;

    GeoInfo::Cache* gc = const_cast<GeoInfo::Cache*>(gi.get_cache_pointer());
    gc->type       = GeoInfo::Cache::REFERENCE;
    gc->primitives = pc->primitives;
    gc->points     = pc->points;
    gc->vertices   = pc->vertices;
    gc->attributes = attrs;
    gc->bbox       = pc->bbox;

    // Public per-object state
    gi.matrix             = rec.xform * proto.matrix;
    gi.material           = proto.material;
    gi.useMaterialContext = proto.useMaterialContext;
    gi.materialContext    = proto.materialContext;
    gi.renderState        = proto.renderState;
    // display3d / render_mode: keep what add_object() derived from this
    // node's own display/render knobs.
    gi.valid_source_node_gl_color = proto.valid_source_node_gl_color;
    gi.source_node_gl_color       = proto.source_node_gl_color;
    gi.selected  = false;
    gi.selectable = selectable();

    ++obj;
  }
}

// --------------------------------------------------------------------------
// bake mode: one output object per prototype containing every copy.
// --------------------------------------------------------------------------
void CopyToPoints::emitBaked(GeometryList& out, unsigned& obj, const std::vector<ProtoRef>& protos,
                             const std::vector<InstanceRec>& inst, const GeometryList& pointsList)
{
  const std::vector<std::string> extraNames = copyAttrNames();
  const bool anyColor = std::any_of(inst.begin(), inst.end(), [](const InstanceRec& r) { return r.hasColor; });

  // bucket instances per variant
  std::vector<std::vector<unsigned> > buckets(protos.size());
  for (size_t k = 0; k < inst.size(); ++k) buckets[size_t(inst[k].variant)].push_back(unsigned(k));

  for (size_t v = 0; v < protos.size(); ++v) {
    const std::vector<unsigned>& bucket = buckets[v];
    if (bucket.empty()) continue;
    const GeoInfo& proto = *protos[v].info;
    const GeoInfo::Cache* pc = proto.get_cache_pointer();
    const unsigned protoPoints = proto.points();
    const unsigned protoPrims = proto.primitives();
    const unsigned protoVerts = proto.vertices();
    const Vector3* PP = proto.point_array();
    const size_t nCopies = bucket.size();

    resetCacheEntry(obj);
    out.add_object(int(obj));
    GeoInfo& gi = out[obj];

    // ---- points --------------------------------------------------------
    PointList* pts = out.writable_points(int(obj));
    if (!pts) { ++obj; continue; }
    pts->clear();
    pts->reserve(size_t(protoPoints) * nCopies);
    std::vector<Matrix4> mats(nCopies);
    for (size_t c = 0; c < nCopies; ++c) {
      const InstanceRec& rec = inst[bucket[c]];
      const Matrix4 M = rec.xform * proto.matrix;
      mats[c] = M;
      for (unsigned p = 0; p < protoPoints; ++p) pts->push_back(M.transform(PP[p]));
    }

    // ---- primitives ------------------------------------------------------
    for (size_t c = 0; c < nCopies; ++c) {
      const int base = int(c * protoPoints);
      for (unsigned q = 0; q < protoPrims; ++q) {
        const Primitive* sp = proto.primitive(q);
        if (!sp) continue;
        Primitive* d = sp->duplicate();
        if (!d) continue;
        if (base) d->offset_point_indices(base);
        out.add_primitive(int(obj), d);
      }
    }

    // ---- attributes ------------------------------------------------------
    // Points / vertices / primitives groups: replicate per copy (normals are
    // rotated).  Object group: copied once.  Colour override: point Cf.
    for (size_t a = 0; a < pc->attributes.size(); ++a) {
      const AttribContext& sac = pc->attributes[a];
      if (!sac.attribute || sac.empty() || !sac.name) continue;
      const bool isColor = std::strcmp(sac.name, kColorAttrName) == 0;
      if (isColor && anyColor) continue;   // replaced by per-copy point colour below
      unsigned perCopy = 0;
      switch (sac.group) {
        case Group_Points:     perCopy = protoPoints; break;
        case Group_Vertices:   perCopy = protoVerts; break;
        case Group_Primitives: perCopy = protoPrims; break;
        case Group_Object:     perCopy = 0; break;
        default: continue;
      }
      const char* name = internName(sac.name);
      Attribute* dst = out.writable_attribute(int(obj), sac.group, name, sac.type);
      if (!dst) continue;
      if (sac.group == Group_Object) {
        dst->resize(1);
        dst->copy(0, *sac.attribute, 0);
        continue;
      }
      if (sac.attribute->size() < perCopy) perCopy = sac.attribute->size();
      dst->resize(size_t(perCopy) * nCopies);
      const bool isNormal = (sac.type == NORMAL_ATTRIB) ||
                            (sac.type == VECTOR3_ATTRIB && std::strcmp(sac.name, kNormalAttrName) == 0);
      for (size_t c = 0; c < nCopies; ++c) {
        const unsigned dbase = unsigned(c * perCopy);
        if (isNormal) {
          const Matrix4& M = mats[c];
          for (unsigned i = 0; i < perCopy; ++i) {
            const Vector3 n = (sac.type == NORMAL_ATTRIB) ? sac.attribute->normal(i) : sac.attribute->vector3(i);
            Vector3 nn = M.ntransform(n);
            nn.normalize();
            if (sac.type == NORMAL_ATTRIB) dst->normal(dbase + i) = nn;
            else dst->vector3(dbase + i) = nn;
          }
        }
        else {
          dst->copy(int(dbase), *sac.attribute, 0, int(perCopy));
        }
      }
    }

    if (anyColor) {
      Attribute* cf = out.writable_attribute(int(obj), Group_Points, kColorAttrName, VECTOR4_ATTRIB);
      if (cf) {
        cf->resize(size_t(protoPoints) * nCopies);
        // if the prototype had a point colour keep it where the copy has none
        const AttribContext* protoCf = proto.get_typed_group_attribcontext(Group_Points, kColorAttrName, VECTOR4_ATTRIB);
        for (size_t c = 0; c < nCopies; ++c) {
          const InstanceRec& rec = inst[bucket[c]];
          const unsigned dbase = unsigned(c * protoPoints);
          for (unsigned p = 0; p < protoPoints; ++p) {
            Vector4 col(1.0f, 1.0f, 1.0f, 1.0f);
            if (rec.hasColor) col = rec.color;
            else if (protoCf && protoCf->attribute && p < protoCf->attribute->size()) col = protoCf->attribute->vector4(p);
            cf->vector4(dbase + p) = col;
          }
        }
      }
    }

    // extra per-copy attributes become POINT attributes in bake mode
    for (size_t e = 0; e < extraNames.size(); ++e) {
      const char* name = internName(extraNames[e]);
      // discover type from the first copy that has it
      const AttribContext* sample = nullptr;
      for (size_t c = 0; c < nCopies && !sample; ++c) {
        const InstanceRec& rec = inst[bucket[c]];
        sample = srcAttrib(pointsList[rec.srcObject], rec.srcObject, name);
      }
      if (!sample) continue;
      Attribute* dst = out.writable_attribute(int(obj), Group_Points, name, sample->type);
      if (!dst) continue;
      dst->resize(size_t(protoPoints) * nCopies);
      for (size_t c = 0; c < nCopies; ++c) {
        const InstanceRec& rec = inst[bucket[c]];
        const AttribContext* sac = srcAttrib(pointsList[rec.srcObject], rec.srcObject, name);
        if (!sac || sac->type != sample->type || rec.srcPoint >= sac->attribute->size()) continue;
        const unsigned dbase = unsigned(c * protoPoints);
        for (unsigned p = 0; p < protoPoints; ++p) dst->copy(int(dbase + p), *sac->attribute, int(rec.srcPoint));
      }
    }

    // ---- per-object state ---------------------------------------------
    // add_object() copied our cache entry into gi BEFORE the primitives were
    // (re)added, so its vertex count is stale on a rebuild (it still says what
    // the previous build had).  Keep it in sync or the renderer sizes vertex
    // data too small and crashes on the next, larger build.
    if (cache_list.size() > obj) {
      // Same story for the point/primitive/attribute references: writable_*()
      // may copy-on-write into our cache entry (the GeoInfo copy holds a
      // reference too), leaving gi pointing at the previous build's arrays.
      GeoInfo::Cache* gc = const_cast<GeoInfo::Cache*>(gi.get_cache_pointer());
      const GeoInfo::Cache& mine = cache_list[obj];
      gc->points     = mine.points;
      gc->primitives = mine.primitives;
      gc->attributes = mine.attributes;
      gc->vertices   = mine.vertices;
      gc->bbox       = mine.bbox;
    }
    gi.matrix.makeIdentity();
    gi.material           = proto.material;
    gi.useMaterialContext = proto.useMaterialContext;
    gi.materialContext    = proto.materialContext;
    gi.renderState        = proto.renderState;
    // display3d / render_mode: keep what add_object() derived from this
    // node's own display/render knobs.
    gi.valid_source_node_gl_color = proto.valid_source_node_gl_color;
    gi.source_node_gl_color       = proto.source_node_gl_color;
    gi.selected  = false;
    gi.selectable = selectable();

    ++obj;
  }
}

// --------------------------------------------------------------------------
// Compact human-readable list of the attributes on the inputs.
std::string CopyToPoints::buildAttrReport(const GeometryList& pointsList, const std::vector<ProtoRef>& protos) const
{
  std::ostringstream o;
  auto listInfo = [&o](const GeoInfo& gi) {
    const int n = gi.get_attribcontext_count();
    int shown = 0;
    for (int a = 0; a < n; ++a) {
      const AttribContext* ac = gi.get_attribcontext(a);
      if (!ac || !ac->name) continue;
      o << "    " << ac->name << " : "
        << ((ac->group >= 0 && ac->group < Group_Last) ? group_names[ac->group] : "?")
        << " / " << Attribute::type_string(ac->type)
        << " / " << (ac->attribute ? ac->attribute->size() : 0) << "\n";
      ++shown;
    }
    if (!shown) o << "    (no attributes)\n";
  };

  unsigned total = 0;
  for (unsigned i = 0; i < pointsList.objects(); ++i) total += pointsList[i].points();
  o << "points input: " << pointsList.objects() << " object(s), " << total << " point(s)\n";
  if (!_sourceNote.empty()) o << "  NOTE: " << _sourceNote << "\n";
  for (unsigned i = 0; i < pointsList.objects(); ++i) {
    const GeoInfo& gi = pointsList[i];
    o << "  object " << i << ": " << gi.points() << " pts, " << gi.primitives() << " prims";
    if (gi.primitives() && gi.primitive(0)) o << " (" << gi.primitive(0)->Class() << ")";
    o << "\n";
    listInfo(gi);
    if (i == 0) {
      for (size_t a = 0; a < _psAttribs.size(); ++a) {
        const AttribContext& ac = _psAttribs[a];
        o << "    " << ac.name << " : particle system / " << Attribute::type_string(ac.type)
          << " / " << (ac.attribute ? ac.attribute->size() : 0) << "\n";
      }
    }
  }
  if (!_psInfo.empty()) o << _psInfo << "\n";
  if (!_scatterInfo.empty()) o << _scatterInfo << "\n";
  o << "prototypes: " << protos.size() << " variant(s)\n";
  for (size_t v = 0; v < protos.size(); ++v) {
    const GeoInfo& gi = *protos[v].info;
    o << "  variant " << v << " (geo" << protos[v].inputIndex << " obj " << protos[v].objectIndex << "): "
      << gi.points() << " pts, " << gi.primitives() << " prims, material " << (gi.material ? "yes" : "no") << "\n";
    listInfo(gi);
  }
  return o.str();
}

// --------------------------------------------------------------------------
// Read the particle system behind the points input (ParticleToGeo or any
// particle node is a ParticleRender) and expose its per-particle fields as
// point attributes of source object 0.  Points are matched to particles via
// the 'id' point attribute ParticleToGeo writes; without it the active
// particles are assumed to be in point order.
// --------------------------------------------------------------------------
void CopyToPoints::buildParticleAttribs(const GeometryList& pointsList)
{
  ParticleRender* pr = dynamic_cast<ParticleRender*>(input0());
  if (!pr || pointsList.objects() == 0) return;
  float prevTime = 0.0f, outTime = 0.0f;
  ParticleSystem* ps = pr->getParticleSystem(prevTime, outTime);
  if (!ps) { _psInfo = "particle system: not available"; return; }

  const GeoInfo& src = pointsList[0];
  const unsigned np = src.points();
  if (np == 0) return;
  const unsigned nParticles = ps->numParticles();

  // particle index per point
  std::vector<int> pidx(np, -1);
  const AttribContext* idAc = findPointAttrib(src, "id");
  const bool* active = ps->particleActive();
  const int* pid = ps->particleId();
  if (idAc && pid) {
    // id -> particle index (ids are small monotonic ints; use a map for safety)
    std::vector<std::pair<int, unsigned> > table;
    table.reserve(nParticles);
    for (unsigned i = 0; i < nParticles; ++i) {
      if (active && !active[i]) continue;
      table.push_back(std::make_pair(pid[i], i));
    }
    std::sort(table.begin(), table.end());
    for (unsigned p = 0; p < np; ++p) {
      int want = 0;
      if (!attribIntAt(idAc, p, want)) continue;
      std::vector<std::pair<int, unsigned> >::const_iterator it =
        std::lower_bound(table.begin(), table.end(), std::make_pair(want, 0u));
      if (it != table.end() && it->first == want) pidx[p] = int(it->second);
    }
  }
  else {
    unsigned p = 0;
    for (unsigned i = 0; i < nParticles && p < np; ++i) {
      if (active && !active[i]) continue;
      pidx[p++] = int(i);
    }
  }

  const Vector3* vel   = ps->particleVelocity();
  const Vector3* lastP = ps->particleLastPosition();
  const Quaternion4f* ori = ps->particleOrientation();
  const Vector3* rax   = ps->particleRotationAxis();
  const float* rang    = ps->particleRotationAngle();
  const float* rvel    = ps->particleRotationVelocity();
  const float* start   = ps->particleStartTime();
  const float* life    = ps->particleLife();
  const float* mass    = ps->particleMass();
  const Vector3* size3 = ps->particleSize();
  const ParticleSystem::BounceInfo* bounce = ps->particleBounceInfo();
  const Vector3* initP = ps->particleInitialPosition();
  const ParticleChannelSet* chans = ps->particleChannels();
  const float sysTime = ps->systemTime();

  AttribContext aVel   = makeAttribContext(Group_Points, kPsVel,      VECTOR3_ATTRIB, np);
  AttribContext aLast  = makeAttribContext(Group_Points, kPsLastP,    VECTOR3_ATTRIB, np);
  AttribContext aOri   = makeAttribContext(Group_Points, kPsOrient,   VECTOR4_ATTRIB, np);
  AttribContext aRax   = makeAttribContext(Group_Points, kPsRotAxis,  VECTOR3_ATTRIB, np);
  AttribContext aRang  = makeAttribContext(Group_Points, kPsRotAngle, FLOAT_ATTRIB,   np);
  AttribContext aRvel  = makeAttribContext(Group_Points, kPsRotVel,   FLOAT_ATTRIB,   np);
  AttribContext aAge   = makeAttribContext(Group_Points, kPsAge,      FLOAT_ATTRIB,   np);
  AttribContext aLife  = makeAttribContext(Group_Points, kPsLife,     FLOAT_ATTRIB,   np);
  AttribContext aMass  = makeAttribContext(Group_Points, kPsMass,     FLOAT_ATTRIB,   np);
  AttribContext aSize  = makeAttribContext(Group_Points, kPsSize3,    VECTOR3_ATTRIB, np);
  AttribContext aBnc   = makeAttribContext(Group_Points, kPsBounce,   INT_ATTRIB,     np);
  AttribContext aInit  = makeAttribContext(Group_Points, kPsInitP,    VECTOR3_ATTRIB, np);
  AttribContext aChan  = makeAttribContext(Group_Points, kPsChannels, INT_ATTRIB,     np);
  AttribContext aSpeed = makeAttribContext(Group_Points, kPsSpeed,    FLOAT_ATTRIB,   np);

  unsigned matched = 0, bounced = 0;
  unsigned chanCount[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
  float minY = 1e30f, maxY = -1e30f;
  for (unsigned p = 0; p < np; ++p) {
    const int i = pidx[p];
    Vector3 v(0, 0, 0), lp(0, 0, 0), ax(0, 1, 0), sz(1, 1, 1), ip(0, 0, 0);
    int ch = 0;
    Vector4 q(0, 0, 0, 1);
    float ang = 0, rv = 0, age = 0, lf = 0, ms = 1;
    int bf = 0;
    if (i >= 0) {
      ++matched;
      if (vel)   v  = vel[i];
      if (lastP) lp = lastP[i];
      if (ori)   q  = Vector4(ori[i].vx, ori[i].vy, ori[i].vz, ori[i].s);
      if (rax)   ax = rax[i];
      if (rang)  ang = rang[i];
      if (rvel)  rv = rvel[i];
      if (start) age = sysTime - start[i];
      if (life)  lf = life[i];
      if (mass)  ms = mass[i];
      if (size3) sz = size3[i];
      if (bounce) { bf = int(bounce[i].flags); if (bf) ++bounced; }
      if (initP) ip = initP[i];
      if (chans) { ch = int(chans[i]); for (int b = 0; b < 8; ++b) if (ch & (1 << b)) ++chanCount[b]; }
      { const float y = src.point_array() ? src.point_array()[p].y : 0.0f; if (y < minY) minY = y; if (y > maxY) maxY = y; }
    }
    aVel.attribute->vector3(p)  = v;
    aLast.attribute->vector3(p) = lp;
    aOri.attribute->vector4(p)  = q;
    aRax.attribute->vector3(p)  = ax;
    aRang.attribute->flt(p)     = ang;
    aRvel.attribute->flt(p)     = rv;
    aAge.attribute->flt(p)      = age;
    aLife.attribute->flt(p)     = lf;
    aMass.attribute->flt(p)     = ms;
    aSize.attribute->vector3(p) = sz;
    aBnc.attribute->integer(p)  = bf;
    aInit.attribute->vector3(p) = ip;
    aChan.attribute->integer(p) = ch;
    aSpeed.attribute->flt(p)    = v.length();
  }
  _psAttribs.push_back(aVel);
  _psAttribs.push_back(aLast);
  _psAttribs.push_back(aOri);
  _psAttribs.push_back(aRax);
  _psAttribs.push_back(aRang);
  _psAttribs.push_back(aRvel);
  _psAttribs.push_back(aAge);
  _psAttribs.push_back(aLife);
  _psAttribs.push_back(aMass);
  _psAttribs.push_back(aSize);
  _psAttribs.push_back(aBnc);
  _psAttribs.push_back(aInit);
  _psAttribs.push_back(aChan);
  _psAttribs.push_back(aSpeed);

  std::ostringstream o;
  o << "particle system (read directly): " << nParticles << " particle slot(s), " << matched << " of " << np
    << " point(s) matched" << (idAc ? " by id" : " by order") << ", " << bounced << " flagged bounce this frame, time "
    << outTime << ", channels a..h:";
  for (int b = 0; b < 8; ++b) o << " " << char('a' + b) << "=" << chanCount[b];
  if (matched) o << ", y range " << minY << ".." << maxY;
  o << "\n"
    << "  virtual point attributes: vel, speed, lastP, initialP, orient (quat xyzw), rotaxis, rotangle, rotvel, age, life, mass, psize, bounce (int flags), channels (mask a=1 b=2 ...)";
  _psInfo = o.str();
}

// --------------------------------------------------------------------------
// Guide geometry: viewer-only objects (render mode off) showing the copy
// positions and, optionally, each copy's local up axis.
// --------------------------------------------------------------------------
void CopyToPoints::emitGuide(GeometryList& out, unsigned& obj, const std::vector<InstanceRec>& inst)
{
  if (inst.empty()) return;
  const unsigned n = unsigned(inst.size());

  // ---- points ------------------------------------------------------------
  resetCacheEntry(obj);
  out.add_object(int(obj));
  {
    PointList* pts = out.writable_points(int(obj));
    if (pts) {
      pts->clear();
      pts->reserve(n);
      for (unsigned i = 0; i < n; ++i) pts->push_back(inst[i].xform.translation());
      out.add_primitive(int(obj), new Particles(Point::POINT, n, 0));
      Attribute* cf = out.writable_attribute(int(obj), Group_Points, kColorAttrName, VECTOR4_ATTRIB);
      if (cf) {
        cf->resize(n);
        float hmax = float(_paintHeatMax);
        if (hmax <= 0.0f) { std::lock_guard<std::mutex> lock(_paintMutex); hmax = _paint.layerMax(_paintLayer); }
        if (hmax <= 0.0f) hmax = 1.0f;
        for (unsigned i = 0; i < n; ++i) {
          float r, g, b;
          guideColor(inst[i], _guideHeat, _paintLayer, hmax, r, g, b);
          cf->vector4(i) = Vector4(r, g, b, 1.0f);
        }
      }
      GeoInfo& gi = out[obj];
      if (cache_list.size() > obj) {
        GeoInfo::Cache* gc = const_cast<GeoInfo::Cache*>(gi.get_cache_pointer());
        const GeoInfo::Cache& mine = cache_list[obj];
        gc->points = mine.points; gc->primitives = mine.primitives; gc->attributes = mine.attributes;
        gc->vertices = mine.vertices; gc->bbox = mine.bbox;
      }
      gi.matrix.makeIdentity();
      gi.material = nullptr;
      gi.render_mode = RENDER_OFF;
      gi.display3d = DISPLAY_SOLID;
      gi.selectable = false;
    }
  }
  ++obj;

  // ---- up-axis lines --------------------------------------------------------
  if (_guideMode == kGuidePointsAxes) {
    resetCacheEntry(obj);
    out.add_object(int(obj));
    PointList* pts = out.writable_points(int(obj));
    if (pts) {
      pts->clear();
      pts->reserve(size_t(n) * 2);
      const float len = float(_guideSize);
      for (unsigned i = 0; i < n; ++i) {
        const Vector3 p0 = inst[i].xform.translation();
        Vector3 up = inst[i].xform.y_axis();
        const float ul = up.length();
        if (ul > 1e-12f) up = up * (1.0f / ul);
        pts->push_back(p0);
        pts->push_back(p0 + up * len);
        out.add_primitive(int(obj), new DD::Image::Polygon(int(i * 2), int(i * 2 + 1), false));
      }
      Attribute* cf = out.writable_attribute(int(obj), Group_Points, kColorAttrName, VECTOR4_ATTRIB);
      if (cf) {
        cf->resize(size_t(n) * 2);
        for (unsigned i = 0; i < n * 2; ++i) cf->vector4(i) = Vector4(0.9f, 0.9f, 0.2f, 1.0f);
      }
      GeoInfo& gi = out[obj];
      if (cache_list.size() > obj) {
        GeoInfo::Cache* gc = const_cast<GeoInfo::Cache*>(gi.get_cache_pointer());
        const GeoInfo::Cache& mine = cache_list[obj];
        gc->points = mine.points; gc->primitives = mine.primitives; gc->attributes = mine.attributes;
        gc->vertices = mine.vertices; gc->bbox = mine.bbox;
      }
      gi.matrix.makeIdentity();
      gi.material = nullptr;
      gi.render_mode = RENDER_OFF;
      gi.display3d = DISPLAY_WIREFRAME;
      gi.selectable = false;
    }
    ++obj;
  }
}

void CopyToPoints::dumpAttributes(const GeometryList& pointsList, const std::vector<ProtoRef>& protos) const
{
  std::string dir;
  if (const char* t = std::getenv("TEMP")) dir = t;
  else if (const char* t2 = std::getenv("TMP")) dir = t2;
  else if (const char* t3 = std::getenv("TMPDIR")) dir = t3;
#ifndef _WIN32
  if (dir.empty()) dir = "/tmp";
#endif
  else dir = ".";
  for (size_t i = 0; i < dir.size(); ++i) if (dir[i] == '\\') dir[i] = '/';
  const std::string path = dir + "/CopyToPoints_attributes.txt";
  std::ofstream f(path.c_str(), std::ios::out | std::ios::trunc);
  if (!f) return;

  auto dumpInfo = [&f](const GeoInfo& gi) {
    f << "    points=" << gi.points() << " primitives=" << gi.primitives()
      << " vertices=" << gi.vertices() << " material=" << (gi.material ? "yes" : "no")
      << " matrixIdentity=" << (gi.matrix.isIdentity() ? "yes" : "no") << "\n";
    for (unsigned q = 0; q < std::min(gi.primitives(), 3u); ++q) {
      const Primitive* p = gi.primitive(q);
      if (p) f << "    prim[" << q << "] class=" << p->Class() << " vertices=" << p->vertices() << " faces=" << p->faces() << "\n";
    }
    const int n = gi.get_attribcontext_count();
    for (int a = 0; a < n; ++a) {
      const AttribContext* ac = gi.get_attribcontext(a);
      if (!ac) continue;
      f << "    attr '" << (ac->name ? ac->name : "?") << "' group=" << ac->group
        << " (" << (ac->group >= 0 && ac->group < Group_Last ? group_names[ac->group] : "?") << ")"
        << " type=" << Attribute::type_string(ac->type)
        << " size=" << (ac->attribute ? ac->attribute->size() : 0)
        << " varying=" << (ac->varying ? 1 : 0) << "\n";
      if (ac->attribute && ac->attribute->size() > 0) {
        f << "      first: ";
        switch (ac->type) {
          case FLOAT_ATTRIB:   f << ac->attribute->flt(0); break;
          case INT_ATTRIB:     f << ac->attribute->integer(0); break;
          case VECTOR2_ATTRIB: { const Vector2& v = ac->attribute->vector2(0); f << v.x << " " << v.y; break; }
          case VECTOR3_ATTRIB: { const Vector3& v = ac->attribute->vector3(0); f << v.x << " " << v.y << " " << v.z; break; }
          case NORMAL_ATTRIB:  { const Vector3& v = ac->attribute->normal(0); f << v.x << " " << v.y << " " << v.z; break; }
          case VECTOR4_ATTRIB: { const Vector4& v = ac->attribute->vector4(0); f << v.x << " " << v.y << " " << v.z << " " << v.w; break; }
          default: f << "(n/a)"; break;
        }
        f << "\n";
      }
    }
  };

  f << "CopyToPoints attribute dump (node " << node_name() << ", frame " << outputContext().frame() << ")\n";
  static unsigned sBuildCounter = 0;   // how many times geometry_engine dumped (debug aid)
  ++sBuildCounter;
  f << "mode=" << (_mode == kModeBake ? "bake" : "instances") << " points=" << _statPoints
    << " variants=" << _statVariants << " copies=" << _statInstances
    << " build=" << sBuildCounter << "\n";
  if (!_lastError.empty()) f << "note=" << _lastError << "\n";
  if (!_scatterInfo.empty()) f << "scatter=" << _scatterInfo << "\n";
  f << "== points input: " << pointsList.objects() << " object(s)\n";
  for (unsigned o = 0; o < pointsList.objects(); ++o) {
    f << "  object " << o << "\n";
    dumpInfo(pointsList[o]);
  }
  if (!_psInfo.empty()) {
    f << "== " << _psInfo << "\n";
    for (size_t a = 0; a < _psAttribs.size() && pointsList.objects() > 0; ++a) {
      const AttribContext& ac = _psAttribs[a];
      f << "    virtual '" << ac.name << "' type=" << Attribute::type_string(ac.type) << " size=" << ac.attribute->size();
      if (ac.attribute->size() > 0) {
        f << " first: ";
        switch (ac.type) {
          case FLOAT_ATTRIB:   f << ac.attribute->flt(0); break;
          case INT_ATTRIB:     f << ac.attribute->integer(0); break;
          case VECTOR3_ATTRIB: { const Vector3& v = ac.attribute->vector3(0); f << v.x << " " << v.y << " " << v.z; break; }
          case VECTOR4_ATTRIB: { const Vector4& v = ac.attribute->vector4(0); f << v.x << " " << v.y << " " << v.z << " " << v.w; break; }
          default: break;
        }
      }
      f << "\n";
    }
  }
  f << "== prototypes: " << protos.size() << "\n";
  for (size_t v = 0; v < protos.size(); ++v) {
    f << "  variant " << v << " (input geo" << protos[v].inputIndex << ", object " << protos[v].objectIndex << ")\n";
    dumpInfo(*protos[v].info);
  }
  f.close();
}

// --------------------------------------------------------------------------
// The velocity always travels with the copies, whether or not it was asked for.
//
// Downstream motion blur needs it and cannot get it any other way: Nuke's
// particle geometry reaches a renderer carrying id, Cf and size and NO velocity
// - VEL_ref empty - so a renderer is left pairing copies between the two ends of
// the shutter and guessing which went where.  That guess fails exactly where
// particles are born and die inside the shutter, and a failed guess renders as a
// sharp copy sitting in the middle of a blurred cloud.
//
// It costs one vector per copy, it is skipped silently when the source has no
// velocity to give, and "should the copies know where they are going" is not a
// preference - so it is not a knob.
std::vector<std::string> CopyToPoints::copyAttrNames() const
{
  std::vector<std::string> names = splitList(_copyAttrs);
  for (size_t i = 0; i < names.size(); ++i)
    if (names[i] == kPsVel) return names;
  names.push_back(kPsVel);
  return names;
}

void CopyToPoints::geometry_engine(Scene& scene, GeometryList& out)
{
  geometryEngineImpl(scene, out);
  // Bring the op's own object cache in line with what was actually emitted.
  // Without this a build that emits fewer objects than the previous one leaves
  // stale (nulled) cache entries behind until the next evaluation, and the
  // viewer's selection / handle code walks them in the meantime -> crash when
  // an upstream node (e.g. the Card) is selected in the node graph.
  try {
    out.synchronize_objects();
    ctpLog("engine:synchronized", "objects()=" + std::to_string(objects()) + " cache_list=" + std::to_string(cache_list.size()) + " out.objects()=" + std::to_string(out.objects()));
  }
  catch (...) {
  }
}

void CopyToPoints::geometryEngineImpl(Scene& scene, GeometryList& out)
{
  try {
    _lastError.clear();
    _statInstances = _statVariants = _statPoints = 0;

    // Drop the shared references held from the previous build.  Nothing here
    // is owned by us in instances mode, and in bake mode Nuke re-allocates on
    // demand through writable_points()/add_primitive().
    // This applies to bake (SOURCE) entries as well: add_object() does not
    // clear a previous build's primitives / vertex count / attributes, and a
    // stale point attribute that is smaller than the new point count crashes
    // the renderer.  Dropping the references (never clearing in place) is
    // safe: the old lists die when nothing references them any more.
    // (done per object in resetCacheEntry(), right before add_object(), so that
    // entries beyond this build's count are never left with null pointers while
    // Nuke's viewer code may still walk them; synchronize_objects() trims them.)

    ctpLog("engine:start");
    // ---- 1. source points ---------------------------------------------
    unsigned obj = 0;
    GeometryList localPoints;
    const GeometryList* pointsList = &localPoints;
    if (_keepPoints) {
      // Official pass-through: input objects land in our output range first.
      input0()->get_geometry(scene, out);
      obj = out.objects();
      pointsList = &out;
    }
    else {
      input0()->get_geometry(scene, localPoints);
    }
    // NOTE: when _keepPoints is set we must not keep GeoInfo references into
    // 'out' across add_object() calls, so everything needed from the source
    // points is copied into InstanceRec first (gatherInstances) and the
    // emitters only read attribute values from pointsList (whose GeoInfos
    // stay valid: add_object appends, the vector may reallocate but we
    // re-index through operator[] every time).

    ctpLog("engine:points fetched", std::to_string(pointsList->objects()));
    // Nothing to build (no prototypes, guard, no points): never leave the output
    // EMPTY - Nuke 14's viewer selection code crashes when an upstream node
    // (e.g. the Sphere on the points input) is selected while the viewed GeoOp
    // has zero objects.  Pass the points input through instead (like 'keep
    // source points'); the node warning says why.
    auto passInputThrough = [&]() {
      if (_keepPoints) return;   // already in the output
      try {
        input0()->get_geometry(scene, out);
        ctpLog("engine:pass-through", std::to_string(out.objects()));
      }
      catch (...) {
      }
    };
    _inputIsParticles = dynamic_cast<ParticleRender*>(input0()) != nullptr;
    // Terrain cache for the viewer brush: world-space points (all-points order)
    // and triangles.  Kept small when painting is off (points only, no tris).
    {
      std::vector<Vector3> pts, tris;
      std::vector<unsigned> base, triIdx, triObj;
      std::vector<Vector4> cfs;
      std::vector<uint8_t> hasCf;
      const bool wantTris = _paintEnable || _scatterMode != kScatterOff;
      unsigned total = 0;
      for (unsigned o = 0; o < pointsList->objects(); ++o) total += (*pointsList)[o].points();
      pts.reserve(total);
      for (unsigned o = 0; o < pointsList->objects(); ++o) {
        const GeoInfo& gi = (*pointsList)[o];
        base.push_back(unsigned(pts.size()));
        const Vector3* P = gi.point_array();
        const unsigned np = gi.points();
        const bool ident = gi.matrix.isIdentity();
        for (unsigned i = 0; i < np; ++i) pts.push_back(ident ? P[i] : gi.matrix.transform(P[i]));
        {
          const AttribContext* cac = _copyColor ? findPointAttrib(gi, trimCopy(_colorAttr).c_str()) : nullptr;
          hasCf.push_back(cac ? 1 : 0);
          for (unsigned i = 0; i < np; ++i) {
            Vector4 c(1, 1, 1, 1);
            if (cac) attribVec4At(cac, i, c);
            cfs.push_back(c);
          }
        }
        if (wantTris && np) {
          const unsigned nprim = gi.primitives();
          std::vector<unsigned> fv;
          for (unsigned q = 0; q < nprim; ++q) {
            const Primitive* pr = gi.primitive(q);
            if (!pr) continue;
            const unsigned nf = pr->faces();
            for (unsigned fidx = 0; fidx < nf; ++fidx) {
              const unsigned nv = pr->face_vertices(int(fidx));
              if (nv < 3) continue;
              fv.resize(nv);
              if (nf == 1) { for (unsigned v = 0; v < nv; ++v) fv[v] = pr->vertex(v); }
              else {
                pr->get_face_vertices(int(fidx), &fv[0]);
                if (pr->getPrimitiveType() == ePolyMesh) for (unsigned v = 0; v < nv; ++v) fv[v] = pr->vertex(fv[v]);
              }
              bool ok = true;
              for (unsigned v = 0; v < nv; ++v) if (fv[v] >= np) { ok = false; break; }
              if (!ok) continue;
              const Vector3 a = pts[base.back() + fv[0]];
              for (unsigned v = 1; v + 1 < nv; ++v) {
                tris.push_back(a);
                tris.push_back(pts[base.back() + fv[v]]);
                tris.push_back(pts[base.back() + fv[v + 1]]);
                triIdx.push_back(base.back() + fv[0]);
                triIdx.push_back(base.back() + fv[v]);
                triIdx.push_back(base.back() + fv[v + 1]);
                triObj.push_back(o);
              }
            }
          }
        }
      }
      std::lock_guard<std::mutex> lock(_paintMutex);
      _paintPts.swap(pts);
      _paintTris.swap(tris);
      _paintBase.swap(base);
      _meshTriIdx.swap(triIdx);
      _meshTriObj.swap(triObj);
      _meshCf.swap(cfs);
      _meshHasCf.swap(hasCf);
    }
    // ---- 1a'. painted colour written onto the kept source points as Cf --------
    if (_keepPoints && _paintColorEnable && _paintColorSource && obj > 0) {
      PaintLayers paintCopy;
      std::vector<unsigned> baseCopy;
      {
        std::lock_guard<std::mutex> lock(_paintMutex);
        paintCopy = _paint;
        baseCopy = _paintBase;
      }
      if (paintCopy.layerHasData(kPaintLayerColA)) {
        for (unsigned o = 0; o < obj && o < baseCopy.size(); ++o) {
          const unsigned np = out[o].points();
          if (!np || baseCopy[o] + np > paintCopy.npoints) continue;
          const AttribContext* srcAc = out[o].get_typed_group_attribcontext(Group_Points, kColorAttrName, VECTOR4_ATTRIB);
          std::vector<Vector4> base(np, Vector4(1, 1, 1, 1));
          if (srcAc && srcAc->attribute) for (unsigned i = 0; i < np && i < srcAc->attribute->size(); ++i) base[i] = srcAc->attribute->vector4(i);
          Attribute* cf = out.writable_attribute(o, Group_Points, kColorAttrName, VECTOR4_ATTRIB);
          if (!cf) continue;
          cf->resize(np);
          for (unsigned i = 0; i < np; ++i) {
            const size_t pi = baseCopy[o] + i;
            const float a = std::min(1.0f, std::max(0.0f, paintCopy.get(kPaintLayerColA, pi)));
            Vector4 c = base[i];
            if (a > 0.0f) {
              Vector4 pc(paintCopy.get(kPaintLayerColR, pi), paintCopy.get(kPaintLayerColG, pi), paintCopy.get(kPaintLayerColB, pi), c.w);
              if (_paintColorMode == kPaintColorMultiply) { pc.x *= c.x; pc.y *= c.y; pc.z *= c.z; }
              c = Vector4(lerpf(c.x, pc.x, a), lerpf(c.y, pc.y, a), lerpf(c.z, pc.z, a), c.w);
            }
            cf->vector4(i) = c;
          }
        }
      }
    }
    // ---- 1a. scatter points across the faces -------------------------------
    _scatter.clear();
    _scatterInfo.clear();
    if (_scatterMode != kScatterOff && (_scatterCount > 0 || (_scatterUsePaint && _scatterPaintMode == kScatterPaintAddRemove && _scatterPaintCount > 0))) {
      PaintLayers paintCopy;
      {
        std::lock_guard<std::mutex> lock(_paintMutex);
        paintCopy = _paint;
      }
      ScatterParams spar;
      spar.weighting = _scatterWeighting; spar.bias = _scatterBias; spar.usePaint = _scatterUsePaint;
      spar.seed = _scatterSeed; spar.count = _scatterCount; spar.separation = _scatterSeparation;
      spar.up = Vector3(static_cast<float>(_up[0]), static_cast<float>(_up[1]), static_cast<float>(_up[2]));
      spar.uniformFaces = _scatterUniformFaces;
      spar.paintMode = _scatterPaintMode; spar.paintCount = _scatterPaintCount;
      std::lock_guard<std::mutex> lock(_paintMutex);
      scatterOnMesh(spar, _paintPts, _meshTriIdx, _meshTriObj, paintCopy, _scatter, _scatterInfo);
    }
    // ---- 1b. particle system fields (virtual point attributes) ---------
    _psAttribs.clear();
    _psInfo.clear();
    if (_readParticles) buildParticleAttribs(*pointsList);

    ctpLog("engine:particles read", _psInfo);
    // ---- 2. prototypes ---------------------------------------------------
    std::vector<GeometryList> protoLists;
    std::vector<ProtoRef> protos;
    gatherPrototypes(scene, protoLists, protos);
    _statVariants = unsigned(protos.size());
    ctpLog("engine:prototypes", std::to_string(protos.size()));

    unsigned nPoints = 0;      // effective copy targets
    unsigned nRawPoints = 0;   // all points on the input
    unsigned nMeshObjects = 0;
    for (unsigned o = 0; o < pointsList->objects(); ++o) {
      const GeoInfo& gi = (*pointsList)[o];
      const unsigned np = gi.points();
      nRawPoints += np;
      if (np == 0) continue;
      const bool asObject = (_pointsSource == kSourceObjects) ||
                            (_pointsSource == kSourceAuto && _inputIsParticles && objectIsMesh(gi));
      if (asObject) { ++nMeshObjects; nPoints += 1; }
      else nPoints += np;
    }
    if (_scatterMode == kScatterReplace) nPoints = unsigned(_scatter.size());
    else if (_scatterMode == kScatterAdd) nPoints += unsigned(_scatter.size());
    _statPoints = nPoints;
    _sourceNote.clear();
    if (nMeshObjects > 0 && _pointsSource != kSourceEveryPoint) {
      std::ostringstream n;
      n << nMeshObjects << " mesh object(s) on the points input treated as one copy target each ("
        << nRawPoints << " raw points, " << nPoints << " copy targets)";
      _sourceNote = n.str();
    }
    if (_maxSourcePoints > 0 && nPoints > unsigned(_maxSourcePoints)) {
      std::ostringstream m;
      m << "too many copy targets: " << nPoints << " > max source points (" << _maxSourcePoints
        << "). If the points input carries geometry per particle, use 'copy onto: auto' or "
        << "'one per object'; otherwise raise the limit deliberately.";
      _lastError = m.str();
      warning("CopyToPoints: %s", _lastError.c_str());
      if (_dumpAttributes) dumpAttributes(*pointsList, protos);
      {
        const std::string rep = buildAttrReport(*pointsList, protos);
        std::lock_guard<std::mutex> lock(_reportMutex);
        _attrReport = "GUARD: " + _lastError + "\n" + rep;
      }
      passInputThrough();
      return;
    }
    {
      const std::string rep = buildAttrReport(*pointsList, protos);
      std::lock_guard<std::mutex> lock(_reportMutex);
      _attrReport = rep;
    }

    if (protos.empty() && _guideMode == kGuideOff) {
      _lastError = "no prototype geometry connected to the geo inputs (points input passed through)";
      if (_dumpAttributes) dumpAttributes(*pointsList, protos);
      ctpLog("engine:no prototypes", "objects()=" + std::to_string(objects()) + " cache_list=" + std::to_string(cache_list.size()) + " out.objects()=" + std::to_string(out.objects()));
      passInputThrough();
      return;
    }
    if (nPoints == 0) {
      if (_dumpAttributes) dumpAttributes(*pointsList, protos);
      passInputThrough();
      return;
    }

    ctpLog("engine:targets", std::to_string(nPoints));
    // ---- 3. per-point transforms ---------------------------------------
    std::vector<InstanceRec> inst;
    inst.reserve(nPoints);
    gatherInstances(*pointsList, std::max<size_t>(1, protos.size()), inst);
    _statInstances = unsigned(inst.size());
    ctpLog("engine:instances gathered", std::to_string(inst.size()));
    if (_dumpAttributes) dumpAttributes(*pointsList, protos);
    if (inst.empty()) return;
    if (_maxCopyPoints > 0.0 && !protos.empty()) {
      double total = 0.0;
      for (size_t i = 0; i < inst.size(); ++i) {
        const int v = inst[i].variant;
        if (v >= 0 && size_t(v) < protos.size() && protos[size_t(v)].info) total += double(protos[size_t(v)].info->points());
      }
      if (total > _maxCopyPoints * 1e6) {
        std::ostringstream m;
        m << total / 1e6 << "M copy points (" << inst.size() << " copies x prototype points) exceed 'max copy points' "
          << _maxCopyPoints << "M - nothing built. Use a lighter prototype or fewer copies, or raise the limit.";
        _lastError = m.str();
        warning("CopyToPoints: %s", _lastError.c_str());
        {
          std::lock_guard<std::mutex> lock(_reportMutex);
          _attrReport = "GUARD: " + _lastError + "\n" + _attrReport;
        }
        ctpLog("engine:guard copy points", std::to_string(total));
        _statInstances = 0;
        passInputThrough();
        return;
      }
    }

    // ---- 4. emit ---------------------------------------------------------
    if (!(_guideMode != kGuideOff && _guideHideCopies) && !protos.empty()) {
      if (_mode == kModeBake) emitBaked(out, obj, protos, inst, *pointsList);
      else                    emitInstances(out, obj, protos, inst, *pointsList);
    }
    if (_guideMode != kGuideOff) emitGuide(out, obj, inst);
    ctpLog("engine:emitted", std::to_string(obj) + " objects()=" + std::to_string(objects()) + " cache_list=" + std::to_string(cache_list.size()) + " out.objects()=" + std::to_string(out.objects()));
  }
  catch (const std::exception& e) {
    _lastError = e.what();
    error("CopyToPoints failed: %s", e.what());
  }
  catch (...) {
    _lastError = "unknown exception";
    error("CopyToPoints failed: unknown exception");
  }
}

// --------------------------------------------------------------------------
static Op* build(Node* node) { return new CopyToPoints(node); }
const Op::Description CopyToPoints::description(kClass, "3D/Modify/CopyToPoints", build);
