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

$input v_worldPos, v_tablePos, v_normal, v_texcoord0
#ifdef STEREO
	$input v_eye
#endif
#ifdef CLIP
	$input v_clipDistance
#endif

#include "common.sh"

uniform vec4 domeParams[10];

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

float domeHash(const float n) { return fract(sin(n) * 43758.5453); }

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
	const vec3 sigma = vec3(0.100, 1.400, 1.500) * domeParams[7].z; // red acrylic absorption (1/mm)
	const float wallB = R - 0.5 * (rootR + ridgeR);

	// Only the outer skin emits (the inner flutes, base and cap underside are hidden behind it or seen through it); the base
	// flange and the tabs still filter what lies under them (in, and the light that lit it, through the flange)
	if (!((Ns.y > 0.5 && P.y > capY + 0.2) || (dot(Ns.xz, P.xz) > 0.3 * rxz && rxz > rootR + 0.1)))
	{
		if (!transPass)
			discard;
		gl_FragColor = vec4(vec3_splat(1.0) - exp(-sigma * 2.5 * wallB), 1.0);
		return;
	}
	const float capW = smoothstep(0.55, 0.9, Ns.y) * smoothstep(capY, capY + 0.5, P.y);

	// Lamp level: emission black below 1 %, mid levels lifted (two-segment map), filament temperature from the level
	const float level = max(domeParams[3].w, 0.0);
	const float lvl = saturate(level);
	const float lvlE = saturate((lvl - 0.01) / 0.99);
	const float lvlG = lvlE >= 0.45 ? sqrt(lvlE) : sqrt(0.45) * pow(lvlE / 0.45, 1.5);
	const float T = 2900.0 * pow(max(lvl, 1e-3), 0.1235);
	const vec3 bb = domeBlackbody(T);
	const float pk = smoothstep(0.9, 1.0, level);                    // the 1-2 true peak frames
	const float heatW = smoothstep(0.85, 1.0, level);

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
	const float fluteF = theta / period;
	const float idx = floor(fluteF + 0.5);
	const float eP = fluteF - idx;
	const float s = eP >= 0.0 ? 1.0 : -1.0;
	const float flankId = idx * 2.0 + (s > 0.0 ? 1.0 : 0.0);
	const float rIn = mix(rootR, ridgeR, 2.0 * abs(eP));
	const float eRoot = abs(eP), eTip = 0.5 - abs(eP);
	const float edge = max(exp(-(eRoot / 0.09) * (eRoot / 0.09)), exp(-(eTip / 0.09) * (eTip / 0.09)));
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
	const float facet = 0.75 + 0.5 * domeHash(flankId * 3.0 + 1.7);
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
	const vec3 body = lvlB * domeParams[5].x * glow * flash;

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
		const float across = 1.0 - smoothstep(0.6 * 0.09, 0.09, eRoot);
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
	const vec3 white = domeParams[5].w * pk * whS * mix(vec3(1.0, 0.05, 0.018), bb * vec3(1.0, 0.8, 0.65), domeParams[8].x * nv * nv * fall / (1.0 + fall));

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
		const float facetGain = mix(0.05, 2.5, hA * hA * hA);
		const float side = mix(s > 0.0 ? 1.0 : 0.25, 1.0, capN);
		const float facing = smoothstep(-0.15, 0.35, dot(V, s * tg));
		const float spark = mix(mix(0.12, 2.2, pow(domeHash(idx * 12.9898 + 4.1), 8.0)), 0.5, capN); // a few hot facets, most dim
		sparkle = Fr * min(domeStudio(wd), mix(8.0, 1.0, capW)) * tint * facetGain * side * spark * mix(1.0, 0.6 + 0.8 * edge, capN);
		// The room seen through the dome (into the cavity, out through the far wall): deep red, lensed by the flutes. Over a
		// see-through body, which shows the real scene behind (transmittance pass), it stays as the room light the flutes
		// scatter (the plastic's own ruby glow), weaker
		{
			const vec3 wt = d2.x * ex + d2.y * up + d2.z * ez;
			sparkle += (mode < 1.5 ? 0.35 : 0.2) * (1.0 - Fr) * domeStudio(wt) * exp(-sigma * (thN + 1.2 * wallB)) * (0.7 + 0.6 * hA);
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
			sparkle += domeParams[6].w * Fg * min(domeStudio(rbv.x * ex + rbv.y * up + rbv.z * ez), 6.0) * tint2;
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
				sparkle += domeParams[6].w * 0.6 * ringW * (0.75 + 0.25 * sin(rB * 7.0)) * min(domeStudio(rr2.x * ex + rr2.y * up + rr2.z * ez), 6.0) * tint2;
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
			sparkle += domeParams[5].z * 0.33 * (hc < 0.45 ? 1.0 : 0.0) * taper * exp(-dE * dE) * mix(0.3, 1.0, hc2 * hc2) * max(Fr, 0.2) * tint * facing * (1.0 - capW);
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
			sparkle += domeParams[5].z * 4.0 * lin * mix(0.3, 1.0, slide) * exp(-dN * dN) * br * max(Fr, 0.2) * nCol * facing * mix(1.0, 0.3, capW);
		}
		sparkle *= domeParams[4].w;
	}

	// GI glow: the table's light scattered in the red plastic (deep red: the hemispherical path through the wall), banded by
	// the flutes and their edges, a little stronger low (the light comes in from below, through the base and the plastic it
	// stands on) and on the cap's ribs; it gives way to the lamp's own glow as the lamp comes up
	// The side facing the light glows, the far side much less (a wrapped lobe: the light spreads round in the plastic)
	const float giSide = mix(1.0, 1.8 * saturate((dot(wN, domeParams[9].xyz) + 0.35) / 1.35), domeParams[9].w);
	const vec3 giGlow = (mode > 1.5 ? 1.0 : 0.0) * domeParams[8].yzw * trH * mix(facet * (0.7 + 0.6 * edge), 0.85 + 0.3 * edge, capW)
		* giSide * (1.15 - 0.3 * saturate(P.y / max(H, 1.0))) * (1.0 - lvlG) * (1.0 / DOME_PI);

	vec3 result = (body + bars + core + cap + white) * domeParams[7].x + sparkle + giGlow;
	if (domeParams[7].y > 0.5)
		result = domeParams[7].y < 1.5 ? (bars + core + cap) * domeParams[7].x : (domeParams[7].y < 2.5 ? body * domeParams[7].x : (domeParams[7].y < 3.5 ? sparkle : bars * domeParams[7].x));
	if (domeParams[7].y > 4.5)
		result = giGlow;
	gl_FragColor = vec4(result, 1.0);
}
