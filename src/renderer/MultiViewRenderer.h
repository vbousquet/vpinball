#pragma once

#include "Renderer.h"
#include "DmdRenderer.h"

class MultiViewRenderer
{
public:
   MultiViewRenderer();
   ~MultiViewRenderer();

   void SetTable(PinTable* const table) { assert(m_table == nullptr); m_table = table; };

   void AddRenderView(VPX::Window* wnd, VideoSyncMode& syncMode, const StereoMode stereo3D);
   void AddVRMirrorView(VPX::Window* wnd, VPX::Window* srcWnd, const VRPreviewMode mode, const bool shrinkToFit);
   void AddDmd2DView(VPX::Window* wnd, const int dmdId);

   Renderer::SharedContext* GetRendererContext() { assert(!m_renderViews.empty()); return m_renderViews.front().renderer->GetContext(); }
   RenderDevice* GetRenderDevice() const { return m_renderDevice; }
   Renderer* GetCurrentRenderer() const { return m_currentRenderer; };

   void ApplyToRenderViews(std::function<void(Renderer*)> operation);

   void Render();

   Renderer* GetRenderer() { assert(!m_renderViews.empty()); return m_renderViews.front().renderer; } // FIXME hack

private:
   PinTable* m_table = nullptr;
   RenderDevice* m_renderDevice = nullptr;

   struct RenderView
   {
      VPX::Window* output;
      Renderer* renderer;
   };
   vector<RenderView> m_renderViews;
   Renderer* m_currentRenderer = nullptr;

   struct VRPreview
   {
      VPX::Window* output;
      VRPreviewMode mode;
      bool shrinkToFit;
      Renderer* renderer;
   };
   vector<VRPreview> m_vrPreviews;

   struct Dmd2DView
   {
      VPX::Window* output;
      int dmdId;
      DmdRenderer* renderer;
      int lastFrameId;
      float exposure;
      vec4 dotColor;
      float dotBrightness;
   };
   vector<Dmd2DView> m_dmd2DViews;
};
