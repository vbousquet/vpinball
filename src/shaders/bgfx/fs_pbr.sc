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

#ifdef STEREO
    uniform mat4 matView[2];
    uniform mat4 matWorldView[2];
    uniform mat4 matWorldViewInverseTranspose[2];
    #define mView                      matView[int(round(v_eye))]
    #define mWorldView                 matWorldView[int(round(v_eye))]
    #define mWorldViewInverseTranspose matWorldViewInverseTranspose[int(round(v_eye))]
#else
    uniform mat4 matView;
    uniform mat4 matWorldView;
    uniform mat4 matWorldViewInverseTranspose;
    #define mView                      matView
    #define mWorldView                 matWorldView
    #define mWorldViewInverseTranspose matWorldViewInverseTranspose
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
