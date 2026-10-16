// license:GPLv3+

// PBR specular ray march (half resolution): marches a GGX importance sampled lobe jitter
// around the mirror direction against the depth prepass for pixels whose front most opaque
// surface is a PBR material (prepass attachment 0: xy = shaded normal, attachment 1:
// x = perceptual roughness, y = opaque PBR flag). The specular weight is not applied here
// but in the resolve pass (it needs the albedo which the prepass does not carry): this
// buffer keeps premultiplied (hit color * confidence, confidence). Legacy parts write
// flag = 0, so they fully mask the PBR SSR contribution.

#ifdef STEREO
$input v_texcoord0, v_eye
#else
$input v_texcoord0
#endif

#include "common.sh"


// w_h_height.xy contains inverse size of the destination (half resolution) texture
uniform vec4 w_h_height;

SAMPLER2DSTEREO(tex_fb_filtered,  0); // Scene framebuffer
SAMPLER2DSTEREO(tex_depth,        4); // Depth prepass
SAMPLER2D(tex_ao_dither,          5); // AO Dither
SAMPLER2DSTEREO(tex_normals,      6); // Prepass attachment 0: xy = shaded normal
SAMPLER2DSTEREO(tex_pbr_data,    10); // Prepass attachment 1: x = roughness, y = opaque PBR flag

#ifdef STEREO
	uniform mat4 matProj[2];
	uniform mat4 matProjInv[2];
#else
	uniform mat4 matProj;
	uniform mat4 matProjInv;
#endif

#include "ssr_common.sh"


void main()
{
	const vec2 u = v_texcoord0;

	const float depth0 = texStereoNoLod(tex_depth, u).x;
	BRANCH if((depth0 == 1.0) || (depth0 == 0.0)) // early out if depth too large (=BG) or too small (=DMD,etc)
	{
		gl_FragColor = vec4_splat(0.0); // premultiplied march buffer: no coverage
		return;
	}

	const vec4 prepass = texStereoNoLod(tex_normals, u);
	const vec2 pbrData = texStereoNoLod(tex_pbr_data, u).xy;
	BRANCH if(pbrData.y < 0.5) // not an opaque PBR pixel: the PBR SSR contribution is masked out
	{
		gl_FragColor = vec4_splat(0.0);
		return;
	}
	const float pbrRoughness = pbrData.x;

	// View space fragment position & surface normal (shaded normal from the prepass)
	#ifdef STEREO
		const vec3 posV = uvDepthToView(u, depth0, v_eye);
	#else
		const vec3 posV = uvDepthToView(u, depth0);
	#endif
	const vec3 normal = vec3(prepass.xy, sqrt(saturate(1.0 - dot(prepass.xy, prepass.xy))));
	const vec3 V = -normalize(posV); // toward the camera (view space: camera at origin)
	const vec3 R = reflect(-V, normal); // reflected incident direction, pointing into the scene

	// 64 is the hardcoded dither texture size for AOdither.bmp
	const vec3 dither = texNoLod(tex_ao_dither, u/(64.0*w_h_height.xy)).xyz;

	// Two rays jittered around the mirror direction, with a GGX importance sampled lobe scaled by
	// the material roughness (the unused blue noise channels provide the per ray offsets): halves
	// the hit/miss variance between neighbors.
	const vec3 t1 = normalize(cross(R, abs(R.z) < 0.9 ? vec3(0.,0.,1.) : vec3(1.,0.,0.)));
	const vec3 t2 = cross(R, t1);
	vec4 acc = vec4_splat(0.0);
	UNROLL for(int ray = 0; ray < 2; ++ray)
	{
		const vec2 Xi = ray == 0 ? dither.xy : vec2(dither.y, fract(dither.x + 0.5));
		const float a = pbrRoughness * pbrRoughness;
		const float phi = 2.0 * PI * Xi.x;
		const float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
		const float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
		const vec3 H = normalize(sinTheta * cos(phi) * t1 + sinTheta * sin(phi) * t2 + cosTheta * normal);
		const vec3 Rj = reflect(-V, H);
		BRANCH if(dot(Rj, normal) > 0.0) // jittered ray must not dive below the surface
		{
			#ifdef STEREO
				acc += ssrRayMarch(posV, Rj, normal, fract(dither.z + float(ray) * 0.5), v_eye);
			#else
				acc += ssrRayMarch(posV, Rj, normal, fract(dither.z + float(ray) * 0.5));
			#endif
		}
	}
	gl_FragColor = acc;
}
