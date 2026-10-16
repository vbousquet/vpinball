// license:GPLv3+

// Physically based material shader (Forward+):
// - base color / alpha from tex_base_color * cBase_Alpha * staticColor_Alpha
// - tangent space normal mapping through tex_base_normalmap
// - packed ORM (occlusion/roughness/metallic) from tex_pbr_orm multiplied by the material factors
// - emission from tex_pbr_emission * pbrEmission
// - indirect diffuse lighting from the prefiltered irradiance envmap (tex_diffuse_env)
// - indirect specular from the envmap mip chain (tex_env) weighted by the split-sum approximation
//   (DFG LUT + multiscattering compensation + Lagarde's specular occlusion)
// - specular sideband output for the resolve pass (attachment 1: W/Wt, attachment 2: L_probe)
// - TRANSPARENT variant outputs premultiplied radiance (ONE/INVSRC_ALPHA blend state)
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

#ifdef STEREO
    uniform mat4 matView[2];
    uniform mat4 matWorldView[2];
    uniform mat4 matWorldViewInverseTranspose[2];
    uniform vec4 cameraPosWorld[2];
    #define mView                      matView[int(round(v_eye))]
    #define mWorldView                 matWorldView[int(round(v_eye))]
    #define mWorldViewInverseTranspose matWorldViewInverseTranspose[int(round(v_eye))]
    #define cCameraPosWorld            cameraPosWorld[int(round(v_eye))].xyz
#else
    uniform mat4 matView;
    uniform mat4 matWorldView;
    uniform mat4 matWorldViewInverseTranspose;
    uniform vec4 cameraPosWorld;
    #define mView                      matView
    #define mWorldView                 matWorldView
    #define mWorldViewInverseTranspose matWorldViewInverseTranspose
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
uniform vec4 pbrMapFlags; // x: ORM map bound, y: emission map bound
#define hasORMMap      (pbrMapFlags.x != 0.0)
#define hasEmissionMap (pbrMapFlags.y != 0.0)

uniform vec4 pbrLightGrid;  // xy: cluster grid origin (table pos), z: inverse cell size, w: cell count x
uniform vec4 pbrLightGrid2; // x: cell count y, y: light count

#include "ball_shadows.sh"

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

// Forward+ clustered direct lighting. The CPU builds a world-space XY grid over the table bounds each
// frame and packs it into tex_pbr_lights (RGBA32F, 256 texels wide, texel fetch):
// [0,512)    light records, 2 texels each: (pos.xyz, range) then (radiance.rgb, mode)
// [512,1024) grid cells: (start, count) into the index list
// [1024,+)   light index list
// World-space evaluation (v_tablePos): view independent, a single grid serves both stereo eyes.
vec4 pbr_light_texel(const int idx)
{
   return texelFetch(tex_pbr_lights, ivec2(idx & 255, idx >> 8), 0);
}

vec3 pbr_direct_light(const vec3 wN, const vec3 wV, const vec3 wPos, const vec3 diffuse, const vec3 F0, const float roughness)
{
   const vec2 cellf = clamp((wPos.xy - pbrLightGrid.xy) * pbrLightGrid.z, vec2_splat(0.0),
      vec2(pbrLightGrid.w - 1.0, pbrLightGrid2.x - 1.0));
   const vec4 cell = pbr_light_texel(512 + int(cellf.y) * int(pbrLightGrid.w) + int(cellf.x));
   const int start = int(cell.x);
   const int count = min(int(cell.y), 16);
   const float a = roughness * roughness;
   const float a2 = a * a;
   const float NdotV = saturate(dot(wN, wV));
   vec3 acc = vec3_splat(0.0);
   for (int i = 0; i < count; i++)
   {
      const int li = int(pbr_light_texel(1024 + start + i).x);
      const vec4 lPos = pbr_light_texel(li * 2);
      const vec3 lCol = pbr_light_texel(li * 2 + 1).rgb;
      const vec3 dl = lPos.xyz - wPos;
      const float dist2 = dot(dl, dl);
      // Windowed inverse square attenuation: (1 - (d/r)^4)^2 / d^2 gives a smooth cutoff at the light range
      const float rangeRatio = dist2 / (lPos.w * lPos.w);
      const float x = saturate(1.0 - rangeRatio * rangeRatio);
      if (x <= 0.0)
         continue;
      const vec3 l = dl * inversesqrt(dist2);
      const float NdotL = saturate(dot(wN, l));
      if (NdotL <= 0.0)
         continue;
      const vec3 h = normalize(l + wV);
      const float NdotH = saturate(dot(wN, h));
      const float VdotH = saturate(dot(wV, h));
      // GGX D + Schlick F + height-correlated Smith visibility (already includes the 1/(4 NdotL NdotV) term)
      const float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
      const float D = a2 / (PI * denom * denom);
      const vec3 F = F0 + (vec3_splat(1.0) - F0) * pow(1.0 - VdotH, 5.0);
      const float gv = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
      const float gl = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
      const float G = 0.5 / max(gv + gl, 1e-6);
      acc += (diffuse * (vec3_splat(1.0) - F) * (1.0 / PI) + D * F * G) * (lCol * (x * x / dist2)) * NdotL;
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

   // Occlusion / roughness / metallic
   vec3 orm = vec3(1.0, 1.0, 1.0);
   BRANCH if (hasORMMap)
   {
      if (noMipMaps)
         orm = texNoLod(tex_pbr_orm, v_texcoord0).xyz;
      else
         orm = texture2D(tex_pbr_orm, v_texcoord0).xyz;
   }
   const float metallic = saturate(orm.z * pbrMetallicFactor);
   const float roughness = clamp(orm.y * pbrRoughnessFactor, 0.045, 1.0);
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

   const vec3 albedo = base.rgb;
   const vec3 F0 = mix(vec3_splat(0.04), albedo, metallic);
   const vec3 diffuse = albedo * (1.0 - metallic);

   // Indirect specular weight (envmap evaluated in-shader for INSHADERSPEC, otherwise deferred to the resolve pass)
   vec3 envSpec;
   #ifdef STEREO
      const vec3 W = pbr_specular_weight(N, V, F0, roughness, occlusion, v_eye, envSpec);
   #else
      const vec3 W = pbr_specular_weight(N, V, F0, roughness, occlusion, envSpec);
   #endif
   vec3 color = vec3_splat(0.0);
   vec3 spec = vec3_splat(0.0);

   if (fDisableLighting_top_below.x != 0.0)
   { // Lighting disabled: fully emissive backward compatible behavior (unlit, twice the albedo)
      color += 2.0 * albedo;
   }
   else
   {
      const vec3 kd = saturate(vec3_splat(1.0) - W);
      #ifdef STEREO
         color += pbr_indirect_diffuse(N, diffuse, kd, occlusion, v_eye);
      #else
         color += pbr_indirect_diffuse(N, diffuse, kd, occlusion);
      #endif
      #if defined(INSHADERSPEC)
         spec = W * envSpec;
      #endif

      // Forward+ clustered direct lighting (world space: the light grid is built over the table XY plane)
      BRANCH if (pbrLightGrid2.y > 0.0)
      {
         const vec3 wN = mul(vec4(N, 0.0), mView).xyz; // trafo back to world (orthonormal, no renormalize needed)
         const vec3 wV = normalize(cCameraPosWorld - v_tablePos);
         color += pbr_direct_light(wN, wV, v_tablePos, diffuse, F0, roughness);
      }
   }

   BRANCH if (lightCenter_doShadow.w != 0.)
   {
      const vec3 light_dir = v_tablePos.xyz - lightCenter_doShadow.xyz;
      const float light_dist = length(light_dir);
      color.rgb *= get_light_ball_shadow(lightCenter_doShadow.xyz, light_dir, light_dist);
   }

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
      // Premultiplied output (used with ONE/INVSRC_ALPHA blending): diffuse & emission are modulated
      // by coverage while the specular contribution stays unmodulated (reflects even on thin glass)
      gl_FragData[0] = vec4(color.rgb * alpha + spec, alpha);
   #else
      gl_FragData[0] = vec4(color.rgb + spec, alpha);
   #endif

   #ifdef SIDEBAND
      #ifdef TRANSPARENT
         // Alpha-blended sideband attenuation (premultiplied blend): stored L_probe/W_probe/W fade by (1-alpha)
         // while the mask in each attachment's alpha accumulates the coverage toward 1
         gl_FragData[1] = vec4(0.0, 0.0, 0.0, alpha);
         gl_FragData[2] = vec4(0.0, 0.0, 0.0, alpha);
      #else
         gl_FragData[1] = vec4(envSpec, 0.0); // L_probe radiance (envmap fallback until probes blend in) + W_probe coverage
         gl_FragData[2] = vec4(W, 1.0); // specular weight + PBR mask flag
      #endif
   #endif
}
