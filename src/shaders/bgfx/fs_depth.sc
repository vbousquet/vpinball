// license:GPLv3+

// Fragment shader of the depth prepass:
// - base variant only performs alpha testing/discards, as color writes are masked out
// - OUT_NORMAL variant outputs the view space shaded normal xy (normal mapping applied), with the
//   z component reconstructed by consumers (normal is flipped to always face the viewer)
// - OUT_NORMAL+MOTION additionally outputs the camera motion vector in zw: the screen space UV
//   displacement of the pixel between the previous and the current frame, consumed by the AO
//   temporal accumulation when the camera is dynamic (VR or headtracking)

// Inputs must exactly match the vertex shader outputs (vs_depth & vs_ball)
$input v_worldPos, v_normal, v_texcoord0
#ifdef STEREO
    $input v_eye
#endif
#ifdef CLIP
    $input v_clipDistance
#endif
#ifdef MOTION
    $input v_clipPos, v_prevClipPos
#endif

#include "common.sh"

#if defined(OUT_NORMAL) && defined(TEX)
SAMPLER2D      (tex_base_normalmap, 4); // normal map
#endif
#ifdef AT
SAMPLER2D      (tex_base_color, 0); // base color
uniform vec4 alphaTestValue; // FIXME Actually float but extended to vec4 for BGFX (and we could use the builtin)
#endif

#if defined(AT) || (defined(OUT_NORMAL) && defined(TEX))
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
      #ifdef MOTION
         // Screen space UV displacement between the previous and the current frame camera transforms,
         // evaluated per pixel (the interpolated clip positions are divided here)
         vec2 motion = vec2(0.0, 0.0);
         BRANCH if (v_prevClipPos.w > 0.0)
         {
            const vec2 delta = v_clipPos.xy / v_clipPos.w - v_prevClipPos.xy / v_prevClipPos.w;
            #if TEX_V_IS_UP
               motion = 0.5 * delta;
            #else
               motion = vec2(0.5 * delta.x, -0.5 * delta.y);
            #endif
         }
         gl_FragColor = vec4(N.xy, motion);
      #else
         gl_FragColor = vec4(N.xy, 0.0, 1.0);
      #endif
   #else
      gl_FragColor = vec4_splat(0.0);
   #endif
}
