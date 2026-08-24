// VolumeToUSD - a Nuke FieldVolume (an OpenVDB file) as a USD Volume prim, so
// the new 3D system, and a renderer reading that stage, can see it.
//
// WHY THIS EXISTS.  Nuke's field graph is a third thing again, alongside the two
// 3D systems.  A FieldVolume reads a .vdb and hands it to the Field nodes, and
// nothing carries it into the USD stage: GeoScene, GeoRender and InstanceRender
// all refuse a field on their scene input, and the two nodes that DO accept one
// do not help.
//
//   GeoFieldMesh   meshes the field - an isosurface.  That is a SURFACE and
//                  renders as one: no depth to it, nothing scattering through
//                  it, no soft edge.
//   GeoFieldSet    attaches the field to geometry for Nuke's own renderers, and
//                  the stage that comes out the other side is unchanged.
//                  Measured: a GeoCube through a GeoFieldSet gives a stage
//                  holding /GeoCube1 [Mesh] and nothing else.
//
// So this authors what USD already has a schema for: a Volume prim with an
// OpenVDBAsset field pointing at the file the FieldVolume was reading.  The grid
// is not copied or converted - the renderer opens the same .vdb.
//
// IT NEEDS A FILE ON DISK, which is the one real limitation.  A procedural field
// - FieldNoise, FieldShape, anything built in the graph - exists only inside
// Nuke and has no .vdb behind it, so there is nothing to point at.  Bake it with
// FieldVolumeWrite first.  The node says so rather than rendering nothing.

#include "CopyCore.h"

#include "DDImage/GeomOp.h"
#include "DDImage/Knobs.h"
#include "DDImage/Enumeration_KnobI.h"
#include "DDImage/Op.h"
#include "DDImage/Hash.h"
#include "DDImage/ddImageVersionNumbers.h"

#include "usg/engine/GeomEngine.h"
#include "usg/engine/GeomSceneContext.h"
#include "usg/geom/Stage.h"
#include "usg/geom/Layer.h"
#include "usg/geom/Prim.h"
#include "usg/geom/Relationship.h"
#include "usg/geom/Attribute.h"
#include "usg/geom/AssetPath.h"
#include "usg/geom/GeomTokens.h"
#include "usg/geom/XformPrim.h"
#include "usg/geom/BasisCurvesPrim.h"
#include "usg/geom/PointsPrim.h"
#include "usg/geom/BoundablePrim.h"
#include "usg/base/ArrayTypes.h"
#include "usg/base/Value.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace DD::Image;

static const char* const kClass = "VolumeToUSD";
// A placeholder menu: the real one is read from the file at knob_changed.
static const char* const kNoGrids[] = { "<no grids found>", nullptr };
// What the VIEWER shows.  A Volume prim on its own draws nothing at all.
static const char* const kPurposeNames[] = {
  "guide (kept out of renders)", "default (always visible, renders too)", nullptr };
static const char* const kPreviewNames[] = {
  "off", "bounding box", "box + points (fog)", nullptr };
enum { kPreviewOff = 0, kPreviewBox = 1, kPreviewPoints = 2 };
// How a renderer should read an emissive grid's values.
static const char* const kEmitModeNames[] = {
  "intensity (the value is the brightness)",
  "blackbody (the value is a temperature in Kelvin)", nullptr };
enum { kEmitIntensity = 0, kEmitBlackbody = 1 };
static const char* const kHelp =
  "A FieldVolume's .vdb as a USD Volume prim, so the new 3D system can see it.\n\n"
  "Connect a FieldVolume to the input and this reads the file and grid straight off it, or "
  "type them here.  The grid is not copied: the renderer opens the same file.\n\n"
  "It needs a file on disk.  A field built in the graph - FieldNoise, FieldShape - has no "
  ".vdb behind it, so bake it with FieldVolumeWrite first.";

// A .vdb path with the frame filled in.
//
// A simulation is a SEQUENCE, and Nuke writes it the way it writes any sequence:
// AerialExplosion_%04d.vdb, or with hashes.  Left as it is, that string is not
// the name of any file - fopen fails, no grids are found, and Hio cannot open it
// either, so the volume does not render at all.  This was the whole of "not
// reading the grids": the file was a sequence and nothing here had ever resolved
// one.
static std::string resolveVdbFrame(const std::string& path, int frame)
{
  if (path.empty()) return path;
  char buf[2048];

  // printf style: %04d, %d
  const size_t pc = path.find('%');
  if (pc != std::string::npos) {
    size_t e = pc + 1;
    while (e < path.size() && (isdigit((unsigned char)path[e]) || path[e] == '0')) ++e;
    if (e < path.size() && (path[e] == 'd' || path[e] == 'i')) {
      const std::string fmt = path.substr(pc, e - pc + 1);
      int width = 0;
      bool zero = false;
      size_t q = pc + 1;
      if (q < path.size() && path[q] == '0') { zero = true; ++q; }
      while (q < e) { width = width * 10 + (path[q] - '0'); ++q; }
      char num[64];
      if (zero && width > 0) snprintf(num, sizeof(num), "%0*d", width, frame);
      else                   snprintf(num, sizeof(num), "%d", frame);
      std::string out = path.substr(0, pc) + num + path.substr(e + 1);
      return out;
    }
  }

  // hash style: ####
  const size_t hs = path.find('#');
  if (hs != std::string::npos) {
    size_t he = hs;
    while (he < path.size() && path[he] == '#') ++he;
    char num[64];
    snprintf(num, sizeof(num), "%0*d", int(he - hs), frame);
    return path.substr(0, hs) + num + path.substr(he);
  }

  (void)buf;
  return path;
}

// The grids a .vdb holds, read straight off the file.
//
// Nuke ships openvdb.dll but no headers, so there is no library call for this -
// and Hio, which reads the voxels, takes a grid NAME and offers no way to list
// them.  So the file's own header is read.
//
// The signature is a grid's NAME followed immediately by its TREE TYPE, both
// length-prefixed, and every tree type begins "Tree_":
//
//   07 00 00 00  "density"   10 00 00 00  "Tree_float_5_4_3"
//
// Anchoring on "Tree_" and stepping back to the length that matches is what
// makes this robust to the parts of the header that change between OpenVDB
// versions - the metadata, the uuid, the compression - none of which are walked.
static std::vector<std::string> vdbGridNames(const std::string& path)
{
  std::vector<std::string> out;
  if (path.empty()) return out;
  FILE* fp = fopen(path.c_str(), "rb");
  if (!fp) return out;

  // THE WHOLE FILE, in chunks.  The grid descriptors are NOT packed together at
  // the front - each one is followed by that grid's data, so they are spread
  // through the file.  Reading only the first few megabytes found the first grid
  // and missed the rest: a 67 MB explosion holding density, temperature and
  // flames reported density alone.
  //
  // Streamed with an overlap, because a name and its tree type can straddle a
  // chunk boundary and would then be invisible to both halves.
  const size_t kChunk = 4u * 1024u * 1024u;
  const size_t kOverlap = 256;
  std::vector<char> buf;
  buf.resize(kChunk + kOverlap);
  size_t carry = 0;
  const char* tree = "Tree_";

  while (true) {
    const size_t got = fread(&buf[carry], 1, kChunk, fp);
    if (got == 0) break;
    const size_t have = carry + got;
    const char* d = &buf[0];
    for (size_t i = 8; i + 5 <= have; ++i) {
      if (memcmp(d + i, tree, 5) != 0) continue;
      int tlen = 0;
      memcpy(&tlen, d + i - 4, 4);
      if (tlen <= 0 || tlen > 64) continue;      // not a length-prefixed tree type
      const size_t nameEnd = i - 4;              // just before the tree type's length
      for (int nlen = 1; nlen <= 64; ++nlen) {
        if (nameEnd < size_t(nlen) + 4) break;
        const size_t nameStart = nameEnd - size_t(nlen);
        int cand = 0;
        memcpy(&cand, d + nameStart - 4, 4);
        if (cand != nlen) continue;
        bool printable = true;
        for (int k = 0; k < nlen; ++k) {
          const unsigned char c = (unsigned char)d[nameStart + k];
          if (c < 32 || c > 126) { printable = false; break; }
        }
        if (printable) {
          const std::string nm(d + nameStart, size_t(nlen));
          bool seen = false;
          for (size_t q = 0; q < out.size(); ++q) if (out[q] == nm) seen = true;
          if (!seen) out.push_back(nm);
        }
        break;
      }
    }
    if (have <= kOverlap) break;
    // keep the tail so a descriptor spanning the join is still seen
    memmove(&buf[0], &buf[have - kOverlap], kOverlap);
    carry = kOverlap;
    if (got < kChunk) break;                     // that was the end of the file
  }
  fclose(fp);
  return out;
}

// Where a grid sits in the world, read from the file's own header.
//
// A Volume prim with no extent is BLANK in the viewer - there is nothing for it
// to draw and nothing to frame on - and the voxels themselves cannot be decoded
// here: they are blosc-compressed and Nuke ships openvdb.dll without headers.
// The header, though, carries everything needed for a box:
//
//   file_bbox_min / file_bbox_max   vec3i, in INDEX space
//   UniformScaleTranslateMap        translation then scale, as doubles
//
// world = translation + index * scale.  Checked against what the renderer's own
// reader reports for the same file: -0.01 .. 0.99, to the digit.
struct VdbBounds {
  bool ok;
  double mn[3], mx[3];
  VdbBounds() : ok(false) { for (int i = 0; i < 3; ++i) { mn[i] = 0.0; mx[i] = 0.0; } }
};

static bool vdbFindMeta(const std::vector<char>& d, size_t from, const char* name,
                        std::vector<char>& out)
{
  const int nlen = int(strlen(name));
  std::vector<char> pat(4 + nlen);
  memcpy(&pat[0], &nlen, 4);
  memcpy(&pat[4], name, size_t(nlen));
  for (size_t i = from; i + pat.size() + 8 < d.size(); ++i) {
    if (memcmp(&d[i], &pat[0], pat.size()) != 0) continue;
    size_t j = i + pat.size();
    int tlen = 0; memcpy(&tlen, &d[j], 4);
    if (tlen < 0 || tlen > 64) return false;
    j += 4 + size_t(tlen);
    int nbytes = 0; memcpy(&nbytes, &d[j], 4);
    if (nbytes <= 0 || nbytes > 4096 || j + 4 + size_t(nbytes) > d.size()) return false;
    out.assign(d.begin() + long(j + 4), d.begin() + long(j + 4 + size_t(nbytes)));
    return true;
  }
  return false;
}

static VdbBounds vdbGridBounds(const std::string& path)
{
  VdbBounds b;
  if (path.empty()) return b;
  FILE* fp = fopen(path.c_str(), "rb");
  if (!fp) return b;
  std::vector<char> d;
  d.resize(1u * 1024u * 1024u);          // the header is at the very front
  const size_t n = fread(&d[0], 1, d.size(), fp);
  fclose(fp);
  if (n < 64) return b;
  d.resize(n);

  std::vector<char> raw;
  int bmin[3] = { 0, 0, 0 }, bmax[3] = { 0, 0, 0 };
  if (!vdbFindMeta(d, 0, "file_bbox_min", raw) || raw.size() < 12) return b;
  memcpy(bmin, &raw[0], 12);
  if (!vdbFindMeta(d, 0, "file_bbox_max", raw) || raw.size() < 12) return b;
  memcpy(bmax, &raw[0], 12);

  const char* mapName = "UniformScaleTranslateMap";
  size_t m = std::string::npos;
  for (size_t i = 0; i + strlen(mapName) < d.size(); ++i)
    if (memcmp(&d[i], mapName, strlen(mapName)) == 0) { m = i + strlen(mapName); break; }
  double tr[3] = { 0, 0, 0 }, sc[3] = { 1, 1, 1 };
  if (m != std::string::npos && m + 48 <= d.size()) {
    memcpy(tr, &d[m], 24);
    memcpy(sc, &d[m + 24], 24);
  }
  for (int i = 0; i < 3; ++i) {
    b.mn[i] = tr[i] + double(bmin[i]) * sc[i];
    b.mx[i] = tr[i] + double(bmax[i]) * sc[i];
  }
  b.ok = (b.mx[0] > b.mn[0] && b.mx[1] > b.mn[1] && b.mx[2] > b.mn[2]);
  return b;
}

// Prim names have to be valid USD identifiers.
static std::string sanitizeVolName(const std::string& s)
{
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    out += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
  }
  if (out.empty() || (out[0] >= '0' && out[0] <= '9')) out = "_" + out;
  return out;
}

// Point one of the Volume's field relationships at an asset prim.
//
// getRelationship FIRST.  The engine authors into a layer that persists between
// calls - it runs again for every shutter sample and again on the next frame -
// and asking USD to CREATE a spec that is already there is an error, not a
// no-op:
//   Failed to create spec of type 'SdfSpecTypeRelationship' at
//   </VolumeToUSD1/volume.field:density>
// which showed up the moment the node met motion blur or a second frame.
static void pointFieldAt(usg::Prim& vol, const char* relName, const usg::Path& target)
{
  usg::Relationship rel;
  if (!vol.getRelationship(usg::Token(relName), rel))
    rel = vol.createRelationship(usg::Token(relName));
  if (!rel) return;
  usg::PathArray targets;
  targets.push_back(target);
  rel.setTargets(targets);
}

// ==========================================================================
class VolumeToUSDEngine : public GeomOpEngine
{
public:
#if kDDImageVersionMajorNum > 16 || (kDDImageVersionMajorNum == 16 && kDDImageVersionMinorNum >= 1)
  VolumeToUSDEngine(ndk::GeomOpNode* parent) : GeomOpEngine(parent) {}
  Op* opPtr() const { return firstOp(); }
#else
  VolumeToUSDEngine(Op* parent) : GeomOpEngine(parent), _op(parent) {}
  Op* opPtr() const { return _op; }
  Op* _op;
#endif
  std::string name() const override { return "VolumeToUSDEngine"; }
protected:
  void processScenegraph(usg::GeomSceneContext& context) override;
};

// ==========================================================================
class VolumeToUSD : public GeomOp
{
  friend class VolumeToUSDEngine;
public:
  VolumeToUSD(Node* node);
  ~VolumeToUSD() override;
#if kDDImageVersionMajorNum > 16 || (kDDImageVersionMajorNum == 16 && kDDImageVersionMinorNum >= 1)
  static const GeomOp::Description description;
#else
  static const Op::Description description;
#endif
  const char* Class() const override { return kClass; }
  const char* node_help() const override { return kHelp; }
  int minimum_inputs() const override { return 1; }
  int maximum_inputs() const override { return 1; }

  bool test_input(int, Op*) const override { return true; }
  Op* default_input(int) const override { return nullptr; }
  const char* input_label(int, char*) const override { return "field"; }

  void knobs(Knob_Callback f) override;
  void append(Hash& hash) override;
  void _validate(bool for_real) override;

private:
  // Settled at validate, where the graph may be touched; the engine only reads.
  std::string _resolvedFile, _resolvedGrid;

  const char* _file;
  const char* _emissionFile;
  int         _gridIdx, _tempIdx, _emIdx;
  int         _preview, _previewPoints, _previewPurpose;
  double      _densityScale;
  double      _tempScale, _emissionScale;
  int         _tempMode, _emMode;
  double      _tempKmin, _tempKmax, _emKmin, _emKmax;
  float       _tempColor[3], _emissionColor[3];
  std::string _resolvedEmFile, _resolvedTempGrid, _resolvedEmGrid;
  std::string _menuFile, _menuEmFile;
  bool        _forceMenuReload;

  // BY FRAME AND BY NODE, the same as ParticlesToUSD and for the same reason.
  //
  // A sequence is a different file every frame, and a node is not one Op: Nuke
  // keeps several objects per node and the engine reads whichever it likes.
  // Measured here before this existed - frames 20 and 60 of an explosion both
  // authored AerialExplosion_0020.vdb, so the whole sequence rendered frame 20.
  struct Resolved {
    std::string file, grid, emFile, tempGrid, emGrid;
  };
  static std::map<Node*, std::map<int, Resolved> > _cache;
  static std::mutex _cacheMutex;
  static int frameKey(double f) { return int(f + 0.5); }
  // what was resolved for one frame, nearest when that exact frame was not read
  static bool resolvedAt(Node* n, int frame, Resolved& out);

  void refreshGridMenus();
  int  knob_changed(Knob* k) override;
};

// --------------------------------------------------------------------------
static Op* build(Node* node) { return new VolumeToUSD(node); }
#if kDDImageVersionMajorNum > 16 || (kDDImageVersionMajorNum == 16 && kDDImageVersionMinorNum >= 1)
const GeomOp::Description VolumeToUSD::description(kClass, build);
#else
const Op::Description VolumeToUSD::description(kClass, build);
#endif

VolumeToUSD::VolumeToUSD(Node* node)
  : GeomOp(node, BuildEngine<VolumeToUSDEngine>())
  , _file("")
  , _emissionFile("")
  , _gridIdx(0), _tempIdx(0), _emIdx(0)
  , _preview(kPreviewBox), _previewPoints(12), _previewPurpose(1)
  , _densityScale(1.0)
  , _tempScale(1.0), _emissionScale(1.0)
  , _tempMode(kEmitBlackbody), _emMode(kEmitIntensity)
  , _tempKmin(0.0), _tempKmax(0.0), _emKmin(0.0), _emKmax(0.0)
  , _forceMenuReload(false)
{
  _tempColor[0] = 1.0f;     _tempColor[1] = 0.45f;     _tempColor[2] = 0.15f;
  _emissionColor[0] = 1.0f; _emissionColor[1] = 0.75f; _emissionColor[2] = 0.4f;
}

void VolumeToUSD::knobs(Knob_Callback f)
{
  GeomOp::knobs(f);
  Named_Text_knob(f, "title", "", "<b><font size=+1>VolumeToUSD</font></b>");
  Named_Text_knob(f, "subtitle", "",
                  "<i>a FieldVolume's .vdb as a USD Volume prim, for the new 3D system</i>");
  Divider(f, "");
  File_knob(f, &_file, "file", "vdb file");
  KnobDefinesGeometry(f);
  // Without this, knob_changed does not fire with the panel closed or from
  // Python, the pulldowns below are never filled from the file, and setting one
  // by name silently does nothing.
  SetFlags(f, Knob::KNOB_CHANGED_ALWAYS);
  Tooltip(f, "The .vdb to render.  Left empty, the file is read from a FieldVolume connected to "
             "the input.");
  Enumeration_knob(f, &_gridIdx, kNoGrids, "grid", "density grid");
  SetFlags(f, Knob::SAVE_MENU);
  KnobDefinesGeometry(f);
  Tooltip(f, "Which grid in the file holds the density - the smoke this absorbs and scatters "
             "with.  The list is read from the file itself.");
  Divider(f, "viewport");
  Enumeration_knob(f, &_preview, kPreviewNames, "viewport_display", "display");
  KnobDefinesGeometry(f);
  Tooltip(f, "What the Viewer draws for this volume.\n\n"
             "A USD Volume prim on its own draws NOTHING - the voxels are only read by a renderer "
             "that understands them - so without this the node looks empty and there is no way to "
             "see where the smoke is or frame on it.\n\n"
             "The box is the grid's own bounds, read from the .vdb header. The points fill it "
             "evenly to read as fog; they are NOT shaped by the density, because the voxels are "
             "compressed and cannot be decoded here - it shows you where the volume is, not what "
             "it looks like.");
  Int_knob(f, &_previewPoints, IRange(4, 40), "viewport_points", "fog points");
  KnobDefinesGeometry(f);
  Tooltip(f, "How many points across the box, so this cubed in total. Only used by 'box + points'.");
  Enumeration_knob(f, &_previewPurpose, kPurposeNames, "viewport_purpose", "purpose");
  KnobDefinesGeometry(f);
  Tooltip(f, "'default' shows the preview in the Viewer, which is the point of it. It is marked "
             "so InstanceRender refuses to render it either way, so this is only about which "
             "viewers draw it - 'guide' hides it until the Viewer is showing guides.");
  Button(f, "reload_grids", "reload grids");
  SetFlags(f, Knob::KNOB_CHANGED_ALWAYS | Knob::STARTLINE);
  Tooltip(f, "Read the file again and refill the pulldowns above.  Needed when the .vdb has been "
             "rewritten under the same name, or the sequence has moved to a frame with different "
             "grids in it.");
  Named_Text_knob(f, "grid_report", "", "");
  Double_knob(f, &_densityScale, IRange(0.0, 20.0), "density_scale", "density scale");
  KnobDefinesGeometry(f);
  Tooltip(f, "Multiplies the grid's values.  A .vdb baked from a shape holds 1 inside and 0 "
             "outside, which is very thin across a small object - raise this to make it read.");

  Divider(f, "emission");
  Enumeration_knob(f, &_tempIdx, kNoGrids, "temperature_grid", "temperature grid");
  SetFlags(f, Knob::SAVE_MENU);
  KnobDefinesGeometry(f);
  Tooltip(f, "The heat field - what most simulations call 'temperature'.  It is emissive: "
             "hotter parts glow, tinted by the temperature colour below.' none ' leaves it out.");
  Enumeration_knob(f, &_tempMode, kEmitModeNames, "temperature_mode", "read as");
  KnobDefinesGeometry(f);
  Tooltip(f, "A simulation's temperature grid holds KELVIN - measured on an explosion, a mean of "
             "1059 and a peak of 8336 - not brightness.\n\n"
             "Read as a blackbody, that number picks the COLOUR and the scale below sets the "
             "brightness, which is what every other renderer does with it. Read as intensity it "
             "is multiplied in directly, which is right for a 'flames' or 'fuel' grid and turns "
             "a temperature grid into 46361.");
  Double_knob(f, &_tempKmin, IRange(0.0, 12000.0), "temperature_kmin", "K min");
  KnobDefinesGeometry(f);
  Tooltip(f, "Only for a blackbody grid that holds 0..1 instead of Kelvin: the range it is "
             "stretched into. Leave both at 0 when the grid already holds Kelvin, which is "
             "usual.");
  Double_knob(f, &_tempKmax, IRange(0.0, 12000.0), "temperature_kmax", "K max");
  KnobDefinesGeometry(f);
  Tooltip(f, "The top of that range - what a grid value of 1 becomes. A flame is roughly 1200 K "
             "at the dull red edge and 3500 K in the bright core. Left equal to K min, no "
             "remapping happens and the grid is taken as Kelvin already.");
  KnobDefinesGeometry(f);
  Color_knob(f, _tempColor, "temperature_color", "temperature tint");
  KnobDefinesGeometry(f);
  Tooltip(f, "Tints whatever colour comes out. White leaves a blackbody exactly the colour that "
             "temperature really is.");
  Double_knob(f, &_tempScale, IRange(0.0, 20.0), "temperature_scale", "temperature scale");
  KnobDefinesGeometry(f);
  Tooltip(f, "How brightly the temperature grid glows.  0 leaves it out whatever is selected.");

  Enumeration_knob(f, &_emIdx, kNoGrids, "emission_grid", "emission grid");
  SetFlags(f, Knob::SAVE_MENU);
  KnobDefinesGeometry(f);
  Tooltip(f, "A SECOND emissive grid, added to the temperature one - simulations often carry "
             "'flames' or 'fuel' alongside the heat, and they light differently.");
  Enumeration_knob(f, &_emMode, kEmitModeNames, "emission_mode_read", "read as");
  KnobDefinesGeometry(f);
  Tooltip(f, "The same choice for the second grid. A 'flames' grid is usually an intensity - "
             "measured on an explosion it peaks at 7.3 - so intensity is the default here.");
  Double_knob(f, &_emKmin, IRange(0.0, 12000.0), "emission_kmin", "K min");
  Tooltip(f, "Only for a blackbody grid that holds 0..1 instead of Kelvin: the bottom of the "
             "range it is stretched into. Leave both at 0 when the grid already holds Kelvin.\n\n"
             "A grid that is NOT in Kelvin and is left unremapped comes out as one flat colour, "
             "because every value clamps to the coldest entry in the table - the renderer says "
             "so in its info box when it happens.");
  KnobDefinesGeometry(f);
  Double_knob(f, &_emKmax, IRange(0.0, 12000.0), "emission_kmax", "K max");
  Tooltip(f, "The top of that range - what a grid value of 1 becomes.");
  KnobDefinesGeometry(f);
  Color_knob(f, _emissionColor, "emission_color", "emission tint");
  Tooltip(f, "Tints the second emissive grid. On an intensity grid this IS its colour, since "
             "the grid only carries a brightness; on a blackbody one it shifts the colour the "
             "temperature already gives.");
  KnobDefinesGeometry(f);
  Double_knob(f, &_emissionScale, IRange(0.0, 20.0), "emission_scale", "emission scale");
  KnobDefinesGeometry(f);
  Tooltip(f, "How brightly.  0 leaves it out.");
  File_knob(f, &_emissionFile, "emission_file", "emission file");
  KnobDefinesGeometry(f);
  SetFlags(f, Knob::KNOB_CHANGED_ALWAYS);
  Tooltip(f, "Only needed when the emissive grids live in a DIFFERENT .vdb from the density.  "
             "Left empty they come from the same file, which is the usual case.");
}

// The pulldowns are filled from the file, so they have to be refilled whenever it
// changes - and once at load, because a saved scene restores the index and would
// otherwise show it against an empty menu.
void VolumeToUSD::refreshGridMenus()
{
  // READ THE KNOBS, not the members.  knob_changed runs BEFORE the new value is
  // stored into _file, so the member still holds the previous path - and on the
  // first change both are empty, the early-out below fires, and the menus are
  // never filled at all.  Measured: knob_changed ran for 'file' and
  // 'emission_file' and the scan never did.
  std::string file, emFile;
  if (Knob* k = knob("file")) { const char* t = k->get_text(); if (t) file = t; }
  // AND THE CONNECTED FieldVolume, which is where the path normally comes from.
  // Reading only this node's own knob meant that the ordinary wiring - a
  // FieldVolume plugged in and nothing typed here - scanned an empty path and
  // every pulldown said "<no grids found>", on a file whose grids this reads
  // perfectly well when the path is typed by hand.
  if (file.empty()) {
    if (Op* in = Op::input(0)) {
      if (Knob* k = in->knob("file_name")) {
        const char* t = k->get_text();
        if (t && *t) file = t;
      }
    }
  }
  if (Knob* k = knob("emission_file")) { const char* t = k->get_text(); if (t && *t) emFile = t; }
  if (emFile.empty()) emFile = file;
  // the frame filled in, or a sequence is not the name of any file
  const int frame = int(outputContext().frame() + 0.5);
  file = resolveVdbFrame(file, frame);
  emFile = resolveVdbFrame(emFile, frame);

  if (!_forceMenuReload && file == _menuFile && emFile == _menuEmFile) return;
  _forceMenuReload = false;
  _menuFile = file;
  _menuEmFile = emFile;

  std::vector<std::string> dens = vdbGridNames(file);
  if (!ctp::ctpLogPath().empty()) {
    std::string j;
    for (size_t i = 0; i < dens.size(); ++i) { if (i) j += ","; j += dens[i]; }
    ctp::ctpLog("vol:menu", "grids in '" + file + "': [" + j + "]");
  }
  std::vector<std::string> emis = (emFile == file) ? dens : vdbGridNames(emFile);
  // "none" first on the emissive ones: they are optional, and a file whose first
  // grid happened to be the density would otherwise switch itself on.
  std::vector<std::string> densMenu = dens;
  if (densMenu.empty()) densMenu.push_back("<no grids found>");
  std::vector<std::string> emMenu;
  emMenu.push_back("none");
  for (size_t i = 0; i < emis.size(); ++i) emMenu.push_back(emis[i]);

  // Say what was actually scanned and found.  "No grids found" with nothing to
  // explain it is what sent this round in circles: the file was a sequence and
  // the path on screen was not the path being opened.
  {
    std::string msg;
    if (file.empty()) msg = "<i>no file: connect a FieldVolume, or type a .vdb above</i>";
    else {
      std::string names;
      for (size_t i = 0; i < dens.size(); ++i) { if (i) names += ", "; names += dens[i]; }
      const size_t slash = file.find_last_of("/" "\\");
      const std::string leaf = (slash == std::string::npos) ? file : file.substr(slash + 1);
      if (dens.empty())
        msg = "<font color=\"#cc8844\">no grids in " + leaf + "</font>";
      else
        msg = std::to_string(dens.size()) + " grid(s) in " + leaf + ": <b>" + names + "</b>";
    }
    if (Knob* k = knob("grid_report")) k->set_text(msg.c_str());
  }
  if (Knob* k = knob("grid"))
    if (Enumeration_KnobI* e = k->enumerationKnob()) e->menu(densMenu);
  if (Knob* k = knob("temperature_grid"))
    if (Enumeration_KnobI* e = k->enumerationKnob()) e->menu(emMenu);
  if (Knob* k = knob("emission_grid"))
    if (Enumeration_KnobI* e = k->enumerationKnob()) e->menu(emMenu);
}

int VolumeToUSD::knob_changed(Knob* k)
{
  if (k && k->is("reload_grids")) { _forceMenuReload = true; refreshGridMenus(); return 1; }
  // ANY knob, not just the file ones.  Connecting an input fires nothing at all,
  // so the file can change without a single knob moving - and then the menus
  // would stay empty until the file knob happened to be touched.  This early-outs
  // when the path has not moved, so asking on every knob costs a string compare.
  refreshGridMenus();
  if (!k || k->is("file") || k->is("emission_file") || k->is("showPanel")) return 1;
  return GeomOp::knob_changed(k);
}

// the label the pulldown is showing, which is the grid's name
static std::string enumText(Knob* k)
{
  if (!k) return std::string();
  Enumeration_KnobI* e = k->enumerationKnob();
  if (!e) return std::string();
  // the index-taking overload: the no-argument one only exists from Nuke 16 on,
  // and this node builds back to 14.1
  const std::string v = e->getItemValueString(e->getSelectedItemIndex());
  return (v == "none" || v == "<no grids found>") ? std::string() : v;
}

void VolumeToUSD::append(Hash& hash)
{
  GeomOp::append(hash);
  hash.append(_resolvedFile);
  hash.append(_resolvedGrid);
  hash.append(outputContext().frame());   // a sequence is a different file each frame
  hash.append(_preview); hash.append(_previewPoints); hash.append(_previewPurpose);
  hash.append(_densityScale);
  hash.append(_resolvedEmFile);
  hash.append(_resolvedTempGrid); hash.append(_tempScale);
  hash.append(_tempMode); hash.append(_tempKmin); hash.append(_tempKmax);
  hash.append(_emMode); hash.append(_emKmin); hash.append(_emKmax);
  hash.append(_tempColor[0]); hash.append(_tempColor[1]); hash.append(_tempColor[2]);
  hash.append(_resolvedEmGrid); hash.append(_emissionScale);
  hash.append(_emissionColor[0]); hash.append(_emissionColor[1]); hash.append(_emissionColor[2]);
  if (Op* in = Op::input(0)) hash.append(in->hash());
}

// The knob reading happens HERE, not in the engine: reaching into another op's
// knobs is graph work and the engine runs on a render thread.  Same rule as
// ParticlesToUSD's classic read.
void VolumeToUSD::_validate(bool for_real)
{
  GeomOp::_validate(for_real);
  _resolvedFile = _file ? _file : "";
  _resolvedGrid.clear();

  if (_resolvedFile.empty()) {
    Op* in = Op::input(0);
    if (in) {
      bool got = false;
      if (Knob* k = in->knob("file_name")) {
        const char* s = k->get_text();
        if (s && *s) { _resolvedFile = s; got = true; }
      }
      if (Knob* k = in->knob("grid_name")) {
        const char* s = k->get_text();
        if (s && *s) _resolvedGrid = s;
      }
      if (!got && for_real)
        warning("%s carries no 'file_name' to read - connect a FieldVolume, or type a .vdb above. "
                "A field built in the graph has no file behind it: bake it with FieldVolumeWrite.",
                in->Class());
    }
    else if (for_real)
      warning("no .vdb: connect a FieldVolume to the input, or type a file above.");
  }
  // WHAT GETS AUTHORED DOES NOT DEPEND ON THE MENU.
  //
  // The pulldown is a convenience for picking a name, not the only way one can
  // arrive: connecting an input fires no knob_changed, so a freshly wired node
  // has an empty menu until its panel is opened, and refreshing the menu from
  // HERE would be setting a knob value during validate - the very thing Nuke
  // warns about elsewhere in this scene.
  //
  // So the order is: what the pulldown says, else what the connected FieldVolume
  // says, else "density". A scene wired up and rendered without the panel ever
  // being opened still gets the right grid.
  const std::string picked = enumText(knob("grid"));
  if (!picked.empty()) _resolvedGrid = picked;
  if (_resolvedGrid.empty()) _resolvedGrid = "density";

  // The emissive slots have no upstream to fall back on - a FieldVolume names one
  // grid - so an empty menu simply means "off", which is the right default.
  _resolvedTempGrid = enumText(knob("temperature_grid"));
  _resolvedEmGrid = enumText(knob("emission_grid"));
  // the emissive grids usually live in the same file as the density
  _resolvedEmFile = (_emissionFile && *_emissionFile) ? _emissionFile : _resolvedFile;

  // THE FRAME, on the paths that are authored.  A sequence written as
  // AerialExplosion_%04d.vdb is not the name of any file, so Hio cannot open it
  // and the volume simply does not appear - and it has to be resolved per frame
  // here rather than once, or the whole sequence renders frame one.
  // THE PATH IS AUTHORED UNRESOLVED - AerialExplosion_%04d.vdb - and the renderer
  // fills the frame in.
  //
  // Baking the frame in here cannot work: the edit layer persists and Nuke's
  // engine does not re-run for a frame it has already built, so going back to
  // frame 20 after visiting 60 left frame 60's path in the layer and rendered
  // frame 60's explosion. Measured, the loader asked at time 20 and was handed
  // _0060.vdb. A frame-independent path in the stage has nothing to go stale.
  const int frame = frameKey(outputContext().frame());
  {
    std::lock_guard<std::mutex> lock(_cacheMutex);
    std::map<int, Resolved>& byFrame = _cache[node()];
    Resolved r;
    r.file = _resolvedFile; r.grid = _resolvedGrid; r.emFile = _resolvedEmFile;
    r.tempGrid = _resolvedTempGrid; r.emGrid = _resolvedEmGrid;
    byFrame[frame] = r;
    while (byFrame.size() > 8) {
      std::map<int, Resolved>::iterator worst = byFrame.begin();
      for (std::map<int, Resolved>::iterator i = byFrame.begin(); i != byFrame.end(); ++i)
        if (std::abs(i->first - frame) > std::abs(worst->first - frame)) worst = i;
      byFrame.erase(worst);
    }
  }
}

std::map<Node*, std::map<int, VolumeToUSD::Resolved> > VolumeToUSD::_cache;
std::mutex VolumeToUSD::_cacheMutex;

bool VolumeToUSD::resolvedAt(Node* n, int frame, Resolved& out)
{
  std::lock_guard<std::mutex> lock(_cacheMutex);
  std::map<Node*, std::map<int, Resolved> >::const_iterator c = _cache.find(n);
  if (c == _cache.end() || c->second.empty()) return false;
  std::map<int, Resolved>::const_iterator it = c->second.find(frame);
  if (it == c->second.end()) {
    for (std::map<int, Resolved>::const_iterator i = c->second.begin(); i != c->second.end(); ++i)
      if (it == c->second.end() || std::abs(i->first - frame) < std::abs(it->first - frame)) it = i;
  }
  if (it == c->second.end()) return false;
  out = it->second;
  return true;
}

VolumeToUSD::~VolumeToUSD()
{
  std::lock_guard<std::mutex> lock(_cacheMutex);
  _cache.erase(node());
}

// --------------------------------------------------------------------------
void VolumeToUSDEngine::processScenegraph(usg::GeomSceneContext& context)
{
  GeomOpEngine::processScenegraph(context);
  VolumeToUSD* op = dynamic_cast<VolumeToUSD*>(opPtr());
  if (!op) return;

  // EVERY time the context asks for, each with its own file.
  //
  // A sequence is a different .vdb per frame, so the path has to be a TIME
  // SAMPLE, not one value written at the default time.  Written at the default
  // time it is simply the last one authored - and since the layer persists, going
  // back to frame 20 after visiting 60 rendered frame 60's explosion.  Measured:
  // 9215 covered pixels going forward, 10591 on the way back.
  const fdk::TimeValueSet& times = context.processTimes();
  std::vector<fdk::TimeValue> timeList(times.begin(), times.end());
  if (timeList.empty()) timeList.push_back(fdk::defaultTimeValue());

  // THE ENGINE CAN RUN BEFORE THIS OP HAS EVER VALIDATED, which is exactly what
  // a freshly opened script does. _validate is what fills the resolved-file
  // cache, so on that first pass there is nothing in it, and returning here
  // authored NO VOLUME AT ALL - a black frame that stayed black, because nothing
  // afterwards changed the hash. Scrubbing the timebar forced a validate and it
  // came good, which is exactly how it was reported.
  //
  // So ask for the validate rather than giving up: it is idempotent, and this
  // only costs anything on the first pass after a load.
  VolumeToUSD::Resolved R;
  const int firstFrame = VolumeToUSD::frameKey(double(timeList[0]));
  if (!VolumeToUSD::resolvedAt(op->node(), firstFrame, R)) {
    op->validate(true);
    if (!VolumeToUSD::resolvedAt(op->node(), firstFrame, R)) return;
  }
  if (R.file.empty()) return;

  usg::LayerRef edit = editLayer();
  if (!edit) return;

  const std::string nodeName = sanitizeVolName(op->node_name());
  const usg::Path rootPath("/" + nodeName);
  const usg::Path volPath = rootPath.appendChild("volume");
  const usg::Path fieldPath = volPath.appendChild("density");

  usg::Prim::defineInLayer(edit, rootPath, usg::Token("Xform"));
  usg::Prim vol = usg::Prim::defineInLayer(edit, volPath, usg::Token("Volume"));
  usg::Prim fld = usg::Prim::defineInLayer(edit, fieldPath, usg::Token("OpenVDBAsset"));
  if (!vol || !fld) return;

  // the asset: which file, which grid
  usg::Attribute fp = fld.createAttr(usg::Token("filePath"), usg::Value::AssetPath);
  usg::Attribute fn = fld.createAttr(usg::Token("fieldName"), usg::Value::Token);
  for (size_t ti = 0; ti < timeList.size(); ++ti) {
    VolumeToUSD::Resolved Rt;
    if (!VolumeToUSD::resolvedAt(op->node(), VolumeToUSD::frameKey(double(timeList[ti])), Rt)) continue;
    if (fp) fp.setValue(usg::AssetPath(Rt.file), timeList[ti]);
    if (fn) fn.setValue(usg::Token(Rt.grid), timeList[ti]);
  }

  // field:density is a RELATIONSHIP, not an attribute - that is how UsdVolVolume
  // names its grids, and an attribute here is simply not seen.
  pointFieldAt(vol, "field:density", fieldPath);

  // Carried as a plain attribute for the renderer to read: USD has no schema for
  // "how thick is this", and every renderer wants its own scale.
  usg::Attribute ds = vol.createAttr(usg::Token("ir:densityScale"), usg::Value::Float);
  if (ds) ds.setValue(float(op->_densityScale), fdk::defaultTimeValue());

  // EVERY ir: value is authored TWICE, once as the plain attribute above and
  // once as a constant primvar below.  The node's stage loader reads the
  // attribute; a Hydra delegate cannot - an rprim can only ask its scene
  // delegate for primvars - so a volume shown through GeoRender would otherwise
  // arrive with every shading knob at its default.
  usg::Attribute dsp = vol.createAttr(usg::Token("primvars:ir:densityScale"), usg::Value::Float);
  if (dsp) dsp.setValue(float(op->_densityScale), fdk::defaultTimeValue());

  // Which FRAME is being drawn.  The file path is authored unresolved - it still
  // says %04d - because a path resolved once renders the whole shot as one frame,
  // and the node resolves it against the frame it is rendering.  Hydra has no
  // frame to hand a render pass, so the frame travels as a primvar and is
  // sampled at whatever time the delegate is showing.
  usg::Attribute frp = vol.createAttr(usg::Token("primvars:ir:frame"), usg::Value::Int);
  if (frp)
    for (size_t ti = 0; ti < timeList.size(); ++ti)
      frp.setValue(int(VolumeToUSD::frameKey(double(timeList[ti]))), timeList[ti]);

  // ---- the emissive grids ---------------------------------------------------
  // TWO of them, summed: simulations carry heat and flames separately and they
  // read differently.  Named through field:temperature and field:emission -
  // UsdVolVolume names its grids by relationship, so any name works and the two
  // stay told apart.
  struct EmSlot { const char* rel; const char* child; const std::string* grid; double scale;
                  const float* col; int mode; double kmin, kmax; };
  const EmSlot slots[2] = {
    { "field:temperature", "temperature", &R.tempGrid, op->_tempScale, op->_tempColor,
      op->_tempMode, op->_tempKmin, op->_tempKmax },
    { "field:emission",    "emission",    &R.emGrid,   op->_emissionScale, op->_emissionColor,
      op->_emMode, op->_emKmin, op->_emKmax },
  };
  for (int si = 0; si < 2; ++si) {
    const EmSlot& sl = slots[si];
    const bool on = !sl.grid->empty() && sl.scale > 0.0;
    if (on) {
      const usg::Path emPath = volPath.appendChild(sl.child);
      usg::Prim em = usg::Prim::defineInLayer(edit, emPath, usg::Token("OpenVDBAsset"));
      if (em) {
        usg::Attribute efp = em.createAttr(usg::Token("filePath"), usg::Value::AssetPath);
        usg::Attribute efn = em.createAttr(usg::Token("fieldName"), usg::Value::Token);
        for (size_t ti = 0; ti < timeList.size(); ++ti) {
          VolumeToUSD::Resolved Rt;
          if (!VolumeToUSD::resolvedAt(op->node(), VolumeToUSD::frameKey(double(timeList[ti])), Rt)) continue;
          const std::string& gname = (si == 0) ? Rt.tempGrid : Rt.emGrid;
          if (gname.empty()) continue;
          if (efp) efp.setValue(usg::AssetPath(Rt.emFile), timeList[ti]);
          if (efn) efn.setValue(usg::Token(gname), timeList[ti]);
        }
        pointFieldAt(vol, sl.rel, emPath);
      }
    }
    // the scale is authored either way, so switching a slot off turns it off in
    // the stage rather than leaving the last value behind
    char sname[64], cname[64];
    snprintf(sname, sizeof(sname), "ir:%sScale", sl.child);
    snprintf(cname, sizeof(cname), "ir:%sColor", sl.child);
    usg::Attribute es = vol.createAttr(usg::Token(sname), usg::Value::Float);
    if (es) es.setValue(float(on ? sl.scale : 0.0), fdk::defaultTimeValue());
    usg::Attribute ec = vol.createAttr(usg::Token(cname), usg::Value::Color3f);
    if (ec) ec.setValue(fdk::Vec3f(sl.col[0], sl.col[1], sl.col[2]), fdk::defaultTimeValue());
    char mname[64], k0name[64], k1name[64];
    snprintf(mname, sizeof(mname), "ir:%sMode", sl.child);
    snprintf(k0name, sizeof(k0name), "ir:%sKmin", sl.child);
    snprintf(k1name, sizeof(k1name), "ir:%sKmax", sl.child);
    usg::Attribute em2 = vol.createAttr(usg::Token(mname), usg::Value::Int);
    if (em2) em2.setValue(int(sl.mode), fdk::defaultTimeValue());
    usg::Attribute k0 = vol.createAttr(usg::Token(k0name), usg::Value::Float);
    if (k0) k0.setValue(float(sl.kmin), fdk::defaultTimeValue());
    usg::Attribute k1 = vol.createAttr(usg::Token(k1name), usg::Value::Float);
    if (k1) k1.setValue(float(sl.kmax), fdk::defaultTimeValue());

    // ...and again as primvars, for the Hydra delegate
    char psname[80], pcname[80], pmname[80], pk0[80], pk1[80];
    snprintf(psname, sizeof(psname), "primvars:ir:%sScale", sl.child);
    snprintf(pcname, sizeof(pcname), "primvars:ir:%sColor", sl.child);
    snprintf(pmname, sizeof(pmname), "primvars:ir:%sMode", sl.child);
    snprintf(pk0, sizeof(pk0), "primvars:ir:%sKmin", sl.child);
    snprintf(pk1, sizeof(pk1), "primvars:ir:%sKmax", sl.child);
    usg::Attribute pes = vol.createAttr(usg::Token(psname), usg::Value::Float);
    if (pes) pes.setValue(float(on ? sl.scale : 0.0), fdk::defaultTimeValue());
    usg::Attribute pec = vol.createAttr(usg::Token(pcname), usg::Value::Color3f);
    if (pec) pec.setValue(fdk::Vec3f(sl.col[0], sl.col[1], sl.col[2]), fdk::defaultTimeValue());
    usg::Attribute pem = vol.createAttr(usg::Token(pmname), usg::Value::Int);
    if (pem) pem.setValue(int(sl.mode), fdk::defaultTimeValue());
    usg::Attribute pk0a = vol.createAttr(usg::Token(pk0), usg::Value::Float);
    if (pk0a) pk0a.setValue(float(sl.kmin), fdk::defaultTimeValue());
    usg::Attribute pk1a = vol.createAttr(usg::Token(pk1), usg::Value::Float);
    if (pk1a) pk1a.setValue(float(sl.kmax), fdk::defaultTimeValue());
  }

  // ---- what the VIEWER sees -------------------------------------------------
  // A Volume prim draws nothing on its own, so without this the node is blank in
  // the viewport and there is nothing to frame on.  The bounds come from the
  // file's own header - the voxels cannot be decoded here, they are compressed
  // and Nuke ships openvdb.dll with no headers - so the preview says WHERE the
  // volume is, not what it looks like.
  {
    const VdbBounds vb = vdbGridBounds(R.file);
    if (vb.ok) {
      // an extent on the Volume itself, so a viewer can frame and select it even
      // with the preview switched off
      fdk::Box3f ext;
      ext.expand(fdk::Vec3f(float(vb.mn[0]), float(vb.mn[1]), float(vb.mn[2])));
      ext.expand(fdk::Vec3f(float(vb.mx[0]), float(vb.mx[1]), float(vb.mx[2])));
      usg::BoundablePrim(vol).setBoundsAttr(ext, fdk::defaultTimeValue());

      const usg::Token purpose = (op->_previewPurpose == 0) ? usg::GeomTokens.guide
                                                            : usg::GeomTokens.default_;
      if (op->_preview != kPreviewOff) {
        // twelve edges of the box
        const usg::Path boxPath = rootPath.appendChild("preview_box");
        usg::BasisCurvesPrim bc = usg::BasisCurvesPrim::defineInLayer(edit, boxPath);
        if (bc) {
          static const int E[12][2] = { {0,1},{1,3},{3,2},{2,0}, {4,5},{5,7},{7,6},{6,4},
                                        {0,4},{1,5},{2,6},{3,7} };
          fdk::Vec3f c[8];
          for (int i = 0; i < 8; ++i)
            c[i] = fdk::Vec3f(float((i & 1) ? vb.mx[0] : vb.mn[0]),
                              float((i & 2) ? vb.mx[1] : vb.mn[1]),
                              float((i & 4) ? vb.mx[2] : vb.mn[2]));
          usg::Vec3fArray pts; pts.resize(24);
          usg::IntArray counts; counts.resize(12);
          usg::FloatArray widths; widths.resize(24);
          for (int e = 0; e < 12; ++e) {
            pts[e * 2] = c[E[e][0]];
            pts[e * 2 + 1] = c[E[e][1]];
            counts[e] = 2;
            widths[e * 2] = widths[e * 2 + 1] = 0.0f;
          }
          bc.setType(usg::GeomTokens.linear);
          bc.setCurveVertexCounts(counts);
          bc.setPoints(pts, fdk::defaultTimeValue());
          bc.setWidths(widths, fdk::defaultTimeValue());
          bc.setPurpose(purpose);
          bc.setBoundsAttr(ext, fdk::defaultTimeValue());
          // Marked, so a renderer can refuse it whatever purpose it carries.
          // Otherwise the only way to keep a preview out of the render is the
          // guide purpose, and the Viewer hides that - which is how "display the
          // volume" ends up displaying nothing.
          usg::Attribute pv = usg::Prim(bc).createAttr(usg::Token("ir:preview"), usg::Value::Bool);
          if (pv) pv.setValue(true, fdk::defaultTimeValue());
        }
      }
      if (op->_preview == kPreviewPoints) {
        const int n = op->_previewPoints < 2 ? 2 : op->_previewPoints;
        const usg::Path ptPath = rootPath.appendChild("preview_points");
        usg::PointsPrim pp = usg::PointsPrim::defineInLayer(edit, ptPath);
        if (pp) {
          usg::Vec3fArray pts; pts.resize(size_t(n) * size_t(n) * size_t(n));
          usg::FloatArray widths; widths.resize(pts.size());
          const double sx = (vb.mx[0] - vb.mn[0]) / double(n);
          const double sy = (vb.mx[1] - vb.mn[1]) / double(n);
          const double sz = (vb.mx[2] - vb.mn[2]) / double(n);
          const float w = float(0.35 * (sx < sy ? (sx < sz ? sx : sz) : (sy < sz ? sy : sz)));
          size_t k = 0;
          for (int z = 0; z < n; ++z)
            for (int y = 0; y < n; ++y)
              for (int x = 0; x < n; ++x, ++k) {
                pts[k] = fdk::Vec3f(float(vb.mn[0] + (double(x) + 0.5) * sx),
                                    float(vb.mn[1] + (double(y) + 0.5) * sy),
                                    float(vb.mn[2] + (double(z) + 0.5) * sz));
                widths[k] = w;
              }
          pp.setPoints(pts, fdk::defaultTimeValue());
          pp.setWidths(widths, fdk::defaultTimeValue());
          pp.setPurpose(purpose);
          pp.setBoundsAttr(ext, fdk::defaultTimeValue());
          usg::Attribute pv2 = usg::Prim(pp).createAttr(usg::Token("ir:preview"), usg::Value::Bool);
          if (pv2) pv2.setValue(true, fdk::defaultTimeValue());
        }
      }
    }
  }

  ctp::ctpLog("vol:process", R.file + " grid " + R.grid);
}
