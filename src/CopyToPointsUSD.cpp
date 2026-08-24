// CopyToPointsUSD.cpp
//
// Nuke 15+ (new USD 3D system) version of CopyToPoints: copies / instances
// geometry onto the points (or the faces, via scatter) of the points input and
// emits a single UsdGeomPointInstancer - real instancing: the prototypes are
// stored once (copied from the geo inputs under the instancer), every copy is
// only a transform, and Hydra / the USD-aware renderers instance natively.
//
// Inputs
//   0        points  : GeomOp stage; every Mesh / Points prim provides targets
//   1..N     geo1..N : GeomOp stages; every root prim becomes one prototype
//                      (variant), copied under <node>/instancer/Prototypes
//
// Shares the per-target logic (variants, alignment, randomness, scatter,
// colour variance) with the classic node through CopyCore.h.  Strict ASCII.

#include "DDImage/GeomOp.h"
#include "DDImage/ddImageVersionNumbers.h"
#include "DDImage/Knobs.h"
#include "DDImage/Op.h"
#include "DDImage/Hash.h"

#include "usg/engine/GeomEngine.h"
#include "usg/engine/GeomSceneContext.h"
#include "usg/geom/Stage.h"
#include "usg/geom/Layer.h"
#include "usg/geom/Prim.h"
#include "usg/geom/PrimRange.h"
#include "usg/geom/GeomTokens.h"
#include "usg/geom/GprimPrim.h"
#include "usg/geom/ImageablePrim.h"
#include "usg/geom/MeshPrim.h"
#include "usg/geom/PointsPrim.h"
#include "usg/geom/BasisCurvesPrim.h"
#include "usg/geom/Attribute.h"
#include "usg/geom/PointInstancerPrim.h"
#include "usg/geom/PrimvarsAPI.h"
#include "usg/geom/Primvar.h"
#include "usg/geom/ScopePrim.h"
#include "usg/geom/XformPrim.h"
#include "usg/geom/XformablePrim.h"
#include "usg/base/ArrayTypes.h"
#include "usg/base/Value.h"
#include "fdk/math/Quat.h"
#include "fdk/math/Mat4.h"

#include "CopyCore.h"
#include "CopyToPointsHelp.h"

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

// shared 3D-viewer brush (pulls in windows.h / GL, so it comes last)
#include "PaintBrushKnob.h"

using namespace DD::Image;
using namespace ctp;

namespace {

#define CTPU_VERSION "1.9.1"
const char* const kClass = "CopyToPointsUSD";
const char* const kHelp =
  "@b;CopyToPointsUSD@n; copies / instances geometry onto every point of the @b;points@n; input "
  "(meshes, point clouds) using a USD PointInstancer: the prototypes connected to the @b;geo@n; "
  "inputs are stored once and every copy is only a transform, so the viewer and USD renderers "
  "instance natively.\n\n"
  "Variants, alignment to normals, random rotation / scale, scatter across faces (uniform, by "
  "slope or height, with separation), colour variance and a viewer-only guide - the same "
  "controls as the classic CopyToPoints (Nuke 14.1).";

const char* const kSourceNames[] = { "every point (mesh vertices, points prims)", "one per prim (prim centre)", nullptr };
const char* const kVariantNames[] = { "sequential", "random", "attribute (point id)", nullptr };
// The same set the classic CopyToPoints offers.  It only had "normal" before,
// so a particle could not be pointed along its own velocity - the one thing you
// most want from a copier fed by an emitter.  The modes map onto the shared
// kAlign* in CopyCore.h, which already knew how to do every one of them.
enum { kAlignUsdNone = 0, kAlignUsdNormal = 1, kAlignUsdDir = 2, kAlignUsdQuat = 3, kAlignUsdEuler = 4 };
const char* const kAlignNamesUSD[] = {
  "none",
  "normal (surface / points normals)",
  "direction attribute (e.g. velocities / N)",
  "quaternion attribute (x y z w)",
  "euler attribute (degrees, XYZ)",
  nullptr };
const char* const kAxisNamesU[] = { "+X", "+Y", "+Z", "-X", "-Y", "-Z", nullptr };
const char* const kScatterNamesU[] = { "off", "add to the points", "replace the points", nullptr };
const char* const kScatterWeightNamesU[] = { "uniform (by area)", "prefer flat areas", "prefer steep slopes", "prefer peaks (high)", "prefer valleys (low)", nullptr };
const char* const kGuideNamesU[] = { "off", "copy positions (points)", "copy positions + up axes", nullptr };
enum { kGuidePurposeDefault = 0, kGuidePurposeGuide = 1, kGuidePurposeProxy = 2 };
const char* const kGuidePurposeNames[] = { "default (always visible, renders too)", "guide (hidden until the Viewer shows guides)", "proxy (hidden until the Viewer shows proxies)", nullptr };
enum { kModeInstancer = 0, kModeCopies = 1 };
const char* const kModeNamesU[] = { "instances (PointInstancer)", "copies (referenced prims, per-copy colour)", nullptr };

std::string sanitizeName(const std::string& s)
{
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') out.push_back(c);
    else out.push_back('_');
  }
  if (out.empty() || (out[0] >= '0' && out[0] <= '9')) out = "_" + out;
  return out;
}

// Author primvars:displayColor with an interpolation - the PrimvarsAPI::createPrimvar
// of Nuke 15+ or a plain attribute + interpolation on the Nuke 14.1 preview API.
inline void authorDisplayColor(usg::Prim& prim, const usg::Vec3fArray& colors, const usg::Token& interp, const fdk::TimeValue& time)
{
#if kDDImageVersionMajorNum >= 15
  usg::PrimvarsAPI pv(prim);
  usg::Primvar cpv = pv.createPrimvar(usg::Token("displayColor"), usg::Value::Color3fArray, interp);
  if (cpv) cpv.attribute().setValue(colors, time);
#else
  usg::Attribute a = prim.createAttr(usg::Token("primvars:displayColor"), usg::Value::Color3fArray);
  if (a) { a.setInterpolation(interp); a.setValue(colors, time); }
#endif
}

// (Re)point a prim at one internal reference
inline void setSingleReference(usg::Prim& prim, const usg::Path& target)
{
#if kDDImageVersionMajorNum >= 15
  std::vector<std::pair<std::string, usg::Path> > refs;
  refs.push_back(std::make_pair(std::string(), target));
  prim.setReferences(refs);
#else
  prim.addReference(std::string(), target);
#endif
}

inline Vector3 toV3(const fdk::Vec3f& v) { return Vector3(v.x, v.y, v.z); }
inline Vector3 toV3(const fdk::Vec3d& v) { return Vector3(float(v.x), float(v.y), float(v.z)); }
inline fdk::Vec3f toF3(const Vector3& v) { return fdk::Vec3f(v.x, v.y, v.z); }

// Decompose a DDImage matrix (T * R * S) into translation, unit quaternion and scale.
void decompose(const Matrix4& m, Vector3& t, fdk::Quatf& q, Vector3& s)
{
  t = m.translation();
  Vector3 x = m.x_axis(), y = m.y_axis(), z = m.z_axis();
  s = Vector3(x.length(), y.length(), z.length());
  if (s.x > 1e-12f) x = x * (1.0f / s.x);
  if (s.y > 1e-12f) y = y * (1.0f / s.y);
  if (s.z > 1e-12f) z = z * (1.0f / s.z);
  // handedness
  if (x.cross(y).dot(z) < 0.0f) { s.z = -s.z; z = z * -1.0f; }
  // rotation matrix columns x, y, z -> quaternion
  const float m00 = x.x, m01 = y.x, m02 = z.x;
  const float m10 = x.y, m11 = y.y, m12 = z.y;
  const float m20 = x.z, m21 = y.z, m22 = z.z;
  const float tr = m00 + m11 + m22;
  float qw, qx, qy, qz;
  if (tr > 0.0f) {
    const float S = std::sqrt(tr + 1.0f) * 2.0f;
    qw = 0.25f * S; qx = (m21 - m12) / S; qy = (m02 - m20) / S; qz = (m10 - m01) / S;
  }
  else if (m00 > m11 && m00 > m22) {
    const float S = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
    qw = (m21 - m12) / S; qx = 0.25f * S; qy = (m01 + m10) / S; qz = (m02 + m20) / S;
  }
  else if (m11 > m22) {
    const float S = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
    qw = (m02 - m20) / S; qx = (m01 + m10) / S; qy = 0.25f * S; qz = (m12 + m21) / S;
  }
  else {
    const float S = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
    qw = (m10 - m01) / S; qx = (m02 + m20) / S; qy = (m12 + m21) / S; qz = 0.25f * S;
  }
  q = fdk::Quatf(qw, qx, qy, qz);
}

} // namespace

class CopyToPointsUSD;

// ==========================================================================
class CopyToPointsUSDEngine : public GeomOpEngine
{
public:
// The engine's parent changed from Op* to GeomOpNode* in Nuke 16.1, not 17.0 -
// guarding on the major alone fails to compile against 16.1.
#if kDDImageVersionMajorNum > 16 || (kDDImageVersionMajorNum == 16 && kDDImageVersionMinorNum >= 1)
  // Nuke 17: engines belong to the GeomOpNode; the Op is reached through firstOp().
  CopyToPointsUSDEngine(ndk::GeomOpNode* parent) : GeomOpEngine(parent) {}
  Op* opPtr() const { return firstOp(); }
#else
  CopyToPointsUSDEngine(Op* parent) : GeomOpEngine(parent), _op(parent) {}   // no logging here: crashes Nuke 16/17
  Op* opPtr() const { return _op; }
  Op* _op;
#endif
  std::string name() const override { return "CopyToPointsUSDEngine"; }
protected:
  void processScenegraph(usg::GeomSceneContext& context) override;
};

// ==========================================================================
class CopyToPointsUSD : public GeomOp, public ctp::PaintHost
{
  friend class CopyToPointsUSDEngine;
public:
  enum { kMaxInputs = 33 };
  CopyToPointsUSD(Node* node);
#if kDDImageVersionMajorNum > 16 || (kDDImageVersionMajorNum == 16 && kDDImageVersionMinorNum >= 1)
  static const GeomOp::Description description;
#else
  static const Op::Description description;
#endif
  const char* Class() const override { return kClass; }
  const char* node_help() const override { return kHelp; }
  int minimum_inputs() const override { return 2; }
  int maximum_inputs() const override { return kMaxInputs; }
  bool test_input(int i, Op* op) const override { (void)i; return dynamic_cast<GeomOp*>(op) != nullptr; }
  Op* default_input(int i) const override { return (i == 0) ? GeomOp::default_input(i) : nullptr; }
  const char* input_label(int input, char* buffer) const override
  {
    if (input == 0) return "points";
    std::snprintf(buffer, 32, "geo%d", input);
    return buffer;
  }
  void knobs(Knob_Callback f) override;
  int knob_changed(Knob* k) override;
  void append(Hash& hash) override;
  void build_handles(ViewerContext* ctx) override;

  // ---- ctp::PaintHost (the shared brush knob talks to the op through this) ---
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
  const char* paintUndoName() const override { return "CopyToPointsUSD paint"; }
  bool paintKnobBuildsHandle() const override { return true; }   // registered through build_knob_handles()
  void draw_handle(ViewerContext* ctx) override
  {
    if (Knob* k = knob("paint_data")) k->draw_handle(ctx);
  }

  // ---- knobs ---------------------------------------------------------------
  int    _mode;
  bool   _copiesInstanceable;
  int    _pointsSource;
  bool   _hideSource;
  int    _maxInstances;
  int    _maxSourcePoints;
  double _maxCopyPoints;   // millions
  double _density;
  int    _guideMode;
  double _guideSize;
  bool   _guideHideCopies;
  bool   _guideHeat;
  int    _guidePurpose;
  int    _scatterMode;
  bool   _scatterUsePaint;
  int    _scatterStick;
  int    _scatterRefFrame;
  int    _scatterPaintMode;
  int    _scatterPaintCount;
  int    _scatterCount;
  int    _scatterSeed;
  int    _scatterWeighting;
  double _scatterBias;
  double _scatterSeparation;
  int    _variantMode;
  int    _variantSeed;
  std::string _variantAttr;
  // Named attributes, the way the classic node takes them. USD has schemas for
  // widths and displayColor but nothing for "the float I want to scale by", so
  // a pipeline that already carries its own names needs to be able to say them.
  bool   _useSizeAttr;
  std::string _sizeAttr;
  std::string _scaleAttr;
  std::string _idAttr;
  std::string _colorAttr;
  std::string _copyAttrs;
  int    _spinMode;
  double _rollRate;
  int    _rollChannels;
  int    _alignMode;
  std::string _alignAttr;
  int    _forwardAxis;
  double _up[3];
  double _rotate[3];
  bool   _randomRotate;
  double _rotMin[3];
  double _rotMax[3];
  double _rotVariance;
  double _scale;
  double _scaleXYZ[3];
  bool   _useWidths;
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
  bool   _copyColor;
  double _colorVarHue, _colorVarSat, _colorVarVal;
  std::string _lastInfo;
  std::string _attrReport;       // attributes seen on the points input during the last build
  std::mutex  _reportMutex;
  const char* _attrListText;
  void refreshAttrListKnob()
  {
    std::string text;
    { std::lock_guard<std::mutex> lock(_reportMutex); text = _attrReport; }
    if (text.empty()) text = "(no geometry built yet: view the node in a 3D viewer or render it, then press 'refresh list')";
    if (Knob* kk = knob("attribute_list")) kk->set_text(text.c_str());
  }

  // ---- paint knobs (same set as the classic node) ---------------------------
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
  // paint data (from the knob) + the source cache the brush ray-casts against,
  // filled by the engine, all under _paintMutex
  PaintLayers _paint;
  unsigned    _paintVersion;
  mutable std::mutex _paintMutex;
  std::vector<Vector3>  _paintPts;    // world-space source points (all prims, paint index order)
  std::vector<Vector3>  _paintTris;   // 3 entries per triangle
  std::vector<unsigned> _paintBase;   // first paint index of every source prim
};

CopyToPointsUSD::CopyToPointsUSD(Node* node)
  : GeomOp(node, BuildEngine<CopyToPointsUSDEngine>())
  , _mode(kModeCopies), _copiesInstanceable(false), _pointsSource(0), _hideSource(false), _maxInstances(0), _maxSourcePoints(1000000), _maxCopyPoints(20.0), _density(1.0)
  , _guideMode(0), _guideSize(0.05), _guideHideCopies(false), _guideHeat(true), _guidePurpose(kGuidePurposeDefault)
  , _scatterMode(0), _scatterUsePaint(true), _scatterStick(kScatterStickOff), _scatterRefFrame(1), _scatterPaintMode(kScatterPaintAddRemove), _scatterPaintCount(1000), _scatterCount(1000), _scatterSeed(0), _scatterWeighting(0), _scatterBias(2.0), _scatterSeparation(0.0)
  , _variantMode(0), _variantSeed(1), _variantAttr("")
  , _useSizeAttr(false), _sizeAttr("size"), _scaleAttr(""), _idAttr("")
  , _colorAttr(""), _copyAttrs("")
  , _spinMode(0), _rollRate(200.0), _rollChannels(0)
  , _alignMode(0), _alignAttr("velocities"), _forwardAxis(kAxisPZ)
  , _randomRotate(false), _rotVariance(0.0)
  , _scale(1.0), _useWidths(false), _randomScale(false), _scaleMin(0.5), _scaleMax(1.5)
  , _scaleBias(0.0), _scaleShape(0.0)
  , _seed(0), _copyColor(true), _colorVarHue(0.0), _colorVarSat(0.0), _colorVarVal(0.0)
  , _attrListText("")
  , _paintEnable(false), _paintLayer(kPaintLayerDensity), _paintRadius(1.0), _paintHardness(0.5), _paintOpacity(0.5)
  , _paintMode(kPaintModeAdd), _paintShow(true), _paintValue(1.0), _paintHeatMax(1.0), _paintPointSize(5.0), _paintLive(true)
  , _paintDensityEnable(false), _paintOcclusion(true), _paintScaleEnable(false), _paintScaleAmount(1.0)
  , _paintRotEnable(false), _paintRotAxis(kPaintAxisY), _paintRotAmount(90.0), _paintVariantEnable(false)
  , _paintColorEnable(false), _paintColorMode(kPaintColorReplace), _paintColorSource(false), _paintVersion(0)
{
  _paintColor[0] = 1.0f; _paintColor[1] = 0.25f; _paintColor[2] = 0.1f;
  _up[0] = 0.0; _up[1] = 1.0; _up[2] = 0.0;
  _rotate[0] = _rotate[1] = _rotate[2] = 0.0;
  _rotMin[0] = _rotMin[1] = _rotMin[2] = 0.0;
  _rotMax[0] = 0.0; _rotMax[1] = 360.0; _rotMax[2] = 0.0;
  _scaleXYZ[0] = _scaleXYZ[1] = _scaleXYZ[2] = 1.0;
  _offset[0] = _offset[1] = _offset[2] = 0.0;
  _randomOffset = false;
  for (int i = 0; i < 3; ++i) { _offMin[i] = _offMax[i] = _offVariance[i] = 0.0; }
}

void CopyToPointsUSD::knobs(Knob_Callback f)
{
  GeomOp::knobs(f);

  Tab_knob(f, "Copy");
  Named_Text_knob(f, "title", "", "<b><font size=+2>CopyToPointsUSD</font></b>&nbsp;&nbsp;<font size=-1>v" CTPU_VERSION "</font>");
  SetFlags(f, Knob::STARTLINE);
  Named_Text_knob(f, "subtitle", "", "<i>USD PointInstancer copy-to-points for Nuke 15+</i>"
                                     "&nbsp;&nbsp;&nbsp;<font size=-1>Created by Marten Blumen</font>");
  SetFlags(f, Knob::STARTLINE);
  PyScript_knob(f, ctp::helpScript(5), "help_copy", "help...");
  Tooltip(f, "Open the popup help (USD node notes, every tab explained, workflows).");
  Divider(f, "output");
  Enumeration_knob(f, &_mode, kModeNamesU, "mode", "mode");
  KnobDefinesGeometry(f);
  Tooltip(f, "instances: one UsdGeomPointInstancer (prototypes stored once, every copy is a transform entry) - "
             "the lightest output. Per-copy colour is authored as an instance-rate displayColor primvar, which "
             "ScanlineRender2 does not shade (Hydra viewers may).\n"
             "copies: every copy is a prim (<node>/copies/copy_N) that references the prototype - real per-copy "
             "displayColor (source colour, colour variance, painted colour) in every renderer, materials bound "
             "to the prototypes are kept. Heavier for many thousands of copies.");
  Bool_knob(f, &_copiesInstanceable, "copies_instanceable", "copies share geometry (instanceable)");
  KnobDefinesGeometry(f);
  Tooltip(f, "copies mode only. On: every copy prim is marked instanceable, so USD / Hydra share one copy of the "
             "prototype geometry between all copies (memory ~ one prototype + one transform per copy, like the "
             "PointInstancer) - but per-copy colours become instance-rate primvars, which ScanlineRender2 does not "
             "shade. Off: each copy is a full prim (per-copy colours render everywhere, memory grows with copies x "
             "prototype size - see 'max copy vertices').");
  Enumeration_knob(f, &_pointsSource, kSourceNames, "points_source", "copy onto");
  KnobDefinesGeometry(f);
  Tooltip(f, "every point: all points of every Mesh / Points prim on the points input.\n"
             "one per prim: one copy at the centre of every prim.");
  Bool_knob(f, &_hideSource, "hide_source", "hide source geometry");
  KnobDefinesGeometry(f);
  Tooltip(f, "Author visibility = invisible on the source prims (the terrain stays in the stage but is not drawn).");
  Int_knob(f, &_maxInstances, "max_instances", "max instances");
  KnobDefinesGeometry(f);
  Tooltip(f, "Safety cap on the number of copies (0 = unlimited).");
  Int_knob(f, &_maxSourcePoints, "max_source_points", "max source points");
  KnobDefinesGeometry(f);
  Tooltip(f, "Guard: refuse to build when the input offers more copy targets than this (0 = no limit). "
             "PointInstancer copies are cheap, so the default is high.");
  Float_knob(f, &_maxCopyPoints, IRange(0.0, 200.0), "max_copy_points", "max copy points (M)");
  KnobDefinesGeometry(f);
  Tooltip(f, "Memory guard, in millions of points: copies x points of the chosen prototypes. Nuke's ScanlineRender2 "
             "un-instances the copies when it renders (measured ~350 bytes per point per copy), so 5000 copies of a "
             "40k-point mesh = 200M points = ~70 GB.\n"
             "In mode = copies, going over the limit builds nothing: there, every copy really is a referencing prim "
             "that a renderer has to pay for.\n"
             "In mode = instances, going over it only WARNS and still builds. A PointInstancer stores the prototype "
             "once and each copy is a transform, so that product is what an un-instancing renderer WOULD pay, not "
             "what the stage costs - render it with InstanceRender, or any Hydra renderer that keeps instancing.\n"
             "Reduce the prototype (fewer polygons, proxy meshes) or the count. 0 = no limit.");
  Float_knob(f, &_density, IRange(0.0, 1.0), "density", "density");
  KnobDefinesGeometry(f);
  Tooltip(f, "Probability that a target receives a copy.");

  Divider(f, "guide geometry (viewer aid: turn off before rendering, or use a guide/proxy purpose)");
  Enumeration_knob(f, &_guideMode, kGuideNamesU, "guide_mode", "guide");
  KnobDefinesGeometry(f);
  Tooltip(f, "Show every copy position as a Points prim with purpose 'guide' (orange = scattered, cyan = vertices), "
             "optionally with the copy's up axis as a yellow line (BasisCurves, purpose guide) so the alignment is visible.");
  Float_knob(f, &_guideSize, IRange(0.001, 1.0), "guide_size", "point width / axis length");
  KnobDefinesGeometry(f);
  Tooltip(f, "How big the guide is drawn: the width of the guide points and the length of the up-axis "
             "line, in world units. It only changes the guide, never the copies.");
  Bool_knob(f, &_guideHideCopies, "guide_hide_copies", "hide the copies (guide only)");
  KnobDefinesGeometry(f);
  Tooltip(f, "Output only the guide - judge a scatter / paint before adding heavy prototypes.");
  Bool_knob(f, &_guideHeat, "guide_heat", "guide shows the painted layer (heat map)");
  KnobDefinesGeometry(f);
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Colour the guide points with the heat map of the current paint layer (Paint tab: layer / heat max) "
             "interpolated at every copy - scattered points included - instead of cyan (vertex) / orange (scattered).");
  Enumeration_knob(f, &_guidePurpose, kGuidePurposeNames, "guide_purpose", "guide purpose");
  KnobDefinesGeometry(f);
  Tooltip(f, "USD purpose of the guide prims. default: always visible in the Viewer, but ScanlineRender2 renders it too - "
             "turn the guide off (or use 'guide only' just for judging) before rendering. guide / proxy: Nuke's Viewer hides "
             "these purposes unless 'display guides' / 'display proxy' is on in the Viewer settings (button below); "
             "ScanlineRender2 excludes them only when its 'prim purpose filter mode' is 'default' or 'render'.");
  PyScript_knob(f,
    "import nuke\n"
    "for _v in nuke.allNodes('Viewer'):\n"
    "    _v['display_guides'].setValue(True)\n"
    "nuke.message('Guide prims are now displayed in every Viewer (Viewer settings > display guides).\\n"
    "ScanlineRender2 renders guide prims unless its \\'prim purpose filter mode\\' is set to default/render.')\n",
    "guide_show_in_viewer", "show guides in the viewer");
  Tooltip(f, "The guide prims carry purpose = guide. Nuke's Viewer hides that purpose by default - this turns "
             "'display guides' on for every Viewer. Note ScanlineRender2 renders every purpose by default: set its "
             "'prim purpose filter mode' to 'default' or 'render' so guides never end up in the render.");

  Divider(f, "scatter points on the geometry");
  Enumeration_knob(f, &_scatterMode, kScatterNamesU, "scatter_mode", "scatter");
  KnobDefinesGeometry(f);
  Tooltip(f, "Houdini-style scatter: copy targets anywhere on the faces of the points input (area weighted). "
             "Scattered points get the face normal (align = normal), the interpolated display colour, and stick to their faces.");
  Int_knob(f, &_scatterCount, IRange(0, 200000), "scatter_count", "count");
  KnobDefinesGeometry(f);
  Tooltip(f, "How many points to scatter over the faces. They are seeded and stick to their faces, so "
             "the arrangement is stable while the topology is.");
  Int_knob(f, &_scatterSeed, "scatter_seed", "scatter seed");
  KnobDefinesGeometry(f);
  Tooltip(f, "Change this for a different random arrangement of the same number of scattered points. "
             "Stable while the seed and the topology stay put, so a scatter does not crawl frame to "
             "frame.");
  Enumeration_knob(f, &_scatterWeighting, kScatterWeightNamesU, "scatter_weighting", "weighting");
  KnobDefinesGeometry(f);
  Tooltip(f, "prefer flat / steep: by slope relative to the up vector; prefer peaks / valleys: by height along up.");
  Float_knob(f, &_scatterBias, IRange(0.1, 8.0), "scatter_bias", "bias");
  KnobDefinesGeometry(f);
  Tooltip(f, "How hard the weighting above is pushed: 1 is straight proportion, higher values crowd "
             "the points into the favoured areas and leave the rest emptier.");
  Float_knob(f, &_scatterSeparation, IRange(0.0, 5.0), "scatter_separation", "separation");
  KnobDefinesGeometry(f);
  Tooltip(f, "Minimum distance between scattered points (0 = off).");
  Bool_knob(f, &_scatterUsePaint, "scatter_use_paint", "multiply by painted 'scatter' layer");
  KnobDefinesGeometry(f);
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Paint the 'scatter' layer on the Paint tab; the scattered density is multiplied by it (0 = no points).");
  Enumeration_knob(f, &_scatterPaintMode, kScatterPaintModeNames, "scatter_paint_mode", "painted scatter");
  KnobDefinesGeometry(f);
  Tooltip(f, "How the painted 'scatter' layer acts.\n"
             "remove only: candidates where the layer is negative are dropped (density x (1 + w), never above 1); the "
             "count stays 'count'.\n"
             "add and remove: negative paint thins the base 'count' out as above, positive paint ADDS points on top: "
             "'paint adds' points for a weight of +1 over the whole surface, scaled by the painted area and weight - "
             "independent of 'count', so you can start from a few (or zero) points and paint the rest in; painting "
             "into an area the weighting left empty adds points there. 'max instances' still caps the total.");
  Int_knob(f, &_scatterPaintCount, IRange(0, 100000), "scatter_paint_count", "paint adds");
  KnobDefinesGeometry(f);
  Tooltip(f, "add and remove mode: number of points a paint weight of +1 over the whole surface adds (a +1 stroke "
             "over 10% of the surface adds 10% of this, a +2 stroke twice that).");
  Enumeration_knob(f, &_scatterStick, kScatterStickNames, "scatter_stick", "deforming geometry");
  KnobDefinesGeometry(f);
  Tooltip(f, "What happens when the points input deforms (same topology, moving points):\n"
             "recompute: the area-weighted scatter is redone from the current shape - points reshuffle when face areas change.\n"
             "reference frame shape: face choice, barycentrics, weighting and separation are computed on the shape at 'reference "
             "frame' (read from the input's time samples), the points then ride the deforming surface. Needs the input to carry that "
             "frame (GeoImport caches do; Nuke-generated geometry may only hold the current frame - then it behaves like recompute).\n"
             "topology only: every face is equally likely regardless of its area - stable under any deformation without a "
             "reference frame, but denser where faces are small.");
  Int_knob(f, &_scatterRefFrame, "scatter_ref_frame", "reference frame");
  KnobDefinesGeometry(f);
  Tooltip(f, "Frame whose shape drives the scatter in 'reference frame shape' mode.");

  Divider(f, "variants");
  Enumeration_knob(f, &_variantMode, kVariantNames, "variant_mode", "pick variant");
  KnobDefinesGeometry(f);
  Tooltip(f, "Every root prim of every geo input is a variant: sequential (index), random (seeded), or by the point id.");
  Int_knob(f, &_variantSeed, "variant_seed", "variant seed");
  KnobDefinesGeometry(f);
  Tooltip(f, "Change this to deal the prototypes out differently when pick variant is random. "
             "Only used by that mode.");
  String_knob(f, &_variantAttr, "variant_attr", "variant attribute");
  KnobDefinesGeometry(f);
  Tooltip(f, "Which primvar picks the prototype in 'attribute' mode. Left empty the point's own "
             "id is used, which is what this node did before - name an int or float primvar here "
             "to drive it from something the pipeline already authored instead.");

  Tab_knob(f, "Transform");
  PyScript_knob(f, ctp::helpScript(2), "help_transform", "help...");
  Tooltip(f, "Open the popup help for the Transform page.");
  Divider(f, "rotation");
  Enumeration_knob(f, &_alignMode, kAlignNamesUSD, "align_mode", "align");
  KnobDefinesGeometry(f);
  Tooltip(f, "normal: point the forward axis along the surface normal - the face normal for scattered points; for "
             "vertices the 'normals' attribute or a primvars:normals primvar (vertex or faceVarying, e.g. from GeoNormals, "
             "averaged per point), and when a mesh has none the normals are computed from its faces.");
  String_knob(f, &_alignAttr, "align_attr", "align attribute");
  KnobDefinesGeometry(f);
  Tooltip(f, "Which primvar or attribute the align mode reads, when it reads one.\n\n"
             "\"velocities\" is the one a particle chain gives you - ParticlesToUSD writes it, and "
             "pointing a copy along its own velocity is what makes debris fly nose first. Any "
             "vector primvar on the points works: N, or something authored upstream.");
  Enumeration_knob(f, &_forwardAxis, kAxisNamesU, "forward_axis", "forward axis");
  KnobDefinesGeometry(f);
  Tooltip(f, "Which of the prototype's own axes is treated as its nose, and so gets pointed along "
             "whatever align is set to. Use this when a model was built facing down Y instead of Z.");
  XYZ_knob(f, _up, "up", "up vector");
  KnobDefinesGeometry(f);
  Tooltip(f, "The direction treated as up when building each copy's orientation. It fixes the roll "
             "that pointing the forward axis leaves undecided, and it is also what the scatter "
             "weighting measures slope and height against.");
  XYZ_knob(f, _rotate, "rotate", "rotate");
  KnobDefinesGeometry(f);
  Tooltip(f, "Extra rotation (degrees, XYZ order) applied to every copy in its local frame.");
  Bool_knob(f, &_randomRotate, "random_rotate", "random rotation");
  KnobDefinesGeometry(f);
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Give every copy its own random rotation, within the min/max range below. Seeded per "
             "point, so a copy keeps its angle from frame to frame instead of flickering.");
  XYZ_knob(f, _rotMin, "rot_min", "min");
  KnobDefinesGeometry(f);
  Tooltip(f, "The low end of the random rotation range, in degrees per axis. Set min and max to the "
             "same value for no randomness on that axis.");
  XYZ_knob(f, _rotMax, "rot_max", "max");
  KnobDefinesGeometry(f);
  Tooltip(f, "The high end of the random rotation range, in degrees per axis.");
  Float_knob(f, &_rotVariance, IRange(0.0, 180.0), "rot_variance", "rotation variance");
  KnobDefinesGeometry(f);
  Tooltip(f, "+/- degrees of random jitter on every axis.");
  Enumeration_knob(f, &_spinMode, ctp::kSpinNames, "spin", "spin");
  KnobDefinesGeometry(f);
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Extra rotation applied after align.\n\n"
             "roll along velocity: turn each copy about (up x velocity) by the rate below for "
             "every unit of distance it has travelled since it was born. It is worked out from "
             "the distance rather than accumulated, so scrubbing backwards gives the same answer, "
             "and it is what makes a rock look like it is rolling rather than sliding.\n\n"
             "Needs velocities and a 'primvars:initialP' - ParticlesToUSD authors both.");
  Double_knob(f, &_rollRate, IRange(0.0, 1000.0), "roll_rate", "roll rate");
  KnobDefinesGeometry(f);
  Tooltip(f, "Degrees of roll per unit travelled. About 57 divided by the radius is what a sphere "
             "rolling without slipping would do.");
  Int_knob(f, &_rollChannels, "roll_channels", "roll channels mask");
  KnobDefinesGeometry(f);
  Tooltip(f, "0 rolls every copy. Otherwise a bitmask of Nuke particle channels (a=1, b=2, c=4, "
             "d=8 ...) read from 'primvars:channel': only copies on one of these roll. Use "
             "ParticleBounce's 'new channels' to move bounced particles into a channel and only "
             "those that hit the ground start rolling.");

  Divider(f, "scale");
  Float_knob(f, &_scale, IRange(0.0, 10.0), "scale", "uniform scale");
  KnobDefinesGeometry(f);
  Tooltip(f, "Scales every copy by the same amount, on top of whatever size the prototype already is. "
             "1 leaves it alone.");
  XYZ_knob(f, _scaleXYZ, "scale_xyz", "scale xyz");
  KnobDefinesGeometry(f);
  Tooltip(f, "Per-axis scale on top of the uniform one, for squashing or stretching the copies. "
             "Multiplied with everything else that touches scale.");
  Bool_knob(f, &_useWidths, "use_widths", "multiply by point widths");
  KnobDefinesGeometry(f);
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "For Points prims: multiply the scale by the per-point 'widths' attribute.");
  Bool_knob(f, &_useSizeAttr, "use_size_attr", "multiply by size attribute");
  KnobDefinesGeometry(f);
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Multiply the scale by a named float primvar, for a pipeline whose sizes are not in "
             "'widths'. Separate from 'multiply by point widths' on purpose - a Points prim often "
             "carries both a width for drawing and a size meant for copies.");
  String_knob(f, &_sizeAttr, "size_attr", "size attribute");
  KnobDefinesGeometry(f);
  Tooltip(f, "The float primvar read when the switch above is on. Looked up plain and then under "
             "'primvars:', so either authoring works.");
  String_knob(f, &_scaleAttr, "scale_attr", "scale attribute (vec3)");
  KnobDefinesGeometry(f);
  Tooltip(f, "Optional Vector3 primvar multiplied into the PER-AXIS scale, so a squashed particle "
             "stays squashed. Leave empty to ignore.");
  Bool_knob(f, &_randomScale, "random_scale", "random scale");
  KnobDefinesGeometry(f);
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Give every copy its own random size, within the min/max range below. Seeded per point, "
             "so a copy keeps its size rather than pulsing frame to frame.");
  Float_knob(f, &_scaleMin, IRange(0.0, 5.0), "scale_min", "min");
  KnobDefinesGeometry(f);
  Tooltip(f, "The low end of the random size range, as a multiplier. Set min and max to the same value "
             "to turn the randomness off while leaving it switched on.");
  Float_knob(f, &_scaleMax, IRange(0.0, 5.0), "scale_max", "max");
  KnobDefinesGeometry(f);
  Tooltip(f, "The high end of the random size range, as a multiplier.");
  Float_knob(f, &_scaleBias, IRange(-1.0, 1.0), "scale_bias", "bias");
  KnobDefinesGeometry(f);
  Tooltip(f, "Slides the random sizes toward one end without changing the range: below zero gives more SMALL copies, above zero more LARGE ones. At 0 the sizes are spread evenly between min and max.");
  Float_knob(f, &_scaleShape, IRange(-1.0, 1.0), "scale_shape", "shape");
  KnobDefinesGeometry(f);
  Tooltip(f, "Gathers the random sizes toward the MIDDLE of the range below zero, or pushes them out to the two ENDS above it - at +1 most copies are near min or near max and few are in between. The average size stays where it was; only the spread changes.");
  XYZ_knob(f, _offset, "offset", "local offset");
  KnobDefinesGeometry(f);
  Tooltip(f, "Offset applied in the prototype's local space (it follows the copy's rotation and scale).");
  Bool_knob(f, &_randomOffset, "random_offset", "random offset");
  KnobDefinesGeometry(f);
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Add a per-copy random local offset in the min..max range per axis (seeded, stable per id).");
  XYZ_knob(f, _offMin, "offset_min", "min");
  KnobDefinesGeometry(f);
  Tooltip(f, "The low end of the random offset range, per axis, in the copy's own local axes.");
  XYZ_knob(f, _offMax, "offset_max", "max");
  KnobDefinesGeometry(f);
  Tooltip(f, "The high end of the random offset range, per axis, in the copy's own local axes.");
  XYZ_knob(f, _offVariance, "offset_variance", "offset variance");
  KnobDefinesGeometry(f);
  Tooltip(f, "Simple jitter: every copy is moved by +/- this much (per axis, local space) on top of the offset above.");
  Divider(f, "randomness");
  Int_knob(f, &_seed, "seed", "seed");
  KnobDefinesGeometry(f);
  Tooltip(f, "The starting point for every random choice on this node - rotation, scale, offset, "
             "variant and colour. Change it to reshuffle them all at once while keeping the ranges.");
  String_knob(f, &_idAttr, "id_attr", "id attribute");
  KnobDefinesGeometry(f);
  Tooltip(f, "An int primvar that identifies a point across frames, so a copy keeps its random "
             "rotation, size and variant as the point moves.\n\n"
             "Left empty the prim's own 'ids' are used, and failing that the point INDEX - which "
             "is fine for a static mesh and wrong for a particle stream, where a particle dying "
             "renumbers everything after it and every copy downstream changes at once.");

  Tab_knob(f, "Attributes");
  PyScript_knob(f, ctp::helpScript(3), "help_attributes", "help...");
  Tooltip(f, "Open the popup help for the Attributes page.");
  String_knob(f, &_colorAttr, "color_attr", "colour attribute");
  KnobDefinesGeometry(f);
  Tooltip(f, "Which primvar the colour is read from. Empty means primvars:displayColor, which is "
             "what USD uses and what this node did before - name another one when the colour a "
             "pipeline cares about lives somewhere else.");
  String_knob(f, &_copyAttrs, "copy_attrs", "copy attributes");
  KnobDefinesGeometry(f);
  Tooltip(f, "A comma separated list of extra primvars to carry from the source point onto every "
             "copy, as instance-rate primvars - for a shader or a renderer downstream that wants "
             "something this node has no knob for.\n\n"
             "Velocities are always carried whether or not they are listed: motion blur is built "
             "on them.");
  Bool_knob(f, &_copyColor, "copy_color", "copy display colour");
  KnobDefinesGeometry(f);
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Take primvars:displayColor of the source (per point, or constant) onto every copy as the instancer's "
             "per-instance displayColor.");
  Float_knob(f, &_colorVarHue, IRange(0.0, 1.0), "color_var_hue", "colour variance hue");
  KnobDefinesGeometry(f);
  Tooltip(f, "Per-copy random hue shift, as a fraction of the colour wheel, applied to the copy's "
             "display colour. It works with no source colour too, starting from white.");
  Float_knob(f, &_colorVarSat, IRange(0.0, 1.0), "color_var_sat", "colour variance saturation");
  KnobDefinesGeometry(f);
  Tooltip(f, "Per-copy random saturation shift applied to the copy's display colour, so the copies are "
             "not all equally vivid. 0 leaves the saturation alone.");
  Float_knob(f, &_colorVarVal, IRange(0.0, 1.0), "color_var_val", "colour variance value");
  KnobDefinesGeometry(f);
  Tooltip(f, "Per-copy random tint / brightness of the instance colour (starts from white when the source has none).");
  Divider(f, "attributes found on the points input");
  Button(f, "refresh_attrs", "refresh list");
  SetFlags(f, Knob::KNOB_CHANGED_ALWAYS);
  Tooltip(f, "Show the prims and attributes (primvars, points, ids, widths ...) seen on the points input during the "
             "last build (view or render the node first, then refresh), plus the prototypes.");
  Multiline_String_knob(f, &_attrListText, "attribute_list", "", 10);
  SetFlags(f, Knob::NO_ANIMATION | Knob::READ_ONLY | Knob::OUTPUT_ONLY | Knob::DO_NOT_WRITE | Knob::STARTLINE);
  Tooltip(f, "Prims of the points input with their authored attributes (name : type [interpolation] count). Read-only.");

  // ------------------------------------------------------------------ Paint tab
  Tab_knob(f, "Paint");
  PyScript_knob(f, ctp::helpScript(4), "help_paint", "help...");
  Tooltip(f, "Open the popup help for the Paint page.");
  Named_Text_knob(f, "paint_help", "",
    "<b>Paint weights and colour on the source geometry in the 3D viewer</b> (GeoCard, meshes, terrain).<br>"
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
  KnobDefinesGeometry(f);   // the guide heat map follows the current layer
  Tooltip(f, "Which layer the brush paints: density, scale, rotation, variant, scatter or colour.");
  Enumeration_knob(f, &_paintMode, kPaintModeNames, "paint_mode", "mode");
  Tooltip(f, "Every layer is an offset from the node's current values (0 = as the knobs say). add / subtract move the "
             "layer up / down by opacity x value per sample, set drives it towards 'value', smooth averages it.");
  Float_knob(f, &_paintRadius, IRange(0.01, 20.0), "paint_radius", "radius");
  Tooltip(f, "Brush radius in world units (Shift+LMB drag in the viewer also changes it).");
  Float_knob(f, &_paintHardness, IRange(0.0, 1.0), "paint_hardness", "hardness");
  Tooltip(f, "How sharp the brush edge is. 0 fades out smoothly from the centre to the rim; 1 paints "
             "the full value right up to the edge.");
  Float_knob(f, &_paintOpacity, IRange(0.0, 1.0), "paint_opacity", "opacity");
  Tooltip(f, "How much of the value each pass of the brush lays down. Low values let you build a "
             "weight up gradually over several strokes.");
  Float_knob(f, &_paintValue, IRange(-8.0, 8.0), "paint_value", "value");
  Tooltip(f, "The amount the brush works with (layers hold -8 .. +8). add/subtract: opacity x value per sample; "
             "set: the layer moves towards this value. Density: +1 = every point gets a copy, -1 = none; scale: "
             "x 'scale per unit'; rotation: x degrees; variant: +1 = next prototype; scatter: -1 = no scattered points.");
  Color_knob(f, _paintColor, "paint_color", "brush colour");
  Tooltip(f, "Colour painted by the 'colour' layer (and used by flood fill on that layer).");
  Bool_knob(f, &_paintShow, "paint_show", "show weights (heat map)");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Draw the source points as a heat map of the current layer: blue = 0 ... red = 'heat max'; the colour layer in colour.");
  Float_knob(f, &_paintHeatMax, IRange(0.01, 16.0), "paint_heat_max", "heat max");
  KnobDefinesGeometry(f);
  Tooltip(f, "The weight shown as red in the heat map. 0 means auto, which stretches the colours to "
             "whatever the layer's current maximum is.");
  Float_knob(f, &_paintPointSize, IRange(1.0, 15.0), "paint_point_size", "point size");
  Tooltip(f, "How large the source points are drawn while painting. It only affects what you see in "
             "the Viewer - a bigger dot is easier to aim at on dense geometry.");
  Bool_knob(f, &_paintLive, "paint_live", "update copies while painting");
  Tooltip(f, "Rebuild the copies during the stroke rather than when the mouse is released. Turn it off "
             "on heavy scenes, where waiting for the release is much faster.");
  Bool_knob(f, &_paintOcclusion, "paint_occlusion", "occlusion test (only visible points)");
  Tooltip(f, "Only paint points that are visible from the camera: a point behind another face (or on the far side "
             "of the geometry) is skipped. Off = everything inside the brush sphere is painted, back faces included.");
  Button(f, "paint_fill_layer", "flood fill layer");
  SetFlags(f, Knob::KNOB_CHANGED_ALWAYS);
  Tooltip(f, "Set every point of the current layer to 'value' (colour layer: the brush colour with full coverage).");
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
  KnobDefinesGeometry(f);
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Copy probability = 'density' (Copy tab) + painted weight, clamped to 0..1: unpainted areas keep the "
             "current density, add raises it (density 0 + paint 1 = copies only where painted), subtract lowers it "
             "(density 1 + paint -1 = holes).");
  Bool_knob(f, &_paintScaleEnable, "paint_scale_enable", "scale layer");
  KnobDefinesGeometry(f);
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Let the painted scale layer change the size of the copies, by the amount per unit weight "
             "below. Off, the layer is kept but ignored.");
  Float_knob(f, &_paintScaleAmount, IRange(0.0, 4.0), "paint_scale_amount", "scale per unit weight");
  KnobDefinesGeometry(f);
  Tooltip(f, "The copy's scale is multiplied by (1 + weight x this): unpainted = current scale, +1 doubles it "
             "(with 1.0), -0.5 halves it.");
  Bool_knob(f, &_paintRotEnable, "paint_rot_enable", "rotation layer");
  KnobDefinesGeometry(f);
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Let the painted rotation layer turn the copies about the axis chosen below. Off, the "
             "layer is kept but ignored.");
  Enumeration_knob(f, &_paintRotAxis, kPaintAxisNames, "paint_rot_axis", "axis");
  KnobDefinesGeometry(f);
  Tooltip(f, "Which of the copy's own local axes the painted rotation turns it about.");
  Float_knob(f, &_paintRotAmount, IRange(-360.0, 360.0), "paint_rot_amount", "degrees at full weight");
  KnobDefinesGeometry(f);
  Tooltip(f, "How far a full weight of 1 turns the copy, in degrees. Negative values turn it the other "
             "way.");
  Bool_knob(f, &_paintVariantEnable, "paint_variant_enable", "variant layer");
  KnobDefinesGeometry(f);
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "The weight shifts the picked variant: +1 = next prototype, -1 = previous (wraps around); unpainted = as 'pick variant' says.");
  Bool_knob(f, &_paintColorEnable, "paint_color_enable", "colour layer");
  KnobDefinesGeometry(f);
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "The painted colour drives the copies' displayColor: where the colour layer has coverage the copy colour "
             "is replaced by / multiplied with the painted colour. Use mode = copies to see it in ScanlineRender2 "
             "(per-instance colours are only visible in Hydra viewers).");
  Enumeration_knob(f, &_paintColorMode, kPaintColorModeNames, "paint_color_mode", "");
  KnobDefinesGeometry(f);
  Tooltip(f, "Whether the painted colour REPLACES the copy's colour or is MULTIPLIED into it. "
             "Multiplying keeps the source colour's variation and tints it; replacing throws it away.");
  Bool_knob(f, &_paintColorSource, "paint_color_source", "also write displayColor onto the source prims");
  KnobDefinesGeometry(f);
  Tooltip(f, "Author the painted colour as primvars:displayColor (vertex) on the source Mesh / Points prims as well.");
  Named_Text_knob(f, "paint_scatter_note", "", "<i>scatter layer</i>: scatter density x (1 + weight) when "
                  "'multiply by painted scatter layer' is on (Copy tab): -1 = no scattered points, +1 = twice as likely.");
  SetFlags(f, Knob::STARTLINE);
  CustomKnob1(ctp::PaintBrushKnob, f, this, "paint_data");
  SetFlags(f, Knob::NO_ANIMATION | Knob::INVISIBLE);
  KnobDefinesGeometry(f);

  Tab_knob(f, "About");
  Named_Text_knob(f, "about", "",
    "<b><font size=+2>CopyToPointsUSD</font></b> v" CTPU_VERSION "<br>"
    "<i>USD PointInstancer copy-to-points for Nuke's new 3D system.</i><br><br>"
    "Prototypes are copied once under the instancer; every copy is a position / orientation / scale entry. "
    "Bind materials to the prototype geometry upstream of the geo inputs.<br><br>"
    "<b>Created by Marten Blumen</b><br>Built for Nuke 15+ (Windows x64).");
  SetFlags(f, Knob::STARTLINE);
  PyScript_knob(f, ctp::helpScript(0), "help_about", "open help...");
  Tooltip(f, "Open the full popup help for this node.");
}

void CopyToPointsUSD::build_handles(ViewerContext* ctx)
{
  ctpLog("usd:build_handles", _paintEnable ? "paint on" : "paint off");
  // same registration as the AttributePainter plugin (Nuke 17 Hydra viewer):
  // the brush knob's build_handle() answers true while painting is enabled
  build_input_handles(ctx);
  build_knob_handles(ctx);
  GeomOp::build_handles(ctx);
  if (_paintEnable && ctx && ctx->viewer_mode() != VIEWER_2D) {
    if (Knob* k = knob("paint_data")) k->add_draw_handle(ctx);
  }
}

int CopyToPointsUSD::knob_changed(Knob* k)
{
  if (k && (k->is("refresh_attrs") || k->is("showPanel") || k->is("updateUI"))) {
    refreshAttrListKnob();
    if (k->is("refresh_attrs") || k->is("updateUI")) return 1;
  }
  if (k && (k->is("paint_clear_layer") || k->is("paint_clear_all") || k->is("paint_fill_layer"))) {
    int layer = _paintLayer;
    double value = _paintValue;
    float col[3] = { _paintColor[0], _paintColor[1], _paintColor[2] };
    if (Knob* kk = knob("paint_layer")) layer = int(kk->get_value());
    if (Knob* kk = knob("paint_value")) value = kk->get_value();
    if (Knob* kk = knob("paint_color")) for (int i = 0; i < 3; ++i) col[i] = float(kk->get_value(i));
    if (ctp::PaintBrushKnob* pk = dynamic_cast<ctp::PaintBrushKnob*>(knob("paint_data"))) {
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
  if (k && (k->is("showPanel") || k->is("random_rotate") || k->is("random_scale") || k->is("scatter_weighting") ||
            k->is("variant_mode") || k->is("align_mode") || k->is("random_offset") || k->is("paint_scale_enable") ||
            k->is("paint_rot_enable") || k->is("paint_color_enable") || k->is("paint_layer"))) {
    if (Knob* kk = knob("offset_min"))         kk->enable(_randomOffset);
    if (Knob* kk = knob("offset_max"))         kk->enable(_randomOffset);
    if (Knob* kk = knob("paint_scale_amount")) kk->enable(_paintScaleEnable);
    if (Knob* kk = knob("paint_rot_axis"))     kk->enable(_paintRotEnable);
    if (Knob* kk = knob("paint_rot_amount"))   kk->enable(_paintRotEnable);
    if (Knob* kk = knob("paint_color_mode"))   kk->enable(_paintColorEnable);
    if (Knob* kk = knob("paint_color_source")) kk->enable(_paintColorEnable);
    if (Knob* kk = knob("paint_color"))        kk->enable(_paintLayer == kPaintLayerColor);
    if (Knob* kk = knob("paint_heat_max"))     kk->enable(_paintLayer != kPaintLayerColor);
    if (Knob* kk = knob("rot_min"))       kk->enable(_randomRotate);
    if (Knob* kk = knob("rot_max"))       kk->enable(_randomRotate);
    if (Knob* kk = knob("scale_min"))     kk->enable(_randomScale);
    if (Knob* kk = knob("scale_max"))     kk->enable(_randomScale);
    if (Knob* kk = knob("scale_bias"))    kk->enable(_randomScale);
    if (Knob* kk = knob("scale_shape"))   kk->enable(_randomScale);
    if (Knob* kk = knob("scatter_bias"))  kk->enable(_scatterWeighting != 0);
    if (Knob* kk = knob("variant_seed"))  kk->enable(_variantMode == 1);
    if (Knob* kk = knob("forward_axis"))  kk->enable(_alignMode == kAlignUsdNormal || _alignMode == kAlignUsdDir);
    if (Knob* kk = knob("align_attr"))    kk->enable(_alignMode >= kAlignUsdDir);
    if (Knob* kk = knob("up"))            kk->enable(_alignMode != 0 || _scatterWeighting != 0);
    return 1;
  }
  return GeomOp::knob_changed(k);
}

void CopyToPointsUSD::append(Hash& h)
{
  GeomOp::append(h);
  h.append(_mode); h.append(_copiesInstanceable); h.append(_pointsSource); h.append(_maxCopyPoints); h.append(_hideSource); h.append(_maxInstances); h.append(_maxSourcePoints); h.append(_density);
  h.append(_guideMode); h.append(_guideSize); h.append(_guideHideCopies); h.append(_guidePurpose); h.append(_guideHeat);
  h.append(_scatterStick); h.append(_scatterRefFrame); h.append(_scatterPaintMode); h.append(_scatterPaintCount);
  h.append(_paintLayer); h.append(_paintHeatMax); h.append(_scatterUsePaint);
  h.append(_paintDensityEnable); h.append(_paintScaleEnable); h.append(_paintScaleAmount); h.append(_paintRotEnable);
  h.append(_paintRotAxis); h.append(_paintRotAmount); h.append(_paintVariantEnable); h.append(_paintColorEnable);
  h.append(_paintColorMode); h.append(_paintColorSource);
  { std::lock_guard<std::mutex> lock(_paintMutex); h.append(_paintVersion); h.append(_paint.npoints); }
  h.append(_scatterMode); h.append(_scatterCount); h.append(_scatterSeed); h.append(_scatterWeighting); h.append(_scatterBias); h.append(_scatterSeparation);
  h.append(_variantMode); h.append(_variantSeed); h.append(_variantAttr);
  h.append(_useSizeAttr); h.append(_sizeAttr); h.append(_scaleAttr);
  h.append(_idAttr); h.append(_colorAttr); h.append(_copyAttrs);
  h.append(_spinMode); h.append(_rollRate); h.append(_rollChannels);
  h.append(_alignMode); h.append(_alignAttr); h.append(_forwardAxis);
  for (int i = 0; i < 3; ++i) { h.append(_up[i]); h.append(_rotate[i]); h.append(_rotMin[i]); h.append(_rotMax[i]); h.append(_scaleXYZ[i]); h.append(_offset[i]); }
  h.append(_randomRotate); h.append(_rotVariance); h.append(_randomOffset);
  for (int i = 0; i < 3; ++i) { h.append(_offMin[i]); h.append(_offMax[i]); h.append(_offVariance[i]); }
  h.append(_scale); h.append(_useWidths); h.append(_randomScale); h.append(_scaleMin); h.append(_scaleMax);
  h.append(_scaleBias); h.append(_scaleShape);
  h.append(_seed); h.append(_copyColor); h.append(_colorVarHue); h.append(_colorVarSat); h.append(_colorVarVal);
}

// ==========================================================================
void CopyToPointsUSDEngine::processScenegraph(usg::GeomSceneContext& context)
{
  GeomOpEngine::processScenegraph(context);
  CopyToPointsUSD* op = dynamic_cast<CopyToPointsUSD*>(opPtr());
  if (!op) return;
  ctpLog("usd:process", "nInputs=" + std::to_string(nInputs())
                        + " times=" + std::to_string(context.numProcessTimes())
                        + " tid=" + std::to_string(::GetCurrentThreadId()));

  // Read the source geometry from input 0's composed stage and author ONLY
  // into the edit layer (editableStage() and editLayer() are exclusive).
  usg::StageRef stage = buildStageFromInput(context, 0);
  if (!stage) return;
  usg::LayerRef edit = editLayer();

  const std::string nodeName = sanitizeName(op->node_name());
  const usg::Path rootPath("/" + nodeName);
  const usg::Path instPath = rootPath.appendChild("instancer");
  const usg::Path protosPath = instPath.appendChild("Prototypes");
  const usg::Path guidePath = rootPath.appendChild("guide");

  // ---- prototypes: every root prim of every geo input -----------------------
  usg::XformPrim::defineInLayer(edit, rootPath);
  usg::PointInstancerPrim inst = usg::PointInstancerPrim::defineInLayer(edit, instPath);
  usg::ScopePrim::defineInLayer(edit, protosPath);
  usg::PathArray protoPaths;
  float protoRadius = 0.0f;                                // largest prototype extent (instancer extent)
  std::vector<double> protoPointCount;                     // points per prototype (memory guard)
  std::vector<std::vector<std::string> > protoGprims;      // gprim paths relative to proto_N (colour overrides)
  bool anySupportPrims = false;   // materials / lights copied along with the prototypes
  for (int i = 1; i < int(nInputs()); ++i) {
    usg::StageRef in = buildStageFromInput(context, i);
    if (!in) continue;
    {
      const fdk::Box3d bb = in->getWorldBbox(fdk::defaultTimeValue());
      if (!bb.isEmpty()) {
        const fdk::Vec3d mn = bb.min, mx = bb.max;
        const double r = 0.5 * std::sqrt((mx.x - mn.x) * (mx.x - mn.x) + (mx.y - mn.y) * (mx.y - mn.y) + (mx.z - mn.z) * (mx.z - mn.z));
        const double c = std::sqrt(mn.x * mn.x + mn.y * mn.y + mn.z * mn.z);   // prototypes may sit away from the origin
        protoRadius = std::max(protoRadius, float(r + c));
      }
    }
    // Root prims that are (or contain) geometry become prototypes.  Everything
    // else (the /materials scope written by GeoBindMaterial, lights, ...) is
    // copied to its ORIGINAL path so material:binding relationships inside the
    // prototypes still resolve.
    usg::PathArray roots, support;
    std::vector<std::string> rootStr;
    usg::PrimRange range = in->traverse();
    for (auto it = range.begin(); it != range.end(); ++it) {
      usg::Prim p = *it;
      if (p.getPath().parent() != usg::Path("/")) continue;
      const std::string nm = p.getName();
      if (!nm.empty() && nm[0] == '_') continue;   // _UsgLayerInfo etc.
      roots.push_back(p.getPath());
      rootStr.push_back(p.getPath().asString() + "/");
    }
    if (roots.empty()) continue;
    std::vector<char> hasGeo(roots.size(), 0);
    std::vector<double> rootPts(roots.size(), 0.0);
    std::vector<std::vector<std::string> > rootGprims(roots.size());
    for (auto it = range.begin(); it != range.end(); ++it) {
      usg::Prim p = *it;
      if (!p.isA<usg::GprimPrim>()) continue;
      const std::string ps = p.getPath().asString() + "/";
      for (size_t r = 0; r < roots.size(); ++r) {
        if (ps.compare(0, rootStr[r].size(), rootStr[r]) != 0) continue;
        hasGeo[r] = 1;
        if (p.isA<usg::PointBasedPrim>()) rootPts[r] += double(usg::PointBasedPrim(p).getPoints(fdk::defaultTimeValue()).size());
        // path relative to the root, including the root's own name (the wrapper's child)
        rootGprims[r].push_back(p.getPath().asString().substr(roots[r].parent().asString() == "/" ? 1 : roots[r].parent().asString().size() + 1));
        break;
      }
    }
    usg::LayerRef flat = in->flatten(false);
    if (!flat) continue;
    for (size_t r = 0; r < roots.size(); ++r) {
      if (!hasGeo[r]) { flat->copySpec(roots[r], edit, roots[r]); anySupportPrims = true; continue; }
      // proto_N is an Xform wrapper around the copied root prim so that copies can
      // reference it as an instanceable subtree (USD scenegraph instancing: the
      // geometry below the wrapper is shared by every copy)
      const usg::Path dst = protosPath.appendChild("proto_" + std::to_string(protoPaths.size()));
      usg::XformPrim::defineInLayer(edit, dst);
      flat->copySpec(roots[r], edit, dst.appendChild(roots[r].name()));
      protoPaths.push_back(dst);
      protoPointCount.push_back(rootPts[r]);
      protoGprims.push_back(rootGprims[r]);
    }
  }
  const size_t nVar = protoPaths.size();
  ctpLog("usd:prototypes", std::to_string(nVar));
  ctpLog("usd:proto radius", std::to_string(protoRadius));

  // ---- source geometry: meshes / points on input 0 (already in the stage) ---
  struct SrcPrim {
    usg::Path path;
    bool isMesh;
    std::vector<Vector3> P;      // world positions
    std::vector<Vector3> Pref;   // world positions at the scatter reference frame (stick mode), may be empty
    std::vector<Vector3> N;      // per point normals (world), may be empty
    std::vector<Vector4> C;      // per point colours, may be empty
    std::vector<int64_t> ids;    // point ids, may be empty
    std::vector<float>   widths; // may be empty
    // Per-point velocity, world space, USD units (per SECOND).  A renderer uses
    // this to blur a particle stream whose topology changes: it extrapolates
    // every motion key from one sample instead of trying to pair particles
    // between two samples that do not hold the same ones.
    std::vector<Vector3> vel;
    // whatever the align attribute names, when it is not the normals
    std::vector<Vector3> alignV;
    std::vector<Vector4> alignQ;
    // whatever the named-attribute knobs asked for, when they name something
    std::vector<float>   sizeA;    // size attribute
    std::vector<Vector3> scaleA;   // per-axis scale attribute
    std::vector<int>     idA;      // id attribute
    std::vector<int>     varA;     // variant attribute
    std::vector<Vector3> birthP;   // primvars:initialP, for roll
    std::vector<int>     chan;     // primvars:channel, for roll channels
    // the 'copy attributes' list, kept per name and per point so a copy can be
    // handed the value of the point it sits on
    std::vector<std::vector<float> >   extraF;
    std::vector<std::vector<Vector3> > extraV;
    usg::IntArray fvc, fvi;      // mesh topology
    Vector4 constColor; bool hasConstColor;
  };

  // the 'copy attributes' list, split once rather than per prim per frame
  std::vector<std::string> extraNames;
  {
    const std::string list = ctp::trimCopy(op->_copyAttrs);
    std::string cur;
    for (size_t i = 0; i <= list.size(); ++i) {
      if (i == list.size() || list[i] == ',') {
        const std::string t = ctp::trimCopy(cur);
        if (!t.empty()) extraNames.push_back(t);
        cur.clear();
      }
      else cur.push_back(list[i]);
    }
  }

  const fdk::TimeValueSet& times = context.processTimes();
  std::vector<fdk::TimeValue> timeList(times.begin(), times.end());
  if (timeList.empty()) timeList.push_back(fdk::defaultTimeValue());

  // painted layers (copied under the lock; the brush may be writing on the UI thread)
  PaintLayers paint;
  {
    std::lock_guard<std::mutex> lock(op->_paintMutex);
    paint = op->_paint;
  }
  const bool usePaint = paint.npoints > 0;   // effects are gated individually; the guide shows the weights

  bool refMissing = false;   // the reference frame is not in the input's stage
  for (size_t ti = 0; ti < timeList.size(); ++ti) {
    const fdk::TimeValue time = timeList[ti];
    std::vector<SrcPrim> srcs;
    usg::PrimRange range = stage->traverse();
    for (auto it = range.begin(); it != range.end(); ++it) {
      usg::Prim p = *it;
      const std::string ps = p.getPath().asString();
      if (ps.compare(0, rootPath.asString().size(), rootPath.asString()) == 0) continue;   // ours
      const bool isMesh = p.isA<usg::MeshPrim>();
      const bool isPts = !isMesh && p.isA<usg::PointsPrim>();
      if (!isMesh && !isPts) continue;
      SrcPrim s; s.path = p.getPath(); s.isMesh = isMesh; s.hasConstColor = false; s.constColor = Vector4(1, 1, 1, 1);
      usg::PointBasedPrim pb(p);
      usg::Vec3fArray pts = pb.getPoints(time);
      const fdk::Mat4d M = usg::XformablePrim::getConcatenatedMatrixAtPrim(p, time);
      s.P.reserve(pts.size());
      for (size_t k = 0; k < pts.size(); ++k) s.P.push_back(toV3(M.transform(fdk::Vec3d(pts[k].x, pts[k].y, pts[k].z))));
      if (op->_scatterStick == kScatterStickRef && op->_scatterMode != kScatterOff) {
        // A FRAME, not seconds.  Measured: a shutter of 1 at frame 35 has the
        // engine asking for times 34.5, 34.75, 35.25 and 35.5, so fdk::TimeValue
        // here is already a frame number and dividing by the rate asked for frame
        // 1.25 when the artist typed 30.
        const fdk::TimeValue refTime = fdk::TimeValue(op->_scatterRefFrame);
        usg::Vec3fArray rp = pb.getPoints(refTime);
        if (rp.size() == pts.size()) {
          const fdk::Mat4d Mr = usg::XformablePrim::getConcatenatedMatrixAtPrim(p, refTime);
          s.Pref.reserve(rp.size());
          for (size_t k = 0; k < rp.size(); ++k) s.Pref.push_back(toV3(Mr.transform(fdk::Vec3d(rp[k].x, rp[k].y, rp[k].z))));
          // Did the stage actually HAVE that frame?
          //
          // Asking a stage for a time it holds no sample at is not an error - it
          // hands back the nearest one it has - so a reference pose identical to
          // the current pose is what "there is no such sample" looks like, and
          // the scatter then sticks to the shape it was already on and nothing
          // appears to happen.  Geometry built by nodes only carries the times
          // the engine asked for, which is this frame; a USD file read off disk
          // carries its own samples and works.  Silence here was the whole
          // problem, so it is said out loud once.
          if (op->_scatterRefFrame != int(std::floor(double(time) + 0.5)) && !s.Pref.empty()) {
            double moved = 0.0;
            for (size_t k = 0; k < s.Pref.size() && k < s.P.size(); ++k)
              moved += (s.P[k] - s.Pref[k]).length();
            if (moved <= 1e-9) { refMissing = true; ctpLog("usd:refmissing", ps); }
          }
        }
      }
      if (isMesh) {
        usg::MeshPrim m(p);
        s.fvc = m.getFaceVertexCounts(time);
        s.fvi = m.getFaceVertexIndices(time);
      }
      // per-point normals: the 'normals' attribute (vertex or faceVarying, averaged
      // per point) or, for meshes without usable normals, computed from the faces
      // (area weighted) - so 'align = normal' always has something to point along.
      // velocities travel with the points if the source has them
      {
        usg::Vec3fArray vel = pb.getVelocities(time);
        if (vel.size() == pts.size()) {
          s.vel.reserve(vel.size());
          for (size_t k = 0; k < vel.size(); ++k) {
            // a velocity is a vector: rotate and scale it, do not translate it
            const fdk::Vec3d v3 = M.vecTransform(fdk::Vec3d(vel[k].x, vel[k].y, vel[k].z));
            s.vel.push_back(Vector3(float(v3.x), float(v3.y), float(v3.z)));
          }
        }
      }
      // The align attribute, when the mode reads one.  Velocities are already
      // read above, so naming them costs nothing extra; anything else is looked
      // up as a plain attribute and then as a primvar, which is where a Points
      // prim usually carries it.
      if (op->_alignMode >= kAlignUsdDir) {
        const std::string an = ctp::trimCopy(op->_alignAttr);
        if (an == "velocities" || an == "v") {
          s.alignV = s.vel;
        }
        else if (!an.empty()) {
          usg::Attribute a = p.getAttr(usg::Token(an));
          if (!a) a = p.getAttr(usg::Token("primvars:" + an));
          if (a) {
            if (op->_alignMode == kAlignUsdQuat) {
              usg::Vec4fArray q;
              if (a.getValue(q, time) && q.size() == pts.size()) {
                s.alignQ.reserve(q.size());
                for (size_t k = 0; k < q.size(); ++k)
                  s.alignQ.push_back(Vector4(q[k].x, q[k].y, q[k].z, q[k].w));
              }
            }
            else {
              usg::Vec3fArray v;
              if (a.getValue(v, time) && v.size() == pts.size()) {
                s.alignV.reserve(v.size());
                for (size_t k = 0; k < v.size(); ++k) {
                  // a direction is a vector, so it rotates with the prim but does
                  // not translate; euler angles are not a vector at all and are
                  // left exactly as authored
                  if (op->_alignMode == kAlignUsdDir) {
                    const fdk::Vec3d w = M.vecTransform(fdk::Vec3d(v[k].x, v[k].y, v[k].z));
                    s.alignV.push_back(Vector3(float(w.x), float(w.y), float(w.z)));
                  }
                  else {
                    s.alignV.push_back(Vector3(v[k].x, v[k].y, v[k].z));
                  }
                }
              }
            }
          }
        }
      }

      // ---- the named attributes -------------------------------------------------
      // Every one is optional and every one is looked up plain and then under
      // "primvars:", because the same value is authored either way depending on
      // what wrote it - a Points prim usually carries primvars, a file off disk
      // often does not.
      {
        const size_t np = pts.size();
        if (op->_useSizeAttr) {
          const std::string an = ctp::trimCopy(op->_sizeAttr);
          if (!an.empty()) {
            usg::Attribute a = p.getAttr(usg::Token(an));
            if (!a) a = p.getAttr(usg::Token("primvars:" + an));
            usg::FloatArray v;
            if (a && a.getValue(v, time) && v.size() == np)
              s.sizeA.assign(v.begin(), v.end());
          }
        }
        {
          const std::string an = ctp::trimCopy(op->_scaleAttr);
          if (!an.empty()) {
            usg::Attribute a = p.getAttr(usg::Token(an));
            if (!a) a = p.getAttr(usg::Token("primvars:" + an));
            usg::Vec3fArray v;
            if (a && a.getValue(v, time) && v.size() == np) {
              s.scaleA.reserve(np);
              for (size_t k = 0; k < np; ++k) s.scaleA.push_back(Vector3(v[k].x, v[k].y, v[k].z));
            }
            else {
              // a single float scaling all three axes is a reasonable thing to
              // have authored, and refusing it would be pedantry
              usg::FloatArray fv;
              if (a && a.getValue(fv, time) && fv.size() == np) {
                s.scaleA.reserve(np);
                for (size_t k = 0; k < np; ++k) s.scaleA.push_back(Vector3(fv[k], fv[k], fv[k]));
              }
            }
          }
        }
        {
          const std::string an = ctp::trimCopy(op->_idAttr);
          if (!an.empty()) {
            usg::Attribute a = p.getAttr(usg::Token(an));
            if (!a) a = p.getAttr(usg::Token("primvars:" + an));
            usg::IntArray v;
            if (a && a.getValue(v, time) && v.size() == np) s.idA.assign(v.begin(), v.end());
          }
        }
        if (op->_variantMode == 2) {
          const std::string an = ctp::trimCopy(op->_variantAttr);
          if (!an.empty()) {
            usg::Attribute a = p.getAttr(usg::Token(an));
            if (!a) a = p.getAttr(usg::Token("primvars:" + an));
            usg::IntArray v;
            if (a && a.getValue(v, time) && v.size() == np) s.varA.assign(v.begin(), v.end());
            else {
              usg::FloatArray fv;
              if (a && a.getValue(fv, time) && fv.size() == np) {
                s.varA.reserve(np);
                for (size_t k = 0; k < np; ++k) s.varA.push_back(int(fv[k]));
              }
            }
          }
        }
        if (!extraNames.empty()) {
          s.extraF.resize(extraNames.size());
          s.extraV.resize(extraNames.size());
          for (size_t e = 0; e < extraNames.size(); ++e) {
            usg::Attribute a = p.getAttr(usg::Token(extraNames[e]));
            if (!a) a = p.getAttr(usg::Token("primvars:" + extraNames[e]));
            if (!a) continue;
            usg::Vec3fArray v3;
            if (a.getValue(v3, time) && v3.size() == np) {
              s.extraV[e].reserve(np);
              for (size_t k = 0; k < np; ++k) s.extraV[e].push_back(Vector3(v3[k].x, v3[k].y, v3[k].z));
              continue;
            }
            usg::FloatArray fv;
            if (a.getValue(fv, time) && fv.size() == np) { s.extraF[e].assign(fv.begin(), fv.end()); continue; }
            usg::IntArray iv;
            if (a.getValue(iv, time) && iv.size() == np) {
              s.extraF[e].reserve(np);
              for (size_t k = 0; k < np; ++k) s.extraF[e].push_back(float(iv[k]));
            }
          }
        }
        if (op->_spinMode == ctp::kSpinRoll) {
          usg::Attribute a = p.getAttr(usg::Token("primvars:initialP"));
          if (!a) a = p.getAttr(usg::Token("initialP"));
          usg::Vec3fArray v;
          if (a && a.getValue(v, time) && v.size() == np) {
            s.birthP.reserve(np);
            for (size_t k = 0; k < np; ++k) {
              const fdk::Vec3d w = M.transform(fdk::Vec3d(v[k].x, v[k].y, v[k].z));
              s.birthP.push_back(Vector3(float(w.x), float(w.y), float(w.z)));
            }
          }
          if (op->_rollChannels != 0) {
            usg::Attribute c = p.getAttr(usg::Token("primvars:channel"));
            if (!c) c = p.getAttr(usg::Token("channel"));
            usg::IntArray cv;
            if (c && c.getValue(cv, time) && cv.size() == np) s.chan.assign(cv.begin(), cv.end());
          }
        }
      }

      usg::Vec3fArray nrm = pb.getNormals(time);
      if (nrm.size() != pts.size() && isMesh) {
        usg::Attribute npa = p.getAttr(usg::Token("primvars:normals"));
        if (npa) {
          usg::Vec3fArray v;
          if (npa.getValue(v, time) && (v.size() == pts.size() || v.size() == s.fvi.size())) nrm = v;
        }
      }
      const fdk::Mat4d Minv = M.inverse();   // normalTransform() = transpose(this) * n, so pass the inverse
      if (nrm.size() == pts.size()) {
        for (size_t k = 0; k < nrm.size(); ++k) {
          fdk::Vec3d n3 = Minv.normalTransform(fdk::Vec3d(nrm[k].x, nrm[k].y, nrm[k].z));
          Vector3 nn(float(n3.x), float(n3.y), float(n3.z)); nn.normalize(); s.N.push_back(nn);
        }
      }
      else if (isMesh && !pts.empty() && (nrm.size() == s.fvi.size() || !s.fvc.empty())) {
        std::vector<Vector3> acc(pts.size(), Vector3(0, 0, 0));
        const bool fromFV = (nrm.size() == s.fvi.size() && !nrm.empty());
        size_t v = 0;
        for (size_t f = 0; f < s.fvc.size(); ++f) {
          const int nv = s.fvc[f];
          if (nv >= 3 && v + size_t(nv) <= s.fvi.size()) {
            bool ok = true;
            for (int k = 0; k < nv; ++k) if (s.fvi[v + k] < 0 || size_t(s.fvi[v + k]) >= pts.size()) { ok = false; break; }
            if (ok) {
              if (fromFV) {
                for (int k = 0; k < nv; ++k) {
                  fdk::Vec3d n3 = Minv.normalTransform(fdk::Vec3d(nrm[v + k].x, nrm[v + k].y, nrm[v + k].z));
                  acc[size_t(s.fvi[v + k])] += Vector3(float(n3.x), float(n3.y), float(n3.z));
                }
              }
              else {
                // face normal (world space, area weighted) via the polygon's Newell normal
                Vector3 fn(0, 0, 0);
                for (int k = 0; k < nv; ++k) {
                  const Vector3& a = s.P[size_t(s.fvi[v + k])];
                  const Vector3& b = s.P[size_t(s.fvi[v + (k + 1) % nv])];
                  fn.x += (a.y - b.y) * (a.z + b.z);
                  fn.y += (a.z - b.z) * (a.x + b.x);
                  fn.z += (a.x - b.x) * (a.y + b.y);
                }
                for (int k = 0; k < nv; ++k) acc[size_t(s.fvi[v + k])] += fn;
              }
            }
          }
          v += size_t(std::max(nv, 0));
        }
        s.N.resize(pts.size());
        for (size_t k = 0; k < pts.size(); ++k) {
          Vector3 nn = acc[k];
          const float l = nn.length();
          s.N[k] = (l > 1e-12f) ? nn * (1.0f / l) : Vector3(0, 1, 0);
        }
      }
      if (op->_copyColor) {
        usg::GprimPrim g(p);
        usg::Vec3fArray col;
        // a named colour primvar wins; empty means displayColor, which is what
        // USD uses and what this node read before
        const std::string can = ctp::trimCopy(op->_colorAttr);
        if (!can.empty()) {
          usg::Attribute ca = p.getAttr(usg::Token(can));
          if (!ca) ca = p.getAttr(usg::Token("primvars:" + can));
          if (ca) ca.getValue(col, time);
        }
        if (col.empty()) col = g.getDisplayColor(time);
        if (col.size() == pts.size()) { for (size_t k = 0; k < col.size(); ++k) s.C.push_back(Vector4(col[k].x, col[k].y, col[k].z, 1.0f)); }
        else if (col.size() == 1) { s.hasConstColor = true; s.constColor = Vector4(col[0].x, col[0].y, col[0].z, 1.0f); }
      }
      if (!isMesh) {
        usg::PointsPrim pp(p);
        usg::Int64Array ids = pp.getIds(time);
        if (ids.size() == pts.size()) s.ids.assign(ids.begin(), ids.end());
        usg::FloatArray w = pp.getWidths(time);
        if (w.size() == pts.size()) s.widths.assign(w.begin(), w.end());
      }
      srcs.push_back(s);
    }

    // attribute report for the Attributes tab (first time sample only)
    if (ti == 0) {
      std::ostringstream rep;
      rep << "points input: " << srcs.size() << " geometry prim(s)\n";
      for (size_t si = 0; si < srcs.size() && si < 64; ++si) {
        usg::Prim p = stage->getPrimAtPath(srcs[si].path);
        if (!p) continue;
        rep << "  " << srcs[si].path.asString() << "  (" << p.getTypeName().asString() << ", " << srcs[si].P.size() << " points)\n";
        const std::vector<usg::Token> names = p.getAttributes();
        for (size_t a = 0; a < names.size(); ++a) {
          usg::Attribute at = p.getAttr(names[a]);
          if (!at) continue;
          rep << "      " << names[a].asString() << " : " << at.getTypeName();
          const usg::Token interp = at.getInterpolation();
          if (!interp.asString().empty()) rep << " [" << interp.asString() << "]";
          const size_t n = at.getArraySize(time);
          if (n) rep << " x" << n;
          rep << "\n";
        }
      }
      if (srcs.size() > 64) rep << "  ...\n";
      rep << "prototypes: " << nVar << "\n";
      for (size_t k = 0; k < protoPaths.size(); ++k) rep << "  " << protoPaths[k].asString() << "\n";
      std::lock_guard<std::mutex> lock(op->_reportMutex);
      op->_attrReport = rep.str();
    }

    // hide the source geometry if asked
    if (op->_hideSource && ti == 0) {
      for (size_t si = 0; si < srcs.size(); ++si) {
        usg::Prim p = stage->getPrimAtPath(srcs[si].path);
        if (p) { usg::ImageablePrim ov = usg::ImageablePrim::overrideInLayer(edit, p); if (ov) ov.setVisibility(usg::Token("invisible")); }
      }
    }

    // ---- flat point / triangle lists for scatter --------------------------------
    std::vector<Vector3> allPts, allN, allRef;
    bool refComplete = (op->_scatterStick == kScatterStickRef);
    std::vector<unsigned> triIdx, triObj, base;
    std::vector<Vector4> allC; std::vector<uint8_t> hasC;
    for (size_t si = 0; si < srcs.size(); ++si) {
      const SrcPrim& s = srcs[si];
      base.push_back(unsigned(allPts.size()));
      hasC.push_back(s.C.size() == s.P.size() ? 1 : 0);
      if (s.Pref.size() != s.P.size()) refComplete = false;
      for (size_t k = 0; k < s.P.size(); ++k) {
        allPts.push_back(s.P[k]);
        allRef.push_back(k < s.Pref.size() ? s.Pref[k] : s.P[k]);
        allC.push_back(hasC.back() ? s.C[k] : (s.hasConstColor ? s.constColor : Vector4(1, 1, 1, 1)));
      }
      if (s.isMesh) {
        size_t v = 0;
        for (size_t f = 0; f < s.fvc.size(); ++f) {
          const int nv = s.fvc[f];
          if (nv >= 3 && v + size_t(nv) <= s.fvi.size()) {
            bool ok = true;
            for (int k = 0; k < nv; ++k) if (s.fvi[v + k] < 0 || size_t(s.fvi[v + k]) >= s.P.size()) { ok = false; break; }
            if (ok) {
              for (int k = 1; k + 1 < nv; ++k) {
                triIdx.push_back(base.back() + unsigned(s.fvi[v]));
                triIdx.push_back(base.back() + unsigned(s.fvi[v + k]));
                triIdx.push_back(base.back() + unsigned(s.fvi[v + k + 1]));
                triObj.push_back(unsigned(si));
              }
            }
          }
          v += size_t(std::max(nv, 0));
        }
      }
    }

    // ---- brush cache: world-space points + triangles for the viewer brush -----------
    if (ti == 0) {
      std::vector<Vector3> tris;
      tris.reserve(triIdx.size());
      for (size_t k = 0; k < triIdx.size(); ++k) tris.push_back(allPts[triIdx[k]]);
      std::lock_guard<std::mutex> lock(op->_paintMutex);
      op->_paintPts = allPts;
      op->_paintTris.swap(tris);
      op->_paintBase = base;
    }

    // ---- painted colour written onto the source prims as displayColor ------------------
    if (op->_paintColorEnable && op->_paintColorSource && paint.layerHasData(kPaintLayerColA)) {
      for (size_t si = 0; si < srcs.size(); ++si) {
        const SrcPrim& sp = srcs[si];
        const size_t np = sp.P.size();
        if (!np || base[si] + np > paint.npoints) continue;
        usg::Prim p = stage->getPrimAtPath(sp.path);
        if (!p) continue;
        usg::GprimPrim ov = usg::GprimPrim::overrideInLayer(edit, p);
        if (!ov) continue;
        usg::Vec3fArray col(np);
        for (size_t k = 0; k < np; ++k) {
          const size_t pi = base[si] + k;
          Vector4 c = (k < sp.C.size()) ? sp.C[k] : (sp.hasConstColor ? sp.constColor : Vector4(1, 1, 1, 1));
          const float a = std::min(1.0f, std::max(0.0f, paint.get(kPaintLayerColA, pi)));
          if (a > 0.0f) {
            Vector4 pc(paint.get(kPaintLayerColR, pi), paint.get(kPaintLayerColG, pi), paint.get(kPaintLayerColB, pi), c.w);
            if (op->_paintColorMode == kPaintColorMultiply) { pc.x *= c.x; pc.y *= c.y; pc.z *= c.z; }
            c = Vector4(lerpf(c.x, pc.x, a), lerpf(c.y, pc.y, a), lerpf(c.z, pc.z, a), c.w);
          }
          col[k] = fdk::Vec3f(c.x, c.y, c.z);
        }
        authorDisplayColor(static_cast<usg::Prim&>(ov), col, usg::GeomTokens.vertex, time);
      }
    }

    // ---- scatter --------------------------------------------------------------
    std::vector<ScatterPoint> scatter;
    std::string scatterInfo;
    if (op->_scatterMode != kScatterOff && (op->_scatterCount > 0 || (op->_scatterUsePaint && op->_scatterPaintMode == kScatterPaintAddRemove && op->_scatterPaintCount > 0))) {
      ScatterParams sp;
      sp.weighting = op->_scatterWeighting; sp.bias = op->_scatterBias; sp.usePaint = op->_scatterUsePaint; sp.seed = op->_scatterSeed;
      sp.count = op->_scatterCount; sp.separation = op->_scatterSeparation;
      sp.up = Vector3(float(op->_up[0]), float(op->_up[1]), float(op->_up[2]));
      sp.refPts = (op->_scatterStick == kScatterStickRef && refComplete) ? &allRef : nullptr;
      sp.uniformFaces = (op->_scatterStick == kScatterStickTopology);
      sp.paintMode = op->_scatterPaintMode; sp.paintCount = op->_scatterPaintCount;
      scatterOnMesh(sp, allPts, triIdx, triObj, paint, scatter, scatterInfo);
    }

    // ---- gather copies -------------------------------------------------------------
    GatherParams g;
    g.seed = uint32_t(op->_seed); g.vseed = uint32_t(op->_variantSeed); g.nVar = int(std::max<size_t>(1, nVar));
    g.axisFix = axisToPlusZ(op->_forwardAxis);
    g.upVec = Vector3(float(op->_up[0]), float(op->_up[1]), float(op->_up[2]));
    g.userRot = eulerXYZ(float(op->_rotate[0]), float(op->_rotate[1]), float(op->_rotate[2]));
    g.hasUserRot = (op->_rotate[0] != 0.0 || op->_rotate[1] != 0.0 || op->_rotate[2] != 0.0);
    g.offset = Vector3(float(op->_offset[0]), float(op->_offset[1]), float(op->_offset[2]));
    g.hasOffset = (op->_offset[0] != 0.0 || op->_offset[1] != 0.0 || op->_offset[2] != 0.0);
    g.density = op->_density;
    g.variantMode = op->_variantMode;
    g.spinMode = op->_spinMode; g.rollRate = op->_rollRate;
    g.colorVarHue = op->_colorVarHue; g.colorVarSat = op->_colorVarSat; g.colorVarVal = op->_colorVarVal;
    switch (op->_alignMode) {
      case kAlignUsdNormal:
      case kAlignUsdDir:   g.alignMode = kAlignDirection; break;
      case kAlignUsdQuat:  g.alignMode = kAlignQuaternion; break;
      case kAlignUsdEuler: g.alignMode = kAlignEuler; break;
      default:             g.alignMode = kAlignNone; break;
    }
    g.randomRotate = op->_randomRotate;
    for (int i = 0; i < 3; ++i) { g.rotMin[i] = op->_rotMin[i]; g.rotMax[i] = op->_rotMax[i]; g.scaleXYZ[i] = op->_scaleXYZ[i]; }
    g.rotVariance = op->_rotVariance;
    g.randomOffset = op->_randomOffset;
    for (int i = 0; i < 3; ++i) { g.offMin[i] = op->_offMin[i]; g.offMax[i] = op->_offMax[i]; g.offVariance[i] = op->_offVariance[i]; }
    g.scale = op->_scale;
    g.randomScale = op->_randomScale; g.scaleMin = op->_scaleMin; g.scaleMax = op->_scaleMax;
    g.scaleBias = op->_scaleBias; g.scaleShape = op->_scaleShape;
    g.paintDensityEnable = op->_paintDensityEnable;
    g.paintVariantEnable = op->_paintVariantEnable;
    g.paintRotEnable = op->_paintRotEnable; g.paintRotAmount = op->_paintRotAmount; g.paintRotAxis = op->_paintRotAxis;
    g.paintScaleEnable = op->_paintScaleEnable; g.paintScaleAmount = op->_paintScaleAmount;
    g.paintColorEnable = op->_paintColorEnable; g.paintColorMode = op->_paintColorMode;

    const unsigned cap = op->_maxInstances > 0 ? unsigned(op->_maxInstances) : 0xFFFFFFFFu;
    std::vector<CopyRec> recs;
    unsigned nTargets = 0;
    if (op->_scatterMode != kScatterReplace) {
      for (size_t si = 0; si < srcs.size(); ++si) nTargets += (op->_pointsSource == 1) ? 1u : unsigned(srcs[si].P.size());
    }
    nTargets += unsigned(scatter.size());
    if (op->_maxSourcePoints > 0 && nTargets > unsigned(op->_maxSourcePoints)) {
      op->warning("CopyToPointsUSD: too many copy targets (%u > max source points %d)", nTargets, op->_maxSourcePoints);
      ctpLog("usd:guard", std::to_string(nTargets));
      return;
    }
    recs.reserve(nTargets);
    uint32_t running = 0;
    if (op->_scatterMode != kScatterReplace) {
      for (size_t si = 0; si < srcs.size(); ++si) {
        const SrcPrim& s = srcs[si];
        if (s.P.empty()) continue;
        if (op->_pointsSource == 1) {
          Vector3 mn = s.P[0], mx = s.P[0];
          for (size_t k = 1; k < s.P.size(); ++k) { const Vector3& q = s.P[k];
            mn.x = std::min(mn.x, q.x); mn.y = std::min(mn.y, q.y); mn.z = std::min(mn.z, q.z);
            mx.x = std::max(mx.x, q.x); mx.y = std::max(mx.y, q.y); mx.z = std::max(mx.z, q.z); }
          PointSample ps; ps.order = running; ps.id = running; ps.Pw = (mn + mx) * 0.5f; ps.srcObject = unsigned(si); ps.srcPoint = 0;
          if (s.hasConstColor) { ps.hasColor = true; ps.color = s.constColor; }
          CopyRec rec; if (processSample(ps, g, rec)) recs.push_back(rec);
          ++running;
          continue;
        }
        for (size_t k = 0; k < s.P.size(); ++k, ++running) {
          if (recs.size() >= cap) break;
          PointSample ps;
          ps.order = running;
          ps.id = (k < s.ids.size()) ? uint32_t(s.ids[k]) : running;
          ps.srcObject = unsigned(si); ps.srcPoint = unsigned(k);
          ps.Pw = s.P[k];
          if (usePaint && base[si] + k < paint.npoints) {
            ps.hasPaint = true;
            for (int l = 0; l < kPaintLayerCount; ++l) ps.w[l] = paint.get(l, base[si] + k);
          }
          // normals feed "align to normal"; the named attribute feeds the rest
          if (op->_alignMode == kAlignUsdDir && k < s.alignV.size()) {
            ps.hasDir = true; ps.dir = s.alignV[k];
          }
          else if (op->_alignMode == kAlignUsdEuler && k < s.alignV.size()) {
            ps.hasEuler = true; ps.euler = s.alignV[k];
          }
          else if (op->_alignMode == kAlignUsdQuat && k < s.alignQ.size()) {
            ps.hasQuat = true; ps.quat = s.alignQ[k];
          }
          else if (k < s.N.size()) { ps.hasDir = true; ps.dir = s.N[k]; }
          if (k < s.C.size()) { ps.hasColor = true; ps.color = s.C[k]; }
          else if (s.hasConstColor) { ps.hasColor = true; ps.color = s.constColor; }
          if (op->_useWidths && k < s.widths.size()) { ps.hasSize = true; ps.size = s.widths[k]; }
          // a named size attribute multiplies whatever widths already gave, so a
          // prim carrying both a draw width and a copy size uses both
          if (k < s.sizeA.size()) { ps.size = (ps.hasSize ? ps.size : 1.0f) * s.sizeA[k]; ps.hasSize = true; }
          if (k < s.scaleA.size()) { ps.hasScaleVec = true; ps.scaleVec = s.scaleA[k]; }
          // The id every random choice hangs off. A named attribute wins, then
          // the prim's own ids; the point INDEX is the last resort and the one
          // that makes a particle stream reshuffle when a particle dies.
          if (k < s.idA.size()) ps.id = uint32_t(s.idA[k]);
          else if (k < s.ids.size()) ps.id = uint32_t(s.ids[k]);
          if (op->_variantMode == 2) {
            if (k < s.varA.size()) { ps.hasVariantAttr = true; ps.variantAttr = s.varA[k]; }
            else if (k < s.ids.size()) { ps.hasVariantAttr = true; ps.variantAttr = int(s.ids[k]); }
          }
          if (op->_spinMode == ctp::kSpinRoll && k < s.vel.size()) {
            bool doRoll = true;
            if (op->_rollChannels != 0)
              doRoll = (k < s.chan.size()) && ((s.chan[k] & op->_rollChannels) != 0);
            if (doRoll) {
              ps.rollOK = true;
              ps.rollVel = s.vel[k];
              // how far it has come since birth: stateless, so scrubbing back
              // gives the same answer an accumulating spin never would
              ps.rollDist = (k < s.birthP.size()) ? (s.P[k] - s.birthP[k]).length() : 0.0f;
            }
          }
          CopyRec rec; if (processSample(ps, g, rec)) recs.push_back(rec);
        }
      }
    }
    for (size_t i = 0; i < scatter.size() && recs.size() < cap; ++i, ++running) {
      const ScatterPoint& sp = scatter[i];
      PointSample ps;
      ps.order = running; ps.id = 0x40000000u + uint32_t(i);
      ps.srcObject = sp.srcObject; ps.srcPoint = sp.i0; ps.Pw = sp.P;
      ps.hasDir = true; ps.dir = sp.N;
      if (op->_copyColor && sp.i2 < allC.size()) { ps.hasColor = true; ps.color = allC[sp.i0] * sp.b0 + allC[sp.i1] * sp.b1 + allC[sp.i2] * sp.b2; }
      if (usePaint && sp.i2 < paint.npoints) {
        ps.hasPaint = true;
        for (int l = 0; l < kPaintLayerCount; ++l)
          ps.w[l] = paint.get(l, sp.i0) * sp.b0 + paint.get(l, sp.i1) * sp.b1 + paint.get(l, sp.i2) * sp.b2;
      }
      CopyRec rec; if (processSample(ps, g, rec)) recs.push_back(rec);
    }

    // ---- memory guard: copies x prototype points ----------------------------------------
    // That product is what an UN-INSTANCING renderer pays.  A PointInstancer does
    // not pay it: the prototype is stored once and every copy is a position, an
    // orientation, a scale and an id, so 1000 copies of a 615k-point tree costs
    // 615k points and 1000 transforms - not 615M points.  Refusing to build there
    // protects nothing and empties the stage in exactly the workflow this node is
    // for (InstanceRender, or any Hydra renderer that keeps instancing): turning
    // the density up made the geometry vanish because of the cost MODEL, not
    // because of any memory that was going to be spent.  So instancer mode warns
    // and builds; `copies` mode, where every copy really is a referencing prim a
    // renderer must pay for, still refuses.
    if (op->_maxCopyPoints > 0.0 && nVar > 0) {
      double total = 0.0;
      for (size_t k = 0; k < recs.size(); ++k) {
        const int v = std::min(recs[k].variant, int(nVar) - 1);
        total += (v >= 0 && size_t(v) < protoPointCount.size()) ? protoPointCount[size_t(v)] : 0.0;
      }
      if (total > op->_maxCopyPoints * 1e6) {
        const bool refuse = (op->_mode == kModeCopies);
        if (refuse) {
          op->warning("CopyToPointsUSD: %.1fM copy points (%u copies x prototype points) exceed 'max copy points' %.1fM - "
                      "nothing built. Every copy here is a referencing prim, so a renderer pays for all of them: use a "
                      "lighter prototype or fewer copies, switch to mode = instances, or raise the limit.",
                      total / 1e6, unsigned(recs.size()), op->_maxCopyPoints);
        } else {
          // ~350 bytes per point per copy, measured against ScanlineRender2.
          op->warning("CopyToPointsUSD: %u copies x prototype points = %.1fM points IF un-instanced (~%.0f GB in "
                      "ScanlineRender2), over 'max copy points' %.1fM - BUILT ANYWAY, because a PointInstancer stores "
                      "the prototype once and each copy is only a transform. Render it with InstanceRender or a Hydra "
                      "renderer that keeps instancing, or raise the limit to silence this.",
                      unsigned(recs.size()), total / 1e6, total * 350.0 / 1e9, op->_maxCopyPoints);
        }
        ctpLog(refuse ? "usd:guard copy points" : "usd:guard copy points (warned, built)", std::to_string(total));
        {
          std::ostringstream ig;
          ig << "CopyToPointsUSD: GUARD - " << total / 1e6 << "M copy points > max copy points " << op->_maxCopyPoints
             << "M (" << (refuse ? "nothing built" : "built anyway: instanced, prototype stored once") << ")";
          op->_lastInfo = ig.str();
          std::lock_guard<std::mutex> lock(op->_reportMutex); op->_attrReport = op->_lastInfo + "\n" + op->_attrReport;
        }
        if (refuse) return;
      }
    }

    // ---- author the instancer at this time ------------------------------------------
    usg::Vec3fArray positions(recs.size()), scales(recs.size());
    usg::QuathArray orientations(recs.size());
    usg::IntArray protoIndices(recs.size());
    usg::Int64Array ids(recs.size());
    usg::Vec3fArray colors(recs.size());
    // Velocities come from the source points, and each copy remembers which
    // point it came from.  Authoring them lets a renderer blur a stream whose
    // particle count changes - it extrapolates from one sample instead of
    // pairing particles between two samples that hold different ones.
    usg::Vec3fArray velocities(recs.size());
    bool anyColor = false, anyVelocity = false;
    for (size_t k = 0; k < recs.size(); ++k) {
      Vector3 t, s; fdk::Quatf q;
      decompose(recs[k].xform, t, q, s);
      positions[k] = toF3(t); scales[k] = toF3(s); orientations[k] = q.asQuath();
      protoIndices[k] = (nVar > 0) ? std::min(recs[k].variant, int(nVar) - 1) : 0;
      ids[k] = int64_t(recs[k].id);
      colors[k] = fdk::Vec3f(recs[k].color.x, recs[k].color.y, recs[k].color.z);
      if (recs[k].hasColor) anyColor = true;
      Vector3 v(0.0f, 0.0f, 0.0f);
      if (recs[k].srcObject < srcs.size()) {
        const SrcPrim& sv = srcs[recs[k].srcObject];
        if (recs[k].srcPoint < sv.vel.size()) { v = sv.vel[recs[k].srcPoint]; anyVelocity = true; }
      }
      velocities[k] = toF3(v);
    }
    if (inst && ti == 0) {
      // the edit layer may persist between calls: only (re)author the rel when it differs
      bool same = false;
      {
        const usg::PathArray cur = inst.getPrototypes();
        same = (cur.size() == protoPaths.size());
        for (size_t k = 0; same && k < cur.size(); ++k) if (!(cur[k] == protoPaths[k])) same = false;
      }
      if (!same) inst.setPrototypes(protoPaths);
    }
    if (inst && op->_guideHideCopies) {
      inst.setPositions(usg::Vec3fArray(), time);
      inst.setOrientations(usg::QuathArray(), time);
      inst.setScales(usg::Vec3fArray(), time);
      inst.setProtoIndices(usg::IntArray(), time);
      inst.setIds(usg::Int64Array(), time);
      inst.setVelocities(usg::Vec3fArray(), time);
    }
    else if (inst && op->_mode == kModeInstancer) {
      inst.setPositions(positions, time);
      inst.setOrientations(orientations, time);
      inst.setScales(scales, time);
      inst.setProtoIndices(protoIndices, time);
      inst.setIds(ids, time);
      // only when the points actually carry one: an authored velocity of zero
      // would tell a renderer the particles are standing still
      if (anyVelocity) inst.setVelocities(velocities, time);
      if (anyColor && ti == 0)
        op->warning("per-instance colours are not shaded by ScanlineRender2 (Hydra viewers may show them): set mode = copies to render them");
      if (anyColor && ti == 0 && anySupportPrims)
        op->warning("a material is bound to a prototype: per-copy colours only show if the material reads primvars:displayColor");
      if (anyColor) {
        usg::Prim ip(inst);
        authorDisplayColor(ip, colors, usg::GeomTokens.vertex, time);
      }
      // ---- the 'copy attributes' list ------------------------------------------
      // One instance-rate primvar per name, each copy taking the value of the
      // point it sits on. Whether it went out as a float or a vector is decided
      // by what was READ, so a name that was a vector stays one.
      for (size_t e = 0; e < extraNames.size(); ++e) {
        bool isVec = false, any = false;
        for (size_t si = 0; si < srcs.size() && !any; ++si)
          if (e < srcs[si].extraV.size() && !srcs[si].extraV[e].empty()) { isVec = true; any = true; }
        for (size_t si = 0; si < srcs.size() && !any; ++si)
          if (e < srcs[si].extraF.size() && !srcs[si].extraF[e].empty()) any = true;
        if (!any) continue;
        usg::Prim ip(inst);
        if (isVec) {
          usg::Vec3fArray v; v.resize(recs.size());
          for (size_t k = 0; k < recs.size(); ++k) {
            Vector3 val(0, 0, 0);
            const unsigned so = recs[k].srcObject, sp = recs[k].srcPoint;
            if (so < srcs.size() && e < srcs[so].extraV.size() && sp < srcs[so].extraV[e].size())
              val = srcs[so].extraV[e][sp];
            v[k] = fdk::Vec3f(val.x, val.y, val.z);
          }
          usg::Attribute a = ip.createAttr(usg::Token("primvars:" + extraNames[e]),
                                           usg::Value::Vector3fArray);
          if (a) a.setValue(v, time);
        }
        else {
          usg::FloatArray v; v.resize(recs.size());
          for (size_t k = 0; k < recs.size(); ++k) {
            float val = 0.0f;
            const unsigned so = recs[k].srcObject, sp = recs[k].srcPoint;
            if (so < srcs.size() && e < srcs[so].extraF.size() && sp < srcs[so].extraF[e].size())
              val = srcs[so].extraF[e][sp];
            v[k] = val;
          }
          usg::Attribute a = ip.createAttr(usg::Token("primvars:" + extraNames[e]),
                                           usg::Value::FloatArray);
          if (a) a.setValue(v, time);
        }
      }
      // instancer extent: positions expanded by the prototype radius x max scale
      if (!recs.empty()) {
        fdk::Box3f bb;
        float maxScale = 0.0f;
        for (size_t k = 0; k < recs.size(); ++k) {
          bb.expand(positions[k]);
          maxScale = std::max(maxScale, std::max(std::fabs(scales[k].x), std::max(std::fabs(scales[k].y), std::fabs(scales[k].z))));
        }
        const float pad = protoRadius * maxScale + 1e-4f;
        bb.expand(fdk::Vec3f(bb.min.x - pad, bb.min.y - pad, bb.min.z - pad));
        bb.expand(fdk::Vec3f(bb.max.x + pad, bb.max.y + pad, bb.max.z + pad));
        inst.setBoundsAttr(bb, time);
      }
    }

    // ---- copies mode: one referencing prim per copy ---------------------------------
    if (op->_mode == kModeCopies && inst && !op->_guideHideCopies) {
      if (anyColor && ti == 0 && anySupportPrims)
        op->warning("a material is bound to a prototype: per-copy colours only show if the material reads primvars:displayColor");
      // the instancer stays (it owns the prototypes, which are not drawn directly) but instances nothing
      inst.setPositions(usg::Vec3fArray(), time);
      inst.setOrientations(usg::QuathArray(), time);
      inst.setScales(usg::Vec3fArray(), time);
      inst.setProtoIndices(usg::IntArray(), time);
      inst.setIds(usg::Int64Array(), time);
      const usg::Path copiesPath = rootPath.appendChild("copies");
      usg::ScopePrim::defineInLayer(edit, copiesPath);
      for (size_t k = 0; k < recs.size() && nVar > 0; ++k) {
        const usg::Path cp = copiesPath.appendChild("copy_" + std::to_string(k));
        usg::XformPrim xp = usg::XformPrim::defineInLayer(edit, cp);
        if (!xp) continue;
        fdk::Mat4d M;
        for (int e = 0; e < 16; ++e) M.element(e) = double(recs[k].xform.array()[e]);
        xp.setXformTo("", M, time);
        const usg::Path gp = cp.appendChild("geo");
        // the edit layer can persist between evaluations (next frame): on 15+ the
        // reference list is replaced, on 14.1 (addReference only) it is authored
        // once - a copy prim that already exists keeps its reference
        const bool existed = bool(edit->getPrimAtPath(gp));
        usg::Prim g = usg::Prim::defineInLayer(edit, gp, usg::Token());
        if (!g) continue;
        if (ti == 0) {
          const int v = std::min(recs[k].variant, int(nVar) - 1);
#if kDDImageVersionMajorNum >= 15
          (void)existed;
          setSingleReference(g, protoPaths[v]);
#else
          if (!existed) setSingleReference(g, protoPaths[v]);
#endif
          g.setInstanceable(op->_copiesInstanceable);
        }
        if (recs[k].hasColor) {
          const usg::Vec3fArray one(1, fdk::Vec3f(recs[k].color.x, recs[k].color.y, recs[k].color.z));
          const int v = std::min(recs[k].variant, int(nVar) - 1);
          if (!op->_copiesInstanceable && v >= 0 && size_t(v) < protoGprims.size() && !protoGprims[size_t(v)].empty()) {
            // full copies: override displayColor on every gprim below the referenced wrapper
            // (ScanlineRender2 does not inherit primvars from ancestors)
            for (size_t gi = 0; gi < protoGprims[size_t(v)].size(); ++gi) {
              usg::Path op2 = gp;
              {
                const std::string& rel = protoGprims[size_t(v)][gi];
                size_t b0 = 0;
                while (b0 <= rel.size()) {
                  size_t e0 = rel.find('/', b0);
                  if (e0 == std::string::npos) e0 = rel.size();
                  if (e0 > b0) op2 = op2.appendChild(rel.substr(b0, e0 - b0));
                  b0 = e0 + 1;
                }
              }
              usg::Prim ov = usg::Prim::overrideInLayer(edit, op2, usg::Token());
              if (!ov) continue;
              authorDisplayColor(ov, one, usg::GeomTokens.constant, time);
            }
          }
          else {
            // instanceable copies: instance-rate primvar on the instance root (Hydra viewers may show it)
            authorDisplayColor(g, one, usg::GeomTokens.constant, time);
          }
        }
      }
    }

    // ---- guide -----------------------------------------------------------------------
    if (op->_guideMode != 0) {
      usg::PointsPrim gp = usg::PointsPrim::defineInLayer(edit, guidePath);
      if (gp) {
        usg::Vec3fArray gpos(recs.size()); usg::Vec3fArray gcol(recs.size()); usg::FloatArray gw(recs.size(), float(op->_guideSize));
        float hmax = float(op->_paintHeatMax);
        if (hmax <= 0.0f) hmax = paint.layerMax(op->_paintLayer);
        if (hmax <= 0.0f) hmax = 1.0f;
        for (size_t k = 0; k < recs.size(); ++k) {
          gpos[k] = toF3(recs[k].xform.translation());
          float r, g, b;
          guideColor(recs[k], op->_guideHeat, op->_paintLayer, hmax, r, g, b);
          gcol[k] = fdk::Vec3f(r, g, b);
        }
        gp.setPoints(gpos, time);
        gp.setWidths(gw, time);
        gp.setDisplayColor(gcol, time);
        gp.setPurpose(op->_guidePurpose == kGuidePurposeProxy ? usg::GeomTokens.proxy : op->_guidePurpose == kGuidePurposeDefault ? usg::GeomTokens.default_ : usg::GeomTokens.guide);
        if (!recs.empty()) { fdk::Box3f bb; for (size_t k = 0; k < gpos.size(); ++k) bb.expand(gpos[k]); gp.setBoundsAttr(bb, time); }
      }
      if (op->_guideMode == 2) {
        // one yellow line per copy along its local up axis (shows the alignment)
        usg::BasisCurvesPrim ax = usg::BasisCurvesPrim::defineInLayer(edit, rootPath.appendChild("guide_axes"));
        if (ax) {
          usg::Vec3fArray apos(recs.size() * 2); usg::IntArray cnt(recs.size(), 2);
          const float len = float(op->_guideSize) * 4.0f;
          for (size_t k = 0; k < recs.size(); ++k) {
            const Vector3 p0 = recs[k].xform.translation();
            Vector3 up = recs[k].xform.y_axis();
            const float ul = up.length();
            if (ul > 1e-12f) up = up * (1.0f / ul);
            apos[k * 2] = toF3(p0); apos[k * 2 + 1] = toF3(p0 + up * len);
          }
          ax.setType(usg::GeomTokens.linear);
          ax.setCurveVertexCounts(cnt, time);
          ax.setPoints(apos, time);
          ax.setWidths(usg::FloatArray(1, float(op->_guideSize) * 0.25f), time);
          ax.setDisplayColor(usg::Vec3fArray(1, fdk::Vec3f(0.9f, 0.9f, 0.2f)), time);
          ax.setPurpose(op->_guidePurpose == kGuidePurposeProxy ? usg::GeomTokens.proxy : op->_guidePurpose == kGuidePurposeDefault ? usg::GeomTokens.default_ : usg::GeomTokens.guide);
          if (!recs.empty()) { fdk::Box3f bb; for (size_t k = 0; k < apos.size(); ++k) bb.expand(apos[k]); ax.setBoundsAttr(bb, time); }
        }
      }
    }

    std::ostringstream info;
    info << "CopyToPointsUSD: " << recs.size() << (op->_mode == kModeCopies ? " referenced copies from " : " instances from ")
         << nTargets << " targets, " << nVar << " variant(s)";
    if (!scatterInfo.empty()) info << "; " << scatterInfo;
    op->_lastInfo = info.str();
    { std::lock_guard<std::mutex> lock(op->_reportMutex); op->_attrReport = info.str() + "\n" + op->_attrReport; }
    ctpLog("usd:done", op->_lastInfo);
  }

  // Said once, after every time has been built, and OUTSIDE the mode branches -
  // the first version of this sat inside the instancer branch and never ran in
  // copies mode, which is the default.
  if (refMissing)
    op->warning("'stick to the surface (reference frame shape)' cannot read frame %d: the input "
                "carries no geometry at that frame, so the reference pose is this frame's own and "
                "the scatter is left unchanged. Geometry built by nodes exists only at the frame "
                "being rendered - read the source from a USD file, or bake it, to scatter against "
                "another frame.", op->_scatterRefFrame);
}

static Op* build(Node* node) { return new CopyToPointsUSD(node); }
#if kDDImageVersionMajorNum > 16 || (kDDImageVersionMajorNum == 16 && kDDImageVersionMinorNum >= 1)
// Nuke 17: GeomOps must be owned by a GeomOpNode (shared engines / stage).
const GeomOp::Description CopyToPointsUSD::description(kClass, build);
#else
const Op::Description CopyToPointsUSD::description(kClass, build);
#endif
