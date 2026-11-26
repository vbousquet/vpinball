// license:GPLv3+

#pragma once

#if defined(ENABLE_BGFX)

#include <bgfx/bgfx.h>
#include "media/VideoDecodeSession.h"

#include <mutex>

class RenderDevice;

// GPU-resident decoded video stream backed by a bgfx video decode destination
// texture. Access units are submitted (Annex-B bitstream, decode order) and
// the hardware decoder blits the picture selected by the presentation clock
// into the texture on each Present call. All bgfx calls issued here are
// resource commands or encoder submissions which bgfx serializes internally,
// so the methods may be called from any thread; m_mutex additionally keeps
// the session's own presentation state consistent.
class VideoDecodeTexture final : public VPX::VideoDecodeSession
{
public:
   // Create a decode session for the given codec/stream. `parameterSets` are
   // the codec's Annex-B parameter sets (SPS/PPS for H.264, VPS/SPS/PPS for
   // H.265, sequence OBUs for AV1). Returns nullptr when the backend can not
   // hardware decode the stream.
   static std::shared_ptr<VideoDecodeTexture> Create(
      RenderDevice* rd, bgfx::VideoCodec::Enum codec, unsigned int width, unsigned int height, const uint8_t* parameterSets, uint32_t parameterSetsSize);

   VideoDecodeTexture(RenderDevice* rd, unsigned int width, unsigned int height);
   ~VideoDecodeTexture() override;

   unsigned int GetWidth() const override { return m_width; }
   unsigned int GetHeight() const override { return m_height; }
   void SubmitAccessUnits(const uint8_t* bitstream, uint32_t bitstreamSize, const VPX::VideoDecodeAu* aus, uint32_t numAus, bool resetStream, bool final) override;
   void Present(int64_t ptsUs) override;
   bool ReadPixels(int64_t ptsUs, uint8_t* dst) override;
   std::shared_ptr<Sampler> GetSampler(bool srgbDecode) override;

private:
   RenderDevice* const m_rd;
   const unsigned int m_width;
   const unsigned int m_height;

   bgfx::TextureHandle m_texture = BGFX_INVALID_HANDLE;
   std::shared_ptr<Sampler> m_samplers[2]; // [0] raw sampling, [1] sRGB to linear decoding

   std::mutex m_mutex;
   int64_t m_lastPresentUs = 0; // Presentation clock of the last submission (reset by stream SET submissions)

   // GPU to CPU readback staging texture (lazily created on first ReadPixels)
   bgfx::TextureHandle m_readbackTexture = BGFX_INVALID_HANDLE;
};

#endif
