// license:GPLv3+

#pragma once

#include "plugins/VPXPlugin.h"
#include "LibAv.h"
#include "VideoDecodeSession.h"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;
struct AVBSFContext;
struct SwsContext;
struct SwrContext;

namespace VPX
{

// Decoded video frame, exposed to plugins as the opaque MediaFrame handle.
// The frame is a lazy token: the decoded source frame is held until the pixels
// are materialized by GetFramePixels/UpdateTextureFromFrame, then released.
struct DecodedFrame
{
   AVFrame* frame = nullptr; // Decoded source frame (owned), freed once materialized or released (nullptr for GPU decoded frames)
   double pts = 0.; // Presentation timestamp [s]
   uint8_t* pixels = nullptr; // Materialized RGBA pixels, from the session pool
   unsigned int width = 0; // Materialized size
   unsigned int height = 0;
   bool isGpuFrame = false; // Frame decoded by the GPU session: gpuPtsUs selects the picture in its reorder pool
   int64_t gpuPtsUs = 0; // Presentation timestamp [µs] of GPU decoded frames
};

// Synchronous media (video & audio) decoding session, implementing the
// plugin-facing MediaAPI. All calls are expected to be made from a
// single thread (the one the plugin decodes on), except ReleaseFrame which
// may also be called from the frame consuming thread as long as calls are
// serialized with the session's use.
class MediaPlayer
{
public:
   // Factory creating a GPU video decode session for a video stream, called once per
   // opened media (codecId is an AVCodecID). Returning nullptr falls back to CPU decode.
   using VideoDecodeSessionFactory
      = std::function<std::shared_ptr<VideoDecodeSession>(int codecId, unsigned int width, unsigned int height, const uint8_t* parameterSets, uint32_t parameterSetsSize)>;

   // Open a session, returning nullptr on failure (also when the FFmpeg runtime
   // libraries are not available, lazily loaded on Windows). When a gpuSessionFactory
   // is provided and the video stream is hardware decodable, frames stay GPU resident.
   static MediaPlayer* TryOpen(const char* path, const VideoDecodeSessionFactory& gpuSessionFactory = {});

   explicit MediaPlayer(const char* path, const VideoDecodeSessionFactory& gpuSessionFactory = {});
   ~MediaPlayer();

   // GPU decoded session: the video stream is fed to the hardware decoder and frames
   // stay GPU resident (nullptr when decoded on CPU)
   std::shared_ptr<VideoDecodeSession> GetGpuSession() const { return m_gpuSession; }
   // Advance the GPU decoder's presentation clock for the given frame
   void PresentGpuFrame(MediaFrame frame);

   bool IsOpen() const { return m_formatContext != nullptr; }

   void GetInfo(MediaInfo* info) const;
   int Read(MediaOutput* output);
   int Seek(double position);
   void SetVideoFrameSize(unsigned int width, unsigned int height)
   {
      m_targetWidth = width;
      m_targetHeight = height;
   }
   void ReleaseFrame(MediaFrame frame);
   int GetFramePixels(MediaFrame frame, const uint8_t** data, unsigned int* width, unsigned int* height);

private:
   AVCodecContext* OpenStream(int streamIndex);
   void ReceiveFrames(AVCodecContext* codecContext);
   void EmitAudioFrame(AVFrame* frame);
   double FramePtsSeconds(const AVFrame* frame, int streamIndex);
   bool Materialize(DecodedFrame* mediaFrame);
   uint8_t* AcquirePixels(unsigned int width, unsigned int height);

   AVFormatContext* m_formatContext = nullptr;
   AVPacket* m_packet = nullptr;

   int m_videoStream = -1;
   AVCodecContext* m_videoContext = nullptr;
   double m_videoTimeBaseSec = 1.; // Stream time base as a pts to seconds factor

   // Alpha layer of alpha videos (Matroska alpha_mode: the alpha plane travels as an encoded
   // frame in the BlockAdditional packet side data). A secondary decoder instance decodes it,
   // and its luma plane is attached to the matching video frame as a fourth (yuva420p) plane.
   AVCodecContext* m_alphaContext = nullptr;
   std::deque<AVFrame*> m_alphaFrames; // Decoded alpha frames awaiting pairing with their video frame
   AVCodecContext* OpenAlphaStream();
   void ReceiveAlphaFrames();
   void AttachAlphaFrame(AVFrame* frame);

   // GPU hardware decode session: when set, the video stream has no avcodec context and
   // demuxed packets are bitstream converted then submitted to the GPU decoder. AUs are
   // submitted in decode order; video outputs are emitted in presentation order once the
   // reorder lookahead window is filled (or at end of stream).
   VideoDecodeSessionFactory m_gpuSessionFactory;
   std::shared_ptr<VideoDecodeSession> m_gpuSession;
   struct AVBSFContext* m_bsfContext = nullptr;
   bool OpenGpuVideoSession();
   void GpuFeedPacket(AVPacket* pkt);
   void GpuVideoStep(); // Submit pending AUs / emit presentation ordered frames
   static constexpr size_t kGpuReorderLookahead = 16; // Matches the decoder reorder pool depth
   bool m_gpuNeedStreamReset = false;
   int64_t m_gpuLastPtsUs = 0;
   struct PendingAu
   {
      std::vector<uint8_t> data;
      int64_t ptsUs;
   };
   std::deque<PendingAu> m_pendingAus; // Converted access units in decode order, awaiting submission
   std::multiset<int64_t> m_submittedPts; // Submitted AUs awaiting emission (presentation order)

   int m_audioStream = -1;
   AVCodecContext* m_audioContext = nullptr;
   double m_audioTimeBaseSec = 1.;

   struct SwsContext* m_swsContext = nullptr;
   std::atomic<unsigned int> m_targetWidth { 0 }; // Decoded output size override (0 = native)
   std::atomic<unsigned int> m_targetHeight { 0 };

   struct SwrContext* m_audioConversionContext = nullptr;
   int m_audioFormat = -1; // AVSampleFormat of the current conversion, -1 = none
   int m_audioFreq = 0;

   // A decoded unit pending emission. Audio chunks own their data so a queued
   // output stays valid until it is consumed by Read (the returned pointer is
   // then kept alive by m_lastOutput until the next Read, per the API contract).
   struct QueuedOutput
   {
      MediaOutput output {};
      std::unique_ptr<uint8_t[]> ownedAudio;
   };
   std::deque<QueuedOutput> m_outputs; // Decoded units pending emission by Read
   void DropQueuedOutputs(); // Clears m_outputs, releasing the video frames it owns
   QueuedOutput m_lastOutput; // Last emitted output, keeps audio data alive until next Read
   bool m_inputEof = false; // The container reported end of file (decoders may still drain)
   double m_frameDuration = 0.; // [s], for timestamp-less frames
   double m_lastVideoPts = 0.;
   double m_lastAudioPts = 0.;

   // Pool of materialized RGBA buffers, recycled through ReleaseFrame
   struct PooledPixels
   {
      uint8_t* data;
      unsigned int width;
      unsigned int height;
   };
   std::mutex m_pixelsMutex; // Serializes materialization: sws context and pixel pool
   std::vector<PooledPixels> m_pixelsPool;
   std::atomic<int> m_liveFrames { 0 }; // Frames currently handed out, for leak checking

   const LibAV::LibAV& m_libAV = LibAV::LibAV::GetInstance(); // Lazily loads the FFmpeg runtime on first session
};

}
