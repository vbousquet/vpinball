// license:GPLv3+

// Debug views of the depth prepass buffers, used by the renderer inspection:
// - DEPTH: the prepass depth attachment (near = white, far/cleared = black)
// - NORMAL: shaded view space normals from attachment 0 (xy, z reconstructed, viewer facing)
// - MOTION: camera/object motion vectors from attachment 0 (zw, UV displacement, centered on 0.5)

#ifdef STEREO
$input v_texcoord0, v_eye
#else
$input v_texcoord0
#endif

#include "common.sh"

#if defined(DEPTH)
SAMPLER2DSTEREO(tex_depth, 4); // Depth prepass depth attachment
#elif defined(NORMAL) || defined(MOTION)
SAMPLER2DSTEREO(tex_normals, 6); // Prepass attachment 0: xy = shaded normal, zw = motion vectors
#endif

void main()
{
#if defined(DEPTH)
   const float d = texStereoNoLod(tex_depth, v_texcoord0).x;
   gl_FragColor = vec4(FBGamma(saturate(vec3_splat(1.0 - d))), 1.0);
#elif defined(NORMAL)
   const vec2 n = texStereoNoLod(tex_normals, v_texcoord0).xy;
   const vec3 N = vec3(n, -sqrt(saturate(1.0 - dot(n, n))));
   gl_FragColor = vec4(FBGamma(saturate(N * 0.5 + 0.5)), 1.0);
#else // MOTION
   const vec2 mv = texStereoNoLod(tex_normals, v_texcoord0).zw;
   gl_FragColor = vec4(FBGamma(saturate(vec3(mv * 20.0 + 0.5, 0.5))), 1.0);
#endif
}
