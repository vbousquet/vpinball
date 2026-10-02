// license:GPLv3+

#pragma once

#include "common.h"

namespace PUP
{

// Plugin-side texture block: pixel data is owned by the plugin so it can be
// produced and consumed on any thread (the VPX texture API is restricted to
// the API thread). A host texture is lazily created/uploaded when the block is
// drawn through a real GPU render context (see ResolveTexture in PUPPlugin.cpp).
struct PUPTextureBlock
{
   void Set(int width, int height, VPXTextureFormat format, const void* image);

   VPXTextureInfo info {};
   vector<uint8_t> pixels;
   VPXTexture hostTexture = nullptr; // lazily created VPX texture, only used for GPU render contexts
   bool hostDirty = false;
};

// CPU compositing target exposing the VPXRenderContext2D interface, allowing the
// existing PUPScreen render path to be reused to render a screen to a raw
// SRGB888 frame buffer advertised through the controller plugin API.
class PUPSoftwareContext final
{
public:
   PUPSoftwareContext(unsigned int width, unsigned int height);

   VPXRenderContext2D* GetContext() { return &m_ctx; }
   unsigned int GetWidth() const { return m_width; }
   unsigned int GetHeight() const { return m_height; }

   // Composite lifecycle, called on the display render thread
   void BeginFrame();
   void EndFrame();

   // Last completed frame, thread safe (the frame pointer remains valid until
   // the next GetFrame call, as required by the controller display API)
   DisplayFrame GetFrame();

   static void MSGPIAPI DrawImageImpl(VPXRenderContext2D* ctx, VPXTexture texture, const float tintR, const float tintG, const float tintB, const float alpha, const float texX,
      const float texY, const float texW, const float texH, const float pivotX, const float pivotY, const float rotation, const float srcX, const float srcY, const float srcW,
      const float srcH);

private:
   VPXRenderContext2D m_ctx {};
   const unsigned int m_width;
   const unsigned int m_height;
   vector<uint8_t> m_frame; // work buffer written during composite (render thread only)

   std::mutex m_frameMutex;
   vector<uint8_t> m_published; // last completed frame
   unsigned int m_publishedId = 0;
};

}
