// MultiplyCf.cpp
//
// Companion material for CopyToPoints (Nuke 14.1, classic 3D system).
//
// ScanlineRender's default shading path samples the material texture and
// ignores the geometry colour attribute "Cf" (only Nuke's internal solid
// shader uses it).  This Material multiplies whatever material/texture is
// connected to its input by the interpolated Cf of the geometry, so that the
// per-particle colour CopyToPoints writes onto every copy actually reaches the
// render.  Put it between the texture and the prototype geometry
// (Constant/CheckerBoard -> MultiplyCf -> Sphere -> CopyToPoints geo input),
// or apply it downstream with ApplyMaterial.
//
// Sources are strict ASCII.

#include "DDImage/Material.h"
#include "DDImage/VertexContext.h"
#include "DDImage/GeoInfo.h"
#include "DDImage/Attribute.h"
#include "DDImage/Pixel.h"
#include "DDImage/Knobs.h"
#include "DDImage/Channel.h"

using namespace DD::Image;

namespace {

const char* const kClass = "MultiplyCf";
const char* const kHelp =
  "@b;MultiplyCf@n; multiplies the incoming material or texture by the geometry's "
  "@b;Cf@n; colour attribute (point, vertex or object colour).\n\n"
  "Nuke's ScanlineRender ignores Cf when a texture is used as the material; this "
  "node makes it visible again. Use it on the prototype geometry feeding "
  "@b;CopyToPoints@n; so every copy is tinted by its particle colour.";

}

class MultiplyCf : public Material
{
  bool  _multiplyAlpha;
  bool  _perspFix;
  float _mix;

public:
  static const Description description;
  const char* Class() const override { return kClass; }
  const char* node_help() const override { return kHelp; }

  explicit MultiplyCf(Node* node)
    : Material(node)
    , _multiplyAlpha(false)
    , _perspFix(true)
    , _mix(1.0f)
  {
  }

  void knobs(Knob_Callback f) override
  {
    Material::knobs(f);
    Float_knob(f, &_mix, IRange(0.0, 1.0), "mix", "mix");
    Tooltip(f, "0 = ignore Cf (plain material), 1 = full multiplication by Cf.");
    Bool_knob(f, &_multiplyAlpha, "multiply_alpha", "multiply alpha");
    Tooltip(f, "Also multiply the alpha channel by Cf's alpha.");
    Bool_knob(f, &_perspFix, "perspective_fix", "perspective fix");
    Tooltip(f, "Undo the renderer's homogeneous W division on the colour channels (keep on; "
               "turn off only if colours look too bright).");
  }

  void _validate(bool for_real) override
  {
    Material::_validate(for_real);
  }

  // Ask the renderer to interpolate the colour channels across the face.
  void vertex_shader(VertexContext& vtx) override
  {
    input0().vertex_shader(vtx);
    vtx.vP.channels += Mask_RGBA;
  }

  void fragment_shader(const VertexContext& vtx, Pixel& out) override
  {
    input0().fragment_shader(vtx, out);
    if (_mix <= 0.0f) return;
    // Geometry without a Cf attribute: the renderer hands us its default
    // vertex colour (0.18 grey) - leave such objects untouched instead of
    // darkening them.
    if (const GeoInfo* gi = vtx.geoinfo()) {
      const AttribContext* ac = gi->get_attribcontext("Cf");
      if (!ac || !ac->attribute) return;
    }
    // The renderer divides every interpolated channel by W at the vertex
    // stage (perspective-correct interpolation) and only restores its own
    // standard channels at the fragment stage.  P().w holds 1/W here, so
    // undo the division for the colour channels ourselves.
    Vector4 cf = vtx.Cf();
    const float invW = vtx.w();
    if (_perspFix && invW > 0.0f) cf = cf * (1.0f / invW);
    const float t = _mix;
    const float mr = 1.0f + (cf.x - 1.0f) * t;
    const float mg = 1.0f + (cf.y - 1.0f) * t;
    const float mb = 1.0f + (cf.z - 1.0f) * t;
    if (out.channels.contains(Chan_Red))   out[Chan_Red]   *= mr;
    if (out.channels.contains(Chan_Green)) out[Chan_Green] *= mg;
    if (out.channels.contains(Chan_Blue))  out[Chan_Blue]  *= mb;
    if (_multiplyAlpha && out.channels.contains(Chan_Alpha)) {
      const float ma = 1.0f + (cf.w - 1.0f) * t;
      out[Chan_Alpha] *= ma;
    }
  }
};

static Op* build(Node* node) { return new MultiplyCf(node); }
const Op::Description MultiplyCf::description(kClass, "3D/Shader/MultiplyCf", build);
