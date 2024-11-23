#include "core/stdafx.h"
#include "MultiViewRenderer.h"

MultiViewRenderer::MultiViewRenderer()
{
}

MultiViewRenderer::~MultiViewRenderer()
{
   Renderer::SharedContext* rendererContext = nullptr;
   for (RenderView& view : m_renderViews)
   {
      rendererContext = view.renderer->GetContext();
      delete view.renderer;
   }
   delete rendererContext;

   for (Dmd2DView& view : m_dmd2DViews)
      delete view.renderer;

   delete m_renderDevice;
}

void MultiViewRenderer::AddRenderView(VPX::Window* wnd, VideoSyncMode& syncMode, const StereoMode stereo3D)
{
   assert(m_table);
   bool isMainWindow = m_renderDevice == nullptr;
   if (m_renderDevice == nullptr)
   {
      const bool useNvidiaApi = m_table->m_settings.LoadValueWithDefault(Settings::Player, "UseNVidiaAPI"s, false);
      const bool disableDWM = m_table->m_settings.LoadValueWithDefault(Settings::Player, "DisableDWM"s, false);
      const bool compressTextures = m_table->m_settings.LoadValueWithDefault(Settings::Player, "CompressTextures"s, false);
      const bool stereo3DfakeStereo = stereo3D == STEREO_VR ? false : m_table->m_settings.LoadValueWithDefault(Settings::Player, "Stereo3DFake"s, false);
      const int nEyes = (stereo3D == STEREO_VR || (stereo3D != STEREO_OFF && !stereo3DfakeStereo)) ? 2 : 1;
      m_renderDevice = new RenderDevice(wnd, nEyes, useNvidiaApi, disableDWM, compressTextures, syncMode);
   }
   Renderer::SharedContext* context = m_renderViews.empty() ? new Renderer::SharedContext(m_table) : m_renderViews[0].renderer->GetContext();
   assert(m_renderViews.size() < MAX_RENDERER_COUNT);
   Renderer* renderer = new Renderer(static_cast<unsigned int>(m_renderViews.size()), context, m_renderDevice, wnd, syncMode, stereo3D);
   m_renderViews.push_back(RenderView { wnd, renderer });
   if (!isMainWindow)
      m_renderDevice->AddWindow(wnd);
}

void MultiViewRenderer::AddVRMirrorView(VPX::Window* wnd, VPX::Window* srcWnd, const VRPreviewMode mode, const bool shrinkToFit)
{
   assert(m_renderDevice);
   for (RenderView& view : m_renderViews)
   {
      if (view.output == srcWnd)
      {
         m_vrPreviews.push_back(VRPreview { wnd, mode, shrinkToFit, view.renderer });
         m_renderDevice->AddWindow(wnd);
         return;
      }
   }
   PLOGE << "Asked to add a VR preview for a not previously registered source output";
}

void MultiViewRenderer::AddDmd2DView(VPX::Window* wnd, const int dmdId)
{
   assert(m_table);
   assert(m_renderDevice);

   Dmd2DView view { wnd, dmdId };
   const int dmdProfile = m_table->m_settings.LoadValueWithDefault(Settings::DMD, "RenderProfile"s, 0);
   const string prefix = "User."s + std::to_string(dmdProfile + 1) + "."s;

   view.exposure = m_table->m_settings.LoadValueWithDefault(Settings::DMD, "Exposure"s, 2.f);
   view.dotColor = convertColor(m_table->m_settings.LoadValueWithDefault(Settings::DMD, prefix + "DotTint"s, 0x002D52FF)); // Default tint is Neon plasma (255, 82, 45)
   // Convert color as settings are sRGB color while shader needs linear RGB color
   #define InvsRGB(x) (((x) <= 0.04045f) ? ((x) * (float)(1.0 / 12.92)) : (powf((x) * (float)(1.0 / 1.055) + (float)(0.055 / 1.055), 2.4f)))
   view.dotColor.x = InvsRGB(view.dotColor.x);
   view.dotColor.y = InvsRGB(view.dotColor.y);
   view.dotColor.z = InvsRGB(view.dotColor.z);
   #undef InvsRGB
   view.dotBrightness = m_table->m_settings.LoadValueWithDefault(Settings::DMD, prefix + "DotBrightness"s, 5.0f);

   // FIXME the renderer should not be attached to the output view but to the DMD and reused for internal rendering (for VR...)
   view.renderer = new DmdRenderer(m_renderDevice);
   view.renderer->SetDotSize(m_table->m_settings.LoadValueWithDefault(Settings::DMD, prefix + "DotSize"s, 0.85f));
   view.renderer->SetDotSharpness(m_table->m_settings.LoadValueWithDefault(Settings::DMD, prefix + "DotSharpness"s, 0.8f));
   view.renderer->SetDotRounding(m_table->m_settings.LoadValueWithDefault(Settings::DMD, prefix + "DotRounding"s, 0.85f));
   view.renderer->SetDotGlow(m_table->m_settings.LoadValueWithDefault(Settings::DMD, prefix + "DotGlow"s, 0.015f));
   view.renderer->SetBackGlow(m_table->m_settings.LoadValueWithDefault(Settings::DMD, prefix + "BackGlow"s, 0.005f));
   vec4 unlitDotColor = convertColor(m_table->m_settings.LoadValueWithDefault(Settings::DMD, prefix + "UnlitDotColor"s, 0x00202020));
   #define InvsRGB(x) (((x) <= 0.04045f) ? ((x) * (float)(1.0 / 12.92)) : (powf((x) * (float)(1.0 / 1.055) + (float)(0.055 / 1.055), 2.4f)))
   unlitDotColor.x = InvsRGB(unlitDotColor.x);
   unlitDotColor.y = InvsRGB(unlitDotColor.y);
   unlitDotColor.z = InvsRGB(unlitDotColor.z);
   #undef InvsRGB
   view.renderer->SetUnlitDotColor(unlitDotColor);
   bool useLegacyRenderer = m_table->m_settings.LoadValueWithDefault(Settings::DMD, prefix + "Legacy"s, false);
   #if !defined(ENABLE_BGFX)
      useLegacyRenderer = true; // Only available for BGFX
   #endif
   view.renderer->SetUseLegacyRenderer(useLegacyRenderer);

   m_dmd2DViews.push_back(view);
   m_renderDevice->AddWindow(wnd);
}

void MultiViewRenderer::ApplyToRenderViews(std::function<void(Renderer*)> operation)
{
   assert(m_currentRenderer == nullptr);
   for (RenderView& view : m_renderViews)
   {
      m_currentRenderer = view.renderer;
      operation(view.renderer);
   }
}

void MultiViewRenderer::Render()
{
   for (RenderView& view : m_renderViews)
   {
      m_currentRenderer = view.renderer;
      view.renderer->RenderFrame();
      m_renderDevice->GetCurrentPass()->m_isFinalPass = true;
   }
   m_currentRenderer = nullptr;

   for (VRPreview& view : m_vrPreviews)
   {
      RenderTarget* const stereoRT = view.renderer->GetVRRenderTarget();
      const int w = stereoRT->GetWidth(), h = stereoRT->GetHeight();
      RenderTarget* const outRT = view.output->GetBackBuffer();
      const int outW = view.mode == VRPREVIEW_BOTH ? outRT->GetWidth() / 2 : outRT->GetWidth(), outH = outRT->GetHeight();
      const float ar = (float)w / (float)h, outAr = (float)outW / (float)outH;
      int x = 0, y = 0, fw = w, fh = h;
      if ((view.shrinkToFit && ar < outAr) || (!view.shrinkToFit && ar > outAr))
      { // Fit on Y
         const int scaledW = (int)(h * outAr);
         x = (w - scaledW) / 2;
         fw = scaledW;
      }
      else
      { // Fit on X
         const int scaledH = (int)(w / outAr);
         y = (h - scaledH) / 2;
         fh = scaledH;
      }
      m_renderDevice->SetRenderTarget("VR Preview"s, outRT, false);
      m_renderDevice->AddRenderTargetDependency(stereoRT);
      if ((view.shrinkToFit && (ar != outAr)) || (view.mode == VRPREVIEW_DISABLED))
         m_renderDevice->Clear(clearType::TARGET | clearType::ZBUFFER, 0, 1.0f, 0L);
      if (view.mode == VRPREVIEW_LEFT || view.mode == VRPREVIEW_RIGHT)
         m_renderDevice->BlitRenderTarget(stereoRT, outRT, true, false, x, y, fw, fh, 0, 0, outW, outH, view.mode == VRPREVIEW_LEFT ? 0 : 1, 0);
      else if (view.mode == VRPREVIEW_BOTH)
      {
         m_renderDevice->BlitRenderTarget(stereoRT, outRT, true, false, x, y, fw, fh, 0, 0, outW, outH, 0, 0);
         m_renderDevice->BlitRenderTarget(stereoRT, outRT, true, false, x, y, fw, fh, outW, 0, outW, outH, 1, 0);
      }
      m_renderDevice->GetCurrentPass()->m_isFinalPass = true;
   }

   for (Dmd2DView& view : m_dmd2DViews)
   {
      Player::ControllerDisplay dmd = g_pplayer->GetControllerDisplay(view.dmdId);
      if (dmd.frame && view.lastFrameId != dmd.frameId)
      {
         m_renderDevice->ResetRenderState();
         m_renderDevice->SetRenderState(RenderState::ALPHABLENDENABLE, RenderState::RS_FALSE);
         m_renderDevice->SetRenderState(RenderState::CULLMODE, RenderState::CULL_NONE);
         m_renderDevice->SetRenderState(RenderState::ZWRITEENABLE, RenderState::RS_FALSE);
         m_renderDevice->SetRenderState(RenderState::ZENABLE, RenderState::RS_FALSE);
         m_renderDevice->SetRenderTarget("DMDView", view.output->GetBackBuffer(), false);
         view.renderer->SetupDMDRender(dmd.frame, view.dotColor, view.dotBrightness, 1.f, true, dmd.frame->m_format != BaseTexture::BW);
         m_renderDevice->m_DMDShader->SetVector(SHADER_exposure_wcg, view.exposure, 1.f, 1.f, 0.f);
         const float rtAR = static_cast<float>(view.output->GetBackBuffer()->GetWidth()) / static_cast<float>(view.output->GetBackBuffer()->GetHeight());
         const float dmdAR = static_cast<float>(dmd.frame->width()) / static_cast<float>(dmd.frame->height());
         const float w = rtAR > dmdAR ? dmdAR / rtAR : 1.f;
         const float h = rtAR < dmdAR ? rtAR / dmdAR : 1.f;
         const Vertex3D_NoTex2 vertices[4] = {
            {  w, -h, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f },
            { -w, -h, 0.f, 0.f, 0.f, 1.f, 0.f, 1.f },
            {  w,  h, 0.f, 0.f, 0.f, 1.f, 1.f, 0.f },
            { -w,  h, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f }
         };
         m_renderDevice->DrawTexturedQuad(m_renderDevice->m_DMDShader, vertices);
         m_renderDevice->GetCurrentPass()->m_isFinalPass = true;
      }
   }
}
