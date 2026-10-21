// license:GPLv3+

// Fragment shader of the depth prepass for the analytic ball impostor (paired with vs_ball_raycast):
// raycasts the unit sphere exactly like fs_ball to get the exact silhouette and surface point, writes
// the analytic hit depth, and:
// - OUT_NORMAL variant outputs the view space normal xy (z is reconstructed by consumers, normal is
//   flipped to always face the viewer)
// - OUT_NORMAL+MOTION additionally outputs the motion vector in zw: the screen space UV displacement
//   of the hit point between the previous and the current frame, accounting for camera and ball motion,
//   consumed by the AO temporal accumulation when the camera is dynamic (VR or headtracking)

$input v_rayOriginView, v_rayDirView, v_rayOriginObj, v_rayDirObj // camera ray in view space and in unit sphere space (same ray parameter)
#ifdef STEREO
    $input v_eye
#endif

#include "common.sh"

uniform mat4 orientation; // unit sphere object space to world space (rot * scale * translate)
#ifdef CLIP
    uniform vec4 clip_plane;
#endif
#ifdef STEREO
    uniform mat4 matProj[2];
    #define mProj matProj[int(round(v_eye))]
    #ifdef OUT_NORMAL
        uniform mat4 ballViewToObj[2];
        #define mBallViewToObj ballViewToObj[int(round(v_eye))]
    #endif
    #ifdef MOTION
        uniform mat4 matRotViewProjPrev[2];
        uniform vec4 cameraPosWorldPrev[2];
        #define mRotViewProjPrev    matRotViewProjPrev[int(round(v_eye))]
        #define cCameraPosWorldPrev cameraPosWorldPrev[int(round(v_eye))].xyz
    #endif
#else
    uniform mat4 matProj;
    #define mProj matProj
    #ifdef OUT_NORMAL
        uniform mat4 ballViewToObj;
        #define mBallViewToObj ballViewToObj
    #endif
    #ifdef MOTION
        uniform mat4 matRotViewProjPrev;
        uniform vec4 cameraPosWorldPrev;
        #define mRotViewProjPrev    matRotViewProjPrev
        #define cCameraPosWorldPrev cameraPosWorldPrev.xyz
    #endif
#endif
#ifdef MOTION
    uniform mat4 orientationPrev; // unit sphere object space to world space at the previous frame
#endif

void main()
{
    // Analytic raycast of the unit sphere, in its own space (same as fs_ball): exact silhouette and surface point
    const vec3 o_o = v_rayOriginObj;
    const vec3 d_o = v_rayDirObj;
    const float invLenD = inversesqrt(dot(d_o, d_o));
    const vec3 dir = d_o * invLenD;
    const float b = dot(o_o, dir);
    const vec3 h = o_o - b * dir; // ray point closest to the center
    const float discr = 1.0 - dot(h, h);
    if (discr <= 0.0)
        discard;
    const float t_dir = -b - sqrt(discr); // along the normalized direction
    const float t_hit = t_dir * invLenD; // ray parameter, shared with view space
    if (t_hit <= 0.0) // Before the ray origin, that is in front of the near plane
        discard;
    const vec3 n_obj = normalize(o_o + t_dir * dir); // hit point on the unit sphere = exact normal
    const vec3 hit_world = mul(orientation, vec4(n_obj, 1.0)).xyz;

    #ifdef CLIP
    // Test the clip plane at the analytic surface point (not at the impostor quad depth)
    if (dot(clip_plane, vec4(hit_world, 1.0)) < 0.0)
       discard;
    #endif

    // Exact per pixel depth of the analytic hit
    const vec3 hit_view = v_rayOriginView + t_hit * v_rayDirView;
    const vec4 hit_clip = mul(mProj, vec4(hit_view, 1.0));
    #if BGFX_SHADER_LANGUAGE_GLSL || BGFX_SHADER_LANGUAGE_ESSL
        gl_FragDepth = 0.5 + 0.5 * (hit_clip.z / hit_clip.w);
    #else
        gl_FragDepth = hit_clip.z / hit_clip.w;
    #endif

    #ifdef OUT_NORMAL
        // Normals transform by the transpose of the inverse transform (same as fs_ball), then are flipped
        // to always face the viewer so that z can be reconstructed from xy
        vec3 N = normalize(mul(vec4(n_obj, 0.0), mBallViewToObj).xyz);
        if (N.z < 0.0)
            N = -N;
        #ifdef MOTION
            // Screen space UV displacement of the hit point between the previous and the current frame
            // (previous position under the previous ball orientation and the previous camera transform)
            vec2 motion = vec2(0.0, 0.0);
            const vec3 prev_world = mul(orientationPrev, vec4(n_obj, 1.0)).xyz;
            const vec4 prev_clip = mul(mRotViewProjPrev, vec4(prev_world - cCameraPosWorldPrev, 1.0));
            BRANCH if (prev_clip.w > 0.0)
            {
                const vec2 delta = hit_clip.xy / hit_clip.w - prev_clip.xy / prev_clip.w;
                #if TEX_V_IS_UP
                    motion = 0.5 * delta;
                #else
                    motion = vec2(0.5 * delta.x, -0.5 * delta.y);
                #endif
            }
            gl_FragColor = vec4(N.xy, motion);
        #else
            gl_FragColor = vec4(N.xy, 0.0, 1.0);
        #endif
    #else
        gl_FragColor = vec4_splat(0.0);
    #endif
}
