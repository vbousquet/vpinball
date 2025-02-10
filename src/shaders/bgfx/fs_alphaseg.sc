// license:GPLv3+

$input v_texcoord0, v_texcoord1
#ifdef CLIP
	$input v_clipDistance
#endif

#include "common.sh"

SAMPLER2D(dmdBackGlow, 2);     // Segment SDF
SAMPLER2D(dmdGlass, 3);        // Glass
#define segSDF       dmdBackGlow

uniform vec4 staticColor_Alpha;
uniform vec4 vColor_Intensity;
uniform vec4 glassAmbient_Roughness;

#define segColor       vColor_Intensity.rgb
#define unlitSeg       staticColor_Alpha.rgb
#define glassAmbient   glassAmbient_Roughness.xyz
#define glassRoughness glassAmbient_Roughness.w


float median(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
}

#if !defined(CLIP)
	EARLY_DEPTH_STENCIL
#endif
void main()
{
	#ifdef CLIP
		if (v_clipDistance < 0.0)
			discard;
	#endif

	vec3 color = vec3(0.0, 0.0, 0.0);
	vec2 glassUv = v_texcoord0;
	vec2 dmdUv = v_texcoord1;

	float pxRange = 16.0;
    vec2 unitRange = vec2_splat(pxRange)/vec2(2048.0, 128.0);
    vec2 screenTexSize = vec2_splat(1.0)/fwidth(dmdUv);
    float screenPxRange = max(0.5*dot(unitRange, screenTexSize), 1.0);
	
	for (int i = 0; i < 16; i++)
	{
		vec3 msd = texNoLod(segSDF, vec2((float(i) + dmdUv.x) / 16.0, dmdUv.y) ).rgb;
		float sd = median(msd.r, msd.g, msd.b);
		float screenPxDistance = screenPxRange * (sd - 0.5);
		float opacity = clamp(screenPxDistance + 0.5, 0.0, 1.0);
		//color += mix(vec3(0.0, 0.0, 0.0), vec3(1.0, 1.0, 1.0), opacity);
		//color += mix(vec3(0.0, 0.0, 0.0), vec3(1.0, 1.0, 1.0), sd);
		color += mix(vec3(0.0, 0.0, 0.0), vec3(1.0, 1.0, 1.0), smoothstep(0.4, 0.6, sd.r));
	}
	
	color = color * segColor;
	
	#ifdef GLASS
		// Apply the glass as a tinted (lighten by the base ambient color + DMD using large blur) additive blend.
		// The glass texture modulates the lighting (so sort of a tinted roughness map)
		// vec4 glass = texture2D(dmdGlass, glassUv);
		// vec3 glassLight = glassAmbient + glassRoughness * convertDmdToColor(texNoLod(dmdBackGlow, glowUv).rgb);
		// color += glass.rgb * glassLight.rgb;
	#endif

	#ifdef SRGB
	// sRGB conversion (tonemapping still to be implemented)
	gl_FragColor = vec4(FBGamma(color), 1.0);

	#else
	// Rendering to a surface part of larger rendering process including tonemapping and sRGB conversion
	gl_FragColor = vec4(color, 1.0);

	#endif
}
