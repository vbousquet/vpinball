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

$input v_worldPos, v_tablePos, v_normal, v_texcoord0
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
uniform vec4 pbrMapFlags; // x: ORM map bound, y: emission map bound, z: thin surface transmission (material wrap lighting)
#define hasORMMap      (pbrMapFlags.x != 0.0)
#define hasEmissionMap (pbrMapFlags.y != 0.0)
#define pbrTransmission (pbrMapFlags.z)

uniform vec4 pbrCoat; // x: clear coat strength (0 = off), y: clear coat perceptual roughness
#define coatStrength  (pbrCoat.x)
#define coatRoughness (pbrCoat.y)

uniform vec4 pbrLightGrid;  // xy: cluster grid origin (table pos), z: inverse cell size, w: cell count x
uniform vec4 pbrLightGrid2; // x: cell count y, y: light count, z: specular AA strength (VPX_SPEC_AA, 1 = default, 0 = off)

uniform vec4 pbrProbes0; // reflection plane of the first bound probe (dot(n,p) + w = 0)
uniform vec4 pbrProbes1; // reflection plane of the second bound probe
uniform vec4 pbrProbeParams; // x/y: influence ranges of probe0/1 (0 = unbound), z: SSR/probe blend factor, w: refraction probe bound
uniform vec4 pbrProbeLods;   // x/y: max mip level of probe0/1, z: max mip level of the refraction probe (roughness-dependent lod)

SAMPLER2DARRAY(tex_vsm_pool, 3);     // VSM physical page pool: depth texture array, one 128^2 page per layer
SAMPLER2D     (tex_vsm_pagetables, 5); // VSM page table: physical layer+1 per virtual page (0 = not resident)
SAMPLER2DARRAY(tex_vsm_trans, 6);    // VSM page pool transmittance: translucent casters' absorbance (rgb) and nearness (a)

uniform mat4 matShadowVP[32];   // ortho virtual map view-projection per shadowed light
uniform vec4 pbrVSMParams[32];  // x: page-table texel base, y: pages across (face grid for cube), z: pages down, w: ortho depth range
uniform vec4 pbrVSMBox[32];     // x/y: world->virtual-uv scale (1/ortho box extents), z: ortho near depth (used CPU side)
uniform vec4 pbrShadowParams;   // x: 1/page size, y: slope-scaled depth bias, z/w: page render linear depth start + 1/range (SHADOW_TRANS)

#ifdef TRANSPARENT
uniform vec4 refractionTint_thickness;
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

mat3 TBN_trafo(const vec3 N, const vec3 V, const vec2 uv)
{
   // derivatives: edge vectors for tri-pos and tri-uv
   const vec3 dpx = dFdx(V);
   const vec3 dpy = dFdy(V);
   #if TEX_V_IS_UP
      const vec2 duvx = dFdx(uv);
      const vec2 duvy = dFdy(uv);
   #else
      const vec2 duvx = dFdx(vec2(uv.x, -uv.y));
      const vec2 duvy = dFdy(vec2(uv.x, -uv.y));
   #endif

   // solve linear system
   const vec3 dp2perp = cross(N, dpy);
   const vec3 dp1perp = cross(dpx, N);
   const vec3 T = dp2perp * duvx.x + dp1perp * duvy.x;
   const vec3 B = dp2perp * duvx.y + dp1perp * duvy.y;

   // construct scale-invariant transformation
   return mat3(T, B, N * sqrt( max(dot(T,T), dot(B,B)) )); // inverse scale, as will be normalized anyhow later-on (to save some mul's)
}

vec3 pbr_normal_map(const vec3 N, const vec3 V, const vec2 uv)
{
   vec3 tn;
   if (noMipMaps)
      tn = texNoLod(tex_base_normalmap, uv).xyz * (255./127.) - (128./127.);
   else
      tn = texture2D(tex_base_normalmap, uv).xyz * (255./127.) - (128./127.);

   // PBR normal maps are always tangent space
   return normalize(mul(TBN_trafo(N, V, uv), tn).xyz);
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
   envSpec = texture2DLod(tex_env, ray_to_equirectangular_uv(R), lod).xyz * fenvEmissionScale_TexWidth.x;
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
vec3 pbr_transmit(const vec3 wPos, const vec3 wN, const vec3 wV, const float roughness, const vec4 fragCoord, const float v_eye)
#else
vec3 pbr_transmit(const vec3 wPos, const vec3 wN, const vec3 wV, const float roughness, const vec4 fragCoord)
#endif
{
   const vec3 R = refract(-wV, wN, 1.0 / 1.5); // n1 = 1.0 (air), n2 = 1.5 (plastic), eta = n1 / n2
   const vec3 refracted_pos = wPos + refractionTint_thickness.w * R; // Shift ray by the thickness of the material
   const vec4 proj = mul(mRotViewProj, vec4(refracted_pos - cCameraPosWorld, 1.0));
   #if TEX_V_IS_UP
   vec2 uv = vec2(0.5, 0.5) + proj.xy * (0.5 / proj.w);
   #else
   vec2 uv = vec2(0.5, 0.5) + vec2(proj.x, -proj.y) * (0.5 / proj.w);
   #endif
   // A refracted sample that lands on something nearer than this fragment (a wire or rail in front of the ramp) is not
   // what lies behind it: fall back to the unrefracted pixel (as the legacy refraction does)
   if (texStereo(tex_probe_depth, uv).x < fragCoord.z)
      uv = fragCoord.xy * w_h_height.xy;
   return refractionTint_thickness.rgb * texStereoLod(tex_pbr_refr, uv, roughness * pbrProbeLods.z).rgb;
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
vec3 pbr_direct_light(const vec3 wN, const vec3 wNc, const vec3 wNg, const vec3 wV, const vec3 wPos, const vec3 diffuse, const vec3 F0, const float roughness, const float alpha, const float transmission, out vec3 specOut)
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
      if (x <= 0.0)
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
      const float gFade = pbrTransmission > 0.0 || alpha < 1.0 ? 1.0 : saturate(gNdotLraw * 20.0);
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
               float shadowAcc = 0.0;
               UNROLL for (int py = -1; py <= 1; py++)
               {
                  UNROLL for (int px = -1; px <= 1; px++)
                  {
                     const vec2 tuv = suv + vec2(float(px), float(py)) * filterRadius;
                     const float d = pbr_vsm_depth(vsmP.x, grid, tuv);
                     shadowAcc += d < 0.0 ? 1.0 : step(sc.z - bias, d);
                  }
               }
               // Translucent casters: one bilinear tap at the centre (receiver 2 VPU behind them at least)
               const float nearness = 1.0 - sc.z + 2.0 / max(vsmP.w, 1.0);
               shadow = shadowAcc * (1.0 / 9.0) * pbr_vsm_tint(pbr_vsm_index(vsmP.x, grid, suv), fract(suv * grid), nearness);
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
            // the face's virtual texel size is 1/(faceGrid * PAGE_SIZE) of a direction unit
            const float invFaceRes = 1.0 / (faceGrid * 128.0);
            const float filterRadius = clamp(lightRadius * worldGap / max(blocker, 1.0) * invFaceRes * 2.0, invFaceRes, 8.0 * invFaceRes);
            const float bias = min(0.5 + 2.0 * (1.0 - NdotLs) / max(NdotLs, 0.05), 8.0);
            const vec3 upRef = abs(ldir.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
            const vec3 tX = normalize(cross(upRef, ldir));
            const vec3 tY = cross(ldir, tX);
            float shadowAcc = 0.0;
            UNROLL for (int py = -1; py <= 1; py++)
            {
               UNROLL for (int px = -1; px <= 1; px++)
               {
                  const vec3 tdir = normalize(ldir + (float(px) * tX + float(py) * tY) * filterRadius);
                  const vec4 tl = pbr_shadow_cube_lookup(tdir);
                  const float tapNdc = pbr_vsm_cube_depth(vsmP.x, faceGrid, tl);
                  const float tapBlocker = tapNdc < 0.0 ? -1.0 : zN * zF / (zF - tapNdc * (zF - zN));
                  shadowAcc += tapBlocker < 0.0 ? 1.0 : step(dist * tl.w - bias, tapBlocker);
               }
            }
            const int g = int(faceGrid + 0.5);
            const vec2 pg = floor(cl.xy * vec2_splat(faceGrid));
            const int cidx = int(vsmP.x + 0.5) + int(cl.z + 0.5) * g * g + int(pg.y) * g + int(pg.x);
            shadow = shadowAcc * (1.0 / 9.0) * pbr_vsm_tint(cidx, fract(cl.xy * vec2_splat(faceGrid)), 1.0 - (dist * cl.w - 2.0) / zF);
         }
      }
      BRANCH if (shadowMode == 1 || shadowMode >= 3) // RAYTRACED_BALL_SHADOWS or PBR ball shadow tiers
         shadow *= get_light_ball_shadow(lPos.xyz, -dl, sqrt(dist2));
      // Thin surface translucency: the transmitted fraction (pbrTransmission) of the light arriving on the far side leaves
      // this side diffusely, filtered by the surface colour (printed ink: dark ink passes little, white flood glows). Light
      // ink scatters wider: a wrapped lobe lets grazing light through, spreading the glow away from the bulb.
      const float wrap = 0.4 * dot(diffuse, vec3(0.2126, 0.7152, 0.0722));
      const float transmitted = NdotLraw < 0.0 ? transmission * saturate((wrap - NdotLraw) / (1.0 + wrap)) : 0.0;
      acc += (gFade * shadow) * (diffuse * ((vec3_splat(1.0) - F) * (NdotL * coatAtt) + transmitted * coatExit) * (1.0 / PI) * (lCol * (x * x / dist2)));
      specOut += (gFade * shadow) * spec;
   }
   return acc;
}

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
      N = pbr_normal_map(N, -V, v_texcoord0);
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

   // Geometric normal (no normal map), facing the viewer: it tells which side of the surface a direct light is on
   vec3 Ng = normalize(v_normal);
   if (dot(Ng, V) < 0.0)
      Ng = -Ng;

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
         color += pbr_direct_light(wN, wNc, mul(vec4(Ng, 0.0), mView).xyz, wV, v_tablePos, diffuse, F0, roughness, base.a * staticColor_Alpha.a, pbrTransmission * orm.w, directSpec);
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
         const vec3 wN = mul(vec4(N, 0.0), mView).xyz; // trafo back to world (orthonormal, no renormalize needed)
         const vec3 wV = normalize(cCameraPosWorld - v_tablePos);
         // Blur (a fraction of the probe's mip chain): the roughness, plus a little where the normal map bends away from the
         // surface (scuffs), capped so the probe never collapses to its average colour
         const float blur = min(roughness + 0.1 * (1.0 - saturate(dot(N, Ng))), 0.08);
         #ifdef STEREO
            const vec3 behind = pbr_transmit(v_tablePos, wN, wV, blur, gl_FragCoord, v_eye) * (staticColor_Alpha.rgb * coatIBL);
         #else
            const vec3 behind = pbr_transmit(v_tablePos, wN, wV, blur, gl_FragCoord) * (staticColor_Alpha.rgb * coatIBL);
         #endif
         gl_FragData[0] = vec4(color.rgb * alpha + behind * (1.0 - cover) + spec, 1.0);
         if (pbrLightGrid2.w == 1.0)
            gl_FragData[0] = vec4(texStereoLod(tex_pbr_refr, gl_FragCoord.xy * w_h_height.xy, 0.0).rgb, 1.0);
         else if (pbrLightGrid2.w == 2.0)
            gl_FragData[0] = vec4(behind, 1.0);
         else if (pbrLightGrid2.w == 3.0)
         {
            #ifdef STEREO
               gl_FragData[0] = vec4(pbr_transmit(v_tablePos, wN, wV, 0.0, gl_FragCoord, v_eye), 1.0);
            #else
               gl_FragData[0] = vec4(pbr_transmit(v_tablePos, wN, wV, 0.0, gl_FragCoord), 1.0);
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
