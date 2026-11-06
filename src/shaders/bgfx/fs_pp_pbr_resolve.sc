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
uniform vec4 pbrResolveParams; // x: SSR coverage fade start roughness, y: fade end roughness, z: max pooling radius (low res texels), w: firefly clamp factor

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

	// Roughness-scaled pooling of the half res march buffer. The 2-ray Monte Carlo input speckles
	// on rough surfaces, so a plain 2x2 upsample is not enough: a golden-angle spiral of taps with
	// a footprint approximating the projected GGX lobe (~roughness^2) pools both hit radiance and
	// hit coverage (the buffer is premultiplied). The same taps pool the probe radiance sideband,
	// giving rough pixels a spatially blurred probe/envmap fallback.
	#ifdef STEREO
		const vec3 posV = uvDepthToView(u, depth0, v_eye);
	#else
		const vec3 posV = uvDepthToView(u, depth0);
	#endif
	const vec4 prepass = texStereoNoLod(tex_normals, u);
	const vec3 normal = vec3(prepass.xy, sqrt(saturate(1.0 - dot(prepass.xy, prepass.xy))));
	const float pbrRoughness = texStereoNoLod(tex_pbr_data, u).x;
	const float radius = 1.0 + pbrResolveParams.z * pbrRoughness * pbrRoughness; // lobe footprint in low res texels

	const vec2 lowInv = 2.0 * w_h_height.xy; // low res texel size in uv
	const vec2 rot = hash22(u / w_h_height.xy); // per pixel disk rotation, decorrelates the taps
	const float ca = cos(rot.x * 2.0 * PI), sa = sin(rot.x * 2.0 * PI);

	vec3 rgbSum = vec3_splat(0.0);
	vec3 att1Sum = vec3_splat(0.0);
	float att1aSum = 0.0;
	float aSum = 0.0;
	float wSum = 0.0;
	UNROLL for(int i = 0; i < 12; ++i)
	{
		const float fi = float(i);
		const float rad = sqrt((fi + 0.5) / 12.0); // sqrt for uniform disk coverage
		const float ang = fi * 2.39996323; // golden angle spiral
		const vec2 offs = vec2(cos(ang), sin(ang)) * rad * radius;
		const vec2 tapUV = u + vec2(offs.x * ca - offs.y * sa, offs.x * sa + offs.y * ca) * lowInv;
		BRANCH if(tapUV.x < 0.0 || tapUV.x > 1.0 || tapUV.y < 0.0 || tapUV.y > 1.0)
			continue;
		const vec4 s = texStereoNoLod(tex_ssr_lowres, tapUV);
		const vec4 a1 = texStereoNoLod(tex_pbr_att1, tapUV);
		float w = 1.0 - rad; // cone falloff inside the footprint
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
		att1Sum += w * a1.rgb;
		att1aSum += w * a1.a;
		wSum += w;
	}

	// Bilateral taps may all be rejected near silhouettes: fall back to the unpooled sideband
	const vec3 Lprobe = wSum > 1e-4 ? att1Sum / wSum : att1.rgb;
	const float Wprobe = wSum > 1e-4 ? att1aSum / wSum : att1.a;

	const float coverage = aSum / (max(wSum, 1e-6) * 2.0); // alpha accumulates the 2 rays
	vec3 Lssr = rgbSum / max(aSum, 1e-6);
	// Firefly clamp: bound marched radiance to a multiple of the pooled probe estimate so a lone
	// hot hit cannot poke through the pooled average
	const float lCap = max(dot(Lprobe, vec3(0.2126, 0.7152, 0.0722)), 0.02) * pbrResolveParams.w;
	Lssr *= min(1.0, lCap / max(dot(Lssr, vec3(0.2126, 0.7152, 0.0722)), 1e-4));
	// Coverage fade: very rough lobes resolve no extra detail from SSR — fall back to probe/envmap
	const float ssrFade = saturate(1.0 - (pbrRoughness - pbrResolveParams.x) / (pbrResolveParams.y - pbrResolveParams.x));
	const float alphaP = saturate(coverage) * ssrFade * (1.0 - pbrProbeParams.z * Wprobe);
	gl_FragColor = vec4(color0 + W * mix(Lprobe, Lssr, alphaP), 1.0);
}
