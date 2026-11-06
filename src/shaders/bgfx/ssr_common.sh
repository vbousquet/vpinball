// license:GPLv3+

// Shared helpers for the screen space ray marched PBR specular pass (march + resolve).
// The including shader must declare the samplers these helpers sample:
//   tex_depth     (prepass depth buffer)
//   tex_normals   (prepass color attachment 0: shaded view space normals xy in .xy)
//   tex_fb_filtered (scene color, only needed by ssrRayMarch)
// and, for the RAYMARCH helpers, the matProj/matProjInv uniforms per eye.
// Like all helpers in these post process shaders, v_eye is passed explicitly since it
// is only in scope inside main().

#ifdef STEREO
	#define mProj    matProj[int(round(v_eye))]
	#define mProjInv matProjInv[int(round(v_eye))]
#else
	#define mProj    matProj
	#define mProjInv matProjInv
#endif

// Depth buffer value to clip space z (OpenGL uses a -1..1 clip range)
float toClipZ(const float d)
{
   #if TEX_V_IS_UP
      return d * 2.0 - 1.0;
   #else
      return d;
   #endif
}

#ifdef STEREO
vec3 uvDepthToView(const vec2 u, const float d, const float v_eye)
#else
vec3 uvDepthToView(const vec2 u, const float d)
#endif
{
   #if TEX_V_IS_UP
      const vec4 clip = vec4(u.x * 2.0 - 1.0, u.y * 2.0 - 1.0, toClipZ(d), 1.0);
   #else
      const vec4 clip = vec4(u.x * 2.0 - 1.0, 1.0 - u.y * 2.0, d, 1.0);
   #endif
   const vec4 p = mul(mProjInv, clip);
   return p.xyz / p.w;
}

// Projects a view space position back to uv + clip z
#ifdef STEREO
vec2 viewToUvZ(const vec3 p, const float v_eye, out float clipZ, out float clipW)
#else
vec2 viewToUvZ(const vec3 p, out float clipZ, out float clipW)
#endif
{
   const vec4 clip = mul(mProj, vec4(p, 1.0));
   clipZ = clip.z / clip.w;
   clipW = clip.w;
   #if TEX_V_IS_UP
      return (clip.xy / clip.w) * 0.5 + 0.5;
   #else
      return vec2(clip.x, -clip.y) * (0.5 / clip.w) + 0.5;
   #endif
}

#ifdef STEREO
vec3 get_shaded_normal(const vec2 u, const float v_eye)
#else
vec3 get_shaded_normal(const vec2 u)
#endif
{
   const vec2 nxy = texStereoNoLod(tex_normals, u).xy;
   return vec3(nxy, sqrt(saturate(1.0 - dot(nxy, nxy))));
}

// Marches a single ray against the prepass depth and returns the premultiplied reflection
// contribution: rgb = hit color * confidence, a = confidence. The confidence combines the
// screen edge & distance fades with a penetration test evaluated at the first behind sample:
// rays ending barely below a surface are likely depth precision noise (faded in), rays far
// behind it tunneled through (rejected).
#ifdef STEREO
vec4 ssrRayMarch(const vec3 posV, const vec3 R, const vec3 normal, const float ushift, const float v_eye)
#else
vec4 ssrRayMarch(const vec3 posV, const vec3 R, const vec3 normal, const float ushift)
#endif
{
   const int steps = 32;
   const float dist0 = length(posV);
   // Grazing rays need a larger start offset to clear the surface they originate from
   const float t0 = 0.02 * dist0 / max(dot(R, normal), 0.25);
   const float maxDist = 4.0 * dist0;
   const float stepGrowth = pow(maxDist / t0, 1.0 / float(steps));
   float t = t0 * pow(stepGrowth, ushift);
   float tPrev = 0.0;
   float tHit = -1.0;
   float hitDepth = 0.0;
   vec2 uvHit = vec2_splat(0.0);
   LOOP for(int i=0; i<steps; ++i)
   {
      const vec3 p = posV + R * t;
      float clipZ, clipW;
      #ifdef STEREO
         const vec2 suv = viewToUvZ(p, v_eye, clipZ, clipW);
      #else
         const vec2 suv = viewToUvZ(p, clipZ, clipW);
      #endif
      BRANCH if(clipW <= 0.0 || clipZ >= 1.0 || suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0)
         break; // ray left the view frustum: it will not come back
      hitDepth = texStereoNoLod(tex_depth, suv).x;
      const float sceneZ = toClipZ(hitDepth);
      BRANCH if(sceneZ < clipZ) // ray went behind the recorded opaque surface
      {
         tHit = t;
         uvHit = suv;
         break;
      }
      tPrev = t;
      t *= stepGrowth;
   }

   if(tHit <= 0.0)
      return vec4_splat(0.0);

   // Penetration depth at the first behind sample, bounded by the size of the last march step
   const float maxGap = max(tHit - tPrev, 0.002 * dist0);
   #ifdef STEREO
      const vec3 surfPos = uvDepthToView(uvHit, hitDepth, v_eye);
   #else
      const vec3 surfPos = uvDepthToView(uvHit, hitDepth);
   #endif
   const float behindGap = surfPos.z - (posV + R * tHit).z; // +z toward the viewer: behind the surface has a smaller z
   if(behindGap <= 0.0 || behindGap > maxGap)
      return vec4_splat(0.0);

   // Refine the hit point between the last front sample and the first back sample
   float tLo = tPrev, tHi = tHit;
   UNROLL for(int i=0; i<6; ++i)
   {
      const float tMid = (tLo + tHi) * 0.5;
      const vec3 p = posV + R * tMid;
      float clipZ, clipW;
      #ifdef STEREO
         const vec2 suv = viewToUvZ(p, v_eye, clipZ, clipW);
      #else
         const vec2 suv = viewToUvZ(p, clipZ, clipW);
      #endif
      BRANCH if(clipW <= 0.0 || suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0)
         break;
      const float sceneZ = toClipZ(texStereoNoLod(tex_depth, suv).x);
      if(sceneZ < clipZ)
      {
         tHi = tMid;
         uvHit = suv;
      }
      else
         tLo = tMid;
   }

   // Reject hits on surfaces whose normal faces away from the ray (hit from behind)
   #ifdef STEREO
      BRANCH if(dot(get_shaded_normal(uvHit, v_eye), R) > 0.0)
   #else
      BRANCH if(dot(get_shaded_normal(uvHit), R) > 0.0)
   #endif
      return vec4_splat(0.0);

   const float edgeFade = saturate(min(uvHit.x, 1.0 - uvHit.x) * 20.0) * saturate(min(uvHit.y, 1.0 - uvHit.y) * 20.0);
   const float distFade = 1.0 - tHi / maxDist;
   const float conf = saturate(behindGap / (0.25 * maxGap)) * edgeFade * distFade;
   const vec3 hitColor = texStereoNoLod(tex_fb_filtered, uvHit).rgb;
   return vec4(hitColor * conf, conf);
}
