$input v_texcoord0

#include "bgfx_shader.sh"


// w_h_height.xy contains inverse size of source texture (1/w, 1/h), i.e. one texel shift to the upper (DX)/lower (OpenGL) left texel. Since OpenGL has upside down textures it leads to a different texel if not sampled on both sides
// . for bloom, w_h_height.z keeps strength
// . for mirror, w_h_height.z keeps inverse strength
// . for AO, w_h_height.zw keeps per-frame offset for temporal variation of the pattern
// . for AA techniques, w_h_height.z keeps source texture width, w_h_height.w is a boolean set to 1 when depth is available
// . for parallax stereo, w_h_height.z keeps source texture height, w_h_height.w keeps the 3D offset
uniform vec4 w_h_height;

SAMPLER2D(tex_fb_filtered,  0); // Framebuffer (filtered)
SAMPLER2D(tex_depth,        4); // DepthBuffer


//#define NFAA_EDGE_DETECTION_VARIANT // different edge detection (sums for finite differences differ)
//#define NFAA_USE_COLOR // use color instead of luminance
//#define NFAA_TEST_MODE // debug output
// undef both of the following for variant 0
#define NFAA_VARIANT // variant 1
//#define NFAA_VARIANT2 // variant 2

float GetLuminance(const vec3 l)
{
	return dot(l, vec3(0.25,0.5,0.25)); // experimental, red and blue should not suffer too much
	//return 0.299*l.x + 0.587*l.y + 0.114*l.z;
	//return 0.2126*l.x + 0.7152*l.y + 0.0722*l.z; // photometric
	//return sqrt(0.299 * l.x*l.x + 0.587 * l.y*l.y + 0.114 * l.z*l.z); // hsp
}

#ifndef NFAA_USE_COLOR
vec2 findContrastByLuminance(const vec2 XYCoord, const float filterSpread)
{
	const vec2 upOffset    = vec2(0.0, w_h_height.y * filterSpread);
	const vec2 rightOffset = vec2(w_h_height.x * filterSpread, 0.0);

	const float topHeight         = GetLuminance(texture2DLod(tex_fb_filtered, XYCoord +               upOffset, 0.0).rgb);
	const float bottomHeight      = GetLuminance(texture2DLod(tex_fb_filtered, XYCoord -               upOffset, 0.0).rgb);
	const float rightHeight       = GetLuminance(texture2DLod(tex_fb_filtered, XYCoord + rightOffset           , 0.0).rgb);
	const float leftHeight        = GetLuminance(texture2DLod(tex_fb_filtered, XYCoord - rightOffset           , 0.0).rgb);
	const float leftTopHeight     = GetLuminance(texture2DLod(tex_fb_filtered, XYCoord - rightOffset + upOffset, 0.0).rgb);
	const float leftBottomHeight  = GetLuminance(texture2DLod(tex_fb_filtered, XYCoord - rightOffset - upOffset, 0.0).rgb);
	const float rightBottomHeight = GetLuminance(texture2DLod(tex_fb_filtered, XYCoord + rightOffset + upOffset, 0.0).rgb);
	const float rightTopHeight    = GetLuminance(texture2DLod(tex_fb_filtered, XYCoord + rightOffset - upOffset, 0.0).rgb);

#ifdef NFAA_EDGE_DETECTION_VARIANT
	const float sum0 = rightTopHeight    + bottomHeight + leftTopHeight;
	const float sum1 = leftBottomHeight  + topHeight    + rightBottomHeight;
	const float sum2 = leftTopHeight     + rightHeight  + leftBottomHeight;
	const float sum3 = rightBottomHeight + leftHeight   + rightTopHeight;
#else
	const float sum0 = rightTopHeight + topHeight + rightBottomHeight;
	const float sum1 = leftTopHeight + bottomHeight + leftBottomHeight;
	const float sum2 = leftTopHeight + leftHeight + rightTopHeight;
	const float sum3 = leftBottomHeight + rightHeight + rightBottomHeight;
#endif

	// finite differences for final vectors
	return vec2( sum1 - sum0, sum2 - sum3 );
}

#else

vec2 findContrastByColor(const vec2 XYCoord, const float filterSpread)
{
	const vec2 upOffset    = vec2(0.0, w_h_height.y * filterSpread);
	const vec2 rightOffset = vec2(w_h_height.x * filterSpread, 0.0);

	const vec3 topHeight         = texture2DLod(tex_fb_filtered, XYCoord +               upOffset, 0.0).rgb;
	const vec3 bottomHeight      = texture2DLod(tex_fb_filtered, XYCoord -               upOffset, 0.0).rgb;
	const vec3 rightHeight       = texture2DLod(tex_fb_filtered, XYCoord + rightOffset           , 0.0).rgb;
	const vec3 leftHeight        = texture2DLod(tex_fb_filtered, XYCoord - rightOffset           , 0.0).rgb;
	const vec3 leftTopHeight     = texture2DLod(tex_fb_filtered, XYCoord - rightOffset + upOffset, 0.0).rgb;
	const vec3 leftBottomHeight  = texture2DLod(tex_fb_filtered, XYCoord - rightOffset - upOffset, 0.0).rgb;
	const vec3 rightBottomHeight = texture2DLod(tex_fb_filtered, XYCoord + rightOffset + upOffset, 0.0).rgb;
	const vec3 rightTopHeight    = texture2DLod(tex_fb_filtered, XYCoord + rightOffset - upOffset, 0.0).rgb;

#ifdef NFAA_EDGE_DETECTION_VARIANT
	const float sum0 = rightTopHeight    + bottomHeight + leftTopHeight;
	const float sum1 = leftBottomHeight  + topHeight    + rightBottomHeight;
	const float sum2 = leftTopHeight     + rightHeight  + leftBottomHeight;
	const float sum3 = rightBottomHeight + leftHeight   + rightTopHeight;
#else
	const float sum0 = rightTopHeight + topHeight + rightBottomHeight;
	const float sum1 = leftTopHeight + bottomHeight + leftBottomHeight;
	const float sum2 = leftTopHeight + leftHeight + rightTopHeight;
	const float sum3 = leftBottomHeight + rightHeight + rightBottomHeight;
#endif

	// finite differences for final vectors
	return vec2( length(sum1 - sum0), length(sum2 - sum3) );
}
#endif

void main()
{
#ifndef NFAA_VARIANT2
 #ifdef NFAA_VARIANT
	const float filterStrength = 1.0;
 #else
	const float filterStrength = 0.5;
 #endif
	const float filterSpread = 4.0; //!! or original 3? or larger 5?
#else
	const float filterSpread = 1.0;
#endif

	const vec2 u = v_texcoord0;

	const vec3 Scene0 = texture2DLod(tex_fb_filtered, u, 0.0).rgb;
	BRANCH if(w_h_height.w == 1.0) // depth buffer available?
	{
		const float depth0 = texture2DLod(tex_depth, u, 0.0).x;
		BRANCH if((depth0 == 1.0) || (depth0 == 0.0)) // early out if depth too large (=BG) or too small (=DMD,etc)
		{
			gl_FragColor = vec4(Scene0, 1.0);
			return;
		}
	}

#ifdef NFAA_USE_COLOR // edges from color
	vec2 Vectors = findContrastByColor(u, filterSpread);
#else
	vec2 Vectors = findContrastByLuminance(u, filterSpread);
#endif

#ifndef NFAA_VARIANT2
	const float filterStrength2 = filterStrength + filterSpread*0.5;
	const float filterClamp = filterStrength2 / filterSpread;

	Vectors = clamp(Vectors * filterStrength2, -vec2(filterClamp, filterClamp), vec2(filterClamp, filterClamp));
#else
	Vectors *= filterSpread;
#endif

	const vec2 Normal = Vectors * (w_h_height.xy /* * 2.0*/);

	const vec3 Scene1 = texture2DLod(tex_fb_filtered, u + Normal, 0.0).rgb;
	const vec3 Scene2 = texture2DLod(tex_fb_filtered, u - Normal, 0.0).rgb;
#if defined(NFAA_VARIANT) || defined(NFAA_VARIANT2)
	const vec3 Scene3 = texture2DLod(tex_fb_filtered, u + vec2(Normal.x, -Normal.y)*0.5, 0.0).rgb;
	const vec3 Scene4 = texture2DLod(tex_fb_filtered, u - vec2(Normal.x, -Normal.y)*0.5, 0.0).rgb;
#else
	const vec3 Scene3 = texture2DLod(tex_fb_filtered, u + vec2(Normal.x, -Normal.y), 0.0).rgb;
	const vec3 Scene4 = texture2DLod(tex_fb_filtered, u - vec2(Normal.x, -Normal.y), 0.0).rgb;
#endif

#ifdef NFAA_TEST_MODE // debug
	const vec3 o_Color = normalize(vec3(Vectors * 0.5 + 0.5, 1.0));
#else
	const vec3 o_Color = (Scene0 + Scene1 + Scene2 + Scene3 + Scene4) * 0.2;
#endif

	gl_FragColor = vec4(o_Color, 1.0);
}