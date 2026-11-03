// license:GPLv3+

// Flasher dome: the lit and sparkle layer of a fluted plastic flasher dome, an additive pass drawn over the dome's own PBR
// body (which carries the OFF body colour and the smooth outer highlight). Live: nothing is baked, the lamp level and the
// view drive every term. Port of the HDRP dome recipe (pinball-rebuild recipes/flasher-dome-vpe-PORTABLE.md): all the
// maths runs in dome-local space (mm, y = dome axis, x = the bulb's coil axis), no UVs. Only the outer skin emits; the inner
// flutes are reached by refracting the view ray through the wall, so thin lines are never resampled into dot chains.
//
// Layers: body glow (near wall and the far wall seen across the dome), bars (the coil seen through each flute flank, a GGX
// transmission lobe against a 5-sample line filament), bulb halo and cap spot, the peak (heat, flash, white-hot outer
// skin), and the OFF sparkle (facet glints of a fixed studio room, ridge glints and needles on the flute edges, the bulb's
// glass envelope and the base ring seen through the plastic).
// A see-through body (transparent material) is drawn after a transmittance pass of this shader (mode 1): the framebuffer
// behind the dome is multiplied by the plastic's Beer-Lambert transmission (near wall along the refracted ray + one more
// wall), dimmed along the flute edges and the cap's starburst ribs, and darkened where the cavity ray meets the bulb's base.
// Colour is blackbody(T(level)) x the plastic's Beer-Lambert transmission; white appears only where the tonemapper clips.
//
// domeParams (dome-local mm unless noted):
// [0] xyz dome base centre (table units), w mm per table unit
// [1] xyz dome axis (table space), w outer radius
// [2] xyz coil axis (table space, across the dome axis), w outer height
// [3] x filament height, y flute count, z flute phase (rad, a groove root = thinnest wall), w lamp level (may pass 1)
// [4] x inner radius at the groove roots, y at the ridge tips, z cap underside height, w room light (OFF sparkle, linear)
// [5] gains: x body glow, y bars (nit), z needles and ridge glints (relative to the room), w white-hot peak (nit)
// [6] gains: x bulb halo, y cap spot, z peak flash, w OFF bulb envelope and base ring (relative to the room)
// [7] x emission scale (nit to linear), y debug (1 bars + core + cap, 2 glow, 3 sparkle, 4 bars only, 5 GI glow), z absorption scale (plastic colour),
//     w pass: 0 additive layer over an opaque body, 1 transmittance (ZERO / INVSRC_COLOR blending), 2 additive layer over a see-through body
// [8] x white leak at the peak: the share of the filament's own white light that the hottest parts (the coil's image, the
//     cap spot, the shell facing the viewer) pass at full flash, so they roll off to white in the tonemapper while the
//     edges, the flood and the bloom keep the red (0 = the 10-29 look, red-orange throughout)
//     yzw the table's light at the dome (GI bulbs, inserts: irradiance at the bulb through a soft knee, linear), which the
//     plastic scatters: the GI glow (OFF and dim levels; over a see-through body only)
// [9] xyz where that light comes from (table space, unit), w how much the glow follows it (a lit side and a dark side)
// Thick body (per-part sliders, all 0 = the look above, pixel-identical):
// [10] x dome_body: 0 see-through, 1 the scene behind is replaced by the scattered interior (milky plastic); y dome_scatter:
//      the lamp's light diffused through the body (in the lamp's colour); z dome_cap: highlight on the top edge (the cap's
//      rim), room-lit and lamp-lit; w unused
// [11] xyz the plastic's absorption (1/mm; the red acrylic preset, or from dome_tint = the colour at 2 mm), w unused
// [12] xyz the lamp's colour (linear, max channel 1), w unused
// [13] x the outer radius at the top edge (mm; the cap's rim), y the table's environment level (env emission x scene lighting),
//      z dome_glint (the OFF sparkle's scale, 1 = as tuned), w dome_peak (white-hot peak, 0 = the red peak)
// [15] x 1 = a baked glow map is bound (dome_glow_map), y its gain, z dome_glint_lights (0 = the studio room, 1 = the table's
//      own lights), w dome_frost (clear plastic: the thick parts of the wall scatter the room to a milky white, 0 = off)
// [16..23] the four strongest table lights at the dome (GI bulbs, inserts, other domes' lamps), two entries each: xyz position
//      (table space), w source radius (table units); then rgb irradiance at the dome (relative to the room light), w unused
//      ([17].w: dome_frost's gain)
// [14] x dome_depth (OFF parallax: the far wall's ridges through the cavity, 0 = off), y dome_cooldown (the white-hot parts
//      follow the level as a continuous blackbody pulse instead of gating on near full, 0 = off), z dome_orb (the lit bulb
//      seen through the flutes as a glowing ball, 0 = off), w the orb's gain (nit)

$input v_worldPos, v_tablePos, v_normal, v_texcoord0
#ifdef STEREO
	$input v_eye
#endif
#ifdef CLIP
	$input v_clipDistance
#endif

#include "common.sh"

uniform vec4 domeParams[24];
SAMPLER2D(tex_traced_env, 7); // dome_glow_map (when domeParams[15].x is set): R glow (x2, gamma 2.2), G wall thickness (0..4 mm)

#ifdef STEREO
	uniform mat4 matView[2];
	uniform vec4 cameraPosWorld[2];
	#define mView           matView[int(round(v_eye))]
	#define cCameraPosWorld cameraPosWorld[int(round(v_eye))].xyz
#else
	uniform mat4 matView;
	uniform vec4 cameraPosWorld;
	#define mView           matView
	#define cCameraPosWorld cameraPosWorld.xyz
#endif

#define DOME_PI 3.14159265
#define FROST_GAIN domeParams[17].w

float domeHash(const float n) { return fract(sin(n) * 43758.5453); }

// A thin line exp(-(e/w)^2) filtered over the pixel footprint fw (in e's units): it widens and dims as it gets thinner than a
// pixel instead of aliasing into speckle (same energy)
float domeLine(const float e, const float w, const float fw)
{
	const float w2 = sqrt(w * w + fw * fw);
	return (w / w2) * exp(-(e / w2) * (e / w2));
}

// Smooth 1D value noise (breaks the bars into segments)
float domeNoise(const float x, const float seed)
{
	const float i = floor(x);
	const float f = fract(x);
	return mix(domeHash(i * 12.9898 + seed * 78.233), domeHash((i + 1.0) * 12.9898 + seed * 78.233), f * f * (3.0 - 2.0 * f));
}

// Kelvin to RGB (Tanner Helland fit), normalised to R = 1 below 6600 K
vec3 domeBlackbody(const float T)
{
	const float t = T / 100.0;
	return vec3(1.0, saturate((99.4708025861 * log(t) - 161.1195681661) / 255.0), t <= 19.0 ? 0.0 : saturate((138.5177312231 * log(t - 10.0) - 305.0447927307) / 255.0));
}

// Fixed procedural studio room for the OFF glints (dome-independent, table space: z up, +y toward the player)
float domeStudio(const vec3 wd)
{
	float env = mix(0.008, 0.16, saturate(wd.z * 0.5 + 0.5));
	env += 5.0 * pow(saturate(dot(wd, normalize(vec3(0.0, 0.66, 0.75)))), 70.0);  // key card above the player
	env += 3.0 * pow(saturate(dot(wd, normalize(vec3(0.85, -0.2, 0.35)))), 60.0); // side card
	env += 2.5 * pow(saturate(dot(wd, normalize(vec3(-0.3, -0.4, 0.95)))), 70.0); // overhead bar
	env += 2.0 * pow(saturate(dot(wd, normalize(vec3(-0.8, 0.5, 0.3)))), 60.0);   // left card
	return env;
}

// What the OFF glints reflect. dome_glint_lights (domeParams[15].z) brings in the table's own light: below the horizon the
// walls mirror the playfield round the dome, lit by the four strongest lights near it (GI bulbs, inserts, other domes'
// lamps; a diffuse floor, brighter toward each light, in its colour), and each light itself is a small bright disc (its
// irradiance at the dome spread over its solid angle, at least ~2 deg). Above the horizon the studio room stays (the
// cabinet and the room are not table lights). The glints move with the real bulbs and follow them as they light and dim.
// floorW: 1 for reflections; 0 for light seen through the dome (the GI glow carries the lit playfield behind it, and the
// studio stays there). k = 0 returns the studio room exactly
vec3 domeEnv(const vec3 wd, const vec3 Pt, const float floorW)
{
	const float k = saturate(domeParams[15].z);
	BRANCH if (k <= 0.0)
		return vec3_splat(domeStudio(wd));
	const float below = smoothstep(0.1, -0.25, wd.z) * floorW;
	vec3 env = vec3_splat(mix(domeStudio(wd), mix(0.008, 0.16, saturate(wd.z * 0.5 + 0.5)), k * below));
	const vec2 hd = normalize(wd.xy + vec2(1e-5, 0.0));
	for (int i = 0; i < 4; i++)
	{
		const vec4 lp = domeParams[16 + 2 * i];
		const vec3 E = domeParams[17 + 2 * i].rgb;
		const vec3 dl = lp.xyz - Pt;
		const float d = max(length(dl), 1.0);
		const float ts = max(lp.w / d, 0.035);
		const float t2 = 2.0 * max(1.0 - dot(wd, dl / d), 0.0); // angle^2
		const vec2 hl = normalize(dl.xy + vec2(1e-5, 0.0));
		env += k * E * (exp(-t2 / (ts * ts)) / (DOME_PI * ts * ts) // the light
			+ below * 0.35 / DOME_PI * (0.4 + 0.6 * max(dot(hd, hl), 0.0))); // the playfield it lights (albedo ~0.35)
	}
	return env;
}

// The bulb along the cavity ray o + t d (dome-local mm): a #906 wedge bulb, a glass capsule of radius rb round the dome axis
// whose round top is centred on Ft, standing on its dark wedge base. Returns the hit distance (< 0: missed) and the normal.
float domeBulb(const vec3 o, const vec3 d, const vec3 Ft, const float rb, out vec3 n)
{
	n = vec3(0.0, 1.0, 0.0);
	float t = -1.0;
	const vec3 oc = o - Ft;
	const float b = dot(oc, d);
	const float disc = b * b - (dot(oc, oc) - rb * rb);
	if (disc > 0.0)
	{
		const float ts = -b - sqrt(disc);
		if (ts > 0.0 && o.y + d.y * ts >= Ft.y)
		{
			t = ts;
			n = (oc + d * ts) / rb;
		}
	}
	const float a2 = dot(d.xz, d.xz);
	const float b2 = dot(o.xz, d.xz);
	const float disc2 = b2 * b2 - a2 * (dot(o.xz, o.xz) - rb * rb);
	if (a2 > 1e-5 && disc2 > 0.0)
	{
		const float tc = (-b2 - sqrt(disc2)) / a2;
		const float y = o.y + d.y * tc;
		if (tc > 0.0 && y < Ft.y && y > 0.0 && (t < 0.0 || tc < t))
		{
			t = tc;
			n = vec3((o.x + d.x * tc) / rb, 0.0, (o.z + d.z * tc) / rb);
		}
	}
	return t;
}

void main()
{
	#ifdef CLIP
	if (v_clipDistance < 0.0)
		discard;
	#endif

	// Dome-local frame
	const vec3 up = domeParams[1].xyz;
	const vec3 ex = domeParams[2].xyz;
	const vec3 ez = cross(up, ex);
	const vec3 dt = (v_tablePos - domeParams[0].xyz) * domeParams[0].w;
	const vec3 P = vec3(dot(dt, ex), dot(dt, up), dot(dt, ez));
	const vec3 wV = normalize(cCameraPosWorld - v_tablePos);
	const vec3 V = vec3(dot(wV, ex), dot(wV, up), dot(wV, ez));
	const vec3 wN = normalize(mul(vec4(normalize(v_normal), 0.0), mView).xyz);
	vec3 Ns = vec3(dot(wN, ex), dot(wN, up), dot(wN, ez));
	if (dot(Ns, V) < 0.0)
		Ns = -Ns;

	const float R = domeParams[1].w;
	const float H = domeParams[2].w;
	const float rootR = domeParams[4].x;
	const float ridgeR = domeParams[4].y;
	const float capY = domeParams[4].z;
	const float rxz = length(P.xz);

	const float mode = domeParams[7].w;
	const bool transPass = mode > 0.5 && mode < 1.5;
	const vec3 sigma = domeParams[11].xyz * domeParams[7].z; // the plastic's absorption (1/mm; red acrylic unless dome_tint is set)
	const float wallB = R - 0.5 * (rootR + ridgeR);
	// Thick body: how much of the scene behind the body replaces (coverage), the lamp-driven body scatter, the cap rim
	const float bodyK = saturate(domeParams[10].x);
	const float scatK = max(domeParams[10].y, 0.0);
	const float capK = max(domeParams[10].z, 0.0);
	const float peakK = max(domeParams[13].w, 0.0);
	const vec3 lampCol = domeParams[12].xyz;

	// Only the outer skin emits (the inner flutes, base and cap underside are hidden behind it or seen through it); the base
	// flange and the tabs still filter what lies under them (in, and the light that lit it, through the flange)
	if (!((Ns.y > 0.5 && P.y > capY + 0.2) || (dot(Ns.xz, P.xz) > 0.3 * rxz && rxz > rootR + 0.1)))
	{
		if (!transPass)
			discard;
		gl_FragColor = vec4(vec3_splat(1.0) - exp(-sigma * 2.5 * wallB) * (1.0 - bodyK), 1.0);
		return;
	}
	const float capW = smoothstep(0.55, 0.9, Ns.y) * smoothstep(capY, capY + 0.5, P.y);

	// Lamp level: emission black below 1 %, mid levels lifted (two-segment map), filament temperature from the level
	const float level = max(domeParams[3].w, 0.0);
	const float lvl = saturate(level);
	const float lvlE = saturate((lvl - 0.01) / 0.99);
	// dome_cooldown (coolK): 0 = the white-hot parts gate on in the top 10-15 % of the level (a short ROM flash shows them for
	// a frame or two: they snap); 1 = they follow the level all the way (a continuous blackbody pulse: white, yellow, orange,
	// red as the filament cools), the brightness tracks the level more linearly and the filament colour cools with it
	const float coolK = saturate(domeParams[14].y);
	const float lvlG = mix(lvlE >= 0.45 ? sqrt(lvlE) : sqrt(0.45) * pow(lvlE / 0.45, 1.5), pow(lvlE, 0.8), coolK);
	const float T = 2900.0 * pow(max(lvl, 1e-3), mix(0.1235, 0.25, coolK));
	const vec3 bb = domeBlackbody(T);
	const float pk = mix(smoothstep(0.9, 1.0, level), pow(lvl, 6.0), coolK); // the 1-2 true peak frames (coolK: the hot end of the pulse)
	const float heatW = mix(smoothstep(0.85, 1.0, level), pow(lvl, 4.0), coolK);

	// The view ray refracted in at the smooth skin, carried through the wall to the inner surface
	vec3 VrIn = refract(-V, Ns, 1.0 / 1.49);
	if (dot(VrIn, VrIn) < 1e-4)
		VrIn = -Ns;
	VrIn = normalize(VrIn);
	const vec3 PinB = P + VrIn * (wallB / max(dot(-VrIn, Ns), 0.25));
	const vec3 PinC = P + VrIn * (max(P.y - capY, 0.0) / max(-VrIn.y, 0.25));
	vec3 Pin = mix(PinB, PinC, capW);
	Pin.y = min(Pin.y, capY);

	// Flute coordinates at the inner point: eP is the signed offset from the nearest groove root in flute periods, the
	// ridge tips sit at +-0.5. Each flank runs straight from a root to a tip (a sawtooth profile, the flutes' V)
	const float nFlutes = domeParams[3].y;
	const float period = 2.0 * DOME_PI / nFlutes;
	float theta = atan2(Pin.z, Pin.x) - domeParams[3].z;
	// pixel footprint (computed in uniform control flow, from continuous quantities): in flute periods around the axis and in
	// mm across the radius, for the line filters below
	const float fwRad = abs(dFdx(length(Pin.xz))) + abs(dFdy(length(Pin.xz)));
	const float fwF = (abs(Pin.z) * (abs(dFdx(Pin.x)) + abs(dFdy(Pin.x))) + abs(Pin.x) * (abs(dFdx(Pin.z)) + abs(dFdy(Pin.z)))) / max(dot(Pin.xz, Pin.xz), 1e-3) * domeParams[3].y / (2.0 * DOME_PI);
	const float fluteF = theta / period;
	const float idx = floor(fluteF + 0.5);
	const float eP = fluteF - idx;
	const float s = eP >= 0.0 ? 1.0 : -1.0;
	const float flankId = idx * 2.0 + (s > 0.0 ? 1.0 : 0.0);
	const float rIn = mix(rootR, ridgeR, 2.0 * abs(eP));
	const float eRoot = abs(eP), eTip = 0.5 - abs(eP);
	// the flute edges as lines; a milky body (dome_body) filters them over the pixel footprint: on the cap they run in to the
	// centre as spokes, which from the player's distance get thinner than a pixel and speckle
	// line filtering is active with a milky body only (defaults unchanged): fwA = the footprint, aaF = how much the per-flank
	// random strengths fade to their mean (a flute only a few pixels wide shows its average, not a hard step per flank)
	const float fwA = domeParams[10].x > 0.0 ? fwF : 0.0;
	const float aaF = saturate(3.0 * fwA - 0.15);
	const float edge = domeParams[10].x > 0.0 ? max(domeLine(eRoot, 0.09, fwF), domeLine(eTip, 0.09, fwF))
		: max(exp(-(eRoot / 0.09) * (eRoot / 0.09)), exp(-(eTip / 0.09) * (eTip / 0.09)));
	const float thN = mix(R - rIn, max(H - capY, 0.5), capW);    // wall thickness across the wall
	const float thH = 1.6 * mix(wallB, max(H - capY, 0.5), capW); // hemispherical mean path
	const vec3 trN = exp(-sigma * thN);
	const vec3 trH = exp(-sigma * thH);

	// Flank normal on the cavity side: the radius falls from root to tip, the flank leans toward its root
	const vec3 radial = normalize(vec3(Pin.x, 0.0, Pin.z) + vec3(1e-5, 0.0, 0.0));
	const vec3 tg = vec3(-radial.z, 0.0, radial.x);
	const float slope = (rootR - ridgeR) / max(0.5 * period * rIn, 1e-3);
	const vec3 nFlank = normalize(-radial - s * slope * tg);
	const vec3 nIn = normalize(mix(nFlank, vec3(0.0, -1.0, 0.0), capW));

	// Into the cavity (total internal reflection keeps the ray in the plastic)
	vec3 d2 = refract(VrIn, -nIn, 1.49);
	const bool tir = dot(d2, d2) < 1e-4;
	d2 = tir ? reflect(VrIn, -nIn) : normalize(d2);

	// Filament: a horizontal coil along x, 3.2 mm long
	const vec3 F = vec3(0.0, domeParams[3].x, 0.0);
	// The bulb's glass envelope (#906, T-3 1/4) round the filament, on its wedge base below ySock
	const float rbulb = 0.42 * rootR;
	const vec3 Fc = F + vec3(0.0, 0.25 * rbulb, 0.0);
	const float ySock = 0.22 * H;

	// Transmittance pass: near wall along the refracted ray, plus one more wall (the far wall, or for the floor inside, the
	// wall its light came in through); the flute edges (the cap's starburst ribs) bend the light away; the bulb's dark base
	// blocks it and its clear envelope reflects a little of it away
	BRANCH if (transPass)
	{
		// The flutes are a row of cylindrical lenses: what lies behind reads in vertical bands, brighter through the thin
		// groove roots, each flank its own strength (not on the cap: one moulding, only its ribs)
		const float band = mix((0.55 + 0.45 * cos(2.0 * DOME_PI * eP)) * (0.45 + 0.55 * domeHash(flankId * 3.71 + 0.9)), 1.0, capW);
		vec3 tr = exp(-sigma * (thN / max(dot(-VrIn, Ns), 0.3) + 1.2 * wallB)) * band * (0.5 - mix(0.3, 0.12, capW) * edge);
		vec3 nb;
		const float tb = tir ? -1.0 : domeBulb(Pin, d2, Fc, rbulb, nb);
		if (tb > 0.0)
			tr *= Pin.y + d2.y * tb < ySock ? 0.15 : 0.7;
		else if (!tir && d2.y < -1e-3)
		{
			// The floor inside (when the cavity ray reaches it before the far wall) lies in the shade of the dome and socket
			const float rMid = 0.5 * (rootR + ridgeR);
			const float a = dot(d2.xz, d2.xz);
			const float b = dot(Pin.xz, d2.xz);
			const float disc = b * b - a * (dot(Pin.xz, Pin.xz) - rMid * rMid);
			const float tFar = (a > 1e-4 && disc > 0.0) ? (-b + sqrt(disc)) / a : 1e9;
			if (-Pin.y / d2.y < tFar)
				tr *= 0.4;
		}
		// The lit look was tuned over an opaque body: as the lamp comes up, its scattered light hides what lies behind
		tr *= (1.0 - lvlG) * (1.0 - lvlG);
		// A thick body (dome_body) scatters it away: the interior (the additive layer) replaces the scene behind
		tr *= 1.0 - bodyK;
		gl_FragColor = vec4(vec3_splat(1.0) - tr, 1.0);
		return;
	}
	// Where the cavity ray passes the filament: vertical (dvF) and horizontal (dhF) miss distances
	const float tF = max(dot(F - Pin, d2), 0.0);
	const vec3 Q = Pin + d2 * tF;
	const float dvF = tir ? 99.0 : Q.y - F.y;
	const float dhF = tir ? 99.0 : length(vec3(Q.x * 0.35, 0.0, Q.z));

	// Body glow: scattered light in the rough flutes, falling off from the filament, +-25 % per facet
	const float dist = max(length(Pin - F), 4.0);
	const float fall = (12.0 / dist) * (12.0 / dist);
	const float facet = 0.75 + 0.5 * mix(domeHash(flankId * 3.0 + 1.7), 0.5, aaF);
	const float lvlB = mix(lvlE, lvlG, saturate(0.7 + 0.3 * edge));
	const float flash = 1.0 + domeParams[6].z * pk * (0.25 + 0.75 * edge);
	vec3 glow = bb * trH * fall * (0.65 + 0.9 * edge * (1.0 - 0.85 * capW)) * mix(facet, 1.0, 0.6 * capW) * (1.0 - 0.5 * capW);
	// ... and the far wall seen across the dome (through the near wall: red)
	{
		const float rMid = 0.5 * (rootR + ridgeR);
		const float a = dot(d2.xz, d2.xz);
		const float b = dot(Pin.xz, d2.xz);
		const float c = dot(Pin.xz, Pin.xz) - rMid * rMid;
		const float disc = b * b - a * c;
		if (!tir && a > 1e-4 && disc > 0.0)
		{
			const float tFar = (-b + sqrt(disc)) / a;
			const vec3 Pf = Pin + d2 * tFar;
			if (Pf.y > 0.0 && Pf.y < capY)
			{
				const float df = max(length(Pf - F), 4.0);
				glow += 0.8 * bb * trH * trN * (12.0 / df) * (12.0 / df);
			}
		}
	}
	// A thick body stays red at the peak: its glow flood steps back (the lamp scatter carries the body) and its flash is damped
	// dome_peak: the white-hot peak (whiteK, from 85 % of full; a level above 1 pushes it further) undoes that damping
	const float whiteK = min(peakK * heatW * (1.0 + 0.5 * saturate(level - 1.0)), 1.5);
	const vec3 body = lvlB * domeParams[5].x * glow * mix(mix(flash, 1.0 + 0.35 * (flash - 1.0), bodyK) * (1.0 - 0.7 * bodyK), flash, saturate(whiteK));

	// Bars: each flank transmits the coil as a narrow vertical bar where its GGX transmission lobe (in the horizontal
	// plane: a vertical flute smears elevation) meets the filament. Broken, staggered and tapered per flank.
	vec3 bars = vec3_splat(0.0);
	BRANCH if (lvlE > 0.0 && capW < 0.99)
	{
		// Each flank is evaluated once, at its own centre, so a flank inside the lobe is one bar and its neighbour stays dark;
		// the bar is drawn along the flank's groove root (the thin wall), BarWidth wide
		const float barAlpha = 0.22;
		const float a2 = barAlpha * barAlpha;
		const float thC = (idx + 0.25 * s) * period + domeParams[3].z;
		const vec3 radC = vec3(cos(thC), 0.0, sin(thC));
		const vec3 Pc = radC * 0.5 * (rootR + ridgeR);
		const vec3 nfH = normalize(-radC - s * slope * vec3(-radC.z, 0.0, radC.x));
		const vec3 woH = normalize(vec3(-VrIn.x, 0.0, -VrIn.z) + vec3(1e-5, 0.0, 0.0));
		float sB = 0.0;
		UNROLL for (int k = -2; k <= 2; k++)
		{
			const vec3 toL = vec3(float(k) * 0.8, 0.0, 0.0) - Pc;
			const vec3 Lb = normalize(vec3(toL.x, 0.0, toL.z) + vec3(1e-5, 0.0, 0.0));
			vec3 h = normalize(-1.49 * woH - Lb);
			if (dot(h, nfH) < 0.0)
				h = -h;
			const float nh = saturate(dot(nfH, h));
			const float den = nh * nh * (a2 - 1.0) + 1.0;
			sB += a2 * a2 / (den * den) * saturate(4.0 * dot(Lb, nfH)); // GGX, 1 at the lobe centre
		}
		const float across = 1.0 - smoothstep(0.6 * 0.09 - fwA, 0.09 + fwA, eRoot);
		const float hb1 = domeHash(flankId * 7.13 + 0.31);
		const float hb2 = domeHash(flankId * 3.77 + 5.17);
		const float hb3 = domeHash(flankId * 9.41 + 2.93);
		const float yc = 1.5 + (hb1 - 0.5) * 8.0;               // bar centre, above the filament
		const float hl = 5.0 * (0.6 + 0.9 * hb2);              // bar half-height
		const float prof = exp(-pow(abs((dvF - yc) / hl), 3.0));
		const float brk = mix(1.0, smoothstep(0.2, 0.7, domeNoise(Pin.y / 4.0 + 17.0 * hb2, s)), 0.7);
		const float yh = (dvF - 1.0 - (hb3 - 0.5) * 8.0) / (4.0 * (0.5 + 1.2 * hb2));
		const float tp = saturate(1.0 - abs(yh) / 2.2);         // parabolic taper: pointed ends, not round beads
		const float coreX = (1.0 - smoothstep(0.025, 0.045, eRoot)) * pow(tp, 1.5) * (hb1 > 0.5 ? 1.0 : 0.0);
		const float heat = mix(1.0, 3.0, heatW * (1.0 - 0.5 * capW));
		vec3 col = mix(bb * trN, vec3(1.0, 0.62, 0.10), 0.9);
		col = mix(col, bb, domeParams[8].x * pk * saturate(coreX * tp + 0.35 * tp)); // the hot coil's image goes white at the peak
		bars = domeParams[5].y * lvlG * (sB * 0.2) * across * brk * prof * mix(0.35, 1.0, tp) * heat * (1.0 + 15.0 * coreX * tp) * col * flash * (1.0 - capW);
		// Small orange bulb halo where the view ray passes the coil, carried by the flute edges
		bars += 0.5 * domeParams[6].x * lvlG * exp(-(dvF * dvF + 3.0 * dhF * dhF) / (2.5 * 2.5)) * edge * edge * edge * bb * vec3(1.0, 0.35, 0.06) * heat;
	}

	// Hot core: the rough flutes scatter the coil's image over a few flutes around the line of sight to the bulb (the
	// bright band at bulb height), yellow-hot at its centre
	vec3 core = vec3_splat(0.0);
	BRANCH if (lvlE > 0.0)
	{
		const float c = dot(VrIn, normalize(F - Pin));
		const float lobe = exp(-(1.0 - saturate(c)) / 0.02) * (1.0 - capW);
		// broken into the flutes: brighter along the groove roots, per-flank strength
		const float streak = (0.25 + 0.75 * exp(-(eRoot / 0.12) * (eRoot / 0.12))) * (0.5 + domeHash(flankId * 5.31 + 0.7));
		core = domeParams[6].x * lvlG * lobe * streak * mix(mix(bb * trN, vec3(1.0, 0.62, 0.10), 0.6 * lobe), bb, domeParams[8].x * pk * lobe) * mix(1.0, 3.0, heatW) * flash;
	}

	// Cap: the coil seen down through the flat cap (parallax correct: where the cavity ray crosses the coil)
	vec3 cap = vec3_splat(0.0);
	BRANCH if (capW > 0.0 && lvlE > 0.0)
	{
		const float rC = dhF / max(rootR, 1.0);
		cap = domeParams[6].y * lvlG * capW * bb * (vec3(1.0, 0.42, 0.10) * trN.r * exp(-rC / 0.1) * (0.3 + 0.7 * edge * edge)
			+ vec3(1.0, 0.85, 0.6) * 1.2 * exp(-(rC / 0.06) * (rC / 0.06))) * mix(1.0, 3.0, heatW);
	}

	// Peak: the outer shell goes white-hot for the 1-2 peak frames (red-wall leak that clips cream), ribs faintly visible,
	// the silhouette stays red
	const float nv = saturate(dot(Ns, V));
	const float whS = (1.0 - 0.45 * edge) * (0.05 + 0.95 * nv * nv) * fall;
	// at the peak the shell facing the viewer passes some of the filament's white (the leak), its silhouette stays red
	const vec3 white = (1.0 - 0.85 * bodyK * (1.0 - saturate(whiteK))) * domeParams[5].w * pk * whS * mix(vec3(1.0, 0.05, 0.018), bb * vec3(1.0, 0.8, 0.65), domeParams[8].x * nv * nv * fall / (1.0 + fall));

	// OFF sparkle: the view ray reflects off the (hash-tilted) inner facet with Fresnel, goes back out through the skin and
	// picks up a fixed studio room. Deep red: two passes through the wall. Ridge glints and needles on the flute edges.
	vec3 sparkle = vec3_splat(0.0);
	BRANCH if (domeParams[4].w > 0.0)
	{
		// The new OFF look (seamless cap, bulb envelope and base ring) is opt-in: over a see-through body only (mode 2), an
		// opaque body keeps the 10-29 look exactly
		const float newLook = mode > 1.5 ? 1.0 : 0.0;
		const float capN = capW * newLook;
		// The cap underside is one moulding (a starburst of thin ribs), not facets: no per-flank tilt or gain there, which drew
		// the cap as a fan of light and dark wedges (the 'seams')
		const float hA = mix(domeHash(flankId * 78.233 + 1.7), 0.5, capN);
		const float hB = mix(domeHash(flankId * 12.9898 + 7.3), 0.5, capN);
		const vec3 nJ = normalize(nIn + tg * (hB - 0.5) * 0.25 + vec3(0.0, (hA - 0.5) * 0.15, 0.0));
		const float ci = saturate(dot(VrIn, nJ));
		const float st2 = 1.49 * 1.49 * (1.0 - ci * ci);
		const float Fr = st2 >= 1.0 ? 1.0 : 0.0387 + 0.9613 * pow(1.0 - sqrt(1.0 - st2), 5.0);
		const vec3 rr = reflect(VrIn, nJ);
		vec3 ro = refract(rr, -Ns, 1.49);
		if (dot(ro, ro) < 1e-4)
			ro = reflect(rr, -Ns);
		ro = normalize(ro);
		const vec3 wd = ro.x * ex + ro.y * up + ro.z * ez;
		const vec3 tint = exp(-sigma * 2.0 * thN);
		const float facetGain = mix(mix(0.05, 2.5, hA * hA * hA), 0.65, aaF);
		const float side = mix(s > 0.0 ? 1.0 : 0.25, 1.0, capN);
		const float facing = smoothstep(-0.15, 0.35, dot(V, s * tg));
		const float spark = mix(mix(mix(0.12, 2.2, pow(domeHash(idx * 12.9898 + 4.1), 8.0)), 0.5, capN), 0.3, aaF); // a few hot facets, most dim
		sparkle = (1.0 + 1.5 * bodyK) * Fr * min(domeEnv(wd, v_tablePos, 1.0), vec3_splat(mix(8.0, 1.0, capW))) * tint * facetGain * side * spark * mix(1.0, 0.6 + 0.8 * edge, capN);
		// The room seen through the dome (into the cavity, out through the far wall): deep red, lensed by the flutes. Over a
		// see-through body, which shows the real scene behind (transmittance pass), it stays as the room light the flutes
		// scatter (the plastic's own ruby glow), weaker
		{
			const vec3 wt = d2.x * ex + d2.y * up + d2.z * ez;
			sparkle += (mode < 1.5 ? 0.35 : mix(0.2, 0.35, bodyK)) * (1.0 - Fr) * domeEnv(wt, v_tablePos, 0.0) * exp(-sigma * (thN + 1.2 * wallB)) * (0.7 + 0.6 * hA);
		}

		// Seen through the plastic (in and out through the walls: deep red): the bulb's glass envelope catches the room as a
		// small highlight, and the spun metal ring round its base a dim one (a shallow dish, its normal leaning to the axis)
		const vec3 tint2 = exp(-sigma * (thN + 1.2 * wallB));
		vec3 nb;
		const float tb = tir ? -1.0 : domeBulb(Pin, d2, Fc, rbulb, nb);
		if (newLook > 0.0 && tb > 0.0 && Pin.y + d2.y * tb >= ySock)
		{
			const float Fg = 0.04 + 0.96 * pow(1.0 - saturate(dot(-d2, nb)), 5.0);
			const vec3 rbv = reflect(d2, nb);
			sparkle += domeParams[6].w * (1.0 - 0.85 * bodyK) * Fg * min(domeEnv(rbv.x * ex + rbv.y * up + rbv.z * ez, v_tablePos, 1.0), vec3_splat(6.0)) * tint2;
		}
		if (newLook > 0.0 && !tir && d2.y < -0.05)
		{
			const float tR = (0.6 - Pin.y) / d2.y;
			const vec3 B = Pin + d2 * tR;
			const float rB = length(B.xz);
			const float ringW = smoothstep(rbulb, rbulb + 1.0, rB) * (1.0 - smoothstep(rootR - 1.5, rootR - 0.4, rB)) * (tb > 0.0 && tb < tR ? 0.0 : 1.0);
			if (ringW > 0.0)
			{
				const vec3 nR = normalize(vec3(-0.35 * B.x / max(rB, 1e-3), 1.0, -0.35 * B.z / max(rB, 1e-3)));
				const vec3 rr2 = reflect(d2, nR);
				sparkle += domeParams[6].w * (1.0 - 0.85 * bodyK) * 0.6 * ringW * (0.75 + 0.25 * sin(rB * 7.0)) * min(domeEnv(rr2.x * ex + rr2.y * up + rr2.z * ez, v_tablePos, 1.0), vec3_splat(6.0)) * tint2;
			}
		}

		// One edge per flute carries a ridge glint (1-D random cells along the height), a fraction of the flutes a long
		// needle on the other edge (linear taper to sharp tips); both slide with the view
		const float hF0 = domeHash(idx * 63.7 + 0.9);
		const float hF1 = domeHash(idx * 21.3 + 4.4);
		const float hF2 = domeHash(idx * 47.9 + 8.1);
		const float hF3 = domeHash(idx * 11.7 + 2.6);
		const float viewShift = 2.2 * dot(-V, tg) * s + 1.6 * V.y;
		const float Hb = max(capY - 1.0, 1.0);
		const bool tipSide = eTip < eRoot;
		const float dEdge = tipSide ? eTip : eRoot;
		if (tipSide == (hF0 < 0.5))
		{
			const float dE = dEdge / 0.022;
			const float Lseg = Hb * mix(0.15, 0.6, hF1);
			const float zc = Pin.y / (Lseg * 1.7) + hF3 * 7.0 + viewShift;
			const float cI = floor(zc);
			const float u = (fract(zc) - 0.5) * 3.4;
			const float hc = domeHash((cI + flankId * 17.0) * 12.9898);
			const float hc2 = domeHash((cI + flankId * 17.0) * 78.233);
			const float taper = exp(-2.5 * u * u) * (abs(u) < 1.3 ? 1.0 : 0.0);
			sparkle += domeParams[5].z * (1.0 + 4.0 * bodyK) * 0.33 * (hc < 0.45 ? 1.0 : 0.0) * taper * (fwA > 0.0 ? domeLine(dEdge, 0.022, fwA) : exp(-dE * dE)) * mix(0.3, 1.0, hc2 * hc2) * max(Fr, 0.2) * tint * facing * (1.0 - capW);
		}
		else if (hF2 < 0.5)
		{
			const float dN = dEdge / 0.04;
			const float Ln = Hb * mix(0.5, 1.0, hF3);
			const float mid = 1.5 + (Hb - min(Ln, Hb)) * hF0 + 0.5 * Ln;
			const float lin = saturate(1.0 - abs(Pin.y - mid) / (0.5 * Ln));
			const float hcN = mid + 0.5 * Ln * clamp(viewShift - 1.12, -1.0, 1.0);
			const float slide = exp(-pow((Pin.y - hcN) / (0.3 * Ln), 2.0));
			const float br = mix(0.35, 1.0, domeHash(idx * 5.13 + 1.1));
			const vec3 nCol = mix(tint, vec3(1.0, 0.6, 0.45) * tint.r, 0.8 * lin * lin * slide);
			sparkle += domeParams[5].z * (1.0 + 4.0 * bodyK) * 4.0 * lin * mix(0.3, 1.0, slide) * (fwA > 0.0 ? domeLine(dEdge, 0.04, fwA) : exp(-dN * dN)) * br * max(Fr, 0.2) * nCol * facing * mix(1.0, 0.3, capW);
		}
		sparkle *= domeParams[4].w * domeParams[13].z; // dome_glint
	}

	// GI glow: the table's light scattered in the red plastic (deep red: the hemispherical path through the wall), banded by
	// the flutes and their edges, a little stronger low (the light comes in from below, through the base and the plastic it
	// stands on) and on the cap's ribs; it gives way to the lamp's own glow as the lamp comes up
	// The side facing the light glows, the far side much less (a wrapped lobe: the light spreads round in the plastic)
	const float giSide = mix(1.0, 1.8 * saturate((dot(wN, domeParams[9].xyz) + 0.35) / 1.35), domeParams[9].w);
	const vec3 giGlow = (mode > 1.5 ? 1.0 : 0.0) * (1.0 - 0.5 * saturate(domeParams[10].x)) * domeParams[8].yzw * trH * mix(facet * (0.7 + 0.6 * edge), 0.85 + 0.3 * edge, capW)
		* giSide * (1.15 - 0.3 * saturate(P.y / max(H, 1.0))) * (1.0 - lvlG) * (1.0 / DOME_PI);

	// Thick body: the milky interior that replaces the scene behind (dome_body), the lamp's light diffused through it
	// (dome_scatter) and the top edge (dome_cap). All zero at the defaults.
	vec3 thick = vec3_splat(0.0);
	const float depthK = max(domeParams[14].x, 0.0);
	const float frostK = max(domeParams[15].w, 0.0);
	BRANCH if (bodyK > 0.0 || scatK > 0.0 || capK > 0.0 || peakK > 0.0 || depthK > 0.0 || frostK > 0.0)
	{
		const float bandF = mix(facet * (0.6 + 0.8 * edge), 0.7 + 0.6 * edge, capW); // the flutes (and the cap's ribs) band the scattered light
		// Interior, lamp off: the room (the table's environment level) and the table's lights at the dome (the GI irradiance, from
		// its side) scattered through the plastic: dim, deep in the plastic's colour, a touch brighter low (lit through the base)
		// and at the flute edges; it gives way to the lamp as it comes up
		// OFF the ridges read as thin lines through the wall (the VPE look: dark deep red, fine ridge lines, glints), not soft bands
		const float ridgeLine = (fwA > 0.0 ? domeLine(eTip, 0.035, fwA) : exp(-(eTip / 0.035) * (eTip / 0.035))) * (1.0 - capW);
		// seen through the smooth skin the inner flutes only ripple the glow (a soft cosine per flute, faint ridge lines), with
		// little flank-to-flank variation: the milky body evens it out
		const float bandOff = mix(mix(facet, 1.0, 0.8) * (0.88 + 0.12 * cos(2.0 * DOME_PI * eP)) + 0.4 * ridgeLine, 0.6 + 1.0 * edge, capW);
		const vec3 interior = bodyK * domeParams[10].w * (0.3 * domeParams[13].y * trH * bandOff * (1.1 - 0.25 * saturate(P.y / max(H, 1.0)))
			+ 0.3 * domeParams[8].yzw * trH * bandOff * giSide * (1.0 / DOME_PI)) * (1.0 - lvlG);
		// The lamp's light diffused through the milky body: broad (the scattering spreads it, a gentler falloff than the glow),
		// in the lamp's colour through the plastic, hot toward white at the peak (the leak)
		const float fallS = 8.0 / max(dist, 8.0);
		// Baked glow (dome_glow_map): the lamp's light scattered in the plastic, baked in Cycles on the dome's own UVs (white
		// light, clear plastic): it replaces the procedural falloff and flute banding, and the wall thickness it carries
		// tints it per texel (Beer-Lambert); outside the UV square (the inner faces) the procedural shape stays
		float gMap = -1.0;
		vec3 trG = trH;
		if (domeParams[15].x > 0.5 && v_texcoord0.x >= 0.0 && v_texcoord0.x <= 1.0 && v_texcoord0.y >= 0.0 && v_texcoord0.y <= 1.0)
		{
			const vec4 gT = texture2D(tex_traced_env, v_texcoord0);
			gMap = domeParams[15].y * 2.0 * pow(gT.r, 2.2);
			trG = exp(-sigma * clamp(4.0 * gT.g, 0.8, 4.0) * 1.6);
		}
		const float shapeS = gMap >= 0.0 ? gMap : fallS * bandF * (1.0 + 0.5 * fallS);
		const vec3 scatCol = mix(lampCol * trG, bb * mix(trH, vec3_splat(1.0), 0.5), domeParams[8].x * pk * fallS * fallS);
		// Soft knee on the level: the scattered body saturates below the tonemapper's desaturation (a red body at full, not
		// pink-white); the bars, core and cap carry the hot streaks
		const float scatL = mix(1.35 * lvlG / (0.35 + lvlG), lvlG, coolK);
		const vec3 lampScat = scatK * domeParams[11].w * 1.5 * scatL * scatCol * shapeS * mix(1.0, 1.0 + 0.2 * heatW, fallS) * (1.0 + 0.2 * (flash - 1.0));
		// The top edge: a rounded rim between the wall and the cap, catching the room (a bright ring on the OFF dome) and, lit,
		// piping the lamp's light out through its thin edge
		const float dEdge = length(vec2(rxz - domeParams[13].x, P.y - H));
		const float rimW = exp(-(dEdge / 0.7) * (dEdge / 0.7));
		const vec3 nB = normalize(radial + vec3(0.0, 1.0, 0.0));
		const vec3 rV = reflect(-V, normalize(mix(Ns, nB, 0.6)));
		const float FrB = 0.04 + 0.96 * pow(1.0 - nv, 5.0);
		const vec3 capRim = capK * domeParams[12].w * rimW * (24.0 * domeParams[13].y * max(FrB, 0.2) * (0.3 + min(domeEnv(rV.x * ex + rV.y * up + rV.z * ez, v_tablePos, 1.0), vec3_splat(6.0))) * mix(vec3_splat(1.0), trH, 0.3)
			+ 4.0 * lvlG * mix(lampCol, bb, domeParams[8].x * pk) * exp(-sigma * 0.5 * wallB) * (1.0 + 0.35 * (flash - 1.0)));
		// White-hot peak (dome_peak): the scattered body rolls to the filament's white and brightens, hottest near the bulb;
		// from above, the cap's middle (right over the bulb) glows as a soft hot spot through the milky plastic
		const vec3 hotCol = bb * vec3(1.0, 0.86, 0.68);
		const vec3 peakScat = (1.0 - 0.8 * saturate(max(domeParams[14].z, 0.0))) * whiteK * scatK * 2.5 * mix(lampCol * trG, hotCol, saturate(0.55 + 0.45 * fallS)) * (gMap >= 0.0 ? 0.5 * gMap : fallS) * (0.85 + 0.3 * edge) * (1.0 + 0.2 * (flash - 1.0));
		const float capHot = capW * exp(-(rxz / (0.5 * rootR)) * (rxz / (0.5 * rootR)));
		const vec3 capSpot = scatK * capHot * lvlG * (1.2 * mix(lampCol * trN, hotCol, 0.3) + 2.5 * whiteK * hotCol);
		// Depth (dome_depth, lamp off): the cavity ray, bent by the near flank it entered through, meets the far wall: its ridge
		// tips and groove roots show as lines at a different depth, each flute lensing its own slice, so they slide against the near
		// ridges as the view moves; where the ray is trapped in the wall (total internal reflection) the flank shines as a line
		vec3 depthL = vec3_splat(0.0);
		BRANCH if (depthK > 0.0)
		{
			float farL = 0.0;
			if (!tir)
			{
				const float rMidD = 0.5 * (rootR + ridgeR);
				const float aD = dot(d2.xz, d2.xz);
				const float bD = dot(Pin.xz, d2.xz);
				const float discD = bD * bD - aD * (dot(Pin.xz, Pin.xz) - rMidD * rMidD);
				if (aD > 1e-4 && discD > 0.0)
				{
					const vec3 Pf = Pin + d2 * ((-bD + sqrt(discD)) / aD);
					const float fF = (atan2(Pf.z, Pf.x) - domeParams[3].z) / period;
					const float eF = fF - floor(fF + 0.5);
					const float eTipF = 0.5 - abs(eF);
					farL = (domeLine(eTipF, 0.11, fwF) + 0.45 * domeLine(eF, 0.13, fwF)) * 0.55 // soft wide bands: the ridges are inside, seen through a smooth skin
						* smoothstep(0.5, 3.0, Pf.y) * (1.0 - smoothstep(capY - 3.0, capY, Pf.y)) * (0.6 + 0.8 * domeHash(floor(fF + 0.5) * 7.77 + 3.1));
				}
			}
			const float tirL = tir ? 0.3 : 0.0; // a trapped ray lights its flank faintly (bright at 1.2, it striped the dome bright-dark from the player's view)
			// The cap: its underside's ribs seen a few mm deeper along the cavity ray, a second starburst that slides against the
			// top one as the view moves
			float capL = 0.0;
			if (capW > 0.0 && !tir)
			{
				const vec3 Pd = Pin + d2 * (4.0 / max(-d2.y, 0.3));
				const float fC = (atan2(Pd.z, Pd.x) - domeParams[3].z) / period + 0.5;
				const float eC = fC - floor(fC + 0.5);
				capL = domeLine(eC, 0.07, fwF) * smoothstep(0.15 * rootR, 0.45 * rootR, length(Pd.xz)) * (1.0 - smoothstep(rootR - 1.0, rootR, length(Pd.xz)));
				// finer lines between the ribs, running in to a small dot at the centre (the moulding's sprue point)
				const float eC2 = 2.0 * eC - floor(2.0 * eC + 0.5);
				capL += 0.4 * domeLine(eC2, 0.06, 2.0 * fwF) * smoothstep(0.06 * rootR, 0.3 * rootR, length(Pd.xz)) * (1.0 - smoothstep(rootR - 1.0, rootR, length(Pd.xz)));
				capL += 0.8 * exp(-(length(Pd.xz) / (0.035 * rootR)) * (length(Pd.xz) / (0.035 * rootR)));
				// the zigzag ring: the flutes' sawtooth outline where they meet the cap, seen through it
				capL += 0.5 * domeLine(length(Pin.xz) - rIn, 0.3, fwRad);
			}
			const vec3 lit = domeParams[13].y * vec3_splat(1.0) + 3.0 * domeParams[8].yzw * giSide * (1.0 / DOME_PI);
			depthL = depthK * (1.0 - lvlG) * lit * ((1.0 - capW) * (2.5 * farL * trH * trN + tirL * trN) + capW * 3.0 * capL * trH);
		}
		// Frost (dome_frost; clear plastic, Ben's VPE Clear preset): a clear dome is neither see-through nor grey. Where the light's
		// path through the wall is long (the ridge tips, the cap, the base band, the rim) the plastic scatters the room to a
		// slightly warm milky white, the thin flanks keep a faint floor; grazing views run longer paths (a whiter silhouette).
		// It is lit by the room (environment level and the table's lights at the dome, from their side), so it takes the room's
		// tone; dark creases at the groove roots (light turned away by total internal reflection) draw the fine vertical lines.
		// As the lamp comes up its own light takes over (kept at 30 %)
		vec3 frost = vec3_splat(0.0);
		BRANCH if (frostK > 0.0)
		{
			const float baseBand = 1.0 - smoothstep(1.5, 3.5, P.y);
			const float pathMM = (thN + 2.0 * baseBand) / max(dot(-VrIn, Ns), 0.2);
			const float crest = mix(exp(-(eTip / 0.1) * (eTip / 0.1)), 1.0, aaF);
			// the cap is one thick moulding: milky all over, its ribs drawn by the other layers
			const float milkW = mix(saturate(0.672 * (smoothstep(1.0, 2.0, pathMM) * mix(1.0, crest, 0.9) + 0.29)), 0.75, capW) + 0.3 * pow(1.0 - nv, 3.0);
			const float crease = 1.0 - 0.3 * (fwA > 0.0 ? domeLine(eRoot, 0.06, fwA) : exp(-(eRoot / 0.06) * (eRoot / 0.06))) * (1.0 - capW);
			const vec3 roomIrr = domeParams[13].y * vec3_splat(1.0) + 3.0 * domeParams[8].yzw * giSide * (1.0 / DOME_PI);
			frost = frostK * FROST_GAIN * vec3(0.7435, 0.6829, 0.6253) * milkW * crease * roomIrr * trH * (1.0 - 0.7 * lvlG);
		}
		thick = interior + lampScat + capRim + peakScat + capSpot + depthL + frost;
	}

	// Orb (dome_orb, lamp lit): the bulb itself seen through the near wall. The cavity ray, bent by the flank it entered
	// through, passes the glass envelope: the coil glows hot inside it and the frosted glass and milky plastic spread it into
	// a soft ball with a halo. Each flute bends its own slice, so the ball breaks into vertical strips (on the cap, the ribs
	// make it a starburst) that slide as the view moves. Seen through the wall (the plastic's colour); bright enough that the
	// tonemapper rolls the hot centre to cream at full, warm orange in a low pulse.
	vec3 orb = vec3_splat(0.0);
	const float orbK = max(domeParams[14].z, 0.0);
	BRANCH if (orbK > 0.0 && lvlE > 0.0 && !tir)
	{
		const float tC = max(dot(Fc - Pin, d2), 0.0);
		const vec3 Qc = Pin + d2 * tC;
		const float miss = length(Qc - Fc) / rbulb; // 0: through the bulb's centre, 1: grazing the envelope
		// the dark wedge base below the socket hides the lower part
		const float base = smoothstep(ySock - 1.0, ySock + 1.0, Qc.y);
		// the coil (horizontal along x, 3.2 mm): its distance from the ray, spread by the frosted glass
		const vec3 Qf = Pin + d2 * max(dot(F - Pin, d2), 0.0);
		const float coilD = length(vec3(max(abs(Qf.x) - 1.6, 0.0), Qf.y - F.y, Qf.z));
		const float coil = exp(-(coilD / 1.6) * (coilD / 1.6));
		const float ball = exp(-2.2 * miss * miss) * base;          // the lit envelope
		const float halo = 0.35 * exp(-max(miss - 0.8, 0.0) / 0.9);   // spread by the plastic around it
		// flute lensing: brighter through the thin groove roots, each flank its own strength (on the cap: the ribs' starburst)
		const float strips = (0.35 + 0.65 * (0.5 + 0.5 * cos(2.0 * DOME_PI * eP))) * (0.7 + 0.6 * domeHash(flankId * 2.91 + 0.3));
		// hot centre white-yellow, the ball the filament's colour, the halo deeper; all through the near wall
		const vec3 hot = mix(bb, vec3(1.0, 0.95, 0.85), 0.6 * pk);
		// through the cap the bulb sits right under thin plastic: the hot centre is a white starburst (the ribs), not a band
		const vec3 col = (6.0 * coil * hot + 1.6 * ball * bb + halo * bb * vec3(1.0, 0.55, 0.3)) * (1.0 + 2.0 * capW)
			+ capW * 4.0 * coil * pk * vec3(1.0, 0.97, 0.9);
		const vec3 thru = mix(trN, vec3_splat(1.0), domeParams[8].x * coil * (0.3 + 0.7 * heatW)); // the white leak at the hot centre
		orb = orbK * domeParams[14].w * lvlG * mix(1.0, 3.0, heatW) * strips * col * thru;
	}

	// With the orb the hot light comes from the bulb's image: the white wash over the whole shell steps back (a red-orange
	// body with a white-hot centre, as in a bake), and so does the white-hot scattered body
	const float orbW = 1.0 - 0.9 * saturate(orbK);
	vec3 result = (body + bars + core + cap + white * orbW + orb) * domeParams[7].x + sparkle + giGlow + thick;
	if (domeParams[7].y > 0.5)
		result = domeParams[7].y < 1.5 ? (bars + core + cap) * domeParams[7].x : (domeParams[7].y < 2.5 ? body * domeParams[7].x : (domeParams[7].y < 3.5 ? sparkle : bars * domeParams[7].x));
	if (domeParams[7].y > 4.5)
		result = giGlow;
	if (domeParams[7].y > 5.5)
		result = thick;
	if (domeParams[7].y > 6.5) // diagnostics: 7 = lamp level map, falloff, lamp colour; 8 = body, scatter, plastic transmission
		result = domeParams[7].y < 7.5 ? 0.5 * vec3(lvlG, 8.0 / max(dist, 8.0), lampCol.r) : 0.5 * vec3(bodyK, scatK, trH.r);
	if (domeParams[7].y > 8.5) // diagnostics: 9 = the orb alone
		result = orb * domeParams[7].x;
	gl_FragColor = vec4(result, 1.0);
}
