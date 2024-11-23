#include "core/stdafx.h"
#include "DmdRenderer.h"

DmdRenderer::DmdRenderer(RenderDevice* renderDevice)
   : m_renderDevice(renderDevice)
{

}

DmdRenderer::~DmdRenderer()
{
   for (int i = 0; i < 4; i++)
      delete m_blurs[i];
}

void DmdRenderer::SetupDMDRender(BaseTexture* dmd, const vec4& dotColor, const float dotBrightness, const float alpha, const bool sRGB, const bool isColored)
{
   // Legacy DMD renderer
   if (m_useLegacyRenderer)
   {
      m_renderDevice->m_DMDShader->SetVector(SHADER_vColor_Intensity, dotColor.x, dotColor.y, dotColor.z, dotBrightness);
      #ifdef DMD_UPSCALE
         m_renderDevice->m_DMDShader->SetVector(SHADER_vRes_Alpha_time, (float)(dmd->width() * 3), (float)(dmd->height() * 3), alpha, (float)(g_pplayer->m_overall_frames % 2048));
      #else
         m_renderDevice->m_DMDShader->SetVector(SHADER_vRes_Alpha_time, (float)dmd->width(), (float)dmd->height(), alpha, (float)(g_pplayer->m_overall_frames % 2048));
      #endif
      m_renderDevice->m_DMDShader->SetTechnique(SHADER_TECHNIQUE_basic_DMD);
      m_renderDevice->m_DMDShader->SetTexture(SHADER_tex_dmd, dmd, SF_NONE, SA_CLAMP, SA_CLAMP, true);
   }
   // New DMD renderer
   else
   {
      Sampler* dmdSampler = m_renderDevice->m_texMan.LoadTexture(dmd, SamplerFilter::SF_BILINEAR, SamplerAddressMode::SA_CLAMP, SamplerAddressMode::SA_CLAMP, true);
      if (m_blurs[0] == nullptr || m_blurs[0]->GetWidth() != dmdSampler->GetWidth() || m_blurs[0]->GetHeight() != dmdSampler->GetHeight())
      {
         for (int i = 0; i < 4; i++)
         {
            delete m_blurs[i];
            m_blurs[i] = new RenderTarget(m_renderDevice, SurfaceType::RT_DEFAULT, "DMDBlur" + std::to_string(i), dmd->width(), dmd->height(), colorFormat::RGBA8, false, 1, "");
         }
      }
      if (m_lastDmd != dmd)
      {
         m_lastDmd = dmd;
         RenderPass* const initial_rt = m_renderDevice->GetCurrentPass();
         for (int i = 0; i < 3; i++)
         {
            {
               m_renderDevice->SetRenderTarget("DMD HBlur "s + std::to_string(i + 1), m_blurs[0], false);
               if (i > 0)
                  m_renderDevice->AddRenderTargetDependency(m_blurs[i]);
               m_renderDevice->m_FBShader->SetTexture(SHADER_tex_fb_filtered, i == 0 ? dmdSampler : m_blurs[i]->GetColorSampler());
               m_renderDevice->m_FBShader->SetVector(SHADER_w_h_height, (float)(1.0 / dmdSampler->GetWidth()), (float)(1.0 / dmdSampler->GetHeight()), 1.0f, 1.0f);
               m_renderDevice->m_FBShader->SetTechnique(i == 0 ? SHADER_TECHNIQUE_fb_blur_horiz7x7 : SHADER_TECHNIQUE_fb_blur_horiz9x9);
               m_renderDevice->DrawFullscreenTexturedQuad(m_renderDevice->m_FBShader);
            }
            {
               m_renderDevice->SetRenderTarget("DMD VBlur "s + std::to_string(i + 1), m_blurs[i + 1], false);
               m_renderDevice->AddRenderTargetDependency(m_blurs[0]);
               m_renderDevice->m_FBShader->SetTexture(SHADER_tex_fb_filtered, m_blurs[0]->GetColorSampler());
               m_renderDevice->m_FBShader->SetVector(SHADER_w_h_height, (float)(1.0 / dmdSampler->GetWidth()), (float)(1.0 / dmdSampler->GetHeight()), 1.0f, 1.0f);
               m_renderDevice->m_FBShader->SetTechnique(i == 0 ? SHADER_TECHNIQUE_fb_blur_vert7x7 : SHADER_TECHNIQUE_fb_blur_vert9x9);
               m_renderDevice->DrawFullscreenTexturedQuad(m_renderDevice->m_FBShader);
            }
         }
         m_renderDevice->SetRenderTarget(initial_rt->m_name, initial_rt->m_rt, true);
         initial_rt->m_name += '-';
      }
      m_renderDevice->m_DMDShader->SetVector(SHADER_w_h_height, m_dotSize, m_dotSharpness, m_dotRounding, isColored ? 1.f : 0.f /* luminance or RGB */);
      m_renderDevice->m_DMDShader->SetVector(SHADER_vColor_Intensity, dotColor.x * dotBrightness, dotColor.y * dotBrightness, dotColor.z * dotBrightness,
         dotBrightness); // dot color (only used if we received brightness data, premultiplied by overall brightness) and overall brightness (used for colored date)
      m_renderDevice->m_DMDShader->SetVector(SHADER_staticColor_Alpha, m_unlitDotColor.x, m_unlitDotColor.y, m_unlitDotColor.z, 0.f /* unused */);
      m_renderDevice->m_DMDShader->SetVector(SHADER_vRes_Alpha_time, (float)dmd->width(), (float)dmd->height(), m_dotGlow * dotBrightness, m_backGlow * dotBrightness);

      m_renderDevice->m_DMDShader->SetTechnique(sRGB ? SHADER_TECHNIQUE_basic_DMD2_srgb : SHADER_TECHNIQUE_basic_DMD2);
      m_renderDevice->m_DMDShader->SetTexture(SHADER_tex_dmd, dmd);
      m_renderDevice->AddRenderTargetDependency(m_blurs[1]);
      m_renderDevice->m_DMDShader->SetTexture(SHADER_dmdDotGlow, m_blurs[1]->GetColorSampler());
      m_renderDevice->AddRenderTargetDependency(m_blurs[3]);
      m_renderDevice->m_DMDShader->SetTexture(SHADER_dmdBackGlow, m_blurs[3]->GetColorSampler()); // FIXME why don't we directly blur from 1 to 3 ?
   }
}
