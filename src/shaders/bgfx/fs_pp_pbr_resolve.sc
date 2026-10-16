// license:GPLv3+

// PBR specular resolve: composites the deferred indirect specular of opaque PBR pixels.
// The scene pass writes the resolve inputs into the sideband attachments (attachment 1:
// blended planar probe radiance L_probe + coverage W_probe, attachment 2: specular weight
// W + PBR mask). A half resolution buffer holds the raymarched screen space radiance
// (premultiplied L_ssr * alpha, alpha = confidence). The resolve outputs:
//    scene + W * lerp(L_probe, L_ssr, alpha')
//    alpha' = alpha_ssr * (1 - mu * W_probe)
// i.e. SSR wins where it hits, probes where SSR misses, with the envmap fallback already
// folded into L_probe by the scene shader. Non PBR pixels are passed through unchanged.
// The march inputs (shaded normals, roughness, opaque PBR flag) come from the depth
// prepass: attachment 0 xy = normal, attachment 1 x = roughness, y = PBR flag. The prepass
// flag is also the resolve mask: unlike the sideband (written only by PBR draws), it is
// written by every opaque part, so a non PBR surface drawn over a PBR one correctly masks
// it out.

#ifdef STEREO
$input v_texcoord0, v_eye
#else
$input v_texcoord0
#endif

#include "common.sh"


// w_h_height.xy contains inverse size of the destination (full resolution) texture
uniform vec4 w_h_height;

uniform vec4 pbrProbeParams; // z: mu, SSR suppression weight against probe coverage

SAMPLER2DSTEREO(tex_fb_filtered, 0); // Scene framebuffer
SAMPLER2DSTEREO(tex_ssr_lowres,  3); // Half resolution premultiplied raymarched specular
SAMPLER2DSTEREO(tex_depth,       4); // Depth prepass
SAMPLER2DSTEREO(tex_normals,     6); // Prepass attachment 0: xy = shaded normal
SAMPLER2DSTEREO(tex_pbr_data,   10); // Prepass attachment 1: x = roughness, y = opaque PBR flag
SAMPLER2DSTEREO(tex_pbr_att1,    8); // Sideband attachment 1: rgb = L_probe, a = W_probe
SAMPLER2DSTEREO(tex_pbr_att2,    9); // Sideband attachment 2: rgb = W, a = PBR mask

#ifdef STEREO
	uniform mat4 matProjInv[2];
	uniform mat4 matProj[2]; // referenced by the shared march helpers (unused in the resolve: stripped)
#else
	uniform mat4 matProjInv;
	uniform mat4 matProj; // referenced by the shared march helpers (unused in the resolve: stripped)
#endif

#include "ssr_common.sh"


void main()
{
	const vec2 u = v_texcoord0;

	const vec3 color0 = texStereoNoLod(tex_fb_filtered, u).xyz; // original pixel

	BRANCH if(texStereoNoLod(tex_pbr_data, u).y < 0.5) // frontmost opaque surface is not PBR: pass the scene color through
	{
		gl_FragColor = vec4(color0, 1.0);
		return;
	}
	const vec4 att2 = texStereoNoLod(tex_pbr_att2, u);
	const vec3 W = att2.rgb;
	const vec4 att1 = texStereoNoLod(tex_pbr_att1, u);

	const float depth0 = texStereoNoLod(tex_depth, u).x;
	BRANCH if((depth0 == 1.0) || (depth0 == 0.0)) // outside of the prepass: probe/envmap result only
	{
		gl_FragColor = vec4(color0 + W * att1.rgb, 1.0);
		return;
	}

	// 2x2 joint bilateral upsample of the half res march buffer (same policy as the SSR upsample)
	#ifdef STEREO
		const vec3 posV = uvDepthToView(u, depth0, v_eye);
		const vec3 normal = get_shaded_normal(u, v_eye);
	#else
		const vec3 posV = uvDepthToView(u, depth0);
		const vec3 normal = get_shaded_normal(u);
	#endif

	const vec2 lowInv = 2.0 * w_h_height.xy; // low res texel size in uv
	const vec2 f = u / lowInv - 0.5; // position in low res texels, relative to texel centers
	const vec2 i0 = floor(f);
	const vec2 fr = f - i0;

	vec3 rgbSum = vec3_splat(0.0);
	float aSum = 0.0;
	float wSum = 0.0;
	UNROLL for(int j = 0; j < 2; ++j)
	UNROLL for(int i = 0; i < 2; ++i)
	{
		const vec2 tapUV = (i0 + vec2(float(i), float(j)) + 0.5) * lowInv;
		BRANCH if(tapUV.x < 0.0 || tapUV.x > 1.0 || tapUV.y < 0.0 || tapUV.y > 1.0)
			continue;
		const vec4 s = texStereoNoLod(tex_ssr_lowres, tapUV);
		float w = (i == 0 ? 1.0 - fr.x : fr.x) * (j == 0 ? 1.0 - fr.y : fr.y);
		// depth similarity between this pixel's surface and the tap's ray origin surface
		#ifdef STEREO
			const float zLo = uvDepthToView(tapUV, texStereoNoLod(tex_depth, tapUV).x, v_eye).z;
		#else
			const float zLo = uvDepthToView(tapUV, texStereoNoLod(tex_depth, tapUV).x).z;
		#endif
		w *= saturate(1.0 - abs(posV.z - zLo) / (0.05 * abs(posV.z) + 0.001));
		#ifdef STEREO
			w *= pow(saturate(dot(normal, get_shaded_normal(tapUV, v_eye))), 8.0);
		#else
			w *= pow(saturate(dot(normal, get_shaded_normal(tapUV))), 8.0);
		#endif
		rgbSum += w * s.rgb;
		aSum += w * s.a;
		wSum += w;
	}

	const float coverage = aSum / (max(wSum, 1e-6) * 2.0); // alpha accumulates the 2 rays
	const vec3 Lssr = rgbSum / max(aSum, 1e-6);
	const float alphaP = saturate(coverage) * (1.0 - pbrProbeParams.z * att1.a);
	gl_FragColor = vec4(color0 + W * mix(att1.rgb, Lssr, alphaP), 1.0);
}
