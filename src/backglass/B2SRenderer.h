#pragma once

#include "B2SDataModel.h"
#include <future>

namespace VPX
{

class B2SRenderer
{
public:
   B2SRenderer();
   ~B2SRenderer();

   void Load(const string& path);
   void RenderSetup(RenderDevice* device, VPX::RenderOutput* output);
   void Render();
   void RenderRelease();

private:
   void DrawImage(const Texture* texture, const vec4& color, const float srcX, const float srcY, const float srcW, const float srcH, const float dstX, const float dstY, const float dstW,
      const float dstH) const;

   std::future<std::shared_ptr<B2STable>> m_loadedB2S;

   RenderDevice* m_rd = nullptr;
   VPX::RenderOutput* m_output = nullptr;
   std::shared_ptr<B2STable> m_b2s;
   std::unique_ptr<class B2SRenderState> m_renderInfo;

   friend class B2SRenderState;
};

}