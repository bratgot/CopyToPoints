// CopyCore.h
//
// Shared, renderer-agnostic core of the CopyToPoints plugins (classic 3D
// CopyToPoints and USD CopyToPointsUSD): painted weight layers and their
// serialisation, seeded randomness, rotation helpers, the per-target
// transform/colour/variant logic (processSample), scattering across faces and
// the paint-mesh queries used by the viewer brush.  Depends only on DDImage
// math types (Vector3/Vector4/Matrix4).  Strict ASCII.

#pragma once

#include "DDImage/Matrix4.h"
#include "DDImage/Vector3.h"
#include "DDImage/Vector4.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall GetEnvironmentVariableA(const char* name, char* buffer, unsigned long size);
#endif

namespace ctp {

using DD::Image::Matrix4;
using DD::Image::Vector3;
using DD::Image::Vector4;

// Paint layers (weights painted in the 3D viewer per source point)
// Layers 5..8 hold a painted colour (RGB + coverage A).  The brush UI exposes
// them as ONE entry ("colour", index kPaintLayerColor == kPaintLayerColR); the
// G/B/A layers are internal.  Old 5-layer scripts load with colour zeroed.
enum { kPaintLayerDensity = 0, kPaintLayerScale = 1, kPaintLayerRotate = 2, kPaintLayerVariant = 3, kPaintLayerScatter = 4,
       kPaintLayerColR = 5, kPaintLayerColG = 6, kPaintLayerColB = 7, kPaintLayerColA = 8, kPaintLayerCount = 9 };
const int kPaintLayerColor = kPaintLayerColR;
const char* const kPaintLayerNames[] = { "density", "scale", "rotation", "variant", "scatter", "colour", nullptr };
enum { kPaintColorReplace = 0, kPaintColorMultiply = 1 };
const char* const kPaintColorModeNames[] = { "replace copy colour", "multiply copy colour", nullptr };

// Scatter weighting by terrain feature
enum { kScatterWUniform = 0, kScatterWFlat = 1, kScatterWSteep = 2, kScatterWPeaks = 3, kScatterWValleys = 4 };
const char* const kScatterWeightNames[] = {
  "uniform (by area)", "prefer flat areas", "prefer steep slopes", "prefer peaks (high)", "prefer valleys (low)", nullptr };
enum { kPaintModeAdd = 0, kPaintModeSubtract = 1, kPaintModeSet = 2, kPaintModeSmooth = 3 };
const char* const kPaintModeNames[] = { "add", "subtract", "set", "smooth", nullptr };
enum { kPaintAxisX = 0, kPaintAxisY = 1, kPaintAxisZ = 2 };
const char* const kPaintAxisNames[] = { "local X", "local Y", "local Z", nullptr };

// Weight storage: SIGNED 16-bit fixed point per source point per layer,
// value = raw / 4096 -> -7.999 .. +7.999 with a resolution of ~0.00024.
// Every layer is an OFFSET from the node's current value (0 = "as the knobs
// say"): painting adds to or subtracts from it.  (Script format v3; v2 tokens
// from 1.x were unsigned 0..16 and are read as-is.)
const float kPaintScale = 4096.0f;
const float kPaintMax = 32767.0f / 4096.0f;
struct PaintLayers {
  unsigned npoints;
  std::vector<int16_t> data[kPaintLayerCount];
  PaintLayers() : npoints(0) {}
  bool hasData() const
  {
    for (int l = 0; l < kPaintLayerCount; ++l)
      for (size_t i = 0; i < data[l].size(); ++i) if (data[l][i]) return true;
    return false;
  }
  bool layerHasData(int l) const
  {
    if (l < 0 || l >= kPaintLayerCount) return false;
    for (size_t i = 0; i < data[l].size(); ++i) if (data[l][i]) return true;
    return false;
  }
  // largest |weight| of the layer (auto range of the heat map)
  float layerMax(int l) const
  {
    int m = 0;
    if (l >= 0 && l < kPaintLayerCount) for (size_t i = 0; i < data[l].size(); ++i) { const int a = data[l][i] < 0 ? -int(data[l][i]) : int(data[l][i]); if (a > m) m = a; }
    return float(m) / kPaintScale;
  }
  void resize(unsigned n)
  {
    npoints = n;
    for (int l = 0; l < kPaintLayerCount; ++l) data[l].resize(n, 0);
  }
  void clearLayer(int l)
  {
    if (l < 0 || l >= kPaintLayerCount) return;
    std::fill(data[l].begin(), data[l].end(), int16_t(0));
    if (l == kPaintLayerColor)   // the colour layer is rgb + coverage
      for (int c = 1; c < 4; ++c) std::fill(data[kPaintLayerColR + c].begin(), data[kPaintLayerColR + c].end(), int16_t(0));
  }
  void clearAll() { for (int l = 0; l < kPaintLayerCount; ++l) clearLayer(l); }
  // flood fill: every point of the layer gets the value (colour layer: rgb + full coverage)
  void fillLayer(int l, float v, float r = 1.0f, float g = 1.0f, float b = 1.0f)
  {
    if (l == kPaintLayerColor) {
      for (size_t i = 0; i < npoints; ++i) {
        set(kPaintLayerColR, i, r); set(kPaintLayerColG, i, g); set(kPaintLayerColB, i, b); set(kPaintLayerColA, i, 1.0f);
      }
      return;
    }
    if (l < 0 || l >= kPaintLayerCount) return;
    for (size_t i = 0; i < npoints; ++i) set(l, i, v);
  }
  float get(int l, size_t i) const
  {
    if (l < 0 || l >= kPaintLayerCount || i >= data[l].size()) return 0.0f;
    return float(data[l][i]) * (1.0f / kPaintScale);
  }
  void set(int l, size_t i, float v)
  {
    if (l < 0 || l >= kPaintLayerCount || i >= data[l].size()) return;
    if (v < -kPaintMax) v = -kPaintMax;
    if (v > kPaintMax) v = kPaintMax;
    data[l][i] = int16_t(v * kPaintScale + (v >= 0.0f ? 0.5f : -0.5f));
  }
};

// heat map colour for t in -1..1: negative = grey -> magenta, 0 = dark grey,
// positive = blue -> cyan -> green -> yellow -> red
inline void heatColor(float t, float& r, float& g, float& b)
{
  if (t < 0.0f) {
    const float u = std::min(1.0f, -t);
    r = 0.25f + 0.75f * u; g = 0.25f * (1.0f - u); b = 0.3f + 0.7f * u;
    return;
  }
  if (t > 1.0f) t = 1.0f;
  const float x = t * 4.0f;
  if (x < 1.0f)      { r = 0.0f;        g = x;            b = 1.0f; }
  else if (x < 2.0f) { r = 0.0f;        g = 1.0f;         b = 2.0f - x; }
  else if (x < 3.0f) { r = x - 2.0f;    g = 1.0f;         b = 0.0f; }
  else               { r = 1.0f;        g = 4.0f - x;     b = 0.0f; }
}

const char* const kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64encode(const std::vector<uint8_t>& in)
{
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  while (i + 2 < in.size()) {
    const unsigned v = (unsigned(in[i]) << 16) | (unsigned(in[i + 1]) << 8) | unsigned(in[i + 2]);
    out.push_back(kB64[(v >> 18) & 63]); out.push_back(kB64[(v >> 12) & 63]);
    out.push_back(kB64[(v >> 6) & 63]);  out.push_back(kB64[v & 63]);
    i += 3;
  }
  if (i + 1 == in.size()) {
    const unsigned v = unsigned(in[i]) << 16;
    out.push_back(kB64[(v >> 18) & 63]); out.push_back(kB64[(v >> 12) & 63]); out.push_back('='); out.push_back('=');
  }
  else if (i + 2 == in.size()) {
    const unsigned v = (unsigned(in[i]) << 16) | (unsigned(in[i + 1]) << 8);
    out.push_back(kB64[(v >> 18) & 63]); out.push_back(kB64[(v >> 12) & 63]); out.push_back(kB64[(v >> 6) & 63]); out.push_back('=');
  }
  return out;
}

bool b64decode(const std::string& in, std::vector<uint8_t>& out)
{
  out.clear();
  int val = 0, bits = -8;
  for (size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    if (c == '=' ) break;
    const char* pos = std::strchr(kB64, c);
    if (!pos || !c) return false;
    val = (val << 6) | int(pos - kB64);
    bits += 6;
    if (bits >= 0) { out.push_back(uint8_t((val >> bits) & 0xFF)); bits -= 8; }
  }
  return true;
}

// run-length for 16-bit values: (hi, lo, run) triples, run 1..255
std::vector<uint8_t> rleEncode16(const std::vector<int16_t>& in)
{
  std::vector<uint8_t> out;
  size_t i = 0;
  while (i < in.size()) {
    const uint16_t v = uint16_t(in[i]);
    size_t run = 1;
    while (i + run < in.size() && uint16_t(in[i + run]) == v && run < 255) ++run;
    out.push_back(uint8_t(v >> 8)); out.push_back(uint8_t(v & 0xFF)); out.push_back(uint8_t(run));
    i += run;
  }
  return out;
}

void rleDecode16(const std::vector<uint8_t>& in, std::vector<int16_t>& out)
{
  out.clear();
  for (size_t i = 0; i + 2 < in.size(); i += 3) {
    const int16_t v = int16_t(uint16_t((unsigned(in[i]) << 8) | in[i + 1])); const unsigned run = in[i + 2];
    for (unsigned k = 0; k < run; ++k) out.push_back(v);
  }
}

// legacy v1 (8-bit 0..255 = 0..1): (value, run) pairs
void rleDecode8(const std::vector<uint8_t>& in, std::vector<int16_t>& out)
{
  out.clear();
  for (size_t i = 0; i + 1 < in.size(); i += 2) {
    const int16_t v = int16_t(float(in[i]) / 255.0f * kPaintScale + 0.5f); const unsigned run = in[i + 1];
    for (unsigned k = 0; k < run; ++k) out.push_back(v);
  }
}

// Moeller-Trumbore ray/triangle
bool rayTriangle(const Vector3& o, const Vector3& d, const Vector3& a, const Vector3& b, const Vector3& c, float& t)
{
  const Vector3 e1 = b - a, e2 = c - a;
  const Vector3 pv = d.cross(e2);
  const float det = e1.dot(pv);
  if (std::fabs(det) < 1e-12f) return false;
  const float inv = 1.0f / det;
  const Vector3 tv = o - a;
  const float u = tv.dot(pv) * inv;
  if (u < 0.0f || u > 1.0f) return false;
  const Vector3 qv = tv.cross(e1);
  const float v = d.dot(qv) * inv;
  if (v < 0.0f || u + v > 1.0f) return false;
  t = e2.dot(qv) * inv;
  return t > 1e-6f;
}

// Guide geometry (viewer only, never rendered)
enum { kGuideOff = 0, kGuidePoints = 1, kGuidePointsAxes = 2 };
const char* const kGuideNames[] = { "off", "copy positions (points)", "positions + up axis lines", nullptr };

// Scatter points across faces (Houdini Scatter)
enum { kScatterOff = 0, kScatterAdd = 1, kScatterReplace = 2 };
const char* const kScatterNames[] = { "off", "add to the points", "replace the points", nullptr };

inline void rgbToHsv(float r, float g, float b, float& h, float& sv, float& v)
{
  const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
  v = mx;
  const float d = mx - mn;
  sv = (mx > 1e-6f) ? d / mx : 0.0f;
  if (d < 1e-6f) { h = 0.0f; return; }
  if (mx == r) h = (g - b) / d + (g < b ? 6.0f : 0.0f);
  else if (mx == g) h = (b - r) / d + 2.0f;
  else h = (r - g) / d + 4.0f;
  h /= 6.0f;
}

inline void hsvToRgb(float h, float sv, float v, float& r, float& g, float& b)
{
  h = h - std::floor(h);
  const float i = std::floor(h * 6.0f);
  const float f = h * 6.0f - i;
  const float p = v * (1.0f - sv), q = v * (1.0f - f * sv), t = v * (1.0f - (1.0f - f) * sv);
  switch (int(i) % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
}

// Extra spin on top of the alignment
enum { kSpinNone = 0, kSpinRoll = 1 };
const char* const kSpinNames[] = { "none", "roll along velocity (distance based)", nullptr };

// Forward axis of the prototype that gets pointed along the direction
enum { kAxisPX = 0, kAxisPY, kAxisPZ, kAxisNX, kAxisNY, kAxisNZ };
const char* const kAxisNames[] = { "+X", "+Y", "+Z", "-X", "-Y", "-Z", nullptr };

const float kDegToRad = 3.14159265358979323846f / 180.0f;

// --------------------------------------------------------------------------
// Small deterministic hash-based RNG (stable per point id / seed / salt).
// --------------------------------------------------------------------------
inline uint32_t hashU32(uint32_t x)
{
  x ^= x >> 16; x *= 0x7feb352dU;
  x ^= x >> 15; x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

inline float rand01(uint32_t id, uint32_t seed, uint32_t salt)
{
  const uint32_t h = hashU32((id + 0x9E3779B9U) ^ hashU32(seed * 0x85ebca6bU + salt * 0xc2b2ae35U + 0x27d4eb2fU));
  return float(h >> 8) * (1.0f / 16777216.0f);   // [0,1)
}

inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// Schlick's bias and gain, the two cheap remaps of an even 0..1 random.
//
//   BIAS  slides the whole distribution toward one end - more small copies or
//         more large ones - and stays monotonic, so nothing crosses over.
//   SHAPE is his gain: it pulls values toward the MIDDLE or pushes them out to
//         the two ENDS, leaving the average where it was.
//
// Both take a signed amount where 0 is untouched, and 0 returns t itself rather
// than something a rounding error away from it: at the default every scene must
// render exactly as it did before these knobs existed.
inline float schlickBias(float t, float b)
{
  if (b <= 0.0f) return 0.0f;
  if (b >= 1.0f) return 1.0f;
  return t / (((1.0f / b) - 2.0f) * (1.0f - t) + 1.0f);
}

inline float shapeRandom(float t, double bias, double shape)
{
  if (bias != 0.0) {
    // -1..1 onto Schlick's 0..1, where his 0.5 is the identity
    const float b = float(0.5 + 0.5 * std::max(-0.999, std::min(0.999, bias)));
    t = schlickBias(t, b);
  }
  if (shape != 0.0) {
    const float g = float(0.5 + 0.5 * std::max(-0.999, std::min(0.999, shape)));
    if (t < 0.5f) t = 0.5f * schlickBias(2.0f * t, 1.0f - g);
    else          t = 1.0f - 0.5f * schlickBias(2.0f - 2.0f * t, 1.0f - g);
  }
  return t;
}

// Optional flushed-per-line debug log: set env var CTP_LOG=<file path>.
inline const std::string& ctpLogPath()
{
  // Copy the value: getenv() returns a pointer into the process environment
  // block, which the host (e.g. Python's os.environ) may reallocate later.
  static const std::string path = [] {
#ifdef _WIN32
    // GetEnvironmentVariableA reads the live process block (std::getenv reads
    // the CRT's copy, which is empty in some hosts / newer toolsets)
    char buf[1024];
    const unsigned long n = ::GetEnvironmentVariableA("CTP_LOG", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) return std::string(buf, n);
#endif
    const char* p = std::getenv("CTP_LOG"); return p ? std::string(p) : std::string(); }();
  return path;
}

inline void ctpLog(const char* stage, const std::string& extra = std::string())
{
  const std::string& path = ctpLogPath();
  if (path.empty()) return;
  static std::mutex mtx;
  std::lock_guard<std::mutex> lock(mtx);
  std::ofstream f(path.c_str(), std::ios::out | std::ios::app);
  if (f) f << stage << (extra.empty() ? "" : " ") << extra << std::endl;
}

std::string trimCopy(const std::string& s)
{
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
  return s.substr(b, e - b);
}

std::vector<std::string> splitList(const std::string& s)
{
  std::vector<std::string> out;
  std::string cur;
  for (size_t i = 0; i <= s.size(); ++i) {
    const char c = (i < s.size()) ? s[i] : ',';
    if (c == ',' || c == ' ' || c == ';') {
      const std::string t = trimCopy(cur);
      if (!t.empty()) out.push_back(t);
      cur.clear();
    }
    else {
      cur.push_back(c);
    }
  }
  return out;
}

// Rotation that maps the chosen local axis onto +Z (so that a +Z-forward
// frame can be used for every axis choice).
Matrix4 axisToPlusZ(int axis)
{
  Matrix4 m;
  m.makeIdentity();
  switch (axis) {
    case kAxisPX: m.rotationY(-90.0f * kDegToRad); break;
    case kAxisPY: m.rotationX( 90.0f * kDegToRad); break;
    case kAxisPZ: break;
    case kAxisNX: m.rotationY( 90.0f * kDegToRad); break;
    case kAxisNY: m.rotationX(-90.0f * kDegToRad); break;
    case kAxisNZ: m.rotationY(180.0f * kDegToRad); break;
    default: break;
  }
  return m;
}

// Build a rotation whose +Z axis is 'dir' and whose +Y is as close to 'up'
// as possible.  Returns identity for a degenerate direction.
Matrix4 lookAlong(Vector3 dir, Vector3 up)
{
  Matrix4 m;
  m.makeIdentity();
  const float len = dir.length();
  if (len < 1e-12f) return m;
  dir = dir * (1.0f / len);
  if (up.lengthSquared() < 1e-12f) up = Vector3(0.0f, 1.0f, 0.0f);
  Vector3 x = up.cross(dir);
  if (x.lengthSquared() < 1e-10f) {
    // dir parallel to up: pick any perpendicular helper
    const Vector3 helper = (std::fabs(dir.y) < 0.9f) ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f);
    x = helper.cross(dir);
  }
  x.normalize();
  Vector3 y = dir.cross(x);
  y.normalize();
  m.setXAxis(x);
  m.setYAxis(y);
  m.setZAxis(dir);
  return m;
}

Matrix4 quaternionToMatrix(float qx, float qy, float qz, float qw)
{
  const float n = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
  Matrix4 m;
  m.makeIdentity();
  if (n < 1e-12f) return m;
  qx /= n; qy /= n; qz /= n; qw /= n;
  const float xx = qx * qx, yy = qy * qy, zz = qz * qz;
  const float xy = qx * qy, xz = qx * qz, yz = qy * qz;
  const float wx = qw * qx, wy = qw * qy, wz = qw * qz;
  m.setXAxis(Vector3(1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz),        2.0f * (xz - wy)));
  m.setYAxis(Vector3(2.0f * (xy - wz),        1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx)));
  m.setZAxis(Vector3(2.0f * (xz + wy),        2.0f * (yz - wx),        1.0f - 2.0f * (xx + yy)));
  return m;
}

// XYZ rotation order: X is applied to the point first, then Y, then Z
// (M = Rz * Ry * Rx), matching Nuke's Axis / Houdini convention.
Matrix4 eulerXYZ(float rxDeg, float ryDeg, float rzDeg)
{
  Matrix4 rx, ry, rz;
  rx.rotationX(rxDeg * kDegToRad);
  ry.rotationY(ryDeg * kDegToRad);
  rz.rotationZ(rzDeg * kDegToRad);
  return rz * ry * rx;
}

// Variant / align / source enumerations shared by both nodes
enum { kVariantSequential = 0, kVariantRandom = 1, kVariantAttribute = 2 };
enum { kAlignNone = 0, kAlignDirection = 1, kAlignQuaternion = 2, kAlignEuler = 3, kAlignParticle = 4 };

// One emitted copy
struct CopyRec {
  Matrix4  xform;        // world transform (before the prototype's own matrix)
  Vector4  color;        // colour (if any)
  bool     hasColor;
  int      variant;
  uint32_t id;           // stable id (point id attribute or running index)
  unsigned srcObject;    // index of the source object
  unsigned srcPoint;     // index of the source point in that object
  bool     hasPaint;     // painted weights interpolated at this copy (guide heat map)
  float    w[kPaintLayerCount];
  CopyRec() : hasColor(false), variant(0), id(0), srcObject(0), srcPoint(0), hasPaint(false)
  { xform.makeIdentity(); color = Vector4(1, 1, 1, 1); for (int l = 0; l < kPaintLayerCount; ++l) w[l] = 0.0f; }
};

// Guide point colour: heat map of the painted layer at the copy (or the colour
// layer's rgb), falling back to the source-type colour (cyan = vertex, orange
// = scattered).
inline void guideColor(const CopyRec& rec, bool heat, int layer, float hmax, float& r, float& g, float& b)
{
  const bool scattered = rec.id >= 0x40000000u;
  if (heat && rec.hasPaint) {
    if (layer == kPaintLayerColor) {
      const float a = std::min(1.0f, std::max(0.0f, rec.w[kPaintLayerColA]));
      r = 0.25f + (rec.w[kPaintLayerColR] - 0.25f) * a;
      g = 0.25f + (rec.w[kPaintLayerColG] - 0.25f) * a;
      b = 0.3f + (rec.w[kPaintLayerColB] - 0.3f) * a;
      return;
    }
    const float w = (layer >= 0 && layer < kPaintLayerCount) ? rec.w[layer] : 0.0f;
    if (w == 0.0f) { r = 0.25f; g = 0.25f; b = 0.3f; return; }
    heatColor(w / (hmax > 0.0f ? hmax : 1.0f), r, g, b);
    return;
  }
  if (scattered) { r = 1.0f; g = 0.55f; b = 0.1f; } else { r = 0.2f; g = 0.9f; b = 1.0f; }
}

// Everything processSample() needs from the node's knobs
struct GatherParams {
  uint32_t seed, vseed; int nVar;
  Matrix4 axisFix; Vector3 upVec; Matrix4 userRot; bool hasUserRot; Vector3 offset; bool hasOffset;
  double density;
  bool   paintDensityEnable;
  int    variantMode;
  bool   paintVariantEnable;
  double colorVarHue, colorVarSat, colorVarVal;
  int    alignMode;
  int    spinMode; double rollRate;
  bool   randomRotate; double rotMin[3]; double rotMax[3];
  double rotVariance;
  bool   paintRotEnable; double paintRotAmount; int paintRotAxis;
  double scale; double scaleXYZ[3];
  bool   randomScale; double scaleMin, scaleMax, scaleBias, scaleShape;
  bool   paintScaleEnable; double paintScaleAmount;
  bool   paintColorEnable; int paintColorMode;
  bool   randomOffset; double offMin[3]; double offMax[3]; double offVariance[3];
  GatherParams()
    : seed(0), vseed(0), nVar(1), hasUserRot(false), offset(0, 0, 0), hasOffset(false), density(1.0), paintDensityEnable(false),
      variantMode(0), paintVariantEnable(false), colorVarHue(0), colorVarSat(0), colorVarVal(0), alignMode(0), spinMode(0),
      rollRate(0), randomRotate(false), rotVariance(0), paintRotEnable(false), paintRotAmount(0), paintRotAxis(1), scale(1.0),
      randomScale(false), scaleMin(1), scaleMax(1), scaleBias(0), scaleShape(0),
      paintScaleEnable(false), paintScaleAmount(1),
      paintColorEnable(false), paintColorMode(0), randomOffset(false)
  {
    axisFix.makeIdentity(); userRot.makeIdentity(); upVec = Vector3(0, 1, 0);
    for (int i = 0; i < 3; ++i) { rotMin[i] = rotMax[i] = 0.0; scaleXYZ[i] = 1.0; offMin[i] = offMax[i] = offVariance[i] = 0.0; }
  }
};

struct PointSample {
  uint32_t id;
  uint32_t order;          // running index (sequential variant)
  Vector3  Pw;             // world position
  bool hasDir;   Vector3 dir;       // world-space direction for align = direction
  bool hasQuat;  Vector4 quat;
  bool hasEuler; Vector3 euler;
  bool hasSize;  float   size;
  bool hasScaleVec; Vector3 scaleVec;
  bool hasColor; Vector4 color;
  bool hasVariantAttr; int variantAttr;
  bool hasPaint; float w[kPaintLayerCount];
  bool rollOK;   Vector3 rollVel; float rollDist;
  unsigned srcObject, srcPoint;
  PointSample()
    : id(0), order(0), Pw(0, 0, 0), hasDir(false), dir(0, 0, 0), hasQuat(false), quat(0, 0, 0, 1), hasEuler(false), euler(0, 0, 0),
      hasSize(false), size(1.0f), hasScaleVec(false), scaleVec(1, 1, 1), hasColor(false), color(1, 1, 1, 1),
      hasVariantAttr(false), variantAttr(0), hasPaint(false), rollOK(false), rollVel(0, 0, 0), rollDist(0.0f),
      srcObject(0), srcPoint(0)
  { for (int l = 0; l < kPaintLayerCount; ++l) w[l] = 0.0f; }
};

// Shared per-target work: culling, variant, colour, rotation, scale, matrix.
// Returns false when the target is culled.
inline bool processSample(const PointSample& ps, const GatherParams& g, CopyRec& rec)
{
  const uint32_t id = ps.id;
  // density: the knob value, plus the painted density layer (offset, may be negative)
  {
    float dens = float(g.density);
    if (ps.hasPaint && g.paintDensityEnable) dens += ps.w[kPaintLayerDensity];
    if (dens < 1.0f && rand01(id, g.seed, 11u) >= dens) return false;
  }

  rec.id = id;
  rec.srcObject = ps.srcObject;
  rec.srcPoint = ps.srcPoint;
  rec.hasColor = ps.hasColor;
  rec.color = ps.color;
  rec.hasPaint = ps.hasPaint;
  for (int l = 0; l < kPaintLayerCount; ++l) rec.w[l] = ps.w[l];

  // variant
  int variant = 0;
  if (g.nVar > 1) {
    switch (g.variantMode) {
      case kVariantRandom:    variant = int(rand01(id, g.vseed, 23u) * float(g.nVar)); break;
      case kVariantAttribute: variant = ps.hasVariantAttr ? ps.variantAttr : int(ps.order); break;
      default:                variant = int(ps.order); break;
    }
    variant %= g.nVar;
    if (variant < 0) variant += g.nVar;
    if (ps.hasPaint && g.paintVariantEnable) {
      // painted variant layer: offset from the picked variant (+1 = next prototype, -1 = previous)
      const float wv = ps.w[kPaintLayerVariant];
      variant += int(wv >= 0.0f ? wv + 0.5f : wv - 0.5f);
      variant = ((variant % g.nVar) + g.nVar) % g.nVar;
    }
  }
  rec.variant = variant;

  // painted colour (Paint tab, "colour" layer): coverage A blends towards the painted rgb
  if (ps.hasPaint && g.paintColorEnable) {
    const float a = std::min(1.0f, std::max(0.0f, ps.w[kPaintLayerColA]));
    if (a > 0.0f) {
      const Vector4 base = rec.hasColor ? rec.color : Vector4(1, 1, 1, 1);
      Vector4 pc(ps.w[kPaintLayerColR], ps.w[kPaintLayerColG], ps.w[kPaintLayerColB], base.w);
      if (g.paintColorMode == kPaintColorMultiply) { pc.x *= base.x; pc.y *= base.y; pc.z *= base.z; }
      rec.color = Vector4(lerpf(base.x, pc.x, a), lerpf(base.y, pc.y, a), lerpf(base.z, pc.z, a), base.w);
      rec.hasColor = true;
    }
  }

  // colour variance (works from white when there is no source colour)
  if (g.colorVarHue > 0.0 || g.colorVarSat > 0.0 || g.colorVarVal > 0.0) {
    Vector4 c = rec.hasColor ? rec.color : Vector4(1, 1, 1, 1);
    float h, sv, v;
    rgbToHsv(c.x, c.y, c.z, h, sv, v);
    h += (rand01(id, g.seed, 83u) - 0.5f) * float(g.colorVarHue);
    sv = std::min(1.0f, std::max(0.0f, sv + (rand01(id, g.seed, 89u) - 0.5f) * 2.0f * float(g.colorVarSat)));
    v  = std::max(0.0f, v * (1.0f + (rand01(id, g.seed, 97u) - 0.5f) * 2.0f * float(g.colorVarVal)));
    if (sv <= 0.0f && g.colorVarHue > 0.0) sv = std::min(1.0f, float(g.colorVarHue));   // white input: hue needs some saturation
    hsvToRgb(h, sv, v, c.x, c.y, c.z);
    rec.color = c;
    rec.hasColor = true;
  }

  // rotation
  Matrix4 R;
  R.makeIdentity();
  switch (g.alignMode) {
    case kAlignDirection:  if (ps.hasDir)  R = lookAlong(ps.dir, g.upVec) * g.axisFix; break;
    case kAlignQuaternion:
    case kAlignParticle:   if (ps.hasQuat) R = quaternionToMatrix(ps.quat.x, ps.quat.y, ps.quat.z, ps.quat.w); break;
    case kAlignEuler:      if (ps.hasEuler) R = eulerXYZ(ps.euler.x, ps.euler.y, ps.euler.z); break;
    default: break;
  }
  if (g.spinMode == kSpinRoll && ps.rollOK) {
    Vector3 axis = g.upVec.cross(ps.rollVel);
    const float alen = axis.length();
    if (alen > 1e-8f) {
      axis = axis * (1.0f / alen);
      Matrix4 roll;
      roll.rotation(float(g.rollRate) * ps.rollDist * kDegToRad, axis);
      R = roll * R;
    }
  }
  if (g.randomRotate) {
    const float rx = lerpf(float(g.rotMin[0]), float(g.rotMax[0]), rand01(id, g.seed, 31u));
    const float ry = lerpf(float(g.rotMin[1]), float(g.rotMax[1]), rand01(id, g.seed, 37u));
    const float rz = lerpf(float(g.rotMin[2]), float(g.rotMax[2]), rand01(id, g.seed, 41u));
    R = R * eulerXYZ(rx, ry, rz);
  }
  if (g.rotVariance > 0.0) {
    const float a = float(g.rotVariance);
    R = R * eulerXYZ((rand01(id, g.seed, 71u) - 0.5f) * 2.0f * a,
                     (rand01(id, g.seed, 73u) - 0.5f) * 2.0f * a,
                     (rand01(id, g.seed, 79u) - 0.5f) * 2.0f * a);
  }
  if (g.hasUserRot) R = R * g.userRot;
  if (ps.hasPaint && g.paintRotEnable && ps.w[kPaintLayerRotate] > 0.0f) {
    const float ang = ps.w[kPaintLayerRotate] * float(g.paintRotAmount) * kDegToRad;
    Matrix4 pr;
    if (g.paintRotAxis == kPaintAxisX) pr.rotationX(ang);
    else if (g.paintRotAxis == kPaintAxisZ) pr.rotationZ(ang);
    else pr.rotationY(ang);
    R = R * pr;
  }

  // scale
  Vector3 sc(float(g.scale * g.scaleXYZ[0]), float(g.scale * g.scaleXYZ[1]), float(g.scale * g.scaleXYZ[2]));
  if (ps.hasSize) sc = sc * ps.size;
  if (ps.hasScaleVec) { sc.x *= ps.scaleVec.x; sc.y *= ps.scaleVec.y; sc.z *= ps.scaleVec.z; }
  // The random itself is untouched - only where it LANDS in min..max is shaped -
  // so bias and shape do not reshuffle which copy gets which size.
  if (g.randomScale) {
    const float t = shapeRandom(rand01(id, g.seed, 43u), g.scaleBias, g.scaleShape);
    sc = sc * lerpf(float(g.scaleMin), float(g.scaleMax), t);
  }
  if (ps.hasPaint && g.paintScaleEnable) sc = sc * std::max(0.0f, 1.0f + ps.w[kPaintLayerScale] * float(g.paintScaleAmount));

  // local offset: user offset + per-copy random range + per-copy variance jitter
  Vector3 off = g.hasOffset ? g.offset : Vector3(0, 0, 0);
  bool anyOffset = g.hasOffset;
  if (g.randomOffset) {
    off.x += lerpf(float(g.offMin[0]), float(g.offMax[0]), rand01(id, g.seed, 101u));
    off.y += lerpf(float(g.offMin[1]), float(g.offMax[1]), rand01(id, g.seed, 103u));
    off.z += lerpf(float(g.offMin[2]), float(g.offMax[2]), rand01(id, g.seed, 107u));
    anyOffset = true;
  }
  if (g.offVariance[0] != 0.0 || g.offVariance[1] != 0.0 || g.offVariance[2] != 0.0) {
    off.x += (rand01(id, g.seed, 109u) - 0.5f) * 2.0f * float(g.offVariance[0]);
    off.y += (rand01(id, g.seed, 113u) - 0.5f) * 2.0f * float(g.offVariance[1]);
    off.z += (rand01(id, g.seed, 127u) - 0.5f) * 2.0f * float(g.offVariance[2]);
    anyOffset = true;
  }

  // assemble: T(Pworld) * R * S * T(offset)
  Matrix4 M;
  M.translation(ps.Pw);
  M *= R;
  M.scale(sc.x, sc.y, sc.z);
  if (anyOffset) M.translate(off);
  rec.xform = M;
  return true;
}

// ---- scattering -----------------------------------------------------------
struct ScatterPoint { Vector3 P; Vector3 N; unsigned i0, i1, i2; float b0, b1, b2; unsigned srcObject; };
enum { kScatterStickOff = 0, kScatterStickRef = 1, kScatterStickTopology = 2 };
const char* const kScatterStickNames[] = {
  "recompute every frame (current shape)", "stick to the surface (reference frame shape)",
  "stick to the surface (topology only, uniform per face)", nullptr };
enum { kScatterPaintRemove = 0, kScatterPaintAddRemove = 1 };
const char* const kScatterPaintModeNames[] = { "remove only (count stays fixed)", "add and remove (count follows the paint)", nullptr };
struct ScatterParams {
  int weighting; double bias; bool usePaint; int seed; int count; double separation; Vector3 up;
  // how the painted 'scatter' layer acts: remove only = candidates where the
  // layer is negative are rejected (density x (1 + w), never above 1, the
  // count stays 'count'); add and remove = the count follows the paint
  // (expected count = count x mean(1 + w) over the surface): +1 doubles the
  // density where painted, -1 empties it, and painting into an area the
  // weighting left empty adds points there
  int paintMode;
  // add and remove: points a paint weight of +1 over the WHOLE surface adds
  // (scaled by the painted area and weight); expected total =
  //   count x mean(min(1, 1 + w))  +  paintCount x mean(max(0, w))
  int paintCount;
  // Deforming geometry: the sample selection (triangle, barycentrics, weighting,
  // separation) is done on refPts when given (same size / order as the current
  // points), the final positions on the current points - so the scattered
  // points ride the surface instead of being reshuffled every frame.
  // uniformFaces: ignore triangle areas (every face equally likely) - stable
  // under deformation without any reference shape.
  const std::vector<Vector3>* refPts; bool uniformFaces;
  ScatterParams() : weighting(0), bias(2.0), usePaint(true), seed(0), count(0), separation(0.0), up(0, 1, 0), paintMode(0), paintCount(1000), refPts(nullptr), uniformFaces(false) {}
};

// Scatter copy targets across the faces of a triangulated mesh (area weighted,
// optionally weighted by terrain feature, painted layer and a separation radius).
inline void scatterOnMesh(const ScatterParams& sp_, const std::vector<Vector3>& _curPts,
                          const std::vector<unsigned>& _meshTriIdx, const std::vector<unsigned>& _meshTriObj,
                          const PaintLayers& paintCopy, std::vector<ScatterPoint>& _scatter, std::string& _scatterInfo)
{
  // selection geometry: the reference shape when it matches, else the current one
  const bool useRef = sp_.refPts && sp_.refPts->size() == _curPts.size() && !_curPts.empty();
  const std::vector<Vector3>& _paintPts = useRef ? *sp_.refPts : _curPts;
  const bool uniformFaces = sp_.uniformFaces;
  const int    _scatterWeighting = sp_.weighting;
  const double _scatterBias = sp_.bias;
  const bool   _scatterUsePaint = sp_.usePaint;
  const int    _scatterSeed = sp_.seed;
  const int    _scatterCount = sp_.count;
  const double _scatterSeparation = sp_.separation;
  const double _up[3] = { sp_.up.x, sp_.up.y, sp_.up.z };
  _scatter.clear();
  _scatterInfo.clear();
  const bool paintCanAdd = sp_.usePaint && sp_.paintMode == kScatterPaintAddRemove && sp_.paintCount > 0 &&
                           paintCopy.npoints > 0 && paintCopy.layerHasData(kPaintLayerScatter);
  if (_scatterCount > 0 || paintCanAdd) {
  const size_t ntri = _meshTriIdx.size() / 3;
  if (ntri > 0) {
    Vector3 up = Vector3(static_cast<float>(_up[0]), static_cast<float>(_up[1]), static_cast<float>(_up[2]));
    if (up.lengthSquared() < 1e-12f) up = Vector3(0, 1, 0);
    up.normalize();
    // height range along up (for peaks / valleys)
    float hMin = 1e30f, hMax = -1e30f;
    if (_scatterWeighting == kScatterWPeaks || _scatterWeighting == kScatterWValleys) {
      for (size_t i = 0; i < _paintPts.size(); ++i) { const float h = _paintPts[i].dot(up); if (h < hMin) hMin = h; if (h > hMax) hMax = h; }
    }
    const float hRange = (hMax > hMin) ? (hMax - hMin) : 1.0f;
    const bool usePaintLayer = _scatterUsePaint && paintCopy.npoints > 0 && paintCopy.layerHasData(kPaintLayerScatter);
    const bool paintAdds = usePaintLayer && sp_.paintMode == kScatterPaintAddRemove;
    // largest positive paint weight (add mode: acceptance is (1 + w) / (1 + wmax))
    float wPaintMax = 0.0f;
    if (paintAdds) for (size_t i = 0; i < paintCopy.npoints; ++i) wPaintMax = std::max(wPaintMax, paintCopy.get(kPaintLayerScatter, i));
    auto triPaint = [&](size_t t) -> float {   // mean painted scatter weight of a triangle
      return (paintCopy.get(kPaintLayerScatter, _meshTriIdx[t * 3]) + paintCopy.get(kPaintLayerScatter, _meshTriIdx[t * 3 + 1]) +
              paintCopy.get(kPaintLayerScatter, _meshTriIdx[t * 3 + 2])) * (1.0f / 3.0f);
    };

    std::vector<double> cdf(ntri);
    double acc = 0.0;
    for (size_t t = 0; t < ntri; ++t) {
      const Vector3& a = _paintPts[_meshTriIdx[t * 3]];
      const Vector3& b = _paintPts[_meshTriIdx[t * 3 + 1]];
      const Vector3& c = _paintPts[_meshTriIdx[t * 3 + 2]];
      const Vector3 nrm = (b - a).cross(c - a);
      double w = uniformFaces ? (nrm.lengthSquared() > 0.0f ? 1.0 : 0.0) : 0.5 * double(nrm.length());
      if (_scatterWeighting != kScatterWUniform && w > 0.0) {
        float feature = 1.0f;
        const float nl = nrm.length();
        const float slope = (nl > 1e-12f) ? 1.0f - std::fabs(nrm.dot(up) / nl) : 0.0f;   // 0 flat .. 1 vertical
        switch (_scatterWeighting) {
          case kScatterWFlat:    feature = 1.0f - slope; break;
          case kScatterWSteep:   feature = slope; break;
          case kScatterWPeaks:   feature = (((a + b + c) * (1.0f / 3.0f)).dot(up) - hMin) / hRange; break;
          case kScatterWValleys: feature = 1.0f - (((a + b + c) * (1.0f / 3.0f)).dot(up) - hMin) / hRange; break;
          default: break;
        }
        feature = std::min(1.0f, std::max(0.0f, feature));
        w *= std::pow(double(feature), _scatterBias);
        // add mode: a positive paint lets points into faces the weighting left empty
        if (paintAdds && w <= 0.0) {
          const float pw = triPaint(t);
          if (pw > 0.0f) w = 0.5 * double(nrm.length()) * double(pw);
        }
      }
      acc += w;
      cdf[t] = acc;
    }
    if (acc <= 0.0 && _scatterWeighting != kScatterWUniform) {
      // degenerate feature (e.g. a flat card with 'prefer steep'): fall back to plain area weighting
      acc = 0.0;
      for (size_t t = 0; t < ntri; ++t) {
        const Vector3& a = _paintPts[_meshTriIdx[t * 3]];
        const Vector3& b = _paintPts[_meshTriIdx[t * 3 + 1]];
        const Vector3& c = _paintPts[_meshTriIdx[t * 3 + 2]];
        acc += 0.5 * double((b - a).cross(c - a).length());
        cdf[t] = acc;
      }
    }
    if (acc > 0.0) {
      const uint32_t sseed = uint32_t(_scatterSeed) * 7919u + 17u;
      const unsigned n = unsigned(_scatterCount);
      const float sep = float(_scatterSeparation);
      const bool useSep = sep > 0.0f;
      // spatial hash for the separation test
      struct GridKey { long long k; };
      std::vector<std::vector<unsigned> > grid;
      std::vector<long long> gridKeys;
      const float cell = useSep ? sep : 1.0f;
      auto cellOf = [&](const Vector3& q, int& gx, int& gy, int& gz) {
        gx = int(std::floor(q.x / cell)); gy = int(std::floor(q.y / cell)); gz = int(std::floor(q.z / cell));
      };
      auto keyOf = [](int gx, int gy, int gz) -> long long {
        return (static_cast<long long>(gx) & 0x1FFFFF) | ((static_cast<long long>(gy) & 0x1FFFFF) << 21) | ((static_cast<long long>(gz) & 0x1FFFFF) << 42);
      };
      // simple open hash: vector of buckets indexed by hash of key
      const size_t nb = std::max<size_t>(1024, size_t(n) * 2);
      std::vector<std::vector<std::pair<long long, unsigned> > > buckets(nb);
      auto bucketIndex = [&](long long key) -> size_t { return size_t((unsigned long long)key * 0x9E3779B97F4A7C15ULL >> 20) % nb; };
      auto tooClose = [&](const Vector3& q) -> bool {
        int gx, gy, gz; cellOf(q, gx, gy, gz);
        for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) for (int dz = -1; dz <= 1; ++dz) {
          const long long key = keyOf(gx + dx, gy + dy, gz + dz);
          const std::vector<std::pair<long long, unsigned> >& bk = buckets[bucketIndex(key)];
          for (size_t e = 0; e < bk.size(); ++e) {
            if (bk[e].first != key) continue;
            if ((_scatter[bk[e].second].P - q).lengthSquared() < sep * sep) return true;
          }
        }
        return false;
      };
      auto insertPt = [&](const Vector3& q, unsigned idx) {
        int gx, gy, gz; cellOf(q, gx, gy, gz);
        const long long key = keyOf(gx, gy, gz);
        buckets[bucketIndex(key)].push_back(std::make_pair(key, idx));
      };

      _scatter.reserve(n);
      // add mode: M = n + paintCount x wmax candidates, each accepted with
      // (n x min(1, 1 + w) + paintCount x max(0, w)) / M  -> expected count =
      // n x mean(min(1, 1 + w)) + paintCount x mean(max(0, w)); other modes: draw until n are accepted
      const double paintN = double(std::max(0, sp_.paintCount));
      const double addM = double(n) + paintN * double(wPaintMax);
      const unsigned wantN = paintAdds ? 0xFFFFFFFFu : n;
      const unsigned maxAttempts = paintAdds ? unsigned(std::min(1e8, std::ceil(addM * (useSep ? 30.0 : 1.0))))
                                             : n * ((useSep || usePaintLayer) ? 30u : 1u);
      const unsigned addBudget = paintAdds ? unsigned(std::min(1e8, std::ceil(addM))) : 0u;
      unsigned candidates = 0;
      unsigned rejectedSep = 0, rejectedPaint = 0;
      for (unsigned i = 0; i < maxAttempts && _scatter.size() < wantN; ++i) {
        if (paintAdds && candidates >= addBudget) break;
        const double r = double(rand01(i, sseed, 101u)) * acc;
        size_t t = size_t(std::lower_bound(cdf.begin(), cdf.end(), r) - cdf.begin());
        if (t >= ntri) t = ntri - 1;
        float u = rand01(i, sseed, 103u), v = rand01(i, sseed, 107u);
        if (u + v > 1.0f) { u = 1.0f - u; v = 1.0f - v; }
        const float w0 = 1.0f - u - v;
        ScatterPoint sp;
        sp.i0 = _meshTriIdx[t * 3]; sp.i1 = _meshTriIdx[t * 3 + 1]; sp.i2 = _meshTriIdx[t * 3 + 2];
        sp.b0 = w0; sp.b1 = u; sp.b2 = v;
        if (usePaintLayer) {
          const float pw = paintCopy.get(kPaintLayerScatter, sp.i0) * w0 + paintCopy.get(kPaintLayerScatter, sp.i1) * u +
                           paintCopy.get(kPaintLayerScatter, sp.i2) * v;
          if (paintAdds) {
            ++candidates;
            const double num = double(n) * double(std::min(1.0f, std::max(0.0f, 1.0f + pw))) + paintN * double(std::max(0.0f, pw));
            const float p = (addM > 0.0) ? float(num / addM) : 0.0f;
            if (rand01(i, sseed, 109u) >= p) { ++rejectedPaint; continue; }
          }
          else if (rand01(i, sseed, 109u) >= std::min(1.0f, 1.0f + pw)) { ++rejectedPaint; continue; }
        }
        const Vector3& a = _paintPts[sp.i0]; const Vector3& b = _paintPts[sp.i1]; const Vector3& c = _paintPts[sp.i2];
        sp.P = a * w0 + b * u + c * v;
        if (useSep && tooClose(sp.P)) { ++rejectedSep; continue; }
        Vector3 nrm = (b - a).cross(c - a);
        const float nl = nrm.length();
        sp.N = (nl > 1e-12f) ? nrm * (1.0f / nl) : Vector3(0, 1, 0);
        sp.srcObject = _meshTriObj[t];
        if (useSep) insertPt(sp.P, unsigned(_scatter.size()));
        if (useRef) {
          // final position / normal on the current (deformed) shape
          const Vector3& ca = _curPts[sp.i0]; const Vector3& cb = _curPts[sp.i1]; const Vector3& cc = _curPts[sp.i2];
          sp.P = ca * w0 + cb * u + cc * v;
          Vector3 cn = (cb - ca).cross(cc - ca);
          const float cl = cn.length();
          sp.N = (cl > 1e-12f) ? cn * (1.0f / cl) : sp.N;
        }
        _scatter.push_back(sp);
      }
      std::ostringstream so;
      so << "scatter: " << _scatter.size() << " of " << n << " points on " << ntri << " triangle(s)";
      if (useSep) so << ", separation " << sep << " (" << rejectedSep << " rejected)";
      if (usePaintLayer) so << ", painted scatter layer (" << rejectedPaint << " rejected" << (paintAdds ? ", add/remove mode" : "") << ")";
      if (_scatterWeighting != kScatterWUniform) so << ", weighting: " << kScatterWeightNames[_scatterWeighting] << " bias " << _scatterBias;
      if (useRef) so << ", stuck to the reference shape";
      if (uniformFaces) so << ", uniform per face";
      if (!_scatter.empty()) {
        double mh = 0.0;
        for (size_t i = 0; i < _scatter.size(); ++i) mh += double(_scatter[i].P.dot(up));
        so << ", mean height " << (mh / double(_scatter.size()));
      }
      _scatterInfo = so.str();
    }
  }
  else {
    _scatterInfo = "scatter: the points input has no faces to scatter on";
  }
}
}

// ---- paint mesh queries (used by the viewer brush) -----------------------------
struct PaintHit { bool valid; Vector3 pos; Vector3 normal; float t; };

inline PaintHit paintIntersect(const std::vector<Vector3>& tris, const std::vector<Vector3>& pts, float fallbackRadius,
                               const Vector3& origin, const Vector3& dir)
{
  PaintHit h; h.valid = false; h.t = 0.0f; h.pos = origin; h.normal = Vector3(0, 1, 0);
  float best = 1e30f;
  const size_t ntri = tris.size() / 3;
  for (size_t i = 0; i < ntri; ++i) {
    float t;
    const Vector3& a = tris[i * 3]; const Vector3& b = tris[i * 3 + 1]; const Vector3& c = tris[i * 3 + 2];
    if (rayTriangle(origin, dir, a, b, c, t) && t < best) {
      best = t;
      h.valid = true;
      h.pos = origin + dir * t;
      Vector3 n = (b - a).cross(c - a);
      if (n.dot(dir) > 0.0f) n = n * -1.0f;
      h.normal = n;
      h.t = t;
    }
  }
  if (!h.valid && !pts.empty()) {
    // point cloud fallback: nearest point to the ray within the brush radius
    const float r = fallbackRadius;
    float bestT = 1e30f;
    for (size_t i = 0; i < pts.size(); ++i) {
      const Vector3 d = pts[i] - origin;
      const float t = d.dot(dir);
      if (t <= 0.0f) continue;
      const Vector3 perp = d - dir * t;
      if (perp.lengthSquared() <= r * r && t < bestT) { bestT = t; h.valid = true; h.pos = pts[i]; h.normal = dir * -1.0f; h.t = t; }
    }
  }
  if (h.valid) h.normal.normalize();
  return h;
}

// Occlusion mask for a dab: 1 = the point is visible from 'eye' (no triangle
// between the eye and the point), 0 = hidden (back side, behind other faces).
// Only points inside the brush radius are tested; the rest stay 0.
inline void paintVisibleMask(const std::vector<Vector3>& tris, const std::vector<Vector3>& pts, const Vector3& center,
                             float radius, const Vector3& eye, std::vector<uint8_t>& mask)
{
  const size_t n = pts.size();
  mask.assign(n, 0);
  const float r2 = radius * radius;
  const size_t ntri = tris.size() / 3;
  for (size_t i = 0; i < n; ++i) {
    if ((pts[i] - center).lengthSquared() > r2) continue;
    Vector3 d = pts[i] - eye;
    const float dist = d.length();
    if (dist < 1e-6f) { mask[i] = 1; continue; }
    d = d * (1.0f / dist);
    const float limit = dist * (1.0f - 1e-3f);
    bool hidden = false;
    for (size_t t = 0; t < ntri && !hidden; ++t) {
      float th;
      if (rayTriangle(eye, d, tris[t * 3], tris[t * 3 + 1], tris[t * 3 + 2], th) && th > 1e-4f * dist && th < limit) hidden = true;
    }
    mask[i] = hidden ? 0 : 1;
  }
}

inline unsigned paintDab(PaintLayers& layers, const std::vector<Vector3>& pts, const Vector3& center, float radius, float hardness,
                         float opacity, float value, int layer, int mode, const std::vector<uint8_t>* mask = nullptr)
{
  if (radius <= 0.0f || layer < 0 || layer >= kPaintLayerCount) return 0;
  const size_t n = std::min(pts.size(), size_t(layers.npoints));
  const float r2 = radius * radius;
  const float hard = std::min(std::max(hardness, 0.0f), 0.999f);
  unsigned touched = 0;
  float smoothSum = 0.0f; unsigned smoothCount = 0;
  if (mode == kPaintModeSmooth) {
    for (size_t i = 0; i < n; ++i) {
      if (mask && i < mask->size() && !(*mask)[i]) continue;
      if ((pts[i] - center).lengthSquared() <= r2) { smoothSum += layers.get(layer, i); ++smoothCount; }
    }
  }
  const float smoothAvg = smoothCount ? smoothSum / float(smoothCount) : 0.0f;
  for (size_t i = 0; i < n; ++i) {
    if (mask && i < mask->size() && !(*mask)[i]) continue;
    const float d2 = (pts[i] - center).lengthSquared();
    if (d2 > r2) continue;
    const float d = std::sqrt(d2) / radius;                 // 0..1
    float fall = 1.0f;
    if (d > hard) { const float t = (d - hard) / (1.0f - hard); fall = 1.0f - t * t * (3.0f - 2.0f * t); }
    const float w = fall * opacity * value;
    const float cur = layers.get(layer, i);
    float nv = cur;
    switch (mode) {
      case kPaintModeAdd:      nv = cur + w; break;
      case kPaintModeSubtract: nv = cur - w; break;
      case kPaintModeSet:      nv = cur + (value - cur) * fall * opacity; break;
      case kPaintModeSmooth:   nv = cur + (smoothAvg - cur) * fall * 0.5f; break;
      default: break;
    }
    layers.set(layer, i, nv);
    ++touched;
  }
  return touched;
}

// Colour brush: add/set move rgb towards the brush colour and coverage A
// towards 1; subtract erases (A towards 0); smooth averages all four.
inline unsigned paintDabColor(PaintLayers& layers, const std::vector<Vector3>& pts, const Vector3& center, float radius,
                              float hardness, float opacity, float r, float g, float b, int mode,
                              const std::vector<uint8_t>* mask = nullptr)
{
  if (mode == kPaintModeSmooth) {
    unsigned t = 0;
    for (int c = 0; c < 4; ++c) t = paintDab(layers, pts, center, radius, hardness, opacity, 0.0f, kPaintLayerColR + c, kPaintModeSmooth, mask);
    return t;
  }
  if (mode == kPaintModeSubtract)
    return paintDab(layers, pts, center, radius, hardness, opacity, 0.0f, kPaintLayerColA, kPaintModeSet, mask);
  paintDab(layers, pts, center, radius, hardness, opacity, r, kPaintLayerColR, kPaintModeSet, mask);
  paintDab(layers, pts, center, radius, hardness, opacity, g, kPaintLayerColG, kPaintModeSet, mask);
  paintDab(layers, pts, center, radius, hardness, opacity, b, kPaintLayerColB, kPaintModeSet, mask);
  return paintDab(layers, pts, center, radius, hardness, opacity, 1.0f, kPaintLayerColA, kPaintModeSet, mask);
}

} // namespace ctp
