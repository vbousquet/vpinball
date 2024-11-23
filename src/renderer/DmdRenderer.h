#pragma once

class DmdRenderer
{
public:
   DmdRenderer(RenderDevice* renderDevice);
   ~DmdRenderer();

   void SetUseLegacyRenderer(const bool useLegacy) { m_useLegacyRenderer = useLegacy; }
   void SetUnlitDotColor(vec4& color) { m_unlitDotColor = color; }
   void SetBackGlow(const float glow) { m_backGlow = glow; }
   void SetDotSize(const float size) { m_dotSize = size; }
   void SetDotSharpness(const float sharpness) { m_dotSharpness = sharpness; }
   void SetDotRounding(const float rounding) { m_dotRounding = rounding; }
   void SetDotGlow(const float glow) { m_dotGlow = glow; }
   void SetupDMDRender(BaseTexture* dmd, const vec4& dotColor, const float dotBrightness, const float alpha, const bool sRGB, const bool isColored);

private:
   RenderDevice* m_renderDevice;
   bool          m_useLegacyRenderer = false;
   float         m_dotSize;
   float         m_dotSharpness;
   float         m_dotRounding;
   float         m_dotGlow;
   float         m_backGlow;
   vec4          m_unlitDotColor;
   RenderTarget* m_blurs[4] = { nullptr };
   BaseTexture*  m_lastDmd = nullptr;
};
