// license:GPLv3+

// Vertex shader of the depth prepass: same transform as vs_basic but it only
// outputs the varyings consumed by fs_depth (BGFX requires vertex shader
// outputs to exactly match the fragment shader inputs)

$input a_position, a_normal, a_texcoord0
$output v_worldPos, v_normal, v_texcoord0
#ifdef STEREO
    $output v_eye
#endif
#ifdef CLIP
    $output v_clipDistance
#endif
#ifdef MOTION
    // Clip position under the current and previous frame camera transforms, evaluated per pixel to
    // derive the screen space motion vector written to the 2 free channels of the prepass target
    $output v_clipPos
    $output v_prevClipPos
#endif

#include "common.sh"

uniform mat4 matWorld;
#ifdef STEREO
    uniform vec4 layer;
    uniform mat4 matWorldView[2];
    uniform mat4 matWorldViewInverseTranspose[2];
    uniform mat4 matRotViewProj[2];
    uniform vec4 cameraPosWorld[2];
    #define mWorldView                 matWorldView[int(round(v_eye))]
    #define mWorldViewInverseTranspose matWorldViewInverseTranspose[int(round(v_eye))]
    #define mRotViewProj               matRotViewProj[int(round(v_eye))]
    #define cCameraPosWorld            cameraPosWorld[int(round(v_eye))].xyz
#else
    uniform mat4 matWorldView;
    uniform mat4 matWorldViewInverseTranspose;
    uniform mat4 matRotViewProj;
    uniform vec4 cameraPosWorld;
    #define mWorldView                 matWorldView
    #define mWorldViewInverseTranspose matWorldViewInverseTranspose
    #define mRotViewProj               matRotViewProj
    #define cCameraPosWorld            cameraPosWorld.xyz
#endif
#ifdef CLIP
    uniform vec4 clip_plane;
#endif
#ifdef MOTION
    #ifdef STEREO
        uniform mat4 matRotViewProjPrev[2];
        uniform vec4 cameraPosWorldPrev[2];
        #define mRotViewProjPrev    matRotViewProjPrev[int(round(v_eye))]
        #define cCameraPosWorldPrev cameraPosWorldPrev[int(round(v_eye))].xyz
    #else
        uniform mat4 matRotViewProjPrev;
        uniform vec4 cameraPosWorldPrev;
        #define mRotViewProjPrev    matRotViewProjPrev
        #define cCameraPosWorldPrev cameraPosWorldPrev.xyz
    #endif
#endif

void main()
{
    #ifdef STEREO
        gl_Layer = gl_InstanceID;
        v_eye = layer.x + gl_InstanceID;
    #endif
    vec4 pos = vec4(a_position, 1.0);
    vec4 tpos = mul(matWorld, pos);
    v_worldPos = mul(mWorldView, pos).xyz;
    #ifdef CLIP
        v_clipDistance = dot(clip_plane, tpos);
    #endif
    // Due to layback, we use non affine transform so we need to use inverse transpose of the world view matrix
    v_normal = normalize(mul(mWorldViewInverseTranspose, vec4(a_normal, 0.0)).xyz);
    // Camera-relative clip-space transform: subtract the camera's world position from the world-space
    // vertex position and apply (viewRotation x proj). This avoids GPU f32 precision loss on Adreno (Quest)
    // that comes from baking the large camera translation into matWorldViewProj on the CPU.
    vec3 wpos_rel = tpos.xyz - cCameraPosWorld;
    gl_Position = mul(mRotViewProj, vec4(wpos_rel, 1.0));
    #ifdef MOTION
        v_clipPos = gl_Position;
        v_prevClipPos = mul(mRotViewProjPrev, vec4(tpos.xyz - cCameraPosWorldPrev, 1.0));
    #endif
    v_texcoord0 = a_texcoord0;
}
