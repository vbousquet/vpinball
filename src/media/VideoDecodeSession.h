// license:GPLv3+

#pragma once

#include <cstdint>
#include <memory>

class Sampler;

namespace VPX
{

// One access unit of a hardware-decoded video stream (Annex-B for H.264/H.265,
// OBU sequence for AV1). `size` bytes of bitstream, presented at `ptsUs`.
struct VideoDecodeAu
{
   uint32_t size;
   int64_t ptsUs;
};

// A GPU-resident decoded video stream. The hardware decoder consumes access
// units submitted in decode order; each Present call selects, in the decoded
// reorder pool, the picture whose pts best matches the presentation clock and
// blits it into a texture that can be bound for rendering or read back through
// ReadPixels. SubmitAccessUnits may be called from the session's decoding
// thread while Present/ReadPixels/GetSampler may be called from the render API
// thread; all calls are serialized internally.
class VideoDecodeSession
{
public:
   virtual ~VideoDecodeSession() = default;

   virtual unsigned int GetWidth() const = 0;
   virtual unsigned int GetHeight() const = 0;

   // Submit a batch of access units stored back-to-back in `bitstream`, in
   // decode order. `resetStream` marks the first batch of a stream segment
   // (after open/seek; its first AU must be a keyframe). `final` marks the
   // last submission at end of stream. The buffers are only read during the
   // call.
   virtual void SubmitAccessUnits(const uint8_t* bitstream, uint32_t bitstreamSize, const VideoDecodeAu* aus, uint32_t numAus, bool resetStream, bool final) = 0;

   // Advance the presentation clock to `ptsUs` [µs]: the decoded picture best
   // matching it is written to the decode texture. Times must be
   // non-decreasing between stream resets (they are clamped otherwise).
   virtual void Present(int64_t ptsUs) = 0;

   // Present `ptsUs`, then read the displayed picture back into `dst` as
   // tightly packed RGBA8 (GetWidth()*4 bytes per row, GetHeight() rows).
   // Blocks the calling thread until the GPU copy has completed. This stalls
   // the GPU pipeline and should only be used when CPU pixels are needed.
   virtual bool ReadPixels(int64_t ptsUs, uint8_t* dst) = 0;

   // Sampler binding the decode texture. `srgbDecode` selects sRGB->linear
   // conversion at sampling time, matching a CPU-uploaded sRGB texture bound
   // for a linear output. The session keeps the decode texture alive; callers
   // needing it to outlive the session should hold the session itself.
   virtual std::shared_ptr<Sampler> GetSampler(bool srgbDecode) = 0;
};

}
