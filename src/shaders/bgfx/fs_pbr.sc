// license:GPLv3+

// Physically based material shader (Forward+):
// - base color / alpha from tex_base_color * cBase_Alpha * staticColor_Alpha
// - tangent space normal mapping through tex_base_normalmap
// - packed ORM (occlusion/roughness/metallic) from tex_pbr_orm multiplied by the material factors
// - emission from tex_pbr_emission * pbrEmission
// - indirect diffuse lighting from the prefiltered irradiance envmap (tex_diffuse_env)
// - indirect specular from the envmap mip chain (tex_env) weighted by the split-sum approximation
//   (DFG LUT + multiscattering compensation + Lagarde's specular occlusion)
// - planar reflection probes blended per pixel by distance to their reflection plane (tex_pbr_probe0/1)
// - specular sideband output for the resolve pass (attachment 1: L_probe+W_probe, attachment 2: W+mask;
//   the march's roughness & PBR flag come from the depth prepass)
// - TRANSPARENT variant outputs premultiplied radiance (ONE/INVSRC_ALPHA blend state)
// - optional clear coat layer (pbrCoat): a smooth dielectric shell (IOR 1.5) over the base, following Filament's model: a
//   second GGX lobe on the geometric normal, the base attenuated by (1 - coat * Fc) and its F0 seen from inside the coat.
//   With a coat, the specular sideband carries the coat lobe (resolved with SSR/probes at the coat roughness) and the
//   base indirect specular is shaded in place. A zero coat strength skips it all (uniform branches).
//
// Defines: TEX (base color texture), AT (alpha test), TRANSPARENT (premultiplied blend output),
//          INSHADERSPEC (in-shader envmap specular, used in probe contexts and before the resolve pass),
//          SIDEBAND (write specular sideband attachments), CLIP, STEREO

$input v_worldPos, v_tablePos, v_normal, v_texcoord0, v_refrCurv
#ifdef STEREO
    $input v_eye
#endif
#ifdef CLIP
    $input v_clipDistance
#endif

#include "common.sh"

#define NUM_BALL_LIGHTS 0 // just to avoid having too much constant mem allocated

#ifdef TEX
SAMPLER2D      (tex_base_color, 0); // base color
#endif
SAMPLER2D      (tex_env, 1); // envmap
SAMPLER2D      (tex_diffuse_env, 2); // envmap radiance
SAMPLER2D      (tex_base_normalmap, 4); // normal map
SAMPLER2D      (tex_pbr_orm, 8); // packed occlusion/roughness/metallic map
SAMPLER2D      (tex_pbr_emission, 9); // emission map
SAMPLER2DSTEREO(tex_pbr_ao, 10); // screenspace ambient occlusion
SAMPLER2D      (tex_pbr_dfg, 11); // GGX split-sum DFG LUT
SAMPLER2D      (tex_pbr_lights, 12); // packed light records + cluster grid + index list (RGBA32F, texel fetch)
SAMPLER2DSTEREO(tex_pbr_probe0, 13); // first planar reflection probe (screen space)
SAMPLER2DSTEREO(tex_pbr_probe1, 14); // second planar reflection probe (screen space)
SAMPLER2DSTEREO(tex_pbr_refr, 15);   // screen space refraction probe (transmitted light of transparent parts)
#ifdef TRANSPARENT
SAMPLER2DSTEREO(tex_probe_depth, 7); // its depth: a refracted sample nearer than the part (a wire in front) is not behind it
#endif

#ifdef STEREO
    uniform mat4 matView[2];
    uniform mat4 matWorldView[2];
    uniform mat4 matWorldViewInverseTranspose[2];
    uniform mat4 matRotViewProj[2];
    uniform vec4 cameraPosWorld[2];
    #define mView                      matView[int(round(v_eye))]
    #define mWorldView                 matWorldView[int(round(v_eye))]
    #define mWorldViewInverseTranspose matWorldViewInverseTranspose[int(round(v_eye))]
    #define mRotViewProj               matRotViewProj[int(round(v_eye))]
    #define cCameraPosWorld            cameraPosWorld[int(round(v_eye))].xyz
#else
    uniform mat4 matView;
    uniform mat4 matWorldView;
    uniform mat4 matWorldViewInverseTranspose;
    uniform mat4 matRotViewProj;
    uniform vec4 cameraPosWorld;
    #define mView                      matView
    #define mWorldView                 matWorldView
    #define mWorldViewInverseTranspose matWorldViewInverseTranspose
    #define mRotViewProj               matRotViewProj
    #define cCameraPosWorld            cameraPosWorld.xyz
#endif

uniform vec4 u_basic_shade_mode;
#define doNormalMapping     (u_basic_shade_mode.y != 0.0)
#define noMipMaps           (u_basic_shade_mode.w != 0.0)

uniform vec4 cBase_Alpha;
uniform vec4 staticColor_Alpha;
#ifdef AT
uniform vec4 alphaTestValue; // FIXME Actually float but extended to vec4 for BGFX
#endif

uniform vec4 w_h_height;

uniform vec4 cAmbient_LightRange;
uniform vec4 fenvEmissionScale_TexWidth; // vec2 extended to vec4 for BGFX

uniform vec4 fDisableLighting_top_below; // vec2 extended to vec4 for BGFX

uniform vec4 lightCenter_doShadow;

uniform vec4 pbrFactors;
#define pbrMetallicFactor  (pbrFactors.x)
#define pbrOcclusionFactor (pbrFactors.y)
#define pbrRoughnessFactor (pbrFactors.z)
#define pbrAOEnabled       (pbrFactors.w)

uniform vec4 pbrEmission; // rgb: emission color, w: emission intensity
uniform vec4 pbrMapFlags; // x: ORM map bound, y: emission map bound, z: thin surface transmission (material wrap lighting), w: own dome lamp radius
#define hasORMMap      (pbrMapFlags.x != 0.0)
#define hasEmissionMap (pbrMapFlags.y != 0.0)
#define pbrTransmission (pbrMapFlags.z)
#define pbrOwnLampRadius (pbrMapFlags.w) // a flasher dome body is not lit by its own dome lamp (its dome layer draws that)

uniform vec4 pbrCoat; // x: clear coat strength (0 = off), y: clear coat perceptual roughness
#define coatStrength  (pbrCoat.x)
#define pbrScatter    (pbrCoat.z) // material scatter 0..1: the see-through surface itself, lit (0 = off)
#define coatRoughness (pbrCoat.y)
#define pbrScatterEdge0 (pbrCoat.w) // look-through starts where (1-|N.V|)/|N.V| exceeds this (3.8 = ~78 deg)

uniform vec4 pbrLightGrid;  // xy: cluster grid origin (table pos), z: inverse cell size, w: cell count x
uniform vec4 pbrLightGrid2; // x: cell count y, y: light count, z: specular AA strength (VPX_SPEC_AA, 1 = default, 0 = off)

uniform vec4 pbrProbes0; // reflection plane of the first bound probe (dot(n,p) + w = 0)
uniform vec4 pbrProbes1; // reflection plane of the second bound probe
uniform vec4 pbrProbeParams; // x/y: influence ranges of probe0/1 (0 = unbound), z: SSR/probe blend factor, w: refraction probe bound
uniform vec4 pbrProbeLods;   // x/y: max mip level of probe0/1, z: max mip level of the refraction probe (roughness-dependent lod)
                             // w: env_reflection_scale (table, reflection-only env level, 1 = as the env)

SAMPLER2DARRAY(tex_vsm_pool, 3);     // VSM physical page pool: depth texture array, one 128^2 page per layer
SAMPLER2D     (tex_vsm_pagetables, 5); // VSM page table: physical layer+1 per virtual page (0 = not resident)
SAMPLER2DARRAY(tex_vsm_trans, 6);    // VSM page pool transmittance: translucent casters' absorbance (rgb) and nearness (a)

uniform mat4 matShadowVP[32];   // ortho virtual map view-projection per shadowed light
uniform vec4 pbrVSMParams[32];  // x: page-table texel base, y: pages across (face grid for cube), z: pages down, w: ortho depth range
uniform vec4 pbrVSMBox[32];     // x/y: world->virtual-uv scale (1/ortho box extents), z: ortho near depth (used CPU side)
uniform vec4 pbrShadowParams;   // x: 1/page size, y: slope-scaled depth bias, z/w: page render linear depth start + 1/range (SHADOW_TRANS)

#ifdef TRANSPARENT
uniform vec4 refractionTint_thickness;
uniform vec4 refractionParams; // x: curvature gain (0 = off), y: anisotropy 0..1 (0 = off), z: soft in-front fallback (VPX_REFR_SOFT, 1 = on),
                               // w: 1 = v_refrCurv carries the per-vertex curvature (else the per-triangle estimate is used)
uniform vec4 refractionParams2; // x: forming wave strength (refraction_wave, 0 = off), y: refraction noise strength, z: noise scale mm, w: wave flow 0..1
uniform vec4 pbrPresence; // x: scatter floor, y: env/ambient share, z: rim gain, w: dense edge albedo (material sliders; defaults = the old constants)
uniform vec4 pbrPresence2; // x: rim width (1 = as is, < 1 chokes the rim band thinner), y: bevel / light-guide brightening scale (1 = as is), z: scatter locality (0 = as is), w: scatter width (1 = as is)
uniform vec4 pbrPresence3; // x: absorption light tint (0 = as is), y: scatter saturation (1 = as is)
uniform vec4 pbrAbsorption; // rgb: material absorption colour (transmittance through one thickness), w: strength (0 = off)
#endif

#include "ball_shadows.sh"

// Near-light cube face lookup: the light->fragment direction selects one of the 6 consecutive face
// groups (major axis), uv comes from the face's axis projection. Faces match the rasterization lookAt
// in the renderer (+X,-X,+Y,-Y,+Z,-Z with the same up vectors, shared with VSMPagePool's CUBE_FACE_*).
// Returns (u, v, face, dir.F) where dir.F is the direction component along the face's forward axis
// (positive in front).
vec4 pbr_shadow_cube_lookup(const vec3 dir)
{
   const vec3 ad = abs(dir);
   float fw; vec2 fuv; float face;
   if (ad.x >= ad.y && ad.x >= ad.z)
   {
      if (dir.x > 0.0) { fw = dir.x; fuv = vec2(-dir.y, dir.z) / fw; face = 0.0; }
      else { fw = -dir.x; fuv = vec2(dir.y, dir.z) / fw; face = 1.0; }
   }
   else if (ad.y >= ad.z)
   {
      if (dir.y > 0.0) { fw = dir.y; fuv = vec2(dir.x, dir.z) / fw; face = 2.0; }
      else { fw = -dir.y; fuv = vec2(-dir.x, dir.z) / fw; face = 3.0; }
   }
   else
   {
      if (dir.z > 0.0) { fw = dir.z; fuv = vec2(dir.x, -dir.y) / fw; face = 4.0; }
      else { fw = -dir.z; fuv = vec2(-dir.x, -dir.y) / fw; face = 5.0; }
   }
   return vec4(fuv * vec2(0.5, -0.5) + 0.5, face, fw);
}

// Page-table fetch + page-local depth sample. idx is the linear virtual-page index into the
// page-table texture (ptBase + page), luv is the page-local uv (fract of the virtual-map scaled uv).
// Returns -1 when the page is not resident (no caster coverage -> lit by construction).
// The sampled value is the render target's depth encoding, converted back to the [0,1] clip z
// domain of the light view projections (OpenGL stores clip z * 0.5 + 0.5, see toClipZ in ssr_common.sh).
float pbr_vsm_fetch(const int idx, const vec2 luv)
{
   const float phys = texelFetch(tex_vsm_pagetables, ivec2(idx & 511, idx >> 9), 0).x;
   if (phys <= 0.0)
      return -1.0;
   const float d = texture2DArrayLod(tex_vsm_pool, vec3(luv, phys - 1.0), 0.0).x;
   #if TEX_V_IS_UP
      return d * 2.0 - 1.0;
   #else
      return d;
   #endif
}

// Coloured transmittance of the translucent casters (printed and clear plastics) between the light and a receiver: the
// page holds the strongest absorbance per channel and the nearness (1 - normalized linear depth) of the nearest
// translucent caster, so receivers behind it get the plastic's colour while those in front of it (or none) stay white
vec3 pbr_vsm_tint(const int idx, const vec2 luv, const float receiverNearness)
{
   if (idx < 0)
      return vec3_splat(1.0);
   const float phys = texelFetch(tex_vsm_pagetables, ivec2(idx & 511, idx >> 9), 0).x;
   if (phys <= 0.0)
      return vec3_splat(1.0);
   const vec4 t = texture2DArrayLod(tex_vsm_trans, vec3(luv, phys - 1.0), 0.0);
   return receiverNearness < t.a ? vec3_splat(1.0) - t.rgb : vec3_splat(1.0);
}

// Linear virtual-page index of a virtual-map uv, or -1 when outside the grid
int pbr_vsm_index(const float ptBase, const vec2 grid, const vec2 vuv)
{
   const vec2 pg = floor(vuv * grid);
   if (any(lessThan(pg, vec2_splat(0.0))) || any(greaterThanEqual(pg, grid)))
      return -1;
   return int(ptBase + pg.y * grid.x + pg.x);
}

// Ortho paged depth at a virtual-map uv (page-local resolution per tap: PCF taps may cross pages)
float pbr_vsm_depth(const float ptBase, const vec2 grid, const vec2 vuv)
{
   const int idx = pbr_vsm_index(ptBase, grid, vuv);
   if (idx < 0)
      return -1.0;
   // vuv is the virtual map's top-down v; the page texel v flips on bottom-left-origin render targets
   #if TEX_V_IS_UP
      return pbr_vsm_fetch(idx, vec2(fract(vuv.x * grid.x), 1.0 - fract(vuv.y * grid.y)));
   #else
      return pbr_vsm_fetch(idx, fract(vuv * grid));
   #endif
}

// Cube paged depth for a lookup (cl.xy = face uv in [0,1], cl.z = face index)
float pbr_vsm_cube_depth(const float ptBase, const float faceGrid, const vec4 cl)
{
   const int g = int(faceGrid + 0.5);
   const vec2 pg = floor(cl.xy * vec2_splat(faceGrid));
   const int idx = int(ptBase + 0.5) + int(cl.z + 0.5) * g * g + int(pg.y) * g + int(pg.x);
   #if TEX_V_IS_UP
      return pbr_vsm_fetch(idx, vec2(fract(cl.x * faceGrid), 1.0 - fract(cl.y * faceGrid)));
   #else
      return pbr_vsm_fetch(idx, fract(cl.xy * vec2_splat(faceGrid)));
   #endif
}

// 2D rotation of a shadow kernel tap by cs = (cos a, sin a): the per-pixel rotation applied to the Poisson disk
vec2 pbr_vsm_rot(const vec2 t, const vec2 cs)
{
   return vec2(t.x * cs.x - t.y * cs.y, t.x * cs.y + t.y * cs.x);
}

vec3 pbr_normal_map(const vec3 N, const vec3 pos, const vec2 uv)
{
   vec3 tn;
   if (noMipMaps)
      tn = texNoLod(tex_base_normalmap, uv).xyz * (255./127.) - (128./127.);
   else
      tn = texture2D(tex_base_normalmap, uv).xyz * (255./127.) - (128./127.);

   // PBR normal maps are always tangent space
   return normalize(mul(TBN_trafo(N, pos, uv), tn).xyz);
}

// Base layer F0 seen through a clear coat of IOR 1.5 instead of air (Filament's f0ClearCoatToSurface): a dielectric
// base (F0 0.04, IOR 1.5) loses its own reflection under the coat, which is what makes printed art read matte
vec3 pbr_f0_under_coat(const vec3 f0)
{
   const vec3 s = sqrt(f0);
   const vec3 t = (vec3_splat(1.0) - 5.0 * s) / (vec3_splat(5.0) - s);
   return t * t;
}

// Lambertian diffuse split-sum weight: the irradiance is preconvolved with /PI baked in
#ifdef STEREO
vec3 pbr_indirect_diffuse(const vec3 N, const vec3 albedo, const vec3 kd, const float ao, const float v_eye)
#else
vec3 pbr_indirect_diffuse(const vec3 N, const vec3 albedo, const vec3 kd, const float ao)
#endif
{
   // trafo back to world for lookup into world space envmap (mView is orthonormal, no need to normalize)
   const vec3 wN = mul(vec4(N, 0.0), mView).xyz;
   const vec3 env = texNoLod(tex_diffuse_env, ray_to_equirectangular_uv(wN)).xyz;
   return albedo * kd * (env * fenvEmissionScale_TexWidth.x + cAmbient_LightRange.xyz) * ao;
}

// Specular weight W from the split-sum approximation with Fdez-Aguera multiscattering compensation
// and Lagarde's specular occlusion. Also evaluates the envmap specular lobe in INSHADERSPEC mode.
#ifdef STEREO
vec3 pbr_specular_weight(const vec3 N, const vec3 V, const vec3 F0, const float roughness, const float ao, const float v_eye, out vec3 envSpec)
#else
vec3 pbr_specular_weight(const vec3 N, const vec3 V, const vec3 F0, const float roughness, const float ao, out vec3 envSpec)
#endif
{
   const float NdotV = saturate(dot(N, V));
   const vec2 dfg = texture2DLod(tex_pbr_dfg, vec2(NdotV, roughness), 0.0).xy;
   const vec3 Fss = F0 * dfg.x + dfg.y;
   // Fdez-Aguera multiscattering compensation
   const float Ess = dfg.x + dfg.y;
   const float Ems = 1.0 - Ess;
   const vec3 Favg = F0 + (vec3_splat(1.0) - F0) * (1.0 / 21.0);
   const vec3 Fms = Fss * Favg / (vec3_splat(1.0) - Ems * Favg);
   // Lagarde's specular occlusion
   const float SO = saturate(pow(NdotV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao);
   const vec3 R = mul(vec4(reflect(-V, N), 0.0), mView).xyz; // trafo back to world (orthonormal, no renormalize needed)
   const float lod = roughness * log2(fenvEmissionScale_TexWidth.y);
   envSpec = texture2DLod(tex_env, ray_to_equirectangular_uv(R), lod).xyz * (fenvEmissionScale_TexWidth.x * pbrProbeLods.w); // env_reflection_scale (table): the room in reflections only
   return (Fss + Fms) * SO;
}

// Automated planar probe specular: the bound probes are sampled in screen space and weighted per
// pixel by the fragment's distance to their reflection plane, faded by their influence range.
// Returns (blended probe radiance, aggregate coverage). No bound probe => coverage 0 => envmap fallback.
// alphaCoverage: the probe's alpha (cleared to 0, written 1 by the reflected parts) is the coverage, so the envmap shows
// where the probe reflects nothing (the room above the table); used by the clear coat lobe.
#ifdef STEREO
vec4 pbr_probe_specular(const vec3 wPos, const vec2 suv, const float roughness, const bool alphaCoverage, const float v_eye)
#else
vec4 pbr_probe_specular(const vec3 wPos, const vec2 suv, const float roughness, const bool alphaCoverage)
#endif
{
   // Rough surfaces sample the probe's mip chain: each mip is a Kaiser filtered downscale of the
   // reflected scene, giving temporally stable blurred reflections under camera/object motion
   vec3 Lp = vec3_splat(0.0);
   float wSum = 0.0;
   BRANCH if (pbrProbeParams.x > 0.0)
   {
      const float d = max(dot(pbrProbes0.xyz, wPos) + pbrProbes0.w, 0.0);
      // Parallax reprojection: the probe is only a flat mirror for fragments on its plane.
      // Mirror the fragment about the plane and reproject it in screen space to sample the
      // probe at the direction of the actual mirror ray (degenerates to suv at d = 0)
      const vec3 mirrored = wPos - 2.0 * d * pbrProbes0.xyz;
      const vec4 proj = mul(mRotViewProj, vec4(mirrored - cCameraPosWorld, 1.0));
      #if TEX_V_IS_UP
      const vec2 uv = vec2(0.5, 0.5) + proj.xy * (0.5 / proj.w);
      #else
      const vec2 uv = vec2(0.5, 0.5) + vec2(proj.x, -proj.y) * (0.5 / proj.w);
      #endif
      // Fade at the probe frustum border (and behind the mirrored camera): no valid content there
      const float inFrustum = saturate(min(uv.x, min(uv.y, min(1.0 - uv.x, 1.0 - uv.y))) * 16.0);
      const float w = saturate(1.0 - d / pbrProbeParams.x) * inFrustum;
      const vec4 t = texStereoLod(tex_pbr_probe0, uv, roughness * pbrProbeLods.x);
      Lp += w * t.rgb;
      wSum += alphaCoverage ? w * t.a : w;
   }
   BRANCH if (pbrProbeParams.y > 0.0)
   {
      const float d = max(dot(pbrProbes1.xyz, wPos) + pbrProbes1.w, 0.0);
      const vec3 mirrored = wPos - 2.0 * d * pbrProbes1.xyz;
      const vec4 proj = mul(mRotViewProj, vec4(mirrored - cCameraPosWorld, 1.0));
      #if TEX_V_IS_UP
      const vec2 uv = vec2(0.5, 0.5) + proj.xy * (0.5 / proj.w);
      #else
      const vec2 uv = vec2(0.5, 0.5) + vec2(proj.x, -proj.y) * (0.5 / proj.w);
      #endif
      const float inFrustum = saturate(min(uv.x, min(uv.y, min(1.0 - uv.x, 1.0 - uv.y))) * 16.0);
      const float w = saturate(1.0 - d / pbrProbeParams.y) * inFrustum;
      const vec4 t = texStereoLod(tex_pbr_probe1, uv, roughness * pbrProbeLods.y);
      Lp += w * t.rgb;
      wSum += alphaCoverage ? w * t.a : w;
   }
   return vec4(Lp / max(wSum, 1e-6), saturate(wSum));
}

#ifdef TRANSPARENT
// Transmitted light through a transparent part: refract the view ray by the part thickness, project
// back to screen space and sample the refraction probe (screen space copy of what's behind the part).
#ifdef STEREO
vec3 pbr_transmit(const vec3 wPos, const vec3 wN, const vec3 wI, const float roughness, const vec4 fragCoord, const float v_eye)
#else
vec3 pbr_transmit(const vec3 wPos, const vec3 wN, const vec3 wI, const float roughness, const vec4 fragCoord)
#endif
{
   // wI: the incident direction (-wV); the legacy refraction passes +wV (VPX_REFR_DEBUG 4, refraction bench)
   const vec3 R = refract(wI, wN, 1.0 / 1.5); // n1 = 1.0 (air), n2 = 1.5 (plastic), eta = n1 / n2
   const vec3 refracted_pos = wPos + refractionTint_thickness.w * R; // Shift ray by the thickness of the material
   const vec4 proj = mul(mRotViewProj, vec4(refracted_pos - cCameraPosWorld, 1.0));
   #if TEX_V_IS_UP
   const vec2 uvR = vec2(0.5, 0.5) + proj.xy * (0.5 / proj.w);
   #else
   const vec2 uvR = vec2(0.5, 0.5) + vec2(proj.x, -proj.y) * (0.5 / proj.w);
   #endif
   // A refracted sample that lands on something nearer than this fragment (a wire or rail in front of the ramp) is not
   // what lies behind it: fall back toward the unrefracted pixel (as the legacy refraction does), through the half offset
   // first so the switch does not leave a per-pixel staircase along the occluder
   const vec2 uv0 = fragCoord.xy * w_h_height.xy;
   const vec2 uvH = 0.5 * (uvR + uv0);
   vec2 uv = uvR;
   if (texStereo(tex_probe_depth, uvR).x < fragCoord.z)
      uv = texStereo(tex_probe_depth, uvH).x < fragCoord.z ? uv0 : uvH;
   return refractionTint_thickness.rgb * texStereoLod(tex_pbr_refr, uv, roughness * pbrProbeLods.z).rgb;
}

// The curvature and anisotropy effects grow with the distance from the part to what is seen behind it, from about zero (a
// decal a few mm under the floor shows the gloss and a little parallax only) to full at REFR_FULL_DEPTH (the playfield
// ~50 mm under a ramp): a curved or scuffed surface deflects by an angle, so the shift it causes is proportional to depth
#define REFR_FULL_DEPTH 90.0
// Anisotropic streak half length (VPU) at anisotropy 1 on a flat panel at full depth: a scuff-sized constant, so a
// wireform 50 to 100 VPU behind a ramp at anisotropy 0.5 stays readable, slightly streaked along the ramp
#define REFR_SMEAR_CAP 1.2
// Thin-sheet incidence term (with refraction_curvature): cap of the base offset multiplier at grazing incidence (a flat
// sheet shifts the view by at most about its thickness), and of the whole offset multiplier (sheet + curvature term)
#define REFR_SHEET_MAX 1.5
#define REFR_OFFSET_MAX 12.0
// Light guide (with refraction_curvature): a steep face (wall, a plastic's rim) seen toward edge-on is looked through
// along the sheet, the path through the plastic grows as thickness / cos(incidence): cap of that path in thicknesses,
// and how much brighter the band gets (light piped along the sheet, plus the Fresnel reflection rising at grazing)
#define REFR_GUIDE_MAX 12.0
#define REFR_GUIDE_GLOW 0.75
// A smidge of blur on curved regions (Ben 10-31, round 3: a quarter of the first cut's): blur radius x (1 + this x the
// curvature term). Flat panels stay sharp.
#define REFR_CURV_BLUR 0.125
// Forming waves (refraction_wave): normal tilt (radians) at strength 1 per unit noise slope, and the two wavelengths (VPU)
#define REFR_WAVE_TILT 0.06
#define REFR_WAVE_L1 22.0
#define REFR_WAVE_L2 9.0
// SURFACE PRESENCE (material scatter / absorption, round 4): the see-through surface itself, lit. Scatter floor (the whole
// part scatters this much of its scatter where it is neither edge-on nor bent), env/ambient share, rim gain
// (floor, env share, rim gain, dense albedo and the edge start are material sliders now: pbrPresence, pbrCoat.w)
// Dense edge: where a see-through part with scatter turns edge-on (|N.V| below PRES_DENSE_C1, fading in from PRES_DENSE_C0:
// the thin cut edge faces, a wall-top bevel at grazing) it stops being see-through and reads as dense shiny plastic of this
// albedo, lit by the local light (dark grey in a dark room, light when lit). Full density from scatter PRES_DENSE_FULL.
#define PRES_DENSE_C0 0.15
#define PRES_DENSE_C1 0.05
#define PRES_DENSE_FULL 0.25
// Scatter locality / width (round 5). Locality blends each lamp's scatter weight from the wrapped |N.L| with plain inverse-square
// toward an unwrapped |N.L| (two-sided: light inside the sheet scatters whichever face it enters) with an extra falloff
// R^2 / (R^2 + d^2) (inverse-square near a bulb, ~1/d^4 beyond R), distance floored at PRES_LOC_DMIN so a bulb touching
// the plastic does not spike. Width: the scatter/rim band weight is raised to e = (1/width) * (1 + (1/width - 1) * (1 - lit_n)),
// lit_n = lum(lit) / (lum(lit) + PRES_LIT_K): the band keeps the slider's width where well lit and chokes to (1/width)^2 in the dark.
// Width is geometric, at constant peak: the band core (weight >= PRES_BAND_CORE) keeps its weight, only the flanks below it
// are choked, w' = w * min(1, (w / core)^(e - 1)) (e = 1 at width 1: w' = w exactly). A plain pow(w, e) lowered the band's
// peak too wherever it stays below 1 (wall-top bevels, fillets), so width dimmed instead of narrowing (Ben 11-01). The
// scatter and rim gains stay the only intensity knobs. A band whose own peak is below the core still thins toward it.
// Width 0 = no band at all (scatter floor only, no rim): the core would otherwise keep a band at any exponent (round 7: the
// band then comes from a separate cut-face mesh/material).
// PRES_LIT_K = 1.0: lit is in the linear radiance of a white Lambert sheet, 1.0 = lit to display white before tonemapping.
#define PRES_LOC_R 60.0
#define PRES_LOC_DMIN 10.0
#define PRES_LIT_K 1.0
#define PRES_BAND_CORE 0.35

// Width choke at constant peak (see PRES_BAND_CORE): weight w in 0..1, exponent e >= 1
float pbr_band_choke(float w, float e)
{
   return e > 1.0 ? w * min(1.0, pow(w * (1.0 / PRES_BAND_CORE), e - 1.0)) : w;
}

// Smooth 3D value noise with its analytic gradient (Inigo Quilez's quintic value noise): x = value 0..1, yzw = gradient
float pbr_hash3(vec3 p)
{
   p = fract(p * 0.3183099 + vec3(0.1, 0.17, 0.13));
   p *= 17.0;
   return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}
vec4 pbr_noised(const vec3 x)
{
   const vec3 i = floor(x);
   const vec3 f = x - i;
   const vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
   const vec3 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
   const float a = pbr_hash3(i);
   const float b = pbr_hash3(i + vec3(1.0, 0.0, 0.0));
   const float c = pbr_hash3(i + vec3(0.0, 1.0, 0.0));
   const float d = pbr_hash3(i + vec3(1.0, 1.0, 0.0));
   const float e = pbr_hash3(i + vec3(0.0, 0.0, 1.0));
   const float g = pbr_hash3(i + vec3(1.0, 0.0, 1.0));
   const float h = pbr_hash3(i + vec3(0.0, 1.0, 1.0));
   const float k = pbr_hash3(i + vec3(1.0, 1.0, 1.0));
   const float k0 = a, k1 = b - a, k2 = c - a, k3 = e - a, k4 = a - b - c + d, k5 = a - c - e + h, k6 = a - b - e + g, k7 = -a + b + c - d + e - g - h + k;
   return vec4(k0 + k1 * u.x + k2 * u.y + k3 * u.z + k4 * u.x * u.y + k5 * u.y * u.z + k6 * u.z * u.x + k7 * u.x * u.y * u.z,
      du * vec3(k1 + k4 * u.y + k6 * u.z + k7 * u.y * u.z, k2 + k5 * u.z + k4 * u.x + k7 * u.z * u.x, k3 + k6 * u.x + k5 * u.y + k7 * u.x * u.y));
}

// Forming-wave noise gradient (two octaves), the field stretched along the fixed unit axis d by 1 / (1 - st) (st = 0: the
// plain field). d is a constant per call: the warp is linear in the position, so the field stays continuous over the mesh
// (a per-pixel axis times the absolute position, as the first wave_flow had it, jumps by axis change x |position| at every
// triangle edge where the axis is per triangle, and shears hard where it turns)
vec3 pbr_wave_grad(const vec3 p, const vec3 d, const float st)
{
   const vec3 pw = p - d * (dot(p, d) * st);
   const vec3 g = pbr_noised(pw * (1.0 / REFR_WAVE_L1)).yzw + 0.5 * pbr_noised(pw * (1.0 / REFR_WAVE_L2) + vec3(17.3, 5.1, 9.7)).yzw;
   return g - d * (dot(g, d) * st); // gradient back to table space (the stretch is symmetric)
}

// pbr_transmit with the refraction options of refractionParams (curvature, anisotropy, soft fallback). The plain version
// above stays as it was so a part without them renders bit for bit as before.
// sheet: multiplier of the base offset (1 = as pbr_transmit; the thin-sheet incidence and light-guide terms with
// refraction_curvature), graze: grazing incidence boost of the curvature term (1 = none),
// curvExtra: the curvature term (the offset grows by thickness x curvExtra, a third of it right behind the part and all
// of it from REFR_FULL_DEPTH: a bevel bends even what is close behind it; no blur, a clean formed bend is bright and bent,
// frost belongs to wear), smear: long axis of the anisotropic blur (world space, length = anisotropy x the
// bend and grazing boosts, 0 = none), waveN: tilt added to the normal by the forming waves (world space, 0 = none)
#ifdef STEREO
vec3 pbr_transmit_ex(const vec3 wPos, const vec3 wN, const vec3 wI, const float roughness, const vec4 fragCoord, const float sheet, const float graze, const float curvExtra, const vec3 smear, const vec3 waveN, const float v_eye)
#else
vec3 pbr_transmit_ex(const vec3 wPos, const vec3 wN, const vec3 wI, const float roughness, const vec4 fragCoord, const float sheet, const float graze, const float curvExtra, const vec3 smear, const vec3 waveN)
#endif
{
   const vec2 uv0 = fragCoord.xy * w_h_height.xy;
   const bool hasSmear = refractionParams.y > 0.0 && dot(smear, smear) > 0.0;
   const bool hasWave = dot(waveN, waveN) > 0.0;
   // Distance to what lies behind, from the probe depth: window depth = a + b / w (w the view depth), a and b fitted from
   // this fragment and a point 10 VPU farther along the view ray
   float az = 0.0, bz = 0.0, w1 = 1.0, depthF0 = 0.0, behind0 = 0.0;
   BRANCH if (curvExtra > 0.0 || hasSmear || hasWave)
   {
      const vec4 c1 = mul(mRotViewProj, vec4(wPos - cCameraPosWorld, 1.0));
      const vec4 c2 = mul(mRotViewProj, vec4(wPos + normalize(wPos - cCameraPosWorld) * 10.0 - cCameraPosWorld, 1.0));
      #if TEX_V_IS_UP
      const float z1 = c1.z / c1.w * 0.5 + 0.5;
      const float z2 = c2.z / c2.w * 0.5 + 0.5;
      #else
      const float z1 = c1.z / c1.w;
      const float z2 = c2.z / c2.w;
      #endif
      bz = (z1 - z2) / (1.0 / c1.w - 1.0 / c2.w);
      az = z1 - bz / c1.w;
      w1 = c1.w;
      // Depth ramp of the curvature term, from what lies straight behind (the offset is not known yet)
      behind0 = max(bz / min(texStereoLod(tex_probe_depth, uv0, 0.0).x - az, -1e-7) - w1, 0.0);
      depthF0 = 0.35 + 0.65 * saturate(behind0 * (1.0 / REFR_FULL_DEPTH));
   }
   // wI: the incident direction (-wV); the legacy refraction passes +wV (VPX_REFR_DEBUG 4, refraction bench)
   const vec3 R = refract(wI, wN, 1.0 / 1.5); // n1 = 1.0 (air), n2 = 1.5 (plastic), eta = n1 / n2
   const float curvF = curvExtra * depthF0 * graze;
   vec3 refracted_pos = wPos + (refractionTint_thickness.w * min(sheet + curvF, REFR_OFFSET_MAX)) * R; // Shift ray by the thickness of the material
   // Forming waves: the tilted normal turns the ray by a small angle, so the view behind shifts by that angle times the
   // distance to it (nothing for a decal right under the floor, a gentle undulation of the playfield)
   if (hasWave)
      refracted_pos += (refract(wI, normalize(wN + waveN), 1.0 / 1.5) - R) * min(behind0, REFR_FULL_DEPTH);
   const vec4 proj = mul(mRotViewProj, vec4(refracted_pos - cCameraPosWorld, 1.0));
   #if TEX_V_IS_UP
   const vec2 uvR = vec2(0.5, 0.5) + proj.xy * (0.5 / proj.w);
   #else
   const vec2 uvR = vec2(0.5, 0.5) + vec2(proj.x, -proj.y) * (0.5 / proj.w);
   #endif
   // A refracted sample that lands on something nearer than this fragment (a wire or rail in front of the ramp) is not
   // what lies behind it: fall back toward the unrefracted pixel (as the legacy refraction does), through the half offset
   // first so the switch does not leave a per-pixel staircase along the occluder
   vec2 uv = uvR;
   float kept = 1.0; // soft fallback: fraction of the offset kept, also the weight of the refracted sample against uv0
   BRANCH if (refractionParams.z > 0.0)
   {
      // Soft fallback: pull the offset back toward the unrefracted pixel only as far as needed. Step it down in thirds to the
      // first accepted sample, then bisect 4 times between it and the last rejected one: the kept offset ends where the offset
      // segment meets the occluder, so it shrinks continuously toward the occluder's edge. That sample sits on the occluder's
      // rim for every pixel of the band, so on its own it stretches the rim into streaks: it is faded into the unrefracted
      // pixel by the kept fraction, a gradient from the refracted view to the plain one at the occluder (no blocks, no streaks)
      BRANCH if (texStereoLod(tex_probe_depth, uvR, 0.0).x < fragCoord.z)
      {
         float tA = 0.0; // largest accepted fraction of the offset found so far
         float tR = 1.0; // smallest rejected one
         if (texStereoLod(tex_probe_depth, mix(uv0, uvR, 2.0 / 3.0), 0.0).x >= fragCoord.z)
            tA = 2.0 / 3.0;
         else
         {
            tR = 2.0 / 3.0;
            if (texStereoLod(tex_probe_depth, mix(uv0, uvR, 1.0 / 3.0), 0.0).x >= fragCoord.z)
               tA = 1.0 / 3.0;
            else
               tR = 1.0 / 3.0;
         }
         UNROLL for (int i = 0; i < 4; i++)
         {
            const float tm = 0.5 * (tA + tR);
            if (texStereoLod(tex_probe_depth, mix(uv0, uvR, tm), 0.0).x >= fragCoord.z)
               tA = tm;
            else
               tR = tm;
         }
         uv = mix(uv0, uvR, tA);
         kept = tA;
      }
   }
   else
   {
      const vec2 uvH = 0.5 * (uvR + uv0);
      if (texStereo(tex_probe_depth, uvR).x < fragCoord.z)
         uv = texStereo(tex_probe_depth, uvH).x < fragCoord.z ? uv0 : uvH;
   }
   // Blur radius (in probe texels, as a mip scale): the roughness blur, as pbr_transmit, and a smidge more on curved regions
   const float rRough = exp2(roughness * pbrProbeLods.z);
   const float frost = 1.0 + REFR_CURV_BLUR * curvF;
   const float lod = log2(rRough * frost);
   const vec3 plain = kept < 1.0 ? texStereoLod(tex_pbr_refr, uv0, lod).rgb : vec3_splat(0.0);
   BRANCH if (hasSmear)
   {
      // Anisotropic blur: taps along the surface's long axis. The streak is the forming waviness of the sheet deflecting by a
      // small angle: it grows with the distance to what is seen behind (about zero for things close under a flat panel) up
      // to the cap, longer in bends and much longer toward grazing incidence (the smear vector's length carries both
      // boosts). The roughness blur shrinks by the anisotropy as the streak takes over, so it does not add a haze on top.
      // The taps are tent weighted and blurred to their spacing so they merge into a streak instead of separate echoes; a
      // tap on something nearer than the part is left out.
      const float behindDist = max(bz / min(texStereoLod(tex_probe_depth, uv, 0.0).x - az, -1e-7) - w1, 0.0);
      const float depthF = saturate(behindDist * (1.0 / REFR_FULL_DEPTH));
      if (pbrLightGrid2.w == 8.0) // debug: distance to what lies behind, 0 (black) .. 100 VPU (white)
         return vec3_splat(min(behindDist, 100.0) * 0.01);
      const vec4 projS = mul(mRotViewProj, vec4(refracted_pos + smear * (REFR_SMEAR_CAP * depthF) - cCameraPosWorld, 1.0));
      #if TEX_V_IS_UP
      const vec2 duv = vec2(0.5, 0.5) + projS.xy * (0.5 / projS.w) - uvR;
      #else
      const vec2 duv = vec2(0.5, 0.5) + vec2(projS.x, -projS.y) * (0.5 / projS.w) - uvR;
      #endif
      const float halfPx = length(duv / w_h_height.xy);
      const float lodIso = log2(max(rRough * (1.0 - refractionParams.y * depthF) * frost, 1.0));
      if (halfPx < 0.5)
         return refractionTint_thickness.rgb * mix(plain, texStereoLod(tex_pbr_refr, uv, lodIso).rgb, kept);
      const int n = halfPx < 3.0 ? 4 : 8;
      const float lodTap = max(lodIso, log2(max(2.0 * halfPx / float(n), 1.0)));
      vec3 acc = vec3_splat(0.0);
      float wsum = 0.0;
      UNROLL for (int i = 0; i < 8; i++)
      {
         if (i < n)
         {
            const float si = (float(i) + 0.5) * (2.0 / float(n)) - 1.0;
            const vec2 uvi = uv + duv * si;
            const float w = texStereoLod(tex_probe_depth, uvi, 0.0).x < fragCoord.z ? 0.0 : 1.0 - 0.5 * abs(si);
            acc += w * texStereoLod(tex_pbr_refr, uvi, lodTap).rgb;
            wsum += w;
         }
      }
      return refractionTint_thickness.rgb * mix(plain, wsum > 0.0 ? acc * (1.0 / wsum) : texStereoLod(tex_pbr_refr, uv, lodIso).rgb, kept);
   }
   return refractionTint_thickness.rgb * mix(plain, texStereoLod(tex_pbr_refr, uv, lod).rgb, kept);
}

// Surface curvature from the screen derivatives of the interpolated vertex normal (the estimate the specular AA uses) over
// those of the position, so it is a property of the surface, the same at any zoom. The squared curvature along a screen
// direction d is (d.G.d) / (d.H.d), G and H the 2x2 metrics of the normal and position derivatives: its extremes are the
// eigenvalues of H^-1.G. Returns the max and min curvature (1/VPU) and, in zw, the screen direction (pixels, unnormalized)
// of the min curvature: the surface's long axis (along a swept ramp wall or fillet).
vec4 pbr_curvature(const vec3 Nv, const vec3 P)
{
   const vec3 dNx = dFdx(Nv);
   const vec3 dNy = dFdy(Nv);
   const vec3 dPx = dFdx(P);
   const vec3 dPy = dFdy(P);
   const float gxx = dot(dNx, dNx), gxy = dot(dNx, dNy), gyy = dot(dNy, dNy);
   const float hxx = dot(dPx, dPx), hxy = dot(dPx, dPy), hyy = dot(dPy, dPy);
   const float detH = hxx * hyy - hxy * hxy;
   // A pixel quad across a silhouette (or edge-on) has no usable surface metric
   const float invDet = detH > 1e-4 * hxx * hyy ? 1.0 / detH : 0.0;
   const float m00 = (hyy * gxx - hxy * gxy) * invDet;
   const float m01 = (hyy * gxy - hxy * gyy) * invDet;
   const float m10 = (hxx * gxy - hxy * gxx) * invDet;
   const float m11 = (hxx * gyy - hxy * gxy) * invDet;
   const float h = 0.5 * (m00 + m11);
   const float disc = sqrt(max(h * h - (m00 * m11 - m01 * m10), 0.0));
   const float lmin = max(h - disc, 0.0);
   const vec2 e1 = vec2(m01, lmin - m00);
   const vec2 e2 = vec2(lmin - m11, m10);
   return vec4(sqrt(max(h + disc, 0.0)), sqrt(lmin), dot(e1, e1) > dot(e2, e2) ? e1 : e2);
}
#endif

// Forward+ clustered direct lighting. The CPU builds a world-space XY grid over the table bounds each
// frame and packs it into tex_pbr_lights (RGBA32F, 256 texels wide, texel fetch):
// [0,512)    light records, 2 texels each: (pos.xyz, range) then (radiance.rgb, mode)
// [512,1024) grid cells: (start, count) into the index list
// [1024,+)   light index list: (light index, light source sphere radius, upward emitter flag or -dome cut elevation,
//            flasher dome cookie: flute count + groove root angle / 2pi, 0 = none)
// World-space evaluation (v_tablePos): view independent, a single grid serves both stereo eyes.
vec4 pbr_light_texel(const int idx)
{
   return texelFetch(tex_pbr_lights, ivec2(idx & 255, idx >> 8), 0);
}

// Geometric specular AA: the GGX roughness widened by the screen-space variance of the normal n (Tokuyoshi & Kaplanyan
// 2019: sigma^2 = 1/2pi, kappa = 0.18). Fragment derivatives: call from uniform control flow only.
float pbr_aa_roughness(const vec3 n, const float roughness)
{
   const vec3 dx = dFdx(n), dy = dFdy(n);
   const float a = roughness * roughness;
   return sqrt(sqrt(min(a * a + min(pbrLightGrid2.z * 0.31831 * (dot(dx, dx) + dot(dy, dy)), 0.18), 1.0)));
}

// Returns the diffuse (and transmitted) direct light; the specular reflection is accumulated apart in specOut, since a
// see-through part reflects it whatever its coverage (it is not modulated by alpha like the diffuse, as the indirect specular)
vec3 pbr_direct_light(const vec3 wN, const vec3 wNc, const vec3 wNg, const vec3 wV, const vec3 wPos, const vec3 diffuse, const vec3 F0, const float roughness, const float alpha, const float transmission, const vec2 vsmRot, out vec3 specOut)
{
   specOut = vec3_splat(0.0);
   const vec2 cellf = clamp((wPos.xy - pbrLightGrid.xy) * pbrLightGrid.z, vec2_splat(0.0),
      vec2(pbrLightGrid.w - 1.0, pbrLightGrid2.x - 1.0));
   const vec4 cell = pbr_light_texel(512 + int(cellf.y) * int(pbrLightGrid.w) + int(cellf.x));
   const int start = int(cell.x);
   const int count = min(int(cell.y), 32); // must match the renderer's maxPerCell
   const float a = roughness * roughness;
   // Specular AA: widen the lobe (alpha2) by the screen-space normal variance so a highlight
   // narrower than a pixel spreads its energy instead of strobing single pixels (no TAA in v1).
   // Only the direct term is widened: indirect paths mip-filter by the material roughness.
   const float a2 = min(a * a + pbrLightGrid2.z * 0.5 * max(dot(dFdx(wN), dFdx(wN)), dot(dFdy(wN), dFdy(wN))), 0.25);
   const float NdotV = saturate(dot(wN, wV));
   const float aAA = sqrt(a2);
   const vec3 wR = reflect(-wV, wN);
   // Clear coat lobe on the geometric normal wNc, with its own specular AA; transmitted light leaves through the coat
   // (attenuated by the coat's Fresnel toward the viewer)
   float ca2 = 0.0, caAA = 0.0, cNdotV = 0.0, coatExit = 1.0;
   vec3 cwR = wR;
   BRANCH if (coatStrength > 0.0)
   {
      const float ca = coatRoughness * coatRoughness;
      ca2 = min(ca * ca + pbrLightGrid2.z * 0.5 * max(dot(dFdx(wNc), dFdx(wNc)), dot(dFdy(wNc), dFdy(wNc))), 0.25);
      caAA = sqrt(ca2);
      cNdotV = saturate(dot(wNc, wV));
      cwR = reflect(-wV, wNc);
      coatExit = 1.0 - coatStrength * (0.04 + 0.96 * pow(1.0 - cNdotV, 5.0));
   }
   vec3 acc = vec3_splat(0.0);
   // VSM shadow kernel: a 16-tap Poisson disk on the unit circle, rotated per pixel by a hash of the
   // fragment coords. The previous axis-aligned 3x3 grid quantized the penumbra into ~10 visibly banded
   // levels; the rotated disk spreads the taps so the residual quantization reads as fine noise instead.
   // (elementwise init: shaderc has no array literals, same pattern as the SMAA offset[] declarations)
   vec2 vsmDisk[16];
   vsmDisk[0] = vec2(-0.94201624, -0.39906216);
   vsmDisk[1] = vec2(0.94558609, -0.76890725);
   vsmDisk[2] = vec2(-0.09418410, -0.92938870);
   vsmDisk[3] = vec2(0.34495938, 0.29387760);
   vsmDisk[4] = vec2(-0.91588581, 0.45771432);
   vsmDisk[5] = vec2(-0.81544232, -0.87912464);
   vsmDisk[6] = vec2(-0.38277543, 0.27676845);
   vsmDisk[7] = vec2(0.97484398, 0.75648379);
   vsmDisk[8] = vec2(0.44323325, -0.97511554);
   vsmDisk[9] = vec2(0.53742981, -0.47373420);
   vsmDisk[10] = vec2(-0.26496911, -0.41893023);
   vsmDisk[11] = vec2(0.79197514, 0.19090188);
   vsmDisk[12] = vec2(-0.24188840, 0.99706507);
   vsmDisk[13] = vec2(-0.81409955, 0.91437590);
   vsmDisk[14] = vec2(0.19984126, 0.78641367);
   vsmDisk[15] = vec2(0.14383161, -0.14100790);
   for (int i = 0; i < count; i++)
   {
      const vec4 lIdx = pbr_light_texel(1024 + start + i);
      const int li = int(lIdx.x);
      const float sphereRadius = lIdx.y;
      const vec4 lPos = pbr_light_texel(li * 2);
      const vec4 lRad = pbr_light_texel(li * 2 + 1);
      vec3 lCol = lRad.rgb;
      const vec3 dl = lPos.xyz - wPos;
      // Clamp the distance to ~1 VPU (~0.5mm), or to the light source sphere: a light embedded in
      // geometry would otherwise produce unbounded inverse-square irradiance
      const float dist2 = max(dot(dl, dl), max(sphereRadius * sphereRadius, 1.0));
      // Windowed inverse square attenuation: (1 - (d/r)^4)^2 / d^2 gives a smooth cutoff at the light range
      const float rangeRatio = dist2 / (lPos.w * lPos.w);
      const float x = saturate(1.0 - rangeRatio * rangeRatio);
      if (x <= 0.0 || (lIdx.w > 0.0 && dist2 < pbrOwnLampRadius * pbrOwnLampRadius))
         continue;
      const vec3 l = dl * inversesqrt(dist2);
      // Upward facing emitter (a lit insert): Lambertian emission around +z, so the light does not spill sideways
      // onto nearby walls as a sphere would
      if (lIdx.z > 0.0)
         lCol *= saturate(-dl.z * inversesqrt(max(dot(dl, dl), 1e-6)));
      // Flasher dome lamp: one soft band per flute (brightest through the thin wall at the groove roots, ~24 % banding as
      // measured on a real dome) and the dome base cutting the light below its rim (8 deg soft edge)
      BRANCH if (lIdx.w > 0.0)
      {
         const float flutes = floor(lIdx.w);
         const float band = pow(0.5 + 0.5 * cos(flutes * (atan2(-dl.y, -dl.x) - fract(lIdx.w) * (2.0 * PI))), 10.0);
         const float elevation = degrees(asin(clamp(-dl.z * inversesqrt(max(dot(dl, dl), 1e-6)), -1.0, 1.0)));
         lCol *= mix(0.76, 1.0, band) * smoothstep(lIdx.z - 4.0, lIdx.z + 4.0, elevation);
      }
      // Thin surface translucency (printed plastics): a light behind the surface transmits through it, see below
      const float NdotLraw = dot(wN, l);
      const float NdotL = saturate(NdotLraw);
      const float cNdotLraw = dot(wNc, l); // wNc = wN without a coat
      if (NdotL <= 0.0 && cNdotLraw <= 0.0 && (NdotLraw >= 0.0 || pbrTransmission <= 0.0))
         continue;
      // The geometric normal wNg (no normal map) decides the side of the surface a light is on. An opaque surface (alpha 1)
      // without a transmission gets nothing from a light behind it, however the shading normal leans (faded over ~3 deg so
      // no terminator seam shows). See-through surfaces (clear plastic ramps, Traced cards) keep the light from behind, as
      // thin translucency does, with their shadow lookup on the light's side below.
      const float gNdotLraw = dot(wNg, l);
      const float gFade = transmission > 0.0 || alpha < 1.0 ? 1.0 : saturate(gNdotLraw * 20.0);
      if (gFade <= 0.0)
         continue;
      // Shadow lookups on the light's side of the geometric surface (the far side for a light behind it), slope bias from
      // the geometric normal: a normal mapped clear ramp lit from below otherwise compared against the opaque decal right
      // under it at grazing incidence, and the result flipped from cube texel to cube texel (stair-step stripes)
      const float NdotLs = max(abs(gNdotLraw), 1e-3);
      // Sphere light specular (Karis 2013, representative point): light from the point of the bulb sphere
      // closest to the reflection ray, the GGX lobe energy renormalized by (a / a')^2, a' = a + radius / (2 dist).
      // a is the specular AA widened alpha so a highlight already spread by the AA is not dimmed twice.
      // Diffuse stays punctual. A zero radius gives back the punctual light.
      vec3 sl = l;
      float sphereNorm = 1.0;
      BRANCH if (sphereRadius > 0.0)
      {
         const vec3 centerToRay = dot(dl, wR) * wR - dl;
         sl = normalize(dl + centerToRay * saturate(sphereRadius * inversesqrt(max(dot(centerToRay, centerToRay), 1e-6))));
         const float aSphere = saturate(aAA + sphereRadius * 0.5 * inversesqrt(dist2));
         sphereNorm = (aAA / aSphere) * (aAA / aSphere);
      }
      const float sNdotL = saturate(dot(wN, sl));
      const vec3 h = normalize(sl + wV);
      const float NdotH = saturate(dot(wN, h));
      const float VdotH = saturate(dot(wV, h));
      // GGX D + Schlick F + height-correlated Smith visibility (already includes the 1/(4 NdotL NdotV) term)
      const float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
      const float D = a2 / (PI * denom * denom) * sphereNorm;
      const vec3 F = F0 + (vec3_splat(1.0) - F0) * pow(1.0 - VdotH, 5.0);
      const float gv = sNdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
      const float gl = NdotV * sqrt(sNdotL * sNdotL * (1.0 - a2) + a2);
      const float G = 0.5 / max(gv + gl, 1e-6);
      // Soft luminance knee on the specular term: a sub-pixel GGX spike can still reach extreme
      // HDR values after specular AA. A continuous compressor (no plateau) preserves ordering
      // inside the clipped region so the bloom blob edge moves smoothly as the view moves
      vec3 spec = D * F * G * (lCol * (x * x / dist2)) * sNdotL * step(0.0, NdotLraw);
      // Clear coat lobe: same GGX/Smith model and sphere light representative point at the coat roughness, F0 0.04. The base
      // (diffuse and specular) under it receives what the coat does not reflect: (1 - coat * Fc)
      float coatAtt = 1.0;
      BRANCH if (coatStrength > 0.0 && cNdotLraw > 0.0)
      {
         vec3 csl = l;
         float cSphereNorm = 1.0;
         BRANCH if (sphereRadius > 0.0)
         {
            const vec3 centerToRay = dot(dl, cwR) * cwR - dl;
            csl = normalize(dl + centerToRay * saturate(sphereRadius * inversesqrt(max(dot(centerToRay, centerToRay), 1e-6))));
            const float aSphere = saturate(caAA + sphereRadius * 0.5 * inversesqrt(dist2));
            cSphereNorm = (caAA / aSphere) * (caAA / aSphere);
         }
         const float cNdotL = saturate(dot(wNc, csl));
         const vec3 ch = normalize(csl + wV);
         const float cNdotH = saturate(dot(wNc, ch));
         const float cdenom = cNdotH * cNdotH * (ca2 - 1.0) + 1.0;
         const float cD = ca2 / (PI * cdenom * cdenom) * cSphereNorm;
         const float Fc = coatStrength * (0.04 + 0.96 * pow(1.0 - saturate(dot(wV, ch)), 5.0));
         const float cgv = cNdotL * sqrt(cNdotV * cNdotV * (1.0 - ca2) + ca2);
         const float cgl = cNdotV * sqrt(cNdotL * cNdotL * (1.0 - ca2) + ca2);
         const float cG = 0.5 / max(cgv + cgl, 1e-6);
         coatAtt = 1.0 - Fc;
         spec = spec * coatAtt + (cD * Fc * cG * cNdotL) * (lCol * (x * x / dist2));
      }
      spec /= 1.0 + dot(spec, vec3(0.176204, 0.812985, 0.0108109)) * (1.0 / 1024.0);

      // Shadowed light record: w packs mode (bits 0-3), VSM index + 1 (bits 4-11, 0 = no map),
      // layout (bits 12-15, 0 = ortho, 2 = cube) and the penumbra radius quantized in 0.5 VPU units (bits 16+)
      vec3 shadow = vec3_splat(1.0);
      const int shadowInfo = int(floor(lRad.w + 0.5));
      const int shadowMode = shadowInfo & 15;
      const int shadowIndex = ((shadowInfo >> 4) & 255) - 1;
      const int shadowLayout = (shadowInfo >> 12) & 15;
      const float lightRadius = float(shadowInfo >> 16) * 0.5;
      BRANCH if (shadowIndex >= 0 && shadowLayout <= 1)
      {
         // Ortho virtual map: project once, then each PCF tap resolves its own page through the
         // page table (a tap landing in a non-resident page counts as lit: pages without casters
         // are never allocated)
         const vec4 sc = mul(matShadowVP[shadowIndex], vec4(wPos, 1.0));
         const vec2 suv = sc.xy * vec2(0.5, -0.5) + 0.5;
         BRANCH if (sc.z > 0.0 && all(greaterThanEqual(suv, vec2_splat(0.0))) && all(lessThanEqual(suv, vec2_splat(1.0))))
         {
            const vec4 vsmP = pbrVSMParams[shadowIndex];
            const vec4 vsmB = pbrVSMBox[shadowIndex];
            const vec2 grid = vsmP.yz;
            // Slope-scaled depth bias plus a constant term to fight self-shadowing acne
            const float bias = pbrShadowParams.y * (1.0 - NdotLs) / max(NdotLs, 0.1) + 0.25 * pbrShadowParams.x;
            const float blocker = pbr_vsm_depth(vsmP.x, grid, suv);
            if (blocker >= 0.0)
            {
               // PCSS-lite: the penumbra radius grows with the receiver/blocker gap, expressed in
               // world units then converted to virtual uv through the box scale
               const float worldGap = (sc.z - blocker) * vsmP.w;
               const vec2 filterRadius = clamp(vec2_splat(lightRadius * worldGap / max(sqrt(dist2), 1.0)) * vsmB.xy, vec2_splat(pbrShadowParams.x), vec2_splat(0.02));
               // Soft compare: each tap ramps over a ~1.5-texel world-depth window (widened at grazing
               // incidence like the bias) instead of a binary step, so the penumbra comes out continuous
               const float cmpWin = 1.5 * max(1.0 / (vsmB.x * grid.x), 1.0 / (vsmB.y * grid.y)) / (max(NdotLs, 0.3) * 128.0);
               float shadowAcc = 0.0;
               UNROLL for (int t = 0; t < 16; t++)
               {
                  const vec2 tuv = suv + pbr_vsm_rot(vsmDisk[t], vsmRot) * filterRadius;
                  const float d = pbr_vsm_depth(vsmP.x, grid, tuv);
                  shadowAcc += d < 0.0 ? 1.0 : smoothstep(-cmpWin, cmpWin, (d - sc.z + bias) * vsmP.w);
               }
               // Translucent casters: one bilinear tap at the centre (receiver 2 VPU behind them at least)
               const float nearness = 1.0 - sc.z + 2.0 / max(vsmP.w, 1.0);
               shadow = shadowAcc * (1.0 / 16.0) * pbr_vsm_tint(pbr_vsm_index(vsmP.x, grid, suv), fract(suv * grid), nearness);
            }
         }
      }
      BRANCH if (shadowIndex >= 0 && shadowLayout == 2)
      {
         // Cube layout: 6 faces of faceGrid^2 pages each, rasterized as regular 90 deg perspective
         // depth and reconstructed to the projected distance along the face's forward axis (world units).
         // PCF taps offset the lookup direction so they cross face edges cleanly.
         // Contact-shadow fix: offset the shading point along the surface normal before computing
         // the light->fragment direction, so the cube lookup samples deeper inside the caster
         // silhouette instead of grazing its edge (removes light leaks at caster contact points)
         const vec4 vsmP = pbrVSMParams[shadowIndex];
         const float faceGrid = vsmP.y;
         const vec3 shadowPos = wPos + (gNdotLraw < 0.0 ? -wNg : wNg) * (0.5 + 0.04 * sqrt(dist2));
         const vec3 sdl = lPos.xyz - shadowPos;
         const float sdist = inversesqrt(dot(sdl, sdl));
         const vec3 ldir = -sdl * sdist; // normalized light -> fragment direction
         const float dist = 1.0 / sdist;
         const float zN = 2.0; // near plane, must match the renderer's cube face projection
         const float zF = max(lPos.w, 60.0);
         const vec4 cl = pbr_shadow_cube_lookup(ldir);
         const float blockerNdc = pbr_vsm_cube_depth(vsmP.x, faceGrid, cl);
         if (blockerNdc >= 0.0)
         {
            const float blocker = zN * zF / (zF - blockerNdc * (zF - zN));
            const float worldGap = dist * cl.w - blocker;
            // PCSS-lite penumbra in direction space (a uv offset of 1.0 maps to ~2 direction units);
            // the face's virtual texel size is 1/(faceGrid * PAGE_DIM) of a direction unit
            const float invFaceRes = 1.0 / (faceGrid * 128.0);
            const float filterRadius = clamp(lightRadius * worldGap / max(blocker, 1.0) * invFaceRes * 2.0, invFaceRes, 8.0 * invFaceRes);
            const float bias = min(0.5 + 2.0 * (1.0 - NdotLs) / max(NdotLs, 0.05), 8.0);
            const vec3 upRef = abs(ldir.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
            const vec3 tX = normalize(cross(upRef, ldir));
            const vec3 tY = cross(ldir, tX);
            // Soft compare window: ~one face texel projected at the receiver distance in world units
            const float cmpWin = max(0.25, dist * 2.0 * invFaceRes) / max(NdotLs, 0.3);
            float shadowAcc = 0.0;
            UNROLL for (int t = 0; t < 16; t++)
            {
               const vec2 toff = pbr_vsm_rot(vsmDisk[t], vsmRot);
               const vec3 tdir = normalize(ldir + (toff.x * tX + toff.y * tY) * filterRadius);
               const vec4 tl = pbr_shadow_cube_lookup(tdir);
               const float tapNdc = pbr_vsm_cube_depth(vsmP.x, faceGrid, tl);
               const float tapBlocker = tapNdc < 0.0 ? -1.0 : zN * zF / (zF - tapNdc * (zF - zN));
               shadowAcc += tapBlocker < 0.0 ? 1.0 : smoothstep(-cmpWin, cmpWin, tapBlocker - (dist * tl.w - bias));
            }
            const int g = int(faceGrid + 0.5);
            const vec2 pg = floor(cl.xy * vec2_splat(faceGrid));
            const int cidx = int(vsmP.x + 0.5) + int(cl.z + 0.5) * g * g + int(pg.y) * g + int(pg.x);
            shadow = shadowAcc * (1.0 / 16.0) * pbr_vsm_tint(cidx, fract(cl.xy * vec2_splat(faceGrid)), 1.0 - (dist * cl.w - 2.0) / zF);
         }
      }
      BRANCH if (shadowMode == 1 || shadowMode >= 3) // RAYTRACED_BALL_SHADOWS or PBR ball shadow tiers
         shadow *= get_light_ball_shadow(lPos.xyz, -dl, sqrt(dist2));
      // Thin surface translucency: the transmitted fraction (transmission) of the light arriving on the far side leaves
      // this side diffusely, filtered by the surface colour (printed ink: dark ink passes little, white flood glows). Light
      // ink scatters wider: a wrapped lobe lets grazing light through, spreading the glow away from the bulb. Clear
      // plastics additionally show a view-dependent transmission cone toward the light (the bulb seen through the
      // sheet), widening from a tight hot spot to the wrap term as the material roughness frosts over.
      float transmitted = 0.0;
      BRANCH if (NdotLraw < 0.0)
      {
         const float wrap = 0.4 * dot(diffuse, vec3(0.2126, 0.7152, 0.0722));
         const vec3 l2 = normalize(-l + wN * 0.5);
         const float lobe = pow(saturate(dot(-wV, l2)), exp2(mix(12.0, 2.0, roughness)));
         transmitted = transmission * saturate((wrap - NdotLraw) / (1.0 + wrap) + lobe * (1.0 - roughness));
      }
      acc += (gFade * shadow) * (diffuse * ((vec3_splat(1.0) - F) * (NdotL * coatAtt) + transmitted * coatExit) * (1.0 / PI) * (lCol * (x * x / dist2)));
      specOut += (gFade * shadow) * spec;
   }
   return acc;
}

#ifdef TRANSPARENT
// Light arriving at a see-through surface from either side, for its scatter lobe: the clustered lights (GI bulbs, inserts,
// flasher domes), unshadowed (the light is inside the plastic), wrapped on |N.L| so light from behind the sheet counts as
// well as from the front. loc > 0 (scatter locality) blends toward the tight local response (see PRES_LOC_R)
vec3 pbr_scatter_light(const vec3 wPos, const vec3 wN, const float loc)
{
   const vec2 cellf = clamp((wPos.xy - pbrLightGrid.xy) * pbrLightGrid.z, vec2_splat(0.0),
      vec2(pbrLightGrid.w - 1.0, pbrLightGrid2.x - 1.0));
   const vec4 cell = pbr_light_texel(512 + int(cellf.y) * int(pbrLightGrid.w) + int(cellf.x));
   const int start = int(cell.x);
   const int count = min(int(cell.y), 32); // must match the renderer's maxPerCell
   vec3 acc = vec3_splat(0.0);
   for (int i = 0; i < count; i++)
   {
      const vec4 lIdx = pbr_light_texel(1024 + start + i);
      const int li = int(lIdx.x);
      const vec4 lPos = pbr_light_texel(li * 2);
      const vec4 lRad = pbr_light_texel(li * 2 + 1);
      const vec3 dl = lPos.xyz - wPos;
      const float dist2 = max(dot(dl, dl), max(lIdx.y * lIdx.y, 1.0));
      const float rangeRatio = dist2 / (lPos.w * lPos.w);
      const float x = saturate(1.0 - rangeRatio * rangeRatio);
      if (x <= 0.0 || (lIdx.w > 0.0 && dist2 < pbrOwnLampRadius * pbrOwnLampRadius))
         continue;
      vec3 lCol = lRad.rgb;
      if (lIdx.z > 0.0) // upward facing emitter (lit insert)
         lCol *= saturate(-dl.z * inversesqrt(max(dot(dl, dl), 1e-6)));
      float w = (x * x / dist2) * (0.5 + 0.5 * abs(dot(wN, dl) * inversesqrt(dist2)));
      if (loc > 0.0)
      {
         const float d2l = max(dist2, PRES_LOC_DMIN * PRES_LOC_DMIN);
         const float wl = (x * x / d2l) * (PRES_LOC_R * PRES_LOC_R / (PRES_LOC_R * PRES_LOC_R + d2l)) * abs(dot(wN, dl) * inversesqrt(dot(dl, dl) + 1e-6));
         w = mix(w, wl, loc);
      }
      acc += lCol * w;
   }
   return acc;
}
#endif

#if !defined(AT) && !defined(CLIP)
EARLY_DEPTH_STENCIL
#endif
void main() {
   #ifdef CLIP
   if (v_clipDistance < 0.0)
      discard;
   #endif

   // Base color & coverage
   #ifdef TEX
      vec4 pixel;
      if (noMipMaps)
         pixel = texNoLod(tex_base_color, v_texcoord0);
      else
         pixel = texture2D(tex_base_color, v_texcoord0);
      #ifdef AT
         if (pixel.a <= alphaTestValue.x)
            discard; //stop the pixel shader if alpha test should reject pixel
      #endif
      const vec4 base = pixel * cBase_Alpha;
   #else
      const vec4 base = cBase_Alpha;
   #endif

   #ifdef SHADOW_TRANS
      // Translucent shadow caster (max blended into the page's transmittance layer): absorbance = 1 - transmittance, the
      // transmitted part (material transmission) filtered by the colour where the part covers (clear plastic: its alpha)
      const float coverage = saturate(base.a * staticColor_Alpha.a);
      const vec3 transmittance = mix(vec3_splat(1.0), saturate(pbrTransmission * base.rgb), coverage);
      const float nearness = 1.0 - saturate((-v_worldPos.z - pbrShadowParams.z) * pbrShadowParams.w);
      gl_FragData[0] = vec4(vec3_splat(1.0) - transmittance, coverage > 0.004 ? nearness : 0.0);
      return;
   #endif

   // Occlusion / roughness / metallic
   // The ORM map's alpha masks the thin surface transmission (1 = the material's, 0 = none): a printed plastic with a black
   // backing passes light only through its unbacked windows (glTF KHR_materials_transmission keeps its own mask likewise)
   vec4 orm = vec4(1.0, 1.0, 1.0, 1.0);
   BRANCH if (hasORMMap)
   {
      if (noMipMaps)
         orm = texNoLod(tex_pbr_orm, v_texcoord0);
      else
         orm = texture2D(tex_pbr_orm, v_texcoord0);
   }
   const float metallic = saturate(orm.z * pbrMetallicFactor);
   float roughness = clamp(orm.y * pbrRoughnessFactor, 0.045, 1.0);
   float occlusion = saturate(orm.x * pbrOcclusionFactor);
   BRANCH if (pbrAOEnabled > 0.0)
      occlusion *= texStereoNoLod(tex_pbr_ao, gl_FragCoord.xy * w_h_height.xy).x;

   // Shading normal (the normal map may be bound on untextured PBR materials too, hence not guarded by TEX)
   const vec3 V = -normalize(v_worldPos);
   vec3 N = normalize(v_normal);
   BRANCH if (doNormalMapping)
      N = pbr_normal_map(N, v_worldPos, v_texcoord0);
   // Flip normal in case of backside lighting (same as legacy shading)
   if (dot(N, V) < 0.0)
      N = -N;
   // Geometric normal (no normal map), facing the viewer: it tells which side of the surface a direct light is on
   vec3 Ng = normalize(v_normal);
   if (dot(Ng, V) < 0.0)
      Ng = -Ng;

   const vec3 albedo = base.rgb;
   vec3 F0 = mix(vec3_splat(0.04), albedo, metallic);
   const vec3 diffuse = albedo * (1.0 - metallic);

   // Clear coat: the coat follows the geometric normal (a smooth shell over the normal mapped base). The base is seen from
   // inside the coat (its F0 relative to IOR 1.5) and is at least as rough as the coat (Filament)
   vec3 Nc = N;
   BRANCH if (coatStrength > 0.0)
   {
      Nc = Ng;
      F0 = mix(F0, pbr_f0_under_coat(F0), coatStrength);
      roughness = mix(roughness, max(roughness, coatRoughness), coatStrength);
   }

   // Specular AA for the indirect lobes too (the direct lobe has its own in pbr_direct_light): a curved metal part a few
   // pixels wide (post, standoff, wire) reflects the env's window or a probe's bulb as a sub-pixel stripe that crawls as
   // the view moves. Widen the GGX alpha^2 by the screen-space variance of the normal (Tokuyoshi & Kaplanyan 2019,
   // sigma^2 = 1/2pi per axis, kappa 0.18), which picks the env / probe mip as well. Flat parts (playfield) keep theirs.
   // The variance is taken on the interpolated vertex normal: the surface's curvature, not its normal map (fine scratches
   // must stay sharp) nor the facing flip (a jump at the silhouette of a double sided sheet)
   const vec3 Nv = normalize(v_normal);
   const float roughnessIBL = pbr_aa_roughness(Nv, roughness);
   const float coatRoughnessIBL = pbr_aa_roughness(Nv, coatRoughness);

   // Indirect specular weight (envmap evaluated in-shader for INSHADERSPEC, otherwise deferred to the resolve pass)
   vec3 envSpec;
   #ifdef STEREO
      const vec3 W = pbr_specular_weight(N, V, F0, roughnessIBL, occlusion, v_eye, envSpec);
   #else
      const vec3 W = pbr_specular_weight(N, V, F0, roughnessIBL, occlusion, envSpec);
   #endif
   // Planar probe radiance, blended per pixel between the assigned probes by distance to their
   // reflection planes, mixed with the envmap specular over the uncovered range. In probe render
   // contexts (INSHADERSPEC without bound probes) the weights are 0 leaving envmap only.
   const vec2 suv = gl_FragCoord.xy * w_h_height.xy;
   #ifdef STEREO
      const vec4 probe = pbr_probe_specular(v_tablePos, suv, roughnessIBL, false, v_eye);
   #else
      const vec4 probe = pbr_probe_specular(v_tablePos, suv, roughnessIBL, false);
   #endif
   // Same soft luminance knee as the direct specular: bound hot probe/envmap radiance (e.g. a
   // bulb inside a probe) before it feeds in-shader specular, the sideband and the resolve's
   // firefly clamp reference
   vec3 specSrc = mix(envSpec, probe.rgb, probe.a);
   specSrc /= 1.0 + dot(specSrc, vec3(0.176204, 0.812985, 0.0108109)) * (1.0 / 1024.0);

   // Clear coat indirect specular: split-sum at the coat roughness on the coat normal (F0 0.04), the base receiving
   // (1 - coat * Fc(NdotV)). The resolve sideband (W, radiance, probe coverage) then carries the coat lobe.
   float coatIBL = 1.0;
   vec3 sideSrc = specSrc;
   float sideCoverage = probe.a;
   vec3 sideW = W;
   vec3 coatSpecInd = vec3_splat(0.0);
   BRANCH if (coatStrength > 0.0)
   {
      vec3 envCoat;
      #ifdef STEREO
         const vec3 Wc = coatStrength * pbr_specular_weight(Nc, V, vec3_splat(0.04), coatRoughnessIBL, occlusion, v_eye, envCoat);
         const vec4 probeCoat = pbr_probe_specular(v_tablePos, suv, coatRoughnessIBL, true, v_eye);
      #else
         const vec3 Wc = coatStrength * pbr_specular_weight(Nc, V, vec3_splat(0.04), coatRoughnessIBL, occlusion, envCoat);
         const vec4 probeCoat = pbr_probe_specular(v_tablePos, suv, coatRoughnessIBL, true);
      #endif
      coatIBL = 1.0 - coatStrength * (0.04 + 0.96 * pow(1.0 - saturate(dot(Nc, V)), 5.0));
      sideSrc = mix(envCoat, probeCoat.rgb, probeCoat.a);
      sideSrc /= 1.0 + dot(sideSrc, vec3(0.176204, 0.812985, 0.0108109)) * (1.0 / 1024.0);
      sideCoverage = probeCoat.a;
      sideW = Wc;
      coatSpecInd = Wc * sideSrc;
   }

   vec3 color = vec3_splat(0.0);
   vec3 spec = vec3_splat(0.0);
   vec3 directSpec = vec3_splat(0.0);

   if (fDisableLighting_top_below.x != 0.0)
   { // Lighting disabled: fully emissive backward compatible behavior (unlit, twice the albedo)
      color += 2.0 * albedo;
   }
   else
   {
      const vec3 kd = saturate(vec3_splat(1.0) - W) * coatIBL;
      #ifdef STEREO
         color += pbr_indirect_diffuse(N, diffuse, kd, occlusion, v_eye);
      #else
         color += pbr_indirect_diffuse(N, diffuse, kd, occlusion);
      #endif
      #if defined(INSHADERSPEC)
         spec = W * coatIBL * specSrc + coatSpecInd;
      #else
         // Clear coat: the resolve carries the coat lobe, the base indirect specular is shaded here
         BRANCH if (coatStrength > 0.0)
            color += W * coatIBL * specSrc;
      #endif

      // Forward+ clustered direct lighting (world space: the light grid is built over the table XY plane)
      BRANCH if (pbrLightGrid2.y > 0.0)
      {
         const vec3 wN = mul(vec4(N, 0.0), mView).xyz; // trafo back to world (orthonormal, no renormalize needed)
         const vec3 wV = normalize(cCameraPosWorld - v_tablePos);
         const vec3 wNc = coatStrength > 0.0 ? mul(vec4(Nc, 0.0), mView).xyz : wN;
         // Per-pixel rotation of the VSM Poisson kernel (gl_FragCoord is only in scope in main(), hence passed in)
         const float vsmPhase = hash22(gl_FragCoord.xy).x * (2.0 * PI);
         color += pbr_direct_light(wN, wNc, mul(vec4(Ng, 0.0), mView).xyz, wV, v_tablePos, diffuse, F0, roughness, base.a * staticColor_Alpha.a, pbrTransmission * orm.w, vec2(cos(vsmPhase), sin(vsmPhase)), directSpec);
      }
   }

   BRANCH if (lightCenter_doShadow.w != 0.)
   {
      const vec3 light_dir = v_tablePos.xyz - lightCenter_doShadow.xyz;
      const float light_dist = length(light_dir);
      const float ballShadow = get_light_ball_shadow(lightCenter_doShadow.xyz, light_dir, light_dist);
      color.rgb *= ballShadow;
      directSpec *= ballShadow;
   }
   #ifdef TRANSPARENT
      spec += directSpec; // reflects at full strength on a see-through part, like the indirect specular
   #else
      color += directSpec;
   #endif

   // Emission
   vec3 emission = pbrEmission.rgb * pbrEmission.w;
   BRANCH if (hasEmissionMap)
   {
      if (noMipMaps)
         emission *= texNoLod(tex_pbr_emission, v_texcoord0).rgb;
      else
         emission *= texture2D(tex_pbr_emission, v_texcoord0).rgb;
   }
   color += emission;

   const float alpha = base.a * staticColor_Alpha.a;
   color *= staticColor_Alpha.rgb;
   spec *= staticColor_Alpha.rgb;

   #ifdef TRANSPARENT
      // What the surface reflects is not transmitted: the coverage of what lies behind grows with the reflectance
      // (Fresnel), so clear plastic darkens and reads as a sheet at grazing angles instead of vanishing
      const float cover = alpha + (1.0 - alpha) * saturate(dot(W * coatIBL, vec3(0.2126, 0.7152, 0.0722)));
      BRANCH if (pbrProbeParams.w > 0.0)
      {
         // Bound refraction probe: the part is drawn unblended (as legacy refraction parts), so it composites what lies
         // behind itself: the probe (a copy of the scene behind), refracted through the part's thickness and tinted,
         // through the uncovered fraction, over its own diffuse and under its reflections. The probe is blurred by the
         // roughness and more where the normal map bends away from the surface (scuffs, scratches): flat runs stay clear.
         // The offset follows the surface (mostly the geometric normal): the scratch normals bending it pixel by pixel
         // shredded the edges behind into hairs along the scratches; they blur it instead
         const vec3 wN = mul(vec4(normalize(mix(Ng, N, 0.2)), 0.0), mView).xyz; // trafo back to world (orthonormal)
         const vec3 wV = normalize(cCameraPosWorld - v_tablePos);
         // Blur (a fraction of the probe's mip chain): the roughness, plus a little where the normal map bends away from the
         // surface (scuffs), capped so the probe never collapses to its average colour
         // Composite as the legacy refraction does (refraction bench vs stock 10.8.1, 2026-10-30): the refracted image comes
         // through 1 - alpha, the Fresnel share is not taken out of it. Fresnel lives only in the added reflection (spec):
         // taking it out of the transmission covered the grazing walls and edges, where the lensing reads as plastic, with
         // a milky sheet. VPX_REFR_DEBUG 4: the legacy offset direction (refract +V) for the bench.
         const vec3 wI = pbrLightGrid2.w == 4.0 ? wV : -wV;
         const float blur = min(roughness + 0.1 * (1.0 - saturate(dot(N, Ng))), 0.08);
         // Curvature-driven refraction (refraction_curvature, 0 = off): a curved region (fillet, bevelled wall top) bends the
         // view more than the flat runs of the same part. The curvature k (relative to a 10 VPU radius bend, less a 0.3 dead
         // zone so gentle bends of the run do not count) makes the term gain.k (at most 3): the offset grows by term times the
         // thickness (a third of it for what is close behind, all of it at full depth, see pbr_transmit_ex), with no extra
         // blur: a clean bevel is bright and bent, not frosted. Flat floor and walls are unchanged by it. k is the per-vertex curvature interpolated over the face (v_refrCurv, computed at upload,
         // see Primitive::UploadMeshToVB), so a low-poly fillet reacts smoothly instead of in per-triangle steps; until the
         // part is reloaded after the option is switched on, the per-triangle estimate stands in.
         // Thin-sheet incidence (with refraction_curvature): the two faces of a thin flat sheet cancel at normal incidence (no
         // visible shift looking straight down through a plastic or a ramp floor, close things behind a flat panel barely
         // move), and a flat sheet never shifts the view by much more than its thickness. So the base offset scales by
         // (1 - c) / c, c = |N.V| on the vertex normal: 0 face-on, 0.3 at 40 deg, 1 (as before) at 60 deg, at most
         // REFR_SHEET_MAX. A steep face (wall, a plastic's rim) seen toward edge-on is a light guide: the eye looks through
         // the sheet along its length (in at the top edge or flange, along by total internal reflection), so the path through
         // the plastic grows as thickness / c, up to REFR_GUIDE_MAX thicknesses: the playfield behind is bent beyond
         // recognition into a bright streaked band (light piped along the sheet, and the Fresnel reflection rises toward
         // grazing). The deflection by the shape of the sheet also grows toward grazing (the exit angle's sensitivity to the
         // surface tilt goes as 1 / c): the curvature term by 1 + (1 - c) / c (at most 3x), the anisotropic streak with the
         // path through the plastic (at most 1 + REFR_GUIDE_MAX).
         float refrSheet = 1.0, refrGuide = 0.0;
         float refrCurvExtra = 0.0; // also read by the debug views
         vec3 refrSmear = vec3_splat(0.0), refrWave = vec3_splat(0.0);
         vec3 behind;
         BRANCH if (refractionParams.x > 0.0 || refractionParams.y > 0.0 || refractionParams.z > 0.0 || refractionParams2.x > 0.0 || refractionParams2.y > 0.0)
         {
            const bool refrAxis = refractionParams.y > 0.0 || refractionParams2.x > 0.0;
            const vec4 curv = refrAxis || (refractionParams.x > 0.0 && refractionParams.w == 0.0) ? pbr_curvature(Nv, v_tablePos) : vec4_splat(0.0);
            const float refrK = refractionParams.w > 0.0 ? v_refrCurv : 10.0 * curv.x; // curvature relative to a 10 VPU radius
            refrCurvExtra = min(refractionParams.x * max(refrK - 0.3, 0.0), 3.0);
            const float refrCosI = max(abs(dot(Ng, V)), 1e-3);
            const float refrGraze = (1.0 - refrCosI) / refrCosI;
            const vec3 wNg = mul(vec4(Ng, 0.0), mView).xyz;
            const float lh = length(wNg.xy); // sine of the face's slope
            if (refractionParams.x > 0.0)
            {
               refrGuide = smoothstep(0.6, 0.85, lh) * min(1.0 / refrCosI, REFR_GUIDE_MAX);
               refrSheet = max(min(refrGraze, REFR_SHEET_MAX), refrGuide);
            }
            // The part's long axis (the sweep of a ramp wall or fillet), for the anisotropic streak and the forming waves. It is
            // the horizontal tangent N x up of the interpolated normal on steep faces (walls) and tight bends (fillets,
            // bevels); on near-flat faces the min curvature direction where the surface is clearly curved one way, else the
            // texture u direction (UV u along the ramp floor); without UVs a flat floor has no axis.
            BRANCH if (refrAxis)
            {
               const vec3 dPx = dFdx(v_tablePos);
               const vec3 dPy = dFdy(v_tablePos);
               const vec2 dUVx = dFdx(v_texcoord0);
               const vec2 dUVy = dFdy(v_texcoord0);
               vec3 Tc = dPx * curv.z + dPy * curv.w; // along the min curvature
               // Along the gradient of u on the surface (screen gradient through the inverse position metric): follows u even
               // where v is not a clean across axis (a plan-view projected ramp map)
               const float hxx = dot(dPx, dPx), hxy = dot(dPx, dPy), hyy = dot(dPy, dPy);
               vec3 Tu = dPx * (hyy * dUVx.x - hxy * dUVy.x) + dPy * (hxx * dUVy.x - hxy * dUVx.x);
               const float lc = length(Tc), lu = length(Tu);
               Tc = lc > 1e-8 ? Tc * (1.0 / lc) : vec3_splat(0.0);
               Tu = lu > 1e-8 ? Tu * (1.0 / lu) : vec3_splat(0.0);
               const vec3 Tuv = Tu; // the UV axis before the wall blend (forming waves)
               // Steep faces (walls, a plastic's rim) run horizontally: there the axis is the horizontal tangent. A ramp's UV map
               // does not follow the walls (PF's centre ramp is a plan-view unwrap that lays them out across, so u runs up them
               // and the axis flipped in bands from triangle to triangle)
               vec3 Th = vec3(wNg.y, -wNg.x, 0.0); // N x up
               Th = lh > 1e-4 ? Th * (1.0 / lh) : vec3_splat(0.0);
               const vec3 Thw = Th; // forming waves
               // Trust the curvature axis where the bend is tight (radius under ~15 VPU, from the smooth per-vertex curvature so
               // the per-triangle noise of the averaged normals on flat faces does not switch it on) and one-directional
               const float wc = smoothstep(0.6, 1.2, refrK) * saturate(1.0 - curv.y / max(curv.x, 1e-8));
               // The smear axis must be continuous over the mesh too (Ben 11-01: the outer wall seen at grazing showed one
               // block per quad, each streaked its own way): the min-curvature direction and the UV gradient are per
               // triangle (screen derivatives), and the smooth per-vertex curvature of a one-quad-tall wall interpolates the
               // fillet's and lip's high k across it, which switched the per-triangle curvature axis on there. So, as for the
               // waves: N x up of the interpolated normal off near-flat faces and on tight bends wherever N x up is defined
               // (on a swept part it is the min-curvature direction there), the UV / curvature axis only on near-flat faces.
               // The streak's taps are symmetric, so only the line matters: the flat-face axis takes its sign from N x up.
               vec3 Tf = mix(Tu, dot(Tc, Tu) < 0.0 ? -Tc : Tc, wc);
               if (dot(Tf, Th) < 0.0)
                  Tf = -Tf;
               const vec3 T = mix(Tf, Th, max(smoothstep(0.2, 0.5, lh), wc * smoothstep(0.03, 0.1, lh)));
               const float lt = length(T);
               // Anisotropic refraction (refraction_anisotropy 0..1, 0 = off): the view through the plastic streaks along the
               // axis, not across: subtle on the flat panels, up to 2x longer in the bends (forming lines in the plastic) and
               // growing with the path through the plastic toward grazing; its length set in pbr_transmit_ex from the distance
               // to what lies behind.
               const float bendBoost = 1.0 + saturate((refrK - 0.3) * (1.0 / 1.7));
               if (refractionParams.y > 0.0)
                  refrSmear = lt > 1e-3 ? T * (saturate(2.0 * lt) / lt * refractionParams.y * bendBoost * (1.0 + max(min(refrGraze, 4.0), refrGuide))) : vec3_splat(0.0);
               // Forming waves (refraction_wave, 0 = off): a vacuum-formed sheet is not flat, it undulates gently, so the view
               // through flat floors and walls wavers. Static 3D value noise (two octaves, REFR_WAVE_L1 / L2 VPU) tilts the
               // refraction normal only (not the shading one), mostly along the sweep (across it at 0.3x), by up to a few
               // degrees x the strength: no blur, the art behind undulates.
               // The sweep axis of the waves must be continuous over the mesh (Ben 11-01: with wave + flow the outer wall broke
               // up into per-triangle steps). T above is not: the min-curvature direction and the UV gradient come from screen
               // derivatives, constant per triangle, and the sign of the wall's horizontal tangent was picked against the UV
               // gradient. Here the axis is the horizontal tangent N x up of the interpolated vertex normal wherever the face is
               // not near flat (walls, fillets, bevels: on a swept ramp every cross-section normal lies in the plane normal to
               // the sweep, so N x up is the sweep there), the UV gradient only on near-flat faces (floor; small per-triangle
               // changes there, no longer amplified). Blended as axis tensors (T T^t), so no sign choice is involved.
               BRANCH if (refractionParams2.x > 0.0)
               {
                  const float wh = smoothstep(0.2, 0.5, lh);
                  const float wu = 1.0 - wh;
                  // Flow (refraction_wave_flow 0..1, 0 = the plain wave): the pattern is stretched along the sweep (up to 4x at 1)
                  // so it reads as flow lines, and its amplitude (up to 3x) no longer fades out toward grazing, which is how the
                  // walls are seen from above. The stretch blends 4 fields stretched along fixed horizontal axes (0, 45, 90, 135
                  // deg), weighted by the sweep axis (cos^8 of the angle, renormalized, the weakest dropped by a continuous
                  // threshold), variance kept by 1 / sqrt(sum w^2). Each field is continuous, the weights follow the smooth axis.
                  const float flow = refractionParams2.w;
                  vec3 g;
                  const vec4 c = vec4(wu * Tuv.x * Tuv.x + wh * Thw.x * Thw.x,
                     0.5 * (wu * (Tuv.x + Tuv.y) * (Tuv.x + Tuv.y) + wh * (Thw.x + Thw.y) * (Thw.x + Thw.y)),
                     wu * Tuv.y * Tuv.y + wh * Thw.y * Thw.y,
                     0.5 * (wu * (Tuv.x - Tuv.y) * (Tuv.x - Tuv.y) + wh * (Thw.x - Thw.y) * (Thw.x - Thw.y)));
                  const float cs = c.x + c.y + c.z + c.w; // 2 x the horizontal trace of the axis tensor
                  BRANCH if (flow > 0.0 && cs > 1e-4)
                  {
                     const float st = 1.0 - 1.0 / (1.0 + 3.0 * flow);
                     vec4 w = c * (2.0 / cs); // cos^2 to each axis
                     w *= w; w *= w; w *= w;
                     w *= 1.0 / (w.x + w.y + w.z + w.w);
                     w = max(w - 0.05, vec4_splat(0.0));
                     w *= 1.0 / (w.x + w.y + w.z + w.w);
                     g = vec3_splat(0.0);
                     BRANCH if (w.x > 0.0) g += w.x * pbr_wave_grad(v_tablePos, vec3(1.0, 0.0, 0.0), st);
                     BRANCH if (w.y > 0.0) g += w.y * pbr_wave_grad(v_tablePos, vec3(0.70710678, 0.70710678, 0.0), st);
                     BRANCH if (w.z > 0.0) g += w.z * pbr_wave_grad(v_tablePos, vec3(0.0, 1.0, 0.0), st);
                     BRANCH if (w.w > 0.0) g += w.w * pbr_wave_grad(v_tablePos, vec3(0.70710678, -0.70710678, 0.0), st);
                     g *= inversesqrt(dot(w, w));
                  }
                  else
                     g = pbr_noised(v_tablePos * (1.0 / REFR_WAVE_L1)).yzw + 0.5 * pbr_noised(v_tablePos * (1.0 / REFR_WAVE_L2) + vec3(17.3, 5.1, 9.7)).yzw;
                  // Faded out toward grazing incidence, where the refraction's sensitivity to the tilt would turn it into ripples
                  // on water (the light-guide band takes over there). refrCosI is on the interpolated normal: continuous.
                  float waveFade = smoothstep(0.15, 0.5, refrCosI);
                  if (flow > 0.0)
                     waveFade = mix(waveFade, smoothstep(0.03, 0.15, refrCosI), flow) * (1.0 + 2.0 * flow);
                  // Tilt mostly along the sweep, 0.3x across: 0.7 (T T^t) g + 0.3 (g in the tangent plane)
                  const vec3 gT = wu * dot(g, Tuv) * Tuv + wh * dot(g, Thw) * Thw;
                  refrWave = (-REFR_WAVE_TILT * refractionParams2.x * waveFade) * (0.7 * gT + 0.3 * (g - dot(g, wN) * wN));
               }
            }
            // Refraction imperfections (refraction_noise, 0 = off): mould / orange-peel, a tiny tilt of the refraction normal only
            // (reflections keep the clean normal) from the gradient of value noise at the scale (mm -> VPU), 0.7 deg per unit
            BRANCH if (refractionParams2.y > 0.0)
            {
               const vec3 gn = pbr_noised(v_tablePos * (1.0 / (refractionParams2.z * 1.853))).yzw;
               refrWave += (-0.0122 * refractionParams2.y * smoothstep(0.1, 0.3, refrCosI)) * (gn - dot(gn, wN) * wN);
            }
            #ifdef STEREO
               behind = pbr_transmit_ex(v_tablePos, wN, wI, blur, gl_FragCoord, refrSheet, 1.0 + min(refrGraze, 2.0), refrCurvExtra, refrSmear, refrWave, v_eye) * (staticColor_Alpha.rgb * coatIBL);
            #else
               behind = pbr_transmit_ex(v_tablePos, wN, wI, blur, gl_FragCoord, refrSheet, 1.0 + min(refrGraze, 2.0), refrCurvExtra, refrSmear, refrWave) * (staticColor_Alpha.rgb * coatIBL);
            #endif
            // The light-guide band reads bright: light piped along the sheet, and the reflection rising toward grazing; so do
            // strong bevels and fillets seen at an angle (the bright highlight lines along a new ramp's wall tops)
            const float refrGlow = REFR_GUIDE_GLOW * saturate((refrGuide - 2.0) * (1.0 / 8.0) + refrCurvExtra * (1.0 / 6.0) * saturate(0.5 * refrGraze));
            behind *= 1.0 + refrGlow * pbrPresence2.y;
            spec *= 1.0 + refrGlow * pbrPresence2.y;
         }
         else
         {
            #ifdef STEREO
               behind = pbr_transmit(v_tablePos, wN, wI, blur, gl_FragCoord, v_eye) * (staticColor_Alpha.rgb * coatIBL);
            #else
               behind = pbr_transmit(v_tablePos, wN, wI, blur, gl_FragCoord) * (staticColor_Alpha.rgb * coatIBL);
            #endif
         }
         // Ramp presence (material scatter > 0, see SURFACE PRESENCE below the defines): the see-through surface is drawn too
         BRANCH if (pbrScatter > 0.0 || pbrAbsorption.w > 0.0)
         {
            const vec3 wNg = mul(vec4(Ng, 0.0), mView).xyz;
            const float pCos = max(abs(dot(Ng, V)), 1e-3);
            // The local lamp light at the surface (scatter lobe, absorption light tint)
            vec3 lamps = vec3_splat(0.0);
            BRANCH if (pbrScatter > 0.0 || (pbrAbsorption.w > 0.0 && pbrPresence3.x > 0.0))
               lamps = pbr_scatter_light(v_tablePos, wNg, pbrPresence2.z) * (1.0 / PI);
            // Absorption (material absorption colour and strength): Beer-Lambert over the path through the plastic, in
            // thicknesses: 1 / cos of the refracted angle on a flat sheet, the light-guide path toward edge-on (curvature on)
            // and longer through a fillet, so edge-on and fillets read as thick tinted plastic
            BRANCH if (pbrAbsorption.w > 0.0)
            {
               const float sinR2 = (1.0 - pCos * pCos) * (1.0 / 2.25);
               const float path = max(inversesqrt(max(1.0 - sinR2, 1e-3)), refrGuide) + 0.5 * refrCurvExtra;
               vec3 absC = pbrAbsorption.rgb;
               // Absorption light tint (0..1, 0 = as is): the colour takes the hue of the local lamp light, keeping its own
               // luminance (chroma from the light: lamps / lum(lamps) x lum(colour), clamped to a transmittance); faded in
               // with the light so a dark spot keeps the user colour
               BRANCH if (pbrPresence3.x > 0.0)
               {
                  const float lL = dot(lamps, vec3(0.2126, 0.7152, 0.0722));
                  const vec3 tinted = saturate(lamps * (dot(absC, vec3(0.2126, 0.7152, 0.0722)) / max(lL, 1e-6)));
                  absC = mix(absC, tinted, pbrPresence3.x * smoothstep(0.0, 0.02, lL));
               }
               behind *= pow(max(absC, vec3_splat(1e-3)), vec3_splat(pbrAbsorption.w * path));
            }
            BRANCH if (pbrScatter > 0.0)
            {
               // Scatter lobe: light inside the plastic scatters toward the eye (white, the plastic's own milkiness), most
               // where the view goes through the sheet steeply (walls, edges seen edge-on) and in the fillets and bevels, and
               // where the wear mask (ORM alpha when an ORM map is bound) says so. Driven by the actual lights (GI bulbs,
               // flashers, inserts: the ramp blooms white next to a flasher) plus a little env/ambient: dark room = clear.
               const float wear = hasORMMap ? orm.w : 1.0;
               // look-through factor: 0 up to ~75 deg from the normal, full from ~86 deg (only edges and wall tops seen
               // nearly edge-on; the walls and floor seen from the player stay clear: Ben 10-31, too much on walls/floor)
               const float edge = saturate(((1.0 - pCos) / pCos - pbrScatterEdge0) * (1.0 / 9.5));
               const float kv = refractionParams.w > 0.0 ? v_refrCurv : 10.0 * pbr_curvature(Nv, v_tablePos).x;
               const float bend = saturate((kv - 0.3) * (1.0 / 1.7));
               const vec3 envIrr = texNoLod(tex_diffuse_env, ray_to_equirectangular_uv(wNg)).xyz * fenvEmissionScale_TexWidth.x + cAmbient_LightRange.xyz;
               vec3 lit = lamps + ((1.0 - pbrPresence2.z) * pbrPresence.y) * envIrr; // locality fades the ambient share
               // Scatter width: band weight exponent, 1 at width 1; larger (thinner band) at low width, more so where lit is low
               float bandE = 1.0;
               if (pbrPresence2.w < 1.0)
               {
                  const float litL = dot(lit, vec3(0.2126, 0.7152, 0.0722));
                  const float litN = litL / (litL + PRES_LIT_K);
                  const float invW = 1.0 / max(pbrPresence2.w, 0.05); // width 0 (no band) is zeroed below
                  bandE = invW * (1.0 + (invW - 1.0) * (1.0 - litN));
               }
               // Scatter saturation (0..2, 1 = as is): chroma of the light in the scatter and rim lobes (0 = grey of the same
               // luminance, 2 = boosted), after the width term (which reads only the luminance)
               if (pbrPresence3.y != 1.0)
               {
                  const float litY = dot(lit, vec3(0.2126, 0.7152, 0.0722));
                  lit = max(vec3_splat(litY) + (lit - litY) * pbrPresence3.y, vec3_splat(0.0));
               }
               const float bandOn = pbrPresence2.w > 0.0 ? 1.0 : 0.0; // scatter width 0 = no band (and no rim)
               const float edgeBend = bandOn * pbr_band_choke(max(edge, bend), bandE);
               // Dense edge: at the edge-on limit the look-through saturates into an opaque scatter surface (rough cut edge,
               // bevel at grazing) that replaces what lies behind; its reflections stay on top (dense, shiny)
               const float dense = (1.0 - smoothstep(PRES_DENSE_C1, PRES_DENSE_C0, pCos)) * saturate(pbrScatter * (1.0 / PRES_DENSE_FULL));
               behind = mix(behind, pbrPresence.w * (lamps + envIrr), dense); // all the local light: dark grey in a dark room
               spec += (pbrScatter * wear * (pbrPresence.x + (1.0 - pbrPresence.x) * edgeBend)) * lit;
               // Rim: the thin bright line along wall tops and fillets seen at an angle (light piped along the sheet by total
               // internal reflection comes out where the surface bends)
               // width < 1 chokes the band's flanks (exponent 1/width on the weight bend*edge below the core, the core keeps its
               // brightness); the scatter width exponent applies on top, so rim and scatter band choke together
               const float rimE = (1.0 / pbrPresence2.x) * bandE;
               const float rimW = bandOn * pbr_band_choke(bend * edge, rimE);
               spec += (pbrScatter * wear * pbrPresence.z * rimW) * lit;
               // The lit playfield mirrored in the underside and the walls: a mirror ray going down meets the playfield plane;
               // the refraction probe (the scene behind, lit) holds the playfield there, so it replaces the env's floor
               const vec3 wRg = reflect(-wV, wNg);
               BRANCH if (wRg.z < -0.05)
               {
                  const vec3 Q = v_tablePos + wRg * min(-v_tablePos.z / wRg.z, 600.0);
                  const vec4 pq = mul(mRotViewProj, vec4(Q - cCameraPosWorld, 1.0));
                  #if TEX_V_IS_UP
                  const vec2 uvQ = vec2(0.5, 0.5) + pq.xy * (0.5 / pq.w);
                  #else
                  const vec2 uvQ = vec2(0.5, 0.5) + vec2(pq.x, -pq.y) * (0.5 / pq.w);
                  #endif
                  const float down = smoothstep(0.05, 0.3, -wRg.z) * saturate(min(uvQ.x, min(uvQ.y, min(1.0 - uvQ.x, 1.0 - uvQ.y))) * 16.0) * step(0.0, pq.w);
                  const vec3 pf = texStereoLod(tex_pbr_refr, uvQ, roughness * pbrProbeLods.z).rgb;
                  #if defined(INSHADERSPEC)
                     spec += (W * coatIBL) * (pf - specSrc) * down;
                  #else
                     spec += (W * coatIBL) * pf * down;
                  #endif
               }
            }
         }
         gl_FragData[0] = vec4(color.rgb * alpha + behind * (1.0 - alpha) + spec, 1.0);
         if (pbrLightGrid2.w == 1.0)
            gl_FragData[0] = vec4(texStereoLod(tex_pbr_refr, gl_FragCoord.xy * w_h_height.xy, 0.0).rgb, 1.0);
         else if (pbrLightGrid2.w == 2.0)
            gl_FragData[0] = vec4(behind, 1.0);
         else if (pbrLightGrid2.w == 6.0) // curvature term at full depth: offset scale 1 (black) .. 4 (white)
            gl_FragData[0] = vec4(vec3_splat(refrCurvExtra / 3.0), 1.0);
         else if (pbrLightGrid2.w == 7.0) // smear axis (world |x|,|y|,|z| x anisotropy), black = none
            gl_FragData[0] = vec4(abs(refrSmear), 1.0);
         else if (pbrLightGrid2.w == 3.0)
         {
            #ifdef STEREO
               gl_FragData[0] = vec4(pbr_transmit(v_tablePos, wN, -wV, 0.0, gl_FragCoord, v_eye), 1.0);
            #else
               gl_FragData[0] = vec4(pbr_transmit(v_tablePos, wN, -wV, 0.0, gl_FragCoord), 1.0);
            #endif
         }
      }
      else
         // Premultiplied output (used with ONE/INVSRC_ALPHA blending): diffuse & emission are modulated by coverage while
         // specular stays unmodulated (reflects even on thin glass)
         gl_FragData[0] = vec4(color.rgb * alpha + spec, cover);
   #else
      gl_FragData[0] = vec4(color.rgb + spec, alpha);
   #endif

   #ifdef SIDEBAND
      #ifdef TRANSPARENT
         // Alpha-blended sideband attenuation (premultiplied blend): stored L_probe/W_probe/W fade by (1-cover)
         // while the mask in each attachment's alpha accumulates the coverage toward 1
         gl_FragData[1] = vec4(0.0, 0.0, 0.0, cover);
         gl_FragData[2] = vec4(0.0, 0.0, 0.0, cover);
      #else
         gl_FragData[1] = vec4(sideSrc, sideCoverage); // probe radiance (envmap fallback mix) + probe coverage (coat lobe when coated)
         gl_FragData[2] = vec4(fDisableLighting_top_below.x != 0.0 ? vec3_splat(0.0) : sideW, 1.0); // specular weight (0 for unlit) + PBR mask flag
      #endif
   #endif
}
