// license:GPLv3+

#ifdef STEREO
$input v_texcoord0, v_eye
#else
$input v_texcoord0
#endif

#include "common.sh"

SAMPLER2DSTEREO(tex_ssr_lowres, 3); // Half resolution premultiplied raymarched specular

void main()
{
   // Debug view of the premultiplied specular march buffer: un-premultiply
   // (rgb = hitColor * confidence, a = confidence) to show the raw hit radiance
   const vec4 s = texStereoNoLod(tex_ssr_lowres, v_texcoord0);
   const vec3 col = s.a > 0.001 ? s.rgb / s.a : vec3_splat(0.0);
   gl_FragColor = vec4(FBGamma(saturate(col)), 1.0);
}
