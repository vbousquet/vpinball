// license:GPLv3+

#include "core/stdafx.h"

#if defined(ENABLE_BGFX)

#include "VideoDecodeTexture.h"
#include "RenderDevice.h"
#include "Sampler.h"

#include <bx/bx.h>
#include <algorithm>
#include <chrono>
#include <thread>

std::shared_ptr<VideoDecodeTexture> VideoDecodeTexture::Create(
   RenderDevice* rd, bgfx::VideoCodec::Enum codec, unsigned int width, unsigned int height, const uint8_t* parameterSets, uint32_t parameterSetsSize)
{
   auto session = std::make_shared<VideoDecodeTexture>(rd, width, height);

   const bgfx::Memory* mem = bgfx::alloc(static_cast<uint32_t>(sizeof(bgfx::VideoDecoderInit) + parameterSetsSize));
   bgfx::VideoDecoderInit* init = reinterpret_cast<bgfx::VideoDecoderInit*>(mem->data);
   init->magic = BX_MAKEFOURCC('V', 'D', 'I', 0x0);
   init->codec = codec;
   init->parameterSets = mem->data + sizeof(bgfx::VideoDecoderInit);
   init->parameterSetsSize = parameterSetsSize;
   memcpy(mem->data + sizeof(bgfx::VideoDecoderInit), parameterSets, parameterSetsSize);
   init->cachedAuBytes = 0; // bgfx default reorder cache size
   init->flags = 0;

   // sRGB mutable: the YUV->RGB conversion writes a linear UAV while the bindable SRV can
   // still be created as sRGB when requested through the Sampler, matching a CPU uploaded
   // sRGB texture bound with BGFX_SAMPLER_SRGB.
   session->m_texture = bgfx::createTexture2D(static_cast<uint16_t>(width), static_cast<uint16_t>(height), false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_SRGB_MUTABLE, mem);
   if (!bgfx::isValid(session->m_texture))
      return nullptr;
   return session;
}

VideoDecodeTexture::VideoDecodeTexture(RenderDevice* rd, unsigned int width, unsigned int height)
   : m_rd(rd)
   , m_width(width)
   , m_height(height)
{
}

VideoDecodeTexture::~VideoDecodeTexture()
{
   if (bgfx::isValid(m_readbackTexture))
      bgfx::destroy(m_readbackTexture);
   if (bgfx::isValid(m_texture))
   {
      // The samplers do not own the decode texture: keep their handles in sync for released sessions
      for (const std::shared_ptr<Sampler>& sampler : m_samplers)
         if (sampler)
            sampler->Invalidate();
      bgfx::destroy(m_texture);
   }
}

void VideoDecodeTexture::SubmitAccessUnits(const uint8_t* bitstream, uint32_t bitstreamSize, const VPX::VideoDecodeAu* aus, uint32_t numAus, bool resetStream, bool final)
{
   if (numAus == 0)
      return;

   std::lock_guard lock(m_mutex);

   // Pack the frame header, its AU table and the bitstream in a single allocation as
   // bgfx only consumes the payload asynchronously on the render thread.
   const uint32_t payloadSize = static_cast<uint32_t>(sizeof(bgfx::VideoDecoderFrame) + numAus * sizeof(bgfx::VideoDecoderAu) + bitstreamSize);
   const bgfx::Memory* mem = bgfx::alloc(payloadSize);
   bgfx::VideoDecoderFrame* frame = reinterpret_cast<bgfx::VideoDecoderFrame*>(mem->data);
   bgfx::VideoDecoderAu* frameAus = reinterpret_cast<bgfx::VideoDecoderAu*>(mem->data + sizeof(bgfx::VideoDecoderFrame));
   uint8_t* frameBitstream = mem->data + sizeof(bgfx::VideoDecoderFrame) + numAus * sizeof(bgfx::VideoDecoderAu);
   frame->magic = BX_MAKEFOURCC('V', 'D', 'F', 0x0);
   frame->bitstream = frameBitstream;
   frame->aus = frameAus;
   frame->numAus = numAus;
   memcpy(frameBitstream, bitstream, bitstreamSize);
   for (uint32_t i = 0; i < numAus; i++)
   {
      frameAus[i].size = aus[i].size;
      frameAus[i].ptsUs = aus[i].ptsUs;
   }

   uint8_t flags = BGFX_VIDEO_DECODE_FRAME_NONE;
   if (resetStream)
   {
      flags |= BGFX_VIDEO_DECODE_FRAME_SET;
      // A new stream segment restarts the presentation clock on its first keyframe
      m_lastPresentUs = aus[0].ptsUs;
   }
   if (final)
      flags |= BGFX_VIDEO_DECODE_FRAME_FINAL;
   frame->flags = flags;

   // Access unit submissions feed the decoder but do not advance the display clock:
   // only Present/ReadPixels ticks select the picture to show
   frame->presentationTimeUs = m_lastPresentUs;

   bgfx::updateTexture2D(m_texture, 0, 0, 0, 0, static_cast<uint16_t>(m_width), static_cast<uint16_t>(m_height), mem);
}

void VideoDecodeTexture::Present(int64_t ptsUs)
{
   std::lock_guard lock(m_mutex);

   // Presentation times must be non-decreasing between stream resets: clamp to
   // the last submitted time (a resend of the same time simply re-picks).
   ptsUs = std::max(ptsUs, m_lastPresentUs);
   m_lastPresentUs = ptsUs;

   const bgfx::Memory* mem = bgfx::alloc(static_cast<uint32_t>(sizeof(bgfx::VideoDecoderFrame)));
   bgfx::VideoDecoderFrame* frame = reinterpret_cast<bgfx::VideoDecoderFrame*>(mem->data);
   frame->magic = BX_MAKEFOURCC('V', 'D', 'F', 0x0);
   frame->bitstream = nullptr;
   frame->aus = nullptr;
   frame->numAus = 0;
   frame->presentationTimeUs = ptsUs;
   frame->flags = 0;

   bgfx::updateTexture2D(m_texture, 0, 0, 0, 0, static_cast<uint16_t>(m_width), static_cast<uint16_t>(m_height), mem);
}

std::shared_ptr<Sampler> VideoDecodeTexture::GetSampler(bool srgbDecode)
{
   std::lock_guard lock(m_mutex);
   std::shared_ptr<Sampler>& sampler = m_samplers[srgbDecode ? 1 : 0];
   if (sampler == nullptr)
      sampler = std::make_shared<Sampler>(m_rd, "VideoDecode[" + std::to_string(m_width) + 'x' + std::to_string(m_height) + ']', SurfaceType::RT_DEFAULT, m_texture,
         bgfx::TextureFormat::RGBA8, m_width, m_height, false, srgbDecode);
   return sampler;
}

bool VideoDecodeTexture::ReadPixels(int64_t ptsUs, uint8_t* dst)
{
   // Tick the presentation clock for the requested time, blit the displayed picture into
   // a compute buffer, then wait for the render thread to complete the copy. Backward
   // ticks are allowed for readback (the monotonic clock only matters for LOOP streams).
   {
      std::lock_guard lock(m_mutex);

      const bgfx::Memory* mem = bgfx::alloc(static_cast<uint32_t>(sizeof(bgfx::VideoDecoderFrame)));
      bgfx::VideoDecoderFrame* frame = reinterpret_cast<bgfx::VideoDecoderFrame*>(mem->data);
      frame->magic = BX_MAKEFOURCC('V', 'D', 'F', 0x0);
      frame->bitstream = nullptr;
      frame->aus = nullptr;
      frame->numAus = 0;
      frame->presentationTimeUs = ptsUs;
      frame->flags = 0;
      bgfx::updateTexture2D(m_texture, 0, 0, 0, 0, static_cast<uint16_t>(m_width), static_cast<uint16_t>(m_height), mem);

      m_lastPresentUs = std::max(m_lastPresentUs, ptsUs);
   }

   if (!bgfx::isValid(m_readbackTexture))
   {
      m_readbackTexture
         = bgfx::createTexture2D(static_cast<uint16_t>(m_width), static_cast<uint16_t>(m_height), false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
      if (!bgfx::isValid(m_readbackTexture))
         return false;
   }

   bgfx::Encoder* encoder = bgfx::begin();
   if (encoder == nullptr)
      return false;
   const bgfx::ViewId viewId = static_cast<bgfx::ViewId>(bgfx::getCaps()->limits.maxViews - 1);
   bgfx::TextureRegion dstRegion;
   dstRegion.init(m_readbackTexture);
   bgfx::TextureRegion srcRegion;
   srcRegion.init(m_texture, 0, 0, static_cast<uint16_t>(m_width), static_cast<uint16_t>(m_height));
   encoder->blit(viewId, dstRegion, srcRegion);
   bgfx::end(encoder);

   bgfx::TextureRegion readRegion;
   readRegion.init(m_readbackTexture, 0, 0, static_cast<uint16_t>(m_width), static_cast<uint16_t>(m_height));
   const uint32_t readyFrame = bgfx::read(readRegion, dst);
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
   while (m_rd->m_lastPresentFrameIdx < readyFrame)
   {
      if (std::chrono::steady_clock::now() > deadline)
         return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
   }
   return true;
}

#endif
