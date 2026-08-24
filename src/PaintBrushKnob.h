// PaintBrushKnob.h
//
// The 3D-viewer paint brush shared by CopyToPoints (classic, Nuke 14) and
// CopyToPointsUSD (Nuke 15+): a hidden custom knob that owns the painted
// weight/colour layers (serialised into the script) and drives the brush.
// Interaction follows the AttributePainter plugin: the mouse and buttons are
// polled while the viewer redraws (Windows), so Nuke's own navigation is
// never intercepted; the GL overlay draws the heat map and the brush ring.
//
// The owning op implements PaintHost.  Include this header AFTER every
// DDImage header (windows.h is pulled in here).  Strict ASCII.

#pragma once

#include "DDImage/Knob.h"
#include "DDImage/Knobs.h"
#include "DDImage/Op.h"
#include "DDImage/ViewerContext.h"
#include "DDImage/Matrix4.h"
#include "DDImage/Vector3.h"
#include "DDImage/Vector4.h"
#include "CopyCore.h"

#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// Platform layer: the brush polls the mouse position (GL window coordinates,
// origin bottom-left), the mouse buttons and the modifier keys while the
// viewer redraws.  Windows: Win32.  Linux: X11 (XQueryPointer on the current
// GLX drawable).  macOS: CoreGraphics button/modifier state + Nuke's own mouse
// coordinates from the ViewerContext.  Everything else: painting disabled.
#if defined(_WIN32)
#  include <windows.h>
#  ifdef POINTS
#    undef POINTS
#  endif
#  include <GL/gl.h>
#  define CTP_HAVE_VIEWER_PAINT 1
#  define CTP_PAINT_WIN32 1
#elif defined(__APPLE__)
#  include <OpenGL/gl.h>
#  include <ApplicationServices/ApplicationServices.h>
#  define CTP_HAVE_VIEWER_PAINT 1
#  define CTP_PAINT_MAC 1
#elif defined(__linux__)
#  include <GL/gl.h>
#  include <GL/glx.h>
#  include <X11/Xlib.h>
#  ifdef Bool
#    undef Bool
#  endif
#  ifdef None
#    undef None
#  endif
#  ifdef Status
#    undef Status
#  endif
#  define CTP_HAVE_VIEWER_PAINT 1
#  define CTP_PAINT_X11 1
#else
#  define CTP_HAVE_VIEWER_PAINT 0
#endif

namespace ctp {

// mouse buttons + modifiers, polled once per viewer tick
struct PaintInputState { bool lmb, mmb, rmb, shift, alt, ctrl; PaintInputState() : lmb(false), mmb(false), rmb(false), shift(false), alt(false), ctrl(false) {} };

#if CTP_HAVE_VIEWER_PAINT
inline PaintInputState paintPollInput()
{
  PaintInputState s;
#if defined(CTP_PAINT_WIN32)
  s.lmb   = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
  s.mmb   = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
  s.rmb   = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
  s.shift = (GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0;
  s.alt   = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
  s.ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
#elif defined(CTP_PAINT_MAC)
  s.lmb   = CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, kCGMouseButtonLeft);
  s.mmb   = CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, kCGMouseButtonCenter);
  s.rmb   = CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, kCGMouseButtonRight);
  const CGEventFlags fl = CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState);
  s.shift = (fl & kCGEventFlagMaskShift) != 0;
  s.alt   = (fl & kCGEventFlagMaskAlternate) != 0;
  s.ctrl  = (fl & (kCGEventFlagMaskControl | kCGEventFlagMaskCommand)) != 0;
#elif defined(CTP_PAINT_X11)
  Display* d = glXGetCurrentDisplay();
  GLXDrawable w = glXGetCurrentDrawable();
  if (d && w) {
    Window root, child; int rx, ry, wx, wy; unsigned mask = 0;
    if (XQueryPointer(d, (Window)w, &root, &child, &rx, &ry, &wx, &wy, &mask)) {
      s.lmb = (mask & Button1Mask) != 0; s.mmb = (mask & Button2Mask) != 0; s.rmb = (mask & Button3Mask) != 0;
      s.shift = (mask & ShiftMask) != 0; s.ctrl = (mask & ControlMask) != 0; s.alt = (mask & Mod1Mask) != 0;
    }
  }
#endif
  return s;
}
#endif

using namespace DD::Image;

// Brush knob values, read by the brush every viewer tick
struct PaintBrushSettings {
  bool   enable;
  int    layer, mode;
  double radius, hardness, opacity, value;
  float  color[3];
  bool   show; double heatMax; double pointSize;
  bool   live;
  bool   occlusion;     // only paint points visible from the camera
  PaintBrushSettings()
    : enable(false), layer(0), mode(0), radius(1.0), hardness(0.5), opacity(0.5), value(1.0),
      show(true), heatMax(1.0), pointSize(5.0), live(true), occlusion(true)
  { color[0] = 1.0f; color[1] = 0.25f; color[2] = 0.1f; }
};

// What the brush needs from the owning op
class PaintHost {
public:
  virtual ~PaintHost() {}
  virtual PaintBrushSettings paintSettings() const = 0;
  virtual void setPaintRadius(double r) = 0;                       // Shift+drag resize
  virtual std::mutex& paintMutex() = 0;
  virtual const std::vector<Vector3>& paintPointsNoLock() const = 0;   // world-space source points (paint index order)
  virtual const std::vector<Vector3>& paintTrisNoLock() const = 0;     // 3 world-space entries per triangle
  virtual void setPaintData(const PaintLayers& layers, unsigned version) = 0;
  virtual const char* paintUndoName() const { return "paint"; }
  //! true: the knob's build_handle() answers true while painting is enabled so
  //! Op::build_knob_handles() registers it (GeomOp / Hydra viewer);
  //! false: the op calls add_draw_handle() itself (classic GeoOp).
  virtual bool paintKnobBuildsHandle() const { return false; }
};

// ==========================================================================
// PaintBrushKnob: a hidden custom knob that owns the painted weight layers
// (serialised into the script), and drives the 3D-viewer brush.  Interaction
// follows the AttributePainter plugin: the mouse and buttons are polled while
// the viewer redraws, so Nuke's own navigation is never intercepted.
// ==========================================================================
class PaintBrushKnob : public Knob
{
public:
  PaintBrushKnob(Knob_Closure* kc, PaintHost* host, const char* name)
    : Knob(kc, name)
    , _host(host)
    , _version(1)
    , _matsCached(false), _prevMvValid(false)
    , _mouseX(0.0f), _mouseY(0.0f)
    , _hitValid(false), _hitT(0.0f)
    , _painting(false), _resizing(false)
    , _resizeStartX(0.0f), _resizeStartRadius(1.0f)
    , _tick(0)
  {
    for (int i = 0; i < 16; ++i) { _mv[i] = _proj[i] = _prevMv[i] = 0.0; }
    _vp[0] = _vp[1] = _vp[2] = _vp[3] = 0;
  }

  const char* Class() const override { return "CopyToPointsPaintKnob"; }
  bool not_default() const override { return _layers.hasData(); }

  // script format (single token, no spaces):
  //   v3:<npoints>:<nlayers>:<base64 of signed 16-bit rle per layer, joined by '.'>
  //   (v2 = unsigned 16-bit 0..16 from 1.x, v1 = 8-bit 0..1: still read; the
  //   layers now mean offsets from the knob values, so old data shifts meaning)
  void to_script(std::ostream& o, const OutputContext*, bool quote) const override
  {
    std::string t = "v3:" + std::to_string(_layers.npoints) + ":" + std::to_string(int(kPaintLayerCount)) + ":";
    for (int l = 0; l < kPaintLayerCount; ++l) {
      if (l) t += ".";
      t += b64encode(rleEncode16(_layers.data[l]));
    }
    if (quote) o << "{" << t << "}"; else o << t;
  }

  bool from_script(const char* v) override
  {
    PaintLayers nl;
    std::string txt = v ? v : "";
    // strip braces / whitespace
    std::string clean;
    for (size_t i = 0; i < txt.size(); ++i) {
      const char c = txt[i];
      if (c == '{' || c == '}' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"') continue;
      clean.push_back(c);
    }
    const bool isV1 = clean.size() > 3 && clean.compare(0, 3, "v1:") == 0;
    const bool isV2 = clean.size() > 3 && (clean.compare(0, 3, "v2:") == 0 || clean.compare(0, 3, "v3:") == 0);
    if (isV1 || isV2) {
      size_t p1 = clean.find(':', 3);
      size_t p2 = (p1 == std::string::npos) ? p1 : clean.find(':', p1 + 1);
      if (p1 != std::string::npos && p2 != std::string::npos) {
        const unsigned n = unsigned(std::atoi(clean.substr(3, p1 - 3).c_str()));
        const int nl_count = std::atoi(clean.substr(p1 + 1, p2 - p1 - 1).c_str());
        nl.resize(n);
        std::string rest = clean.substr(p2 + 1);
        int l = 0;
        size_t start = 0;
        while (l < nl_count && l < kPaintLayerCount) {
          size_t dot = rest.find('.', start);
          const std::string part = rest.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
          std::vector<uint8_t> raw;
          std::vector<int16_t> dec;
          if (b64decode(part, raw)) {
            if (isV1) rleDecode8(raw, dec); else rleDecode16(raw, dec);
            dec.resize(n, 0);
            nl.data[l] = dec;
          }
          if (dot == std::string::npos) break;
          start = dot + 1;
          ++l;
        }
      }
    }
    _layers = nl;
    ++_version;
    changed();
    return true;
  }

  // Nuke calls this to push the knob value into the op (p == the op)
  void store(StoreType, void* p, Hash& hash, const OutputContext&) override
  {
    (void)p;   // the host was given to the constructor
    if (_host) _host->setPaintData(_layers, _version);
    hash.append(_version);
    hash.append(_layers.npoints);
  }

  bool build_handle(ViewerContext* ctx) override
  {
    if (!_host || !_host->paintKnobBuildsHandle()) return false;   // the op registers us explicitly
    return _host->paintSettings().enable && ctx && ctx->viewer_mode() != VIEWER_2D;
  }
  void draw_handle(ViewerContext* ctx) override;

  void clearLayers(int layer)
  {
    new_undo("clear paint");
    if (layer < 0) _layers.clearAll(); else _layers.clearLayer(layer);
    ++_version;
    changed();
  }
  void fillLayer(unsigned npoints, int layer, float value, float r, float g, float b)
  {
    new_undo("flood fill");
    if (_layers.npoints != npoints) _layers.resize(npoints);
    _layers.fillLayer(layer, value, r, g, b);
    ++_version;
    changed();
  }

private:
  bool unproject(double wx, double wy, double wz, Vector3& out) const;
  void cacheGLMatrices();
  void updateMouse(ViewerContext* ctx);
  void updateHit();
  void drawOverlay();

  PaintHost*    _host;
  PaintLayers   _layers;
  std::vector<uint8_t> _mask;   // occlusion mask of the current dab
  unsigned      _version;

  double _mv[16], _proj[16], _prevMv[16];
  int    _vp[4];
  bool   _matsCached, _prevMvValid;
  float  _mouseX, _mouseY;          // GL window coordinates (origin bottom-left)
  bool   _hitValid;
  Vector3 _hitPos, _hitNormal;
  Vector3 _rayOrigin;          // eye position of the last hit test (occlusion)
  float  _hitT;
  bool   _painting, _resizing;
  float  _resizeStartX, _resizeStartRadius;
  Vector3 _resizeLockPos;
  int    _tick;
};

void PaintBrushKnob::cacheGLMatrices()
{
#if CTP_HAVE_VIEWER_PAINT
  if (_matsCached) { std::memcpy(_prevMv, _mv, sizeof(_prevMv)); _prevMvValid = true; }
  glGetDoublev(GL_MODELVIEW_MATRIX, _mv);
  glGetDoublev(GL_PROJECTION_MATRIX, _proj);
  glGetIntegerv(GL_VIEWPORT, _vp);
  _matsCached = true;
#endif
}

// gluUnProject without glu: invert (proj * modelview)
bool PaintBrushKnob::unproject(double wx, double wy, double wz, Vector3& out) const
{
  if (!_matsCached || _vp[2] <= 0 || _vp[3] <= 0) return false;
  Matrix4 mv, pr;
  for (int i = 0; i < 16; ++i) { (&mv.a00)[i] = float(_mv[i]); (&pr.a00)[i] = float(_proj[i]); }
  const Matrix4 pm = pr * mv;
  const Matrix4 inv = pm.inverse();
  const float nx = float((wx - _vp[0]) / _vp[2] * 2.0 - 1.0);
  const float ny = float((wy - _vp[1]) / _vp[3] * 2.0 - 1.0);
  const float nz = float(wz * 2.0 - 1.0);
  const Vector4 r = inv * Vector4(nx, ny, nz, 1.0f);
  if (std::fabs(r.w) < 1e-12f) return false;
  out = Vector3(r.x / r.w, r.y / r.w, r.z / r.w);
  return true;
}

void PaintBrushKnob::updateMouse(ViewerContext* ctx)
{
#if defined(CTP_PAINT_WIN32)
  (void)ctx;
  HDC hdc = wglGetCurrentDC();
  HWND hwnd = WindowFromDC(hdc);
  if (!hwnd) return;
  POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
  RECT rc; GetClientRect(hwnd, &rc);
  _mouseX = float(pt.x);
  _mouseY = float(rc.bottom - rc.top - 1 - pt.y);
#elif defined(CTP_PAINT_X11)
  (void)ctx;
  Display* d = glXGetCurrentDisplay();
  GLXDrawable w = glXGetCurrentDrawable();
  if (!d || !w) return;
  Window root, child; int rx, ry, wx, wy; unsigned mask = 0;
  if (!XQueryPointer(d, (Window)w, &root, &child, &rx, &ry, &wx, &wy, &mask)) return;
  Window r2; int gx, gy; unsigned gw, gh, gb, gd;
  if (!XGetGeometry(d, (Drawable)w, &r2, &gx, &gy, &gw, &gh, &gb, &gd)) return;
  _mouseX = float(wx);
  _mouseY = float(int(gh) - 1 - wy);
#elif defined(CTP_PAINT_MAC)
  // Nuke's viewer context carries the last mouse position in viewer coordinates
  if (ctx) { _mouseX = float(ctx->mouse_x()); _mouseY = float(ctx->mouse_y()); }
#else
  (void)ctx;
#endif
}

void PaintBrushKnob::updateHit()
{
  _hitValid = false;
  if (!_host || !_matsCached) return;
  Vector3 nearP, farP;
  if (!unproject(_mouseX, _mouseY, 0.0, nearP)) return;
  if (!unproject(_mouseX, _mouseY, 1.0, farP)) return;
  Vector3 dir = farP - nearP;
  const float len = dir.length();
  if (len < 1e-8f) return;
  dir = dir * (1.0f / len);
  _rayOrigin = nearP;
  PaintHit h;
  {
    const PaintBrushSettings st = _host->paintSettings();
    std::lock_guard<std::mutex> lock(_host->paintMutex());
    h = ctp::paintIntersect(_host->paintTrisNoLock(), _host->paintPointsNoLock(), float(st.radius), nearP, dir);
  }
  if (h.valid) { _hitValid = true; _hitPos = h.pos; _hitNormal = h.normal; _hitT = h.t; }
}

void PaintBrushKnob::draw_handle(ViewerContext* ctx)
{
#if CTP_HAVE_VIEWER_PAINT
  if (!_host || !ctx) return;
  const ViewerEvent ev = ctx->event();
  const PaintBrushSettings st = _host->paintSettings();
  if (ev == DRAW_OPAQUE) {
    if (!st.enable) { _painting = false; return; }
    if ((_tick % 60) == 0) ctpLog("brush:draw", "tick " + std::to_string(_tick));
    cacheGLMatrices();
    updateMouse(ctx);
    if (!_resizing) updateHit();
    ++_tick;

    const PaintInputState in = paintPollInput();
    const bool lmb = in.lmb, mmb = in.mmb, rmb = in.rmb, shift = in.shift, alt = in.alt, ctrl = in.ctrl;
    bool navigating = alt || mmb || rmb || ctrl;
    if (!navigating && lmb && _prevMvValid) {
      for (int i = 0; i < 16; ++i) if (std::fabs(_mv[i] - _prevMv[i]) > 1e-10) { navigating = true; break; }
      if (navigating) ctpLog("brush:camera moved -> navigating");
    }
    // only paint when the mouse is inside the GL window
    const bool inside = _mouseX >= 0.0f && _mouseY >= 0.0f && _mouseX < float(_vp[0] + _vp[2]) && _mouseY < float(_vp[1] + _vp[3]);
    const bool canPaint = lmb && !navigating && inside;

    if (navigating && _painting) { _painting = false; ++_version; ctpLog("brush:stroke cancelled (navigating)"); changed(); }
    if ((_tick % 60) == 0) ctpLog("brush:state", std::string("lmb=") + (lmb ? "1" : "0") + " inside=" + (inside ? "1" : "0") + " hit=" + (_hitValid ? "1" : "0") + " nav=" + (navigating ? "1" : "0"));

    // Shift + LMB drag: resize the brush
    if (shift && canPaint && !_painting) {
      if (!_resizing) {
        _resizing = true;
        _resizeStartX = _mouseX;
        _resizeStartRadius = float(st.radius);
        _resizeLockPos = _hitPos;
      }
      float scale = 1.0f;
      if (_vp[2] > 0) scale = 4.0f / float(_vp[2]);   // full window width = x4
      const float nr = std::max(0.01f, _resizeStartRadius * (1.0f + (_mouseX - _resizeStartX) * scale));
      _host->setPaintRadius(nr);
    }
    else if (_resizing && (!lmb || navigating)) {
      _resizing = false;
    }

    // paint
    if (!shift && !_resizing && canPaint && _hitValid) {
      if (!_painting) { _painting = true; ctpLog("brush:stroke start"); new_undo(_host->paintUndoName()); }
      {
        std::lock_guard<std::mutex> lock(_host->paintMutex());
        const std::vector<Vector3>& pts = _host->paintPointsNoLock();
        if (_layers.npoints != unsigned(pts.size())) _layers.resize(unsigned(pts.size()));
        // occlusion: only points visible from the camera (nothing between the eye and the point)
        const std::vector<uint8_t>* mask = nullptr;
        if (st.occlusion && !_host->paintTrisNoLock().empty()) {
          ctp::paintVisibleMask(_host->paintTrisNoLock(), pts, _hitPos, float(st.radius), _rayOrigin, _mask);
          mask = &_mask;
        }
        if (st.layer == kPaintLayerColor)
          ctp::paintDabColor(_layers, pts, _hitPos, float(st.radius), float(st.hardness), float(st.opacity),
                             st.color[0], st.color[1], st.color[2], st.mode, mask);
        else
          ctp::paintDab(_layers, pts, _hitPos, float(st.radius), float(st.hardness), float(st.opacity),
                        float(st.value), st.layer, st.mode, mask);
      }
      if (st.live && (_tick % 6) == 0) { ++_version; ctpLog("brush:changed (live)"); changed(); }
    }
    if (!lmb && _painting) { _painting = false; ++_version; ctpLog("brush:stroke end"); changed(); }

    redraw();   // keep polling while the brush is active
    return;
  }
  if (ev == DRAW_OVERLAY || ev == DRAW_LINES) {
    if (!st.enable) return;
    if (ev == DRAW_LINES) drawOverlay();
    return;
  }
#else
  (void)ctx;
#endif
}

void PaintBrushKnob::drawOverlay()
{
#if CTP_HAVE_VIEWER_PAINT
  glPushAttrib(GL_ALL_ATTRIB_BITS);
  glDisable(GL_LIGHTING);
  glDisable(GL_TEXTURE_2D);
  const PaintBrushSettings st = _host->paintSettings();
  // weights
  if (st.show) {
    std::lock_guard<std::mutex> lock(_host->paintMutex());
    const std::vector<Vector3>& pts = _host->paintPointsNoLock();
    const int layer = st.layer;
    float hmax = float(st.heatMax);
    if (hmax <= 0.0f) hmax = _layers.layerMax(layer);
    if (hmax <= 0.0f) hmax = 1.0f;
    glPointSize(float(st.pointSize));
    glBegin(GL_POINTS);
    for (size_t i = 0; i < pts.size(); ++i) {
      float r, g, b;
      if (layer == kPaintLayerColor) {
        const float a = std::min(1.0f, std::max(0.0f, _layers.get(kPaintLayerColA, i)));
        r = 0.25f + (_layers.get(kPaintLayerColR, i) - 0.25f) * a;
        g = 0.25f + (_layers.get(kPaintLayerColG, i) - 0.25f) * a;
        b = 0.3f + (_layers.get(kPaintLayerColB, i) - 0.3f) * a;
      }
      else {
        const float w = _layers.get(layer, i);
        if (w == 0.0f) { r = 0.25f; g = 0.25f; b = 0.3f; }
        else heatColor(w / hmax, r, g, b);
      }
      glColor3f(r, g, b);
      glVertex3f(pts[i].x, pts[i].y, pts[i].z);
    }
    glEnd();
  }
  // brush circle
  const bool showBrush = _hitValid || _resizing;
  if (showBrush) {
    const Vector3 c = _resizing ? _resizeLockPos : _hitPos;
    Vector3 n = _hitNormal;
    if (n.lengthSquared() < 1e-12f) n = Vector3(0.0f, 1.0f, 0.0f);
    n.normalize();
    Vector3 u = (std::fabs(n.y) < 0.9f) ? Vector3(0.0f, 1.0f, 0.0f).cross(n) : Vector3(1.0f, 0.0f, 0.0f).cross(n);
    u.normalize();
    const Vector3 v = n.cross(u);
    const float r = float(st.radius);
    const float rInner = r * float(st.hardness);
    glLineWidth(2.0f);
    glDisable(GL_DEPTH_TEST);
    for (int ring = 0; ring < 2; ++ring) {
      const float rr = ring ? rInner : r;
      if (rr <= 0.0f) continue;
      if (ring) glColor4f(1.0f, 0.9f, 0.2f, 0.6f); else glColor4f(1.0f, 0.6f, 0.1f, 1.0f);
      glBegin(GL_LINE_LOOP);
      for (int i = 0; i < 48; ++i) {
        const float a = float(i) * 6.28318530718f / 48.0f;
        const Vector3 q = c + u * (std::cos(a) * rr) + v * (std::sin(a) * rr);
        glVertex3f(q.x, q.y, q.z);
      }
      glEnd();
    }
    glPointSize(6.0f);
    glBegin(GL_POINTS);
    glColor3f(1.0f, 0.6f, 0.1f);
    glVertex3f(c.x, c.y, c.z);
    glEnd();
  }
  glPopAttrib();
#endif
}


} // namespace ctp
