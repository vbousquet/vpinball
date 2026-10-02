// license:GPLv3+

#include "PUPSoftwareContext.h"

#include <cfloat>

namespace PUP
{

static int BytesPerPixel(VPXTextureFormat format)
{
   switch (format)
   {
   case VPXTextureFormat::VPXTEXFMT_BW32F: return 4;
   case VPXTextureFormat::VPXTEXFMT_sRGB8: return 3;
   case VPXTextureFormat::VPXTEXFMT_sRGBA8: return 4;
   case VPXTextureFormat::VPXTEXFMT_sRGB565: return 2;
   }
   assert(false);
   return 4;
}

void PUPTextureBlock::Set(int width, int height, VPXTextureFormat format, const void* image)
{
   pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(BytesPerPixel(format)));
   if (image != nullptr)
      memcpy(pixels.data(), image, pixels.size());
   else
      memset(pixels.data(), 0, pixels.size());
   info.width = width;
   info.height = height;
   info.format = format;
   info.data = pixels.data();
   hostDirty = true;
}

// Decode a single texel to floating point RGBA in [0..1]
static void DecodeTexel(const PUPTextureBlock* tex, int x, int y, float& r, float& g, float& b, float& a)
{
   const uint8_t* const data = static_cast<const uint8_t*>(tex->info.data);
   const uint8_t* const p = data + (static_cast<size_t>(y) * static_cast<size_t>(tex->info.width) + static_cast<size_t>(x)) * static_cast<size_t>(BytesPerPixel(tex->info.format));
   switch (tex->info.format)
   {
   case VPXTextureFormat::VPXTEXFMT_sRGBA8:
      r = static_cast<float>(p[0]) / 255.f;
      g = static_cast<float>(p[1]) / 255.f;
      b = static_cast<float>(p[2]) / 255.f;
      a = static_cast<float>(p[3]) / 255.f;
      break;
   case VPXTextureFormat::VPXTEXFMT_sRGB8:
      r = static_cast<float>(p[0]) / 255.f;
      g = static_cast<float>(p[1]) / 255.f;
      b = static_cast<float>(p[2]) / 255.f;
      a = 1.f;
      break;
   case VPXTextureFormat::VPXTEXFMT_sRGB565:
   {
      const uint16_t v = *reinterpret_cast<const uint16_t*>(p);
      r = static_cast<float>((v >> 11) & 0x1F) / 31.f;
      g = static_cast<float>((v >> 5) & 0x3F) / 63.f;
      b = static_cast<float>(v & 0x1F) / 31.f;
      a = 1.f;
      break;
   }
   case VPXTextureFormat::VPXTEXFMT_BW32F:
   {
      const float lum = *reinterpret_cast<const float*>(p);
      r = lum;
      g = lum;
      b = lum;
      a = 1.f;
      break;
   }
   default:
      r = g = b = 0.f;
      a = 0.f;
      break;
   }
}

// Bilinear sample of a texel position (pixel center convention), clamped to texture bounds
static void SampleTexel(const PUPTextureBlock* tex, float x, float y, float& r, float& g, float& b, float& a)
{
   const float fx = clamp(x - 0.5f, 0.f, static_cast<float>(tex->info.width - 1));
   const float fy = clamp(y - 0.5f, 0.f, static_cast<float>(tex->info.height - 1));
   const int x0 = static_cast<int>(fx);
   const int y0 = static_cast<int>(fy);
   const int x1 = std::min(x0 + 1, static_cast<int>(tex->info.width) - 1);
   const int y1 = std::min(y0 + 1, static_cast<int>(tex->info.height) - 1);
   const float tx = fx - static_cast<float>(x0);
   const float ty = fy - static_cast<float>(y0);

   float r00, g00, b00, a00, r10, g10, b10, a10, r01, g01, b01, a01, r11, g11, b11, a11;
   DecodeTexel(tex, x0, y0, r00, g00, b00, a00);
   DecodeTexel(tex, x1, y0, r10, g10, b10, a10);
   DecodeTexel(tex, x0, y1, r01, g01, b01, a01);
   DecodeTexel(tex, x1, y1, r11, g11, b11, a11);

   r = lerp(lerp(r00, r10, tx), lerp(r01, r11, tx), ty);
   g = lerp(lerp(g00, g10, tx), lerp(g01, g11, tx), ty);
   b = lerp(lerp(b00, b10, tx), lerp(b01, b11, tx), ty);
   a = lerp(lerp(a00, a10, tx), lerp(a01, a11, tx), ty);
}

PUPSoftwareContext::PUPSoftwareContext(unsigned int width, unsigned int height)
   : m_width(width)
   , m_height(height)
{
   m_frame.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 3u, 0u);
   m_published.resize(m_frame.size(), 0u);

   m_ctx.window = VPXWindowId::VPXWINDOW_Playfield;
   m_ctx.srcWidth = static_cast<float>(width);
   m_ctx.srcHeight = static_cast<float>(height);
   m_ctx.is2D = 1;
   m_ctx.outWidth = static_cast<float>(width);
   m_ctx.outHeight = static_cast<float>(height);
   m_ctx.DrawImage = &PUPSoftwareContext::DrawImageImpl;
   m_ctx.DrawDisplay = [](VPXRenderContext2D*, VPXDisplayRenderStyle, VPXTexture, float, float, float, float, float, float, float, float, float, float, float, VPXTexture, float, float,
                          float, float, float, float, float, float, float, float, float, float, float) { };
   m_ctx.DrawSegDisplay = [](VPXRenderContext2D*, VPXSegDisplayRenderStyle, VPXSegDisplayHint, VPXTexture, float, float, float, float, float, float, float, float, float, float, float,
                             SegElementType, const float*, float, float, float, float, float, float, float, float, float, float, float, float, float) { };
   m_ctx.rendererData = this;
}

void PUPSoftwareContext::BeginFrame() { memset(m_frame.data(), 0, m_frame.size()); }

void PUPSoftwareContext::EndFrame()
{
   // Publish the completed composite: consumers only ever see whole frames
   std::lock_guard lock(m_frameMutex);
   m_published = m_frame;
   ++m_publishedId;
}

DisplayFrame PUPSoftwareContext::GetFrame()
{
   std::lock_guard lock(m_frameMutex);
   return { m_publishedId, m_published.data() };
}

void MSGPIAPI PUPSoftwareContext::DrawImageImpl(VPXRenderContext2D* ctx, VPXTexture texture, const float tintR, const float tintG, const float tintB, const float alpha, const float texX,
   const float texY, const float texW, const float texH, const float pivotX, const float pivotY, const float rotation, const float srcX, const float srcY, const float srcW, const float srcH)
{
   PUPSoftwareContext* const self = static_cast<PUPSoftwareContext*>(ctx->rendererData);
   const PUPTextureBlock* const tex = static_cast<const PUPTextureBlock*>(texture);
   if (tex == nullptr || tex->info.data == nullptr || alpha <= 0.f || texW == 0.f || texH == 0.f || srcW == 0.f || srcH == 0.f)
      return;

   // The texture sub-rect maps to the destination rect, then the result is
   // rotated around the pivot (expressed in texture coordinates).
   const float pivotU = (pivotX - texX) / texW;
   const float pivotV = (pivotY - texY) / texH;
   const float pivotDX = srcX + pivotU * srcW;
   const float pivotDY = srcY + pivotV * srcH;

   const float rad = -rotation * 0.017453292519943295769f;
   const float c = cosf(rad);
   const float s = sinf(rad);

   // Bounding box of the rotated destination rect
   float minX, maxX, minY, maxY;
   if (rotation == 0.f)
   {
      minX = srcX;
      maxX = srcX + srcW;
      minY = srcY;
      maxY = srcY + srcH;
   }
   else
   {
      minX = minY = FLT_MAX;
      maxX = maxY = -FLT_MAX;
      for (int i = 0; i < 4; ++i)
      {
         const float cx = srcX + ((i & 1) ? srcW : 0.f) - pivotDX;
         const float cy = srcY + ((i & 2) ? srcH : 0.f) - pivotDY;
         const float rx = pivotDX + c * cx - s * cy;
         const float ry = pivotDY + s * cx + c * cy;
         minX = std::min(minX, rx);
         maxX = std::max(maxX, rx);
         minY = std::min(minY, ry);
         maxY = std::max(maxY, ry);
      }
   }
   const int x0 = std::max(0, static_cast<int>(floorf(minX)));
   const int y0 = std::max(0, static_cast<int>(floorf(minY)));
   const int x1 = std::min(static_cast<int>(self->m_width), static_cast<int>(ceilf(maxX)));
   const int y1 = std::min(static_cast<int>(self->m_height), static_cast<int>(ceilf(maxY)));

   for (int dy = y0; dy < y1; ++dy)
   {
      uint8_t* const dstRow = self->m_frame.data() + (static_cast<size_t>(dy) * self->m_width) * 3u;
      const float py = static_cast<float>(dy) + 0.5f;
      const float dyp = py - pivotDY;
      for (int dx = x0; dx < x1; ++dx)
      {
         // Inverse map the output pixel through the rotation into texture space
         const float dxp = static_cast<float>(dx) + 0.5f - pivotDX;
         const float qx = pivotDX + c * dxp - s * dyp;
         const float qy = pivotDY + s * dxp + c * dyp;
         const float u = (qx - srcX) / srcW;
         const float v = (qy - srcY) / srcH;
         if (u < 0.f || u >= 1.f || v < 0.f || v >= 1.f)
            continue;

         float r, g, b, a;
         SampleTexel(tex, texX + u * texW, texY + v * texH, r, g, b, a);
         const float sa = a * alpha;
         if (sa <= 0.f)
            continue;

         uint8_t* const dst = dstRow + static_cast<size_t>(dx) * 3u;
         dst[0] = static_cast<uint8_t>(saturate(lerp(static_cast<float>(dst[0]) / 255.f, r * tintR, sa)) * 255.f + 0.5f);
         dst[1] = static_cast<uint8_t>(saturate(lerp(static_cast<float>(dst[1]) / 255.f, g * tintG, sa)) * 255.f + 0.5f);
         dst[2] = static_cast<uint8_t>(saturate(lerp(static_cast<float>(dst[2]) / 255.f, b * tintB, sa)) * 255.f + 0.5f);
      }
   }
}

}
