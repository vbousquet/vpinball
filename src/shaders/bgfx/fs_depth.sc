// license:GPLv3+

// Fragment shader of the depth prepass:
// - base variant only performs alpha testing/discards, as color writes are masked out
// - OUT_NORMAL variant outputs the view space shaded normal xy (normal mapping applied), with the
//   z component reconstructed by consumers (normal is flipped to always face the viewer)
// - PBR variant additionally outputs the material perceptual roughness in z and the opaque
//   PBR flag in w (the PBR specular march is gated by these; non PBR pixels write 0)

// Inputs must exactly match the vertex shader outputs (vs_depth & vs_ball)
$input v_worldPos, v_normal, v_texcoord0
#ifdef STEREO
    $input v_eye
#endif
#ifdef CLIP
    $input v_clipDistance
#endif

#include "common.sh"

#if defined(OUT_NORMAL) && defined(TEX)
SAMPLER2D      (tex_base_normalmap, 4); // normal map
#endif
#if defined(OUT_NORMAL) && defined(PBR)
SAMPLER2D      (tex_pbr_orm, 8); // packed occlusion/roughness/metallic map
uniform vec4 pbrFactors;
#define pbrRoughnessFactor (pbrFactors.z)
uniform vec4 pbrMapFlags; // x: ORM map bound, y: emission map bound
#define hasORMMap      (pbrMapFlags.x != 0.0)
#endif
#ifdef AT
SAMPLER2D      (tex_base_color, 0); // base color
uniform vec4 alphaTestValue; // FIXME Actually float but extended to vec4 for BGFX (and we could use the builtin)
#endif

#if defined(AT) || (defined(OUT_NORMAL) && (defined(TEX) || defined(PBR)))
uniform vec4 u_basic_shade_mode;
#define noMipMaps           (u_basic_shade_mode.w != 0.0)
#endif

#if defined(OUT_NORMAL) && defined(TEX)
#ifdef STEREO
    uniform mat4 matWorldViewInverseTranspose[2];
    #define mWorldViewInverseTranspose matWorldViewInverseTranspose[int(round(v_eye))]
#else
    uniform mat4 matWorldViewInverseTranspose;
    #define mWorldViewInverseTranspose matWorldViewInverseTranspose
#endif

uniform vec4 objectSpaceNormalMap; // float extended to vec4 for BGFX FIXME float uniforms are not supported: group or declare as vec4

#define doNormalMapping     (u_basic_shade_mode.y != 0.0)

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
#endif

#if !defined(AT) && !defined(CLIP)
EARLY_DEPTH_STENCIL
#endif
void main() {
   #ifdef CLIP
   if (v_clipDistance < 0.0)
      discard;
   #endif

   #ifdef AT
      vec4 pixel;
      if (noMipMaps)
         pixel = texNoLod(tex_base_color, v_texcoord0);
      else
         pixel = texture2D(tex_base_color, v_texcoord0);
      if (pixel.a <= alphaTestValue.x)
         discard; //stop the pixel shader if alpha test should reject pixel
   #endif

   #ifdef OUT_NORMAL
      vec3 N = normalize(v_normal);
      #if defined(TEX)
         BRANCH if (doNormalMapping)
         {
            const vec3 V = -normalize(v_worldPos);
            vec3 tn;
            if (noMipMaps)
               tn = texNoLod(tex_base_normalmap, v_texcoord0).xyz * (255./127.) - (128./127.);
            else
               tn = texture2D(tex_base_normalmap, v_texcoord0).xyz * (255./127.) - (128./127.);

            BRANCH if (objectSpaceNormalMap.x != 0.0)
            { // Object space: this matches the object space, +X +Y +Z, export/baking in Blender with our trafo setup
               N = normalize(mul(mWorldViewInverseTranspose, vec4(tn.x, tn.y, -tn.z, 0.0)).xyz);
            }
            else
            { // Tangent space
               N = normalize(mul(TBN_trafo(N, V, v_texcoord0), tn).xyz);
            }
         }
      #endif
      // Flip the normal to always face the viewer so that z can be reconstructed from xy
      if (N.z < 0.0)
         N = -N;
      #ifdef PBR
         // Same perceptual roughness as the PBR shading path, flagging the pixel as opaque PBR
         float roughness = pbrRoughnessFactor;
         BRANCH if (hasORMMap)
         {
            if (noMipMaps)
               roughness *= texNoLod(tex_pbr_orm, v_texcoord0).y;
            else
               roughness *= texture2D(tex_pbr_orm, v_texcoord0).y;
         }
         gl_FragColor = vec4(N.xy, clamp(roughness, 0.045, 1.0), 1.0);
      #else
         gl_FragColor = vec4(N.xy, 0.0, 0.0);
      #endif
   #else
      gl_FragColor = vec4_splat(0.0);
   #endif
}
