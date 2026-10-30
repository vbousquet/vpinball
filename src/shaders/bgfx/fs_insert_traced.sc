// license:GPLv3+

// Traced insert: an additive lightmap primitive whose glow comes from an offline path-traced render of the insert
// (a bulb under the real plastic, seen from four view elevations, one per channel of the base image), instead of a
// single view-independent lightmap. The primitive is a flat card at the top of the insert, its UV the bake's square.
//
// - tex_base_color (stage 0): the traced light, 4 elevations in RGBA, linear HDR, normalised to its brightest texel.
// - tex_base_normalmap (stage 4, optional): the bake's facet normals, x and 'up the table' in RG, faceted floor in A.
// - tex_traced_env (stage 7, optional): the lamp-off look, the 4 elevations in a 2x2 atlas (15 | 45 over 60 | 70), linear HDR.
// - tex_pbr_orm (stage 8, optional): the table's print map over the table rectangle: rgb the playfield art (sRGB), alpha 1 where
//   the print leaves an insert clear, 0 where it covers it.
// - staticColor_Alpha: the lamp's colour x level, as for any additive lightmap primitive.
//
// tracedParams (insert space in mm, angles in degrees):
// [0] the 4 traced elevations, off vertical
// [1] depths: x cover slab whose refraction is undone, y facets under the top, z glow parallax, w rim fade
// [2] x glow gain, y glow spread, z IOR, w debug (1 = alignment view)
// [3] x glint strength, y glint sharpness, z facet tilt, w colour fringe
// [4] x glint keys, y glint turn (incl. the per-insert turn), z facet waviness, w waviness wavelength
// [5] x per-insert seed, y has facets, z mm per table unit, w frost (0 clear top, saturating: 3 about two thirds, 10+ a fully frosted cap)
// [6] rgb: room light glinting in the facets, lamp on or off, w: lamp-off look gain (0 = no lamp-off atlas)
// [7] rgb: the plastic's colour, tinting the (neutral) lamp-off look, w: facet blur for the glints in mips (hides a hard-edged facet map)
// [8] x 1 = spill pass (the card scaled up about its centre), y spill size in card widths, z spill strength,
//     w spill reach as a print map mip (-1: no print map, a round falloff)
// [9] table position to print map UV: xy scale, zw offset (x = 0: no print map)
// [10] xy the print map's texel (UV) at the spill mip, z white leak at the glow's peak (the share of the lamp's white light the
//      plastic lets through: a hot insert's core goes white, as on camera, while its body, spill and lamp-off look keep the colour),
//      w how much the plastic's colour tints the room glints (0 white, 1 the plastic's colour)
// [11] x print map tint (inktint): an insert under printed art (Police Force's jackpot car and character patches) shows
//      through the print by its alpha (the print's transmission) tinted by its colour; 0 = alpha only, as a cut-out window

$input v_worldPos, v_tablePos, v_normal, v_texcoord0
#ifdef STEREO
	$input v_eye
#endif
#ifdef CLIP
	$input v_clipDistance
#endif

#include "common.sh"

uniform vec4 staticColor_Alpha;
uniform vec4 tracedParams[12];

#ifdef STEREO
	uniform vec4 cameraPosWorld[2];
	#define cCameraPosWorld cameraPosWorld[int(round(v_eye))].xyz
#else
	uniform vec4 cameraPosWorld;
	#define cCameraPosWorld cameraPosWorld.xyz
#endif

SAMPLER2D(tex_base_color, 0);     // traced light
SAMPLER2D(tex_base_normalmap, 4); // traced facets
SAMPLER2D(tex_traced_env, 7);     // traced lamp-off atlas
SAMPLER2D(tex_pbr_orm, 8);        // the table's print map: art in rgb, clear insert windows in alpha

// How much each of the four traced elevations counts at this one: linear between neighbours, held at the ends
vec4 tracedAngleWeights(float elevation)
{
	const vec4 a = tracedParams[0];
	vec4 w = vec4(0.0, 0.0, 0.0, 0.0);
	if (elevation <= a.x)
		w.x = 1.0;
	else if (elevation <= a.y)
	{
		const float t = (elevation - a.x) / max(a.y - a.x, 1e-3);
		w.xy = vec2(1.0 - t, t);
	}
	else if (elevation <= a.z)
	{
		const float t = (elevation - a.y) / max(a.z - a.y, 1e-3);
		w.yz = vec2(1.0 - t, t);
	}
	else if (elevation <= a.w)
	{
		const float t = (elevation - a.z) / max(a.w - a.z, 1e-3);
		w.zw = vec2(1.0 - t, t);
	}
	else
		w.w = 1.0;
	return w;
}

// The facets catching a few fixed highlights that turn with the eye's bearing, each colour channel tilted a little apart
float tracedGlint(vec3 n, vec3 v, float turn, float sharp, float channel)
{
	const int keys = int(clamp(tracedParams[4].x, 1.0, 8.0));
	const float tilt = radians(tracedParams[3].z + (channel - 1.0) * tracedParams[3].w);
	const float st = sin(tilt);
	const float ct = cos(tilt);
	const float azimuth = atan2(-v.y, v.x) + radians(tracedParams[4].y + turn); // as the bake's frame: y up the table
	float sum = 0.0;
	for (int j = 0; j < 8; j++)
	{
		if (j >= keys)
			break;
		const float a = azimuth + 6.28318530718 * float(j) / float(keys);
		sum += pow(saturate(dot(n, vec3(st * cos(a), -st * sin(a), ct))), sharp);
	}
	// Phong normalisation: a wide glint is a dim one, not a flood into the bloom. Faded out straight under the eye where the bearing spins.
	return sum * ((sharp + 2.0) / 602.0) * smoothstep(0.05, 0.25, length(v.xy));
}

// The lamp-off look at a card point: the four elevations of the atlas, each read at the mip the insert's screen size (and the
// frost) asks for, half a texel of that mip in from its slice's edge so no slice bleeds into its neighbour
vec3 tracedEnv(vec2 uv, vec4 weights, float lodBias)
{
	const vec2 slice = vec2(512.0, 512.0);
	const vec2 dx = dFdx(uv) * slice, dy = dFdy(uv) * slice;
	const float lod = clamp(0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-8)) + lodBias, 0.0, 8.0);
	const vec2 margin = exp2(lod) / (2.0 * slice);
	const vec2 q = clamp(uv, margin, 1.0 - margin) * 0.5;
	return texture2DLod(tex_traced_env, q, lod).rgb * weights.x
		+ texture2DLod(tex_traced_env, q + vec2(0.5, 0.0), lod).rgb * weights.y
		+ texture2DLod(tex_traced_env, q + vec2(0.0, 0.5), lod).rgb * weights.z
		+ texture2DLod(tex_traced_env, q + vec2(0.5, 0.5), lod).rgb * weights.w;
}

#ifndef CLIP
EARLY_DEPTH_STENCIL
#endif
void main()
{
	#ifdef CLIP
	if (v_clipDistance < 0.0)
		discard;
	#endif

	// Spill pass: the lit insert's light on the art around it. It leaves the window's edges and falls off fast (the window
	// mask blurred over the reach, three mips for a soft tail), lights the art under it (dark ink stays dark), never the
	// windows themselves, and fades out before the scaled card's rim. Without a print map: the old round falloff.
	if (tracedParams[8].x > 0.5)
	{
		const float r = length(v_texcoord0 * 2.0 - 1.0);
		if (tracedParams[9].x > 0.0 && tracedParams[8].w >= 0.0)
		{
			const vec2 t = v_tablePos.xy * tracedParams[9].xy + tracedParams[9].zw;
			const float lod = tracedParams[8].w;
			const vec4 here = texture2DLod(tex_pbr_orm, t, 0.0);
			// each mip read at 4 points half its texel apart (a mip alone is a box: blocky)
			float spread = 0.0;
			for (int i = 0; i < 3; i++)
			{
				const vec2 o = tracedParams[10].xy * (0.5 * exp2(float(i)));
				const float l = lod + float(i);
				const float m = texture2DLod(tex_pbr_orm, t + vec2(o.x, o.y), l).a + texture2DLod(tex_pbr_orm, t + vec2(-o.x, o.y), l).a
					+ texture2DLod(tex_pbr_orm, t + vec2(o.x, -o.y), l).a + texture2DLod(tex_pbr_orm, t - o, l).a;
				spread += (i == 0 ? 0.5 : i == 1 ? 0.3 : 0.2) * 0.25 * m;
			}
			const float spill = spread * (1.0 - here.a) * saturate((1.0 - r) / 0.4);
			gl_FragColor = vec4(staticColor_Alpha.rgb * here.rgb * (tracedParams[8].z * spill), 1.0);
		}
		else
		{
			const float h = exp(-(r / 0.38) * (r / 0.38)) * pow(saturate((1.0 - r) / 0.35), 1.5);
			gl_FragColor = vec4(staticColor_Alpha.rgb * (tracedParams[8].z * pow(h, 2.2)), 1.0);
		}
		return;
	}

	// Insert frame from the card's UV derivatives: table units per UV along u and v, so the card may be placed, turned
	// and scaled freely. Table space is z up; the card's u and v are its own x and y.
	const vec2 uv = v_texcoord0;
	const vec3 dpx = dFdx(v_tablePos);
	const vec3 dpy = dFdy(v_tablePos);
	const vec2 dux = dFdx(uv);
	const vec2 duy = dFdy(uv);
	const float det = dux.x * duy.y - dux.y * duy.x;
	const vec3 dpdu = (dpx * duy.y - dpy * dux.y) / det;
	const vec3 dpdv = (dpy * dux.x - dpx * duy.x) / det;
	const float mmPerUnit = tracedParams[5].z;
	const float size = length(dpdu) * mmPerUnit; // the bake square's width in mm
	const vec3 tu = normalize(dpdu);
	const vec3 tv = normalize(dpdv - tu * dot(dpdv, tu));

	// The eye in the card frame: x along u, y along v (down the table for an unturned card), z up
	const vec3 vt = normalize(cCameraPosWorld - v_tablePos);
	const vec3 v = normalize(vec3(dot(vt, tu), dot(vt, tv), max(vt.z, 1e-5)));
	const float eta = max(tracedParams[2].z, 1.001);
	const vec3 inside = refract(-v, vec3(0.0, 0.0, 1.0), 1.0 / eta);
	const float steep = max(v.z, 0.05);
	const float steepInside = max(-inside.z, 0.05);

	// Frost: a frosted cap scatters what passes the top, so the light leaves from the top itself: the refraction shift
	// fades, the view response flattens, and the light blurs and spreads over the whole outline. Saturating, no ceiling.
	const float frost = 1.0 - exp(-max(tracedParams[5].w, 0.0) / 3.0);

	// The card point the cover shows here, its refraction through a slab undone
	const vec2 card = uv - (1.0 - frost) * (tracedParams[1].x / size) * (v.xy / steep + inside.xy / steepInside);

	// The traced elevation weights, and how far the inside of the plastic slides against the traced direction
	// (the bake looks up the table from the player's side: its eye is along +v)
	const float elevation = degrees(acos(saturate(v.z)));
	const vec4 weights = mix(tracedAngleWeights(elevation), vec4(0.25, 0.25, 0.25, 0.25), 0.6 * frost);
	const vec3 tracedView = vec3(0.0, sin(radians(elevation)), cos(radians(elevation)));
	const vec3 tracedInside = refract(-tracedView, vec3(0.0, 0.0, 1.0), 1.0 / eta);
	const vec2 slide = (1.0 - frost) * (inside.xy / max(-inside.z, 0.25) - tracedInside.xy / max(-tracedInside.z, 0.25)) * (tracedParams[1].z / size);

	const vec2 lightUv = card + slide;
	float light;
	if (frost > 0.0)
	{
		// Blurred by up to ~4 mm, then mixed toward a milky fill: the broad local glow and the insert's mean glow, spread
		// evenly to the outline (the bake's mean over its square, over the share of the square the outline covers)
		const float footprint = max(length(dFdx(lightUv)), length(dFdy(lightUv))) * 1024.0;
		const float lod = log2(max(footprint, 1.0)) + 8.0 * frost;
		const float blurred = dot(texture2DLod(tex_base_color, lightUv, lod), weights);
		const float broad = dot(texture2DLod(tex_base_color, lightUv, 8.0), weights);
		const float mean = dot(texture2DLod(tex_base_color, vec2(0.5, 0.5), 10.0), weights) / max(tracedParams[1].w > 0.0 ? 0.62 : 1.0, 0.05);
		light = mix(blurred, 0.5 * (broad + mean), 0.95 * frost);
	}
	else
		light = dot(texture2DGrad(tex_base_color, lightUv, dFdx(lightUv), dFdy(lightUv)), weights);
	light = pow(max(light, 0.0), 1.0 / max(tracedParams[2].y, 0.05));

	const bool hasFacets = tracedParams[5].y > 0.5;
	vec3 glint = vec3(0.0, 0.0, 0.0);
	if (hasFacets)
	{
		// Fade the light over the last millimetres inside the outline (the render's pocket wall seen at a slant): the floor
		// mask blurred over that width is a half on the outline
		if (tracedParams[1].w > 0.0)
		{
			const float texel = size / 1024.0;
			const float mip = log2(max(2.0 * tracedParams[1].w / texel, 1.0));
			light *= mix(smoothstep(0.5, 0.95, texture2DLod(tex_base_normalmap, card, mip).a), 1.0, frost);
		}

		// The facets under the card, where the refracted eye ray meets them. The map's green is 'up the table', -v.
		const vec2 floorUv = card + inside.xy * (tracedParams[1].y / size / steepInside);
		const vec2 gx = dFdx(floorUv), gy = dFdy(floorUv);

		// No two inserts glint alike: a per-insert seed turns the highlights and phases the moulding's waviness
		const float seed = tracedParams[5].x;
		const float seedB = fract(seed * 17.13 + 0.37);
		const vec2 wave = card * size * (6.28318530718 / max(tracedParams[4].w, 1e-3));
		const vec2 wobble = vec2(sin(wave.x * 0.83 + wave.y * 0.57 + seed * 6.283), sin(wave.y * 0.91 - wave.x * 0.41 + seedB * 6.283));
		const vec2 lean = wobble * tan(radians(clamp(tracedParams[4].z, 0.0, 30.0)));

		// Specular anti-aliasing: widen the highlight by how much the filtered facet normal changes across the pixel
		const vec2 filtered = texture2DGrad(tex_base_normalmap, floorUv, gx, gy).rg;
		const float variance = max(dot(dFdx(filtered), dFdx(filtered)), dot(dFdy(filtered), dFdy(filtered)));
		const float alpha2 = 2.0 / (max(tracedParams[3].y, 1.0) + 2.0) + min(variance, 0.25);
		// ...and frost turns the sparkle into a soft sheen
		const float sharp = max((2.0 / alpha2 - 2.0) * (1.0 - 0.9 * frost), 1.0);

		// The highlights are sharp and the facet seams hard, so one sample per pixel switches whole pixels on and off along
		// every seam (stair-stepped dashes, with no temporal AA to hide them): average 4 samples on a rotated grid inside
		// the pixel, each reading the facets over a quarter of its footprint
		const vec2 taps[4] = { vec2(-0.125, -0.375), vec2(0.375, -0.125), vec2(0.125, 0.375), vec2(-0.375, 0.125) };
		const float footprint = 0.5 * exp2(max(tracedParams[7].w, 0.0));
		for (int i = 0; i < 4; i++)
		{
			const vec4 f = texture2DGrad(tex_base_normalmap, floorUv + gx * taps[i].x + gy * taps[i].y, gx * footprint, gy * footprint);
			const vec2 nxy = vec2(f.r * 2.0 - 1.0, 1.0 - f.g * 2.0);
			const vec3 wavy = normalize(vec3(nxy + lean, sqrt(saturate(1.0 - dot(nxy, nxy)))));
			glint += vec3(tracedGlint(wavy, v, 0.0, sharp, 0.0), tracedGlint(wavy, v, 0.0, sharp, 1.0), tracedGlint(wavy, v, 0.0, sharp, 2.0)) * f.a;
		}
		glint *= 0.25 * (1.0 - 0.95 * frost);
	}

	// The lamp-off look, always there under the lamp, at the same card point and weights as the traced light
	vec3 lampOff = vec3(0.0, 0.0, 0.0);
	if (tracedParams[6].w > 0.0)
		lampOff = tracedParams[6].w * tracedParams[7].rgb * tracedEnv(lightUv, weights, 8.0 * frost);

	// The lamp's glow and glints, plus the room glinting in the facets whatever the lamp does
	// (the white share grows with the glow: the bake's peak (1) gets all of it, the dim body almost none)
	// The room's light glints off the facets under the plastic, so it comes back through it: tinted by its colour (roomtint)
	const vec3 roomGlint = tracedParams[6].rgb * mix(vec3_splat(1.0), tracedParams[7].rgb, tracedParams[10].w);
	const vec3 glowTint = mix(staticColor_Alpha.rgb, vec3_splat(staticColor_Alpha.a), tracedParams[10].z * saturate(light));
	vec3 result = glowTint * (tracedParams[2].x * light) + staticColor_Alpha.rgb * (10.0 * tracedParams[3].x * glint) + roomGlint * (10.0 * tracedParams[3].x) * glint + lampOff;

	// The print on the playfield covers the insert (its glow, the lamp-off look and the facets alike)
	if (tracedParams[9].x > 0.0)
	{
		const vec4 ink = texture2DLod(tex_pbr_orm, v_tablePos.xy * tracedParams[9].xy + tracedParams[9].zw, 0.0);
		result *= ink.a * mix(vec3_splat(1.0), mix(ink.rgb, vec3_splat(1.0), ink.a), tracedParams[11].x);
	}

	// Alignment view: the bake's square in a 2 mm grid, red outside it, the traced light in green
	if (tracedParams[2].w > 0.5)
	{
		const vec2 mm = fract(card * size * 0.5);
		const float grid = step(0.9, max(mm.x, mm.y));
		const float outside = (card.x < 0.0 || card.y < 0.0 || card.x > 1.0 || card.y > 1.0) ? 1.0 : 0.0;
		result = vec3(outside, light, grid);
	}

	gl_FragColor = vec4(result, 1.0);
}
