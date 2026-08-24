// ParticlesToUSD - classic Nuke particles (and any classic point cloud) as a USD
// Points prim, so the new 3D system can use them.
//
// WHY THIS EXISTS.  Nuke has two 3D systems and nothing carries particles from
// one to the other.  Every particle node - ParticleEmitter, the forces,
// ParticleFieldForce, the Field* nodes - is CLASSIC 3D (DD::Image::GeoOp), while
// CopyToPointsUSD takes only NEW 3D (DD::Image::GeomOp, note the m).  Connecting
// them does not fail with a message; setInput simply returns false.
//
// The workaround people find is to put a GeoPointInstancer in between, because
// it does accept a classic emitter.  It also quietly does the wrong thing: its
// output is a PointInstancer prim, and CopyToPointsUSD takes its targets from
// Mesh and Points prims - so it never sees the particles at all and instances
// onto whatever mesh it does find.  Measured on a scene doing exactly that:
// 9,900 particles in, "726 instances from 726 targets" out.
//
// So this node reads the particles and authors what the new system understands:
// one Points prim, one point per PARTICLE.  Not per vertex - the classic
// CopyToPoints on the same emitter produces 7,187,400 copies, because a particle
// arrives as a tessellated sphere and it copies onto every vertex of it.
//
// It carries ids, widths, velocities and displayColor, and the velocities matter
// as much as the positions: motion blur downstream is built on them, and a
// converter that dropped them would look like it worked and kill the blur.

#include "CopyCore.h"

#include "DDImage/GeomOp.h"
#include "DDImage/GeometryList.h"
#include "DDImage/ParticleOp.h"
#include "DDImage/ParticleRender.h"
#include "DDImage/Scene.h"
#include "DDImage/ddImageVersionNumbers.h"
#include "DDImage/Knobs.h"
#include "DDImage/Op.h"
#include "DDImage/Hash.h"

#include "usg/engine/GeomEngine.h"
#include "usg/engine/GeomSceneContext.h"
#include "usg/geom/Stage.h"
#include "usg/geom/Layer.h"
#include "usg/geom/Prim.h"
#include "usg/geom/GeomTokens.h"
#include "usg/geom/PointsPrim.h"
#include "usg/geom/PrimvarsAPI.h"
#include "usg/geom/Primvar.h"
#include "usg/geom/Attribute.h"
#include "usg/geom/XformPrim.h"
#include "usg/base/ArrayTypes.h"
#include "usg/base/Value.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace DD::Image;

namespace {

// A prim path may only hold letters, digits and underscores, and may not start
// with a digit.  Node names can hold neither guarantee.
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

const char* const kClass = "ParticlesToUSD";
const char* const kHelp =
  "Classic Nuke particles - or any classic 3D point cloud - as a USD Points prim, "
  "so the new 3D system can use them.\n\n"
  "Nuke's particle nodes are all classic 3D and CopyToPointsUSD only accepts new 3D "
  "geometry, so the two will not connect at all. Put this between them.\n\n"
  "One point per PARTICLE, carrying its id, size, velocity and colour. The ids are "
  "what let a renderer match a particle to itself from frame to frame, and the "
  "velocities are what motion blur is built on.";

// ==========================================================================
// What was read out of the classic input.  Plain arrays, filled at validate and
// only read by the engine - see the note on _readClassic() for why.
struct PointData {
  std::vector<fdk::Vec3f> positions;
  std::vector<fdk::Vec3f> velocities;
  std::vector<fdk::Vec3f> colors;
  std::vector<float>      widths;
  std::vector<int64_t>    ids;
  // Where each particle was BORN, and which channels it is on.  Neither is
  // needed to draw a point - they are here so a copier downstream can roll a
  // rock the distance it has travelled, and roll only the ones that bounced.
  std::vector<fdk::Vec3f> initialP;
  std::vector<int>        channels;
  bool anyVelocity = false;
  bool anyColor = false;
  bool cutShort = false;          // the read was abandoned: do not remember it

  void clear()
  {
    positions.clear(); velocities.clear(); colors.clear(); widths.clear(); ids.clear();
    initialP.clear(); channels.clear();
    anyVelocity = anyColor = false;
    cutShort = false;
  }
};

class ParticlesToUSD;

// ==========================================================================
class ParticlesToUSDEngine : public GeomOpEngine
{
public:
// The engine's parent changed from Op* to GeomOpNode* in Nuke 16.1, not 17.0 -
// guarding on the major alone fails to compile against 16.1.
#if kDDImageVersionMajorNum > 16 || (kDDImageVersionMajorNum == 16 && kDDImageVersionMinorNum >= 1)
  ParticlesToUSDEngine(ndk::GeomOpNode* parent) : GeomOpEngine(parent) {}
  Op* opPtr() const { return firstOp(); }
#else
  ParticlesToUSDEngine(Op* parent) : GeomOpEngine(parent), _op(parent) {}   // no logging here: crashes Nuke 16/17
  Op* opPtr() const { return _op; }
  Op* _op;
#endif
  std::string name() const override { return "ParticlesToUSDEngine"; }
protected:
  void processScenegraph(usg::GeomSceneContext& context) override;
};

// ==========================================================================
class ParticlesToUSD : public GeomOp
{
  friend class ParticlesToUSDEngine;
public:
  ParticlesToUSD(Node* node);
  ~ParticlesToUSD() override;
#if kDDImageVersionMajorNum > 16 || (kDDImageVersionMajorNum == 16 && kDDImageVersionMinorNum >= 1)
  static const GeomOp::Description description;
#else
  static const Op::Description description;
#endif
  const char* Class() const override { return kClass; }
  const char* node_help() const override { return kHelp; }
  int minimum_inputs() const override { return 1; }
  int maximum_inputs() const override { return 1; }

  // CLASSIC only, which is the whole point of the node.  A new-3D input would
  // already be usable downstream and does not need converting.
  bool test_input(int, Op* op) const override { return dynamic_cast<GeoOp*>(op) != nullptr; }
  // and no default: GeomOp's own default is a new-3D input, which this would
  // then refuse, leaving the node looking broken before it is even connected
  Op* default_input(int) const override { return nullptr; }
  const char* input_label(int, char*) const override { return "particles"; }

  void knobs(Knob_Callback f) override;
  void append(Hash& hash) override;
  void _validate(bool for_real) override;

private:
  void readClassic();
  void store(const PointData& d);

  bool   _useParticleSystem = true;
  bool   _fromVertices = false;
  double _widthScale = 1.0;
  int    _maxPoints = 5000000;

  // Kept BY FRAME and BY NODE - neither on its own is enough.
  //
  // By frame, because one "current" value has to be written by validate and read
  // by the engine in that order and they are not ordered.
  //
  // By node, because a node is not one Op.  Measured on a five frame render, the
  // reads for frames 20, 40 and 60 landed on one ParticlesToUSD object and those
  // for 30 and 50 on a second one, while the engine only ever saw the first:
  //
  //   read frame 30: 6000 point(s)
  //   process want frame 30, got 4000 point(s); have [20:4000]
  //
  // so every second frame authored the frame before it - 4000, 4000, 8000, 8000,
  // 12000 instances for reads of 4000, 6000, 8000, 10000, 12000.  That is what
  // "does not capture every change of frame" looks like from the outside.  All
  // the Op objects of one node share its Node*, so keyed by that there is a
  // single cache and the engine finds what validate just read.
  struct FrameCache {
    std::map<int, PointData> byFrame;
    int last = 0;
  };
  static std::map<Node*, FrameCache> _cache;
  static std::mutex _cacheMutex;

  static int frameKey(double f) { return int(std::floor(f + 0.5)); }
};

// --------------------------------------------------------------------------
static Op* build(Node* node) { return new ParticlesToUSD(node); }
#if kDDImageVersionMajorNum > 16 || (kDDImageVersionMajorNum == 16 && kDDImageVersionMinorNum >= 1)
const GeomOp::Description ParticlesToUSD::description(kClass, build);
#else
const Op::Description ParticlesToUSD::description(kClass, build);
#endif

ParticlesToUSD::ParticlesToUSD(Node* node)
  : GeomOp(node, BuildEngine<ParticlesToUSDEngine>())
{
}

void ParticlesToUSD::knobs(Knob_Callback f)
{
  GeomOp::knobs(f);
  Named_Text_knob(f, "title", "", "<b><font size=+1>ParticlesToUSD</font></b>");
  Named_Text_knob(f, "subtitle", "",
                  "<i>classic particles as a USD Points prim, for CopyToPointsUSD "
                  "and the rest of the new 3D system</i>");
  Divider(f, "");

  Bool_knob(f, &_useParticleSystem, "read_particle_system", "read the particle system");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Ask the particle system for its particles, rather than reading the geometry it "
             "hands a renderer.\n\n"
             "This is what makes one point per PARTICLE. Read as geometry instead, a particle "
             "is a tessellated sphere and every one of its vertices looks like a point - the "
             "classic CopyToPoints on a 9,900 particle emitter produces 7,187,400 copies that "
             "way. It is also the only route to the ids and velocities: those live in the "
             "system and never reach the geometry.\n\n"
             "Turn it off only for a non-particle point cloud.");
  Bool_knob(f, &_fromVertices, "from_vertices", "use every vertex");
  Tooltip(f, "For ordinary classic geometry: take a point per VERTEX of whatever is connected. "
             "Ignored when the input is a particle system being read as one.");
  Double_knob(f, &_widthScale, IRange(0.0, 10.0), "width_scale", "width scale");
  Tooltip(f, "Multiplies the particle size written into the Points prim's widths. Downstream "
             "nodes use it as the point's diameter.");
  Int_knob(f, &_maxPoints, "max_points", "max points");
  SetRange(f, 1000, 20000000);
  Tooltip(f, "A ceiling, so a runaway emitter cannot take the session down with it. Points "
             "past it are dropped and the node says so.");
}

void ParticlesToUSD::append(Hash& hash)
{
  GeomOp::append(hash);
  hash.append(_useParticleSystem);
  hash.append(_fromVertices);
  hash.append(_widthScale);
  hash.append(_maxPoints);
  // The frame, and whatever is upstream.  Without these the points freeze the
  // moment anything scrubs: the engine would keep handing back the cache it
  // built the first time.
  hash.append(outputContext().frame());
  if (Op* in = Op::input(0)) hash.append(in->hash());
}

// --------------------------------------------------------------------------
// The classic read happens HERE, at validate, and never in the engine.
//
// Reading classic geometry means validate() and get_geometry() and asking a
// particle system for its particles - all of which take Nuke's graph lock. The
// engine runs on another thread (measured: processScenegraph on 41900 while
// validate is the main one), and a render thread that touches the graph while
// the main thread waits on the render deadlocks the two against each other.
// This is the same rule the InstanceRender node learned the hard way: the WORK
// moves to validate, and what runs later only reads plain arrays.
void ParticlesToUSD::_validate(bool for_real)
{
  GeomOp::_validate(for_real);
  if (for_real) readClassic();
}

void ParticlesToUSD::readClassic()
{
  PointData d;

  GeoOp* geo = dynamic_cast<GeoOp*>(Op::input(0));
  if (!geo) {
    store(d);
    return;
  }

  // Never after an abort: validating an upstream op once Nuke has cancelled the
  // render deadlocks against the main thread, which is holding the graph while
  // it waits for that render to stop.
  if (aborted()) {
    d.cutShort = true;
    store(d);
    return;
  }

  try {
    geo->validate(true);
  }
  catch (...) {
    d.cutShort = true;
    store(d);
    return;
  }

  Scene scene;
  GeometryList list;
  try {
    geo->get_geometry(scene, list);
  }
  catch (...) {
    d.cutShort = true;
    store(d);
    return;
  }

  // ---- the particle system, when there is one ------------------------------
  // Nuke's particle geometry carries id, colour and size, but NOT velocity: that
  // never leaves the system.  So the system is asked directly, which is also the
  // only way to get one point per particle rather than per vertex.
  // The system is reached through ParticleRender, and it hands back the two
  // times the step spans - which is where a velocity comes from at all.
  ParticleRender* pr = _useParticleSystem ? dynamic_cast<ParticleRender*>(geo) : nullptr;
  float prevTime = 0.0f, outTime = 0.0f;
  ParticleSystem* ps = pr ? pr->getParticleSystem(prevTime, outTime) : nullptr;
  if (ps) {
    const unsigned n = ps->numParticles();
    const unsigned take = std::min<unsigned>(n, unsigned(std::max(1, _maxPoints)));
    d.positions.reserve(take);
    for (unsigned i = 0; i < n && d.positions.size() < take; ++i) {
      if (!ps->particleActive(i)) continue;
      const Vector3& p = ps->particlePosition(i);
      d.positions.push_back(fdk::Vec3f(p.x, p.y, p.z));
      const Vector3& v = ps->particleVelocity(i);
      d.velocities.push_back(fdk::Vec3f(v.x, v.y, v.z));
      if (v.x != 0.0f || v.y != 0.0f || v.z != 0.0f) d.anyVelocity = true;
      d.ids.push_back(int64_t(ps->particleId(i)));
      const Vector3& ip = ps->particleInitialPosition(i);
      d.initialP.push_back(fdk::Vec3f(ip.x, ip.y, ip.z));
      d.channels.push_back(int(ps->particleChannels(i)));
      // size is a Vector3 - a particle can be squashed - and a Points prim has
      // one width, so the largest extent is the honest one to give it
      const Vector3 sz = ps->particleSize(i);
      const float w = std::max(sz.x, std::max(sz.y, sz.z));
      d.widths.push_back(float(w * _widthScale));
      const Vector4& c = ps->particleColor(i);
      d.colors.push_back(fdk::Vec3f(c.x, c.y, c.z));
      if (c.x != 1.0f || c.y != 1.0f || c.z != 1.0f) d.anyColor = true;
    }
    if (n > take) warning("more than %d particles: %u dropped", _maxPoints, n - take);
  }
  else {
    // ---- ordinary classic geometry: a point per vertex ---------------------
    const unsigned objs = list.objects();
    for (unsigned o = 0; o < objs && d.positions.size() < size_t(_maxPoints); ++o) {
      GeoInfo& info = list[o];
      const PointList* pts = info.point_list();
      if (!pts) continue;
      const Matrix4 xf = info.matrix;
      for (unsigned i = 0; i < pts->size() && d.positions.size() < size_t(_maxPoints); ++i) {
        const Vector3 p = xf.transform((*pts)[i]);
        d.positions.push_back(fdk::Vec3f(p.x, p.y, p.z));
        d.velocities.push_back(fdk::Vec3f(0.0f, 0.0f, 0.0f));
        d.ids.push_back(int64_t(d.positions.size() - 1));
        d.widths.push_back(float(_widthScale));
        d.colors.push_back(fdk::Vec3f(1.0f, 1.0f, 1.0f));
      }
    }
  }

  ctp::ctpLog("p2u:read", "frame " + std::to_string(frameKey(outputContext().frame()))
                     + ": " + std::to_string(d.positions.size()) + " point(s)"
                     + (ps ? " from the particle system" : " from geometry")
                     + (d.anyVelocity ? ", with velocities" : ", no velocities"));

  store(d);
}

std::map<Node*, ParticlesToUSD::FrameCache> ParticlesToUSD::_cache;
std::mutex ParticlesToUSD::_cacheMutex;

ParticlesToUSD::~ParticlesToUSD()
{
  // The last Op of a node takes the node's cache with it.  Nuke keeps several
  // Ops alive at once, so this only empties when they have all gone.
  std::lock_guard<std::mutex> lock(_cacheMutex);
  _cache.erase(node());
}

// Keep this frame's read, and a few either side of it so that stepping back and
// forth does not re-read what was just read.  Bounded, because a scrub would
// otherwise hold every frame of the timeline; what goes is whatever is furthest
// from where the timeline is now, never the frame just read.
void ParticlesToUSD::store(const PointData& d)
{
  const int key = frameKey(outputContext().frame());
  std::lock_guard<std::mutex> lock(_cacheMutex);
  FrameCache& fc = _cache[node()];
  fc.byFrame[key] = d;
  fc.last = key;
  while (fc.byFrame.size() > 8) {
    std::map<int, PointData>::iterator worst = fc.byFrame.begin();
    for (std::map<int, PointData>::iterator i = fc.byFrame.begin();
         i != fc.byFrame.end(); ++i) {
      if (std::abs(i->first - key) > std::abs(worst->first - key)) worst = i;
    }
    fc.byFrame.erase(worst);
  }
}

// ==========================================================================
void ParticlesToUSDEngine::processScenegraph(usg::GeomSceneContext& context)
{
  GeomOpEngine::processScenegraph(context);
  ParticlesToUSD* op = dynamic_cast<ParticlesToUSD*>(opPtr());
  if (!op) return;

  // The frame this pass is authoring decides which read is used.  Falling back
  // to the newest is for the case where the engine is asked for a time nothing
  // was read at; it is better than authoring nothing, and the frame-keyed hit is
  // what happens in practice.
  const fdk::TimeValueSet& times0 = context.processTimes();
  const int wantFrame = times0.empty() ? 0 : ParticlesToUSD::frameKey(*times0.begin());
  PointData d;
  int gotFrame = wantFrame;
  {
    std::lock_guard<std::mutex> lock(ParticlesToUSD::_cacheMutex);
    std::map<Node*, ParticlesToUSD::FrameCache>::const_iterator c =
      ParticlesToUSD::_cache.find(op->node());
    if (c != ParticlesToUSD::_cache.end()) {
      const ParticlesToUSD::FrameCache& fc = c->second;
      std::map<int, PointData>::const_iterator it = fc.byFrame.find(wantFrame);
      // Nearest, not newest, when there is no exact frame: with motion blur the
      // first shutter time can round off the frame, and scrubbing backwards the
      // newest read is the one furthest from where the timeline now is.
      if (it == fc.byFrame.end()) {
        for (std::map<int, PointData>::const_iterator i = fc.byFrame.begin();
             i != fc.byFrame.end(); ++i) {
          if (it == fc.byFrame.end() ||
              std::abs(i->first - wantFrame) < std::abs(it->first - wantFrame)) it = i;
        }
      }
      if (it != fc.byFrame.end()) { d = it->second; gotFrame = it->first; }
    }
  }
  // Traced because this is where the node went wrong once and would go wrong
  // again silently: the frame asked for and the frames held are the whole story.
  if (!ctp::ctpLogPath().empty()) {
    char buf[256];
    std::string have;
    { std::lock_guard<std::mutex> lock(ParticlesToUSD::_cacheMutex);
      const ParticlesToUSD::FrameCache& fc = ParticlesToUSD::_cache[op->node()];
      for (std::map<int, PointData>::const_iterator i = fc.byFrame.begin();
           i != fc.byFrame.end(); ++i) {
        snprintf(buf, sizeof(buf), "%d:%d ", i->first, int(i->second.positions.size()));
        have += buf;
      }
    }
    std::string tl;
    for (fdk::TimeValueSet::const_iterator t = times0.begin(); t != times0.end(); ++t) {
      char tb[48]; snprintf(tb, sizeof(tb), "%.4f ", double(*t)); tl += tb;
    }
    snprintf(buf, sizeof(buf), "want frame %d, got %d point(s) read at %d; "
             "shutter times [%s]; held [%s]",
             wantFrame, int(d.positions.size()), gotFrame, tl.c_str(), have.c_str());
    ctp::ctpLog("p2u:process", buf);
  }
  if (d.positions.empty()) {
    ctp::ctpLog("p2u:process", "nothing to author");
    return;
  }

  usg::LayerRef edit = editLayer();
  if (!edit) return;

  const std::string nodeName = sanitizeName(op->node_name());
  const usg::Path rootPath("/" + nodeName);
  const usg::Path pointsPath = rootPath.appendChild("Points");

  usg::Prim::defineInLayer(edit, rootPath, usg::Token("Xform"));
  usg::Prim prim = usg::Prim::defineInLayer(edit, pointsPath, usg::Token("Points"));
  if (!prim) return;

  usg::PointsPrim points(prim);

  // A USD velocity is units per SECOND. A Nuke particle's is units per FRAME -
  // measured, |P(N) - P(N-1)| came out equal to |V| to five decimal places - so
  // the two differ by the frame rate and the difference is not academic.
  //
  // USD PREFERS velocities over the position samples when both are authored, and
  // divides by timeCodesPerSecond on the way: UsdGeomPointInstancer's transforms
  // come out as P + V * (t - base) / rate. Left per frame, a particle moving 1.5
  // units a frame blurred 0.0616 of a unit instead of 1.5 - a streak 24 times too
  // short, which at this scale is a sharp particle with a slightly soft edge.
  // That is what "no motion blur" looked like.
  //
  // The rate comes from the script rather than the layer: usg::Layer only grew
  // hasFramesPerSecond() in 16.1 and this node goes back to 14.1, and the stage
  // Nuke builds carries the script's rate anyway.
  double fps = 24.0;
  if (DD::Image::root_real_fps && DD::Image::root_real_fps() > 0.0f)
    fps = double(DD::Image::root_real_fps());

  // Filled element by element rather than from a pair of iterators: usg::Array
  // grew that constructor after 15.2, and building from iterators there resolves
  // to Array(size_t, const T&) instead and will not compile.
  usg::Vec3fArray positions;
  positions.resize(d.positions.size());
  for (size_t i = 0; i < d.positions.size(); ++i) positions[i] = d.positions[i];
  usg::FloatArray widths;
  widths.resize(d.widths.size());
  for (size_t i = 0; i < d.widths.size(); ++i) widths[i] = d.widths[i];
  usg::Int64Array ids;
  ids.resize(d.ids.size());
  for (size_t i = 0; i < d.ids.size(); ++i) ids[i] = d.ids[i];

  // MOTION BLUR.  The times the context asks for are sub-frame - measured, a
  // shutter of 1 at frame 35 asks for 34.5, 34.75, 35.25, 35.5 - and every one of
  // them used to be given the same positions, so the render was identical with
  // the shutter open and shut. Now each time gets the positions the particles
  // hold AT that time.
  //
  // They are worked out from the velocity rather than read from the simulation.
  // Not for speed: a particle simulation has no sub-frame state to read. It steps
  // whole frames, and asking it for 34.5 would not interpolate, it would re-run
  // the solve on a different step and give different particles.  The velocity is
  // the sub-frame information the simulation does carry.
  //
  // Measured over 2000 matched ids on three consecutive frames, the velocity at
  // frame N is EXACTLY where the particle moved from N-1 to N - |move - V| came
  // out 0.00000 against a move of 0.614 - so it is the step just taken, in units
  // per frame, not a forward prediction. Positions before the frame are therefore
  // exact and positions after it are extrapolated; using the PREVIOUS frame's
  // velocity to predict this one was out by 0.049 in 0.614, about 8%, because the
  // force fields accelerate the particles. There is nothing better available at
  // render time, and it is what the shutter has always been given elsewhere.
  //
  // Offset from the frame the data was READ at, which is not always the frame
  // asked for - see the nearest-frame fallback above.
  const fdk::TimeValueSet& times = context.processTimes();
  std::vector<fdk::TimeValue> timeList(times.begin(), times.end());
  if (timeList.empty()) timeList.push_back(fdk::defaultTimeValue());
  const int nTimes = int(timeList.size());
  for (size_t ti = 0; ti < timeList.size(); ++ti) {
    const fdk::TimeValue time = timeList[ti];

    // fdk::TimeValue is a FRAME here, not seconds: the 34.5 .. 35.5 above is a
    // frame 35 render, so this subtraction is already in the units the velocity
    // is in and needs no frame rate.
    const double dt = double(time) - double(gotFrame);
    usg::Vec3fArray posAt = positions;
    if (d.anyVelocity && dt != 0.0) {
      const float f = float(dt);
      for (size_t i = 0; i < posAt.size(); ++i) {
        posAt[i] = fdk::Vec3f(d.positions[i][0] + d.velocities[i][0] * f,
                              d.positions[i][1] + d.velocities[i][1] * f,
                              d.positions[i][2] + d.velocities[i][2] * f);
      }
    }
    points.setPoints(posAt, time);
    points.setWidths(widths, time);
    points.setIds(ids, time);
    if (d.anyVelocity) {
      // per second on the way out; everything above works in frames
      usg::Vec3fArray velocities;
      velocities.resize(d.velocities.size());
      for (size_t i = 0; i < d.velocities.size(); ++i)
        velocities[i] = fdk::Vec3f(d.velocities[i][0] * float(fps),
                                   d.velocities[i][1] * float(fps),
                                   d.velocities[i][2] * float(fps));
      points.setVelocities(velocities, time);
    }
    // Birth position and channels, as plain primvars.  A rolling rock needs to
    // know how far it has come since it was born, and which particles bounced -
    // neither survives as anything USD has a schema for, so they travel by name.
    if (!d.initialP.empty()) {
      usg::Vec3fArray ip;
      ip.resize(d.initialP.size());
      for (size_t i = 0; i < d.initialP.size(); ++i) ip[i] = d.initialP[i];
      usg::Attribute a0 = prim.createAttr(usg::Token("primvars:initialP"), usg::Value::Vector3fArray);
      if (a0) a0.setValue(ip, time);
    }
    if (!d.channels.empty()) {
      usg::IntArray ch;
      ch.resize(d.channels.size());
      for (size_t i = 0; i < d.channels.size(); ++i) ch[i] = d.channels[i];
      usg::Attribute a1 = prim.createAttr(usg::Token("primvars:channel"), usg::Value::IntArray);
      if (a1) a1.setValue(ch, time);
    }
    if (d.anyColor) {
      usg::Vec3fArray colors;
      colors.resize(d.colors.size());
      for (size_t i = 0; i < d.colors.size(); ++i) colors[i] = d.colors[i];
      // PrimvarsAPI::createPrimvar from 15 on; the 14.1 preview API has only a
      // plain attribute with an interpolation set on it.  Same split as
      // CopyToPointsUSD's authorDisplayColor.
#if kDDImageVersionMajorNum >= 15
      usg::PrimvarsAPI pv(prim);
      usg::Primvar cpv = pv.createPrimvar(usg::Token("displayColor"),
                                          usg::Value::Color3fArray, usg::GeomTokens.vertex);
      if (cpv) cpv.attribute().setValue(colors, time);
#else
      usg::Attribute a = prim.createAttr(usg::Token("primvars:displayColor"),
                                         usg::Value::Color3fArray);
      if (a) { a.setInterpolation(usg::GeomTokens.vertex); a.setValue(colors, time); }
#endif
    }
    // an extent, or a viewer has nothing to frame on - and it has to follow the
    // MOVED points, or a renderer that culls on it clips the ends of the streaks
    fdk::Box3f bb;
    for (size_t i = 0; i < posAt.size(); ++i) bb.expand(posAt[i]);
    points.setBoundsAttr(bb, time);
  }

  ctp::ctpLog("p2u:process", std::to_string(d.positions.size()) + " point(s) authored at "
                        + std::to_string(nTimes) + " time(s)");
}

} // namespace
