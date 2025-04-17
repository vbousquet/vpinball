#pragma once

#include "core/stdafx.h"
#include "B2SRenderer.h"

namespace VPX
{

class B2SRenderState
{
public:
   B2SRenderState(std::shared_ptr<B2STable> b2s, RenderDevice* rd, VPX::RenderOutput* output)
      : m_b2s(b2s)
      , m_rd(rd)
      , m_output(output)
   {
   }

   ~B2SRenderState()
   {
      delete m_renderRT;
      m_renderRT = nullptr;
   }

   void RenderBegin()
   {
      m_initialRT = m_rd->GetCurrentRenderTarget();
      m_outputRT = nullptr;
#ifdef ENABLE_BGFX
      if (m_output->GetMode() == VPX::RenderOutput::OM_WINDOW)
      {
         m_output->GetWindow()->Show();
         m_outputRT = m_output->GetWindow()->GetBackBuffer();
         m_outputX = m_outputY = 0;
         m_outputW = m_outputRT->GetWidth();
         m_outputH = m_outputRT->GetHeight();
      }
      else
#endif
         if (m_output->GetMode() == VPX::RenderOutput::OM_EMBEDDED)
      {
         m_output->GetEmbeddedWindow()->GetPos(m_outputX, m_outputY);
         m_outputRT = g_pplayer->m_playfieldWnd->GetBackBuffer();
         m_outputW = m_output->GetEmbeddedWindow()->GetWidth();
         m_outputW = m_output->GetEmbeddedWindow()->GetHeight();
         m_outputH = m_outputRT->GetHeight() - m_outputY - m_outputH;
      }

      if ((m_initialRT != m_outputRT) && (m_renderRT == nullptr))
         m_renderRT = new RenderTarget(
            m_rd, SurfaceType::RT_DEFAULT, "BackglassBackBuffer"s, m_outputW, m_outputH, colorFormat::RGBA16F, false, 1, "Fatal Error: unable to create backglass back buffer");

      bool m_showGrill = false;
      m_grillCut = m_showGrill ? 0.f : m_b2s->m_grillHeight;
      const B2SImage& bgImage = m_b2s->m_backglassImage.m_image ? m_b2s->m_backglassImage : m_b2s->m_backglassOffImage;
      m_b2sHeight = bgImage.m_image->m_height - m_grillCut;
      m_windowPos.x = -1.f;
      m_windowPos.y = -1.f;
      m_windowScale.x = 2.f / static_cast<float>(bgImage.m_image->m_width);
      m_windowScale.y = 2.f / static_cast<float>(m_b2sHeight);
   }

   const std::shared_ptr<B2STable> m_b2s;
   RenderDevice* const m_rd;
   VPX::RenderOutput* const m_output;

   RenderTarget* m_initialRT = nullptr;
   RenderTarget* m_renderRT = nullptr;
   RenderTarget* m_outputRT = nullptr;
   int m_outputX, m_outputY, m_outputW, m_outputH;

   Vertex2D m_windowPos;
   Vertex2D m_windowScale;
   float m_b2sHeight = 0.f;
   float m_grillCut = 0.f;

   friend class B2SRenderer;
};


B2SRenderer::B2SRenderer() { }

B2SRenderer::~B2SRenderer() { }

void B2SRenderer::Load(const string& path)
{
   auto loadFile = [](const string& path)
   {
      std::shared_ptr<B2STable> b2s;
      try
      {
         tinyxml2::XMLDocument b2sTree;
         b2sTree.LoadFile(path.c_str());
         if (b2sTree.FirstChildElement("DirectB2SData"))
            b2s = std::make_shared<B2STable>(*b2sTree.FirstChildElement("DirectB2SData"));
      }
      catch (...)
      {
         PLOGE << "Failed to load B2S file: " << path;
      }
      return b2s;
   };
   // B2S file format is heavily unoptimized so perform loading asynchronously (all assets are directly included in the XML file using Base64 encoding)
   m_loadedB2S = std::async(std::launch::async, loadFile, path);
}

void B2SRenderer::RenderSetup(RenderDevice* device, VPX::RenderOutput* output)
{
   m_rd = device;
   m_output = output;
}

void B2SRenderer::RenderRelease()
{
   m_rd = nullptr;
   m_output = nullptr;
   m_renderInfo = nullptr;
}

void B2SRenderer::Render()
{
   if (m_b2s == nullptr)
   {
      if (m_loadedB2S.valid())
      {
         m_b2s = m_loadedB2S.get();
         m_loadedB2S = std::future<std::shared_ptr<B2STable>>();
         if (m_b2s == nullptr)
            return;
         m_renderInfo = std::make_unique<B2SRenderState>(m_b2s, m_rd, m_output);
      }
      else
         return;
   }

   m_renderInfo->RenderBegin();

   if (m_renderInfo->m_initialRT != m_renderInfo->m_outputRT)
   {
      m_renderInfo->m_rd->SetRenderTarget("BackglassView"s, m_renderInfo->m_renderRT, true, true);
      m_renderInfo->m_rd->AddRenderTargetDependency(m_renderInfo->m_initialRT, false);
      m_renderInfo->m_rd->Clear(clearType::TARGET, 0);
   }
   else
   {
      m_renderInfo->m_rd->SetRenderTarget("BackglassView"s, m_renderInfo->m_outputRT, true, true);
      // FIXME clear rect
   }

   const vec4 white(1.f, 1.f, 1.f, 1.f);

   // Draw backgound
   {
      float backgroundLight = 0.f;
      if (m_b2s->m_backglassOnImage.m_image)
         backgroundLight = g_pplayer->m_resURIResolver.GetOutput(m_b2s->m_backglassOnImage.m_uriBrightness);
      if (backgroundLight < 1.f)
      {
         if (m_b2s->m_backglassImage.m_image)
            DrawImage(m_b2s->m_backglassImage.m_image.get(), white, 0, m_renderInfo->m_grillCut, m_b2s->m_backglassImage.m_image->m_width,
               m_b2s->m_backglassImage.m_image->m_height - m_renderInfo->m_grillCut, 0, 0, m_b2s->m_backglassImage.m_image->m_width,
               m_b2s->m_backglassImage.m_image->m_height - m_renderInfo->m_grillCut);
         else if (m_b2s->m_backglassOffImage.m_image)
            DrawImage(m_b2s->m_backglassOffImage.m_image.get(), white, 0, m_renderInfo->m_grillCut, m_b2s->m_backglassOffImage.m_image->m_width,
               m_b2s->m_backglassOffImage.m_image->m_height - m_renderInfo->m_grillCut, 0, 0, m_b2s->m_backglassOffImage.m_image->m_width,
               m_b2s->m_backglassOffImage.m_image->m_height - m_renderInfo->m_grillCut);
      }
      if (backgroundLight > 0.f)
         DrawImage(m_b2s->m_backglassOnImage.m_image.get(), vec4(1.f, 1.f, 1.f, backgroundLight), 0, m_renderInfo->m_grillCut, m_b2s->m_backglassOnImage.m_image->m_width,
            m_b2s->m_backglassOnImage.m_image->m_height - m_renderInfo->m_grillCut, 0, 0, m_b2s->m_backglassOnImage.m_image->m_width,
            m_b2s->m_backglassOnImage.m_image->m_height - m_renderInfo->m_grillCut);
   }

   // Draw illuminations
   for (const auto& bulb : m_b2s->m_backglassIlluminations)
   {
      const float x = bulb.m_locationX;
      const float y = bulb.m_locationY;
      const float w = bulb.m_width;
      const float h = bulb.m_height;
      float state = g_pplayer->m_resURIResolver.GetOutput(bulb.m_uriBrightness);
      if (bulb.m_romInverted)
         state = 1.f - state;
      DrawImage(bulb.m_image.get(), vec4(bulb.m_lightColor.x, bulb.m_lightColor.y, bulb.m_lightColor.z, state), 0, 0, bulb.m_image->m_width, bulb.m_image->m_height, x, y, w, h);
   }

   g_pplayer->m_renderer->UpdateBasicShaderMatrix();

   if (m_renderInfo->m_initialRT != m_renderInfo->m_outputRT)
   {
      const float jitter = (float)((msec() & 2047) / 1000.0);
      m_renderInfo->m_rd->SetRenderTarget("BackglassOutput"s, m_renderInfo->m_outputRT, true, true);
      m_renderInfo->m_rd->AddRenderTargetDependency(m_renderInfo->m_renderRT, false);
      m_renderInfo->m_rd->m_FBShader->SetTexture(SHADER_tex_fb_unfiltered, m_renderInfo->m_renderRT->GetColorSampler());
      m_renderInfo->m_rd->m_FBShader->SetTexture(SHADER_tex_fb_filtered, m_renderInfo->m_renderRT->GetColorSampler());
      m_renderInfo->m_rd->m_FBShader->SetVector(SHADER_bloom_dither_colorgrade,
         0.f, // Bloom
         1.f, // Dither
         0.f, // LUT colorgrade
         0.f);
      m_renderInfo->m_rd->m_FBShader->SetVector(SHADER_w_h_height, static_cast<float>(1.0 / static_cast<double>(m_renderInfo->m_outputW)),
         static_cast<float>(1.0 / static_cast<double>(m_renderInfo->m_outputH)),
         jitter, // radical_inverse(jittertime) * 11.0f,
         jitter); // sobol(jittertime) * 13.0f); // jitter for dither pattern}
      ShaderTechniques tonemapTechnique;
      switch (g_pplayer->m_renderer->m_toneMapper)
      {
      case TM_REINHARD: tonemapTechnique = SHADER_TECHNIQUE_fb_rhtonemap_no_filter; break;
      case TM_FILMIC: tonemapTechnique = SHADER_TECHNIQUE_fb_fmtonemap_no_filter; break;
      case TM_NEUTRAL: tonemapTechnique = SHADER_TECHNIQUE_fb_nttonemap_no_filter; break;
      case TM_AGX: tonemapTechnique = SHADER_TECHNIQUE_fb_agxtonemap_no_filter; break;
      case TM_AGX_PUNCHY: tonemapTechnique = SHADER_TECHNIQUE_fb_agxptonemap_no_filter; break;
      default: assert(!"unknown tonemapper"); break;
      }
      m_renderInfo->m_rd->m_FBShader->SetTechnique(tonemapTechnique);
      m_renderInfo->m_rd->DrawFullscreenTexturedQuad(m_renderInfo->m_rd->m_FBShader);
   }
}

void B2SRenderer::DrawImage(
   const Texture* texture, const vec4& color,
   const float srcX, const float srcY, const float srcW, const float srcH,
   const float dstX, const float dstY, const float dstW, const float dstH) const
{
   // Needed since y=0 is at bottom of the image
   const B2SImage& bgImage = m_b2s->m_backglassImage.m_image ? m_b2s->m_backglassImage : m_b2s->m_backglassOffImage;
   const float vx1 = m_renderInfo->m_windowPos.x + dstX * m_renderInfo->m_windowScale.x;
   const float vy1 = m_renderInfo->m_windowPos.y + (m_renderInfo->m_b2sHeight - dstY - dstH) * m_renderInfo->m_windowScale.y;
   const float vx2 = vx1 + dstW * m_renderInfo->m_windowScale.x;
   const float vy2 = vy1 + dstH * m_renderInfo->m_windowScale.y;
   const float tx1 = srcX / texture->m_width;
   const float ty1 = 1.f - srcY / texture->m_height;
   const float tx2 = (srcX + srcW) / texture->m_width;
   const float ty2 = 1.f - (srcY + srcH) / texture->m_height;
   const Vertex3D_NoTex2 vertices[4]
      = { { vx2, vy1, 0.f, 0.f, 0.f, 1.f, tx2, ty1 }, { vx1, vy1, 0.f, 0.f, 0.f, 1.f, tx1, ty1 }, { vx2, vy2, 0.f, 0.f, 0.f, 1.f, tx2, ty2 }, { vx1, vy2, 0.f, 0.f, 0.f, 1.f, tx1, ty2 } };

   Matrix3D matWorldViewProj[2];
   matWorldViewProj[0] = Matrix3D::MatrixIdentity();
   const int eyes = m_renderInfo->m_rd->GetCurrentRenderTarget()->m_nLayers;
   if (eyes > 1)
      matWorldViewProj[1] = matWorldViewProj[0];
   m_renderInfo->m_rd->m_DMDShader->SetMatrix(SHADER_matWorldViewProj, &matWorldViewProj[0], eyes);
   m_renderInfo->m_rd->m_DMDShader->SetVector(SHADER_vColor_Intensity, &color);
   m_renderInfo->m_rd->m_DMDShader->SetVector(SHADER_glassArea, 0.0f, 0.0f, 1.0f, 1.0f);
   m_renderInfo->m_rd->m_DMDShader->SetVector(SHADER_glassPad, 0.0f, 0.0f, 0.0f, 0.0f);
   m_renderInfo->m_rd->m_DMDShader->SetTexture(SHADER_tex_sprite, texture->m_pdsBuffer);
   m_renderInfo->m_rd->m_DMDShader->SetTechnique(SHADER_TECHNIQUE_basic_noDMD_world);
   m_renderInfo->m_rd->m_DMDShader->SetFloat(SHADER_alphaTestValue, 0.0f);
   m_renderInfo->m_rd->ResetRenderState();
   m_renderInfo->m_rd->SetRenderState(RenderState::ZWRITEENABLE, RenderState::RS_FALSE);
   m_renderInfo->m_rd->SetRenderState(RenderState::ZENABLE, RenderState::RS_FALSE);
   m_renderInfo->m_rd->SetRenderState(RenderState::CULLMODE, RenderState::CULL_NONE);
   if (color.w != 1.f)
   {
      m_renderInfo->m_rd->SetRenderState(RenderState::ALPHABLENDENABLE, RenderState::RS_TRUE);
      m_renderInfo->m_rd->SetRenderState(RenderState::SRCBLEND, RenderState::SRC_ALPHA);
      m_renderInfo->m_rd->SetRenderState(RenderState::DESTBLEND, RenderState::INVSRC_ALPHA);
      m_renderInfo->m_rd->SetRenderState(RenderState::BLENDOP, RenderState::BLENDOP_ADD);
   }
   else
      m_renderInfo->m_rd->SetRenderState(RenderState::ALPHABLENDENABLE, RenderState::RS_FALSE);
   m_renderInfo->m_rd->DrawTexturedQuad(m_renderInfo->m_rd->m_DMDShader, vertices, true, 0.f);
}

}