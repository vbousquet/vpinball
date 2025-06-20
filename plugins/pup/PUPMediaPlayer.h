#pragma once

#include "PUPManager.h"
#include "LibAv.h"
/*
#include "parts/Sound.h"
#include "../common/Window.h"*/

namespace PUP {

class PUPMediaPlayer final
{
public:
   PUPMediaPlayer();
   ~PUPMediaPlayer();

   void Play(const string& filename);
   bool IsPlaying();
   void Pause(bool pause);
   const string& GetFilename() const { return m_filename; }
   int GetPriority() const { return m_priority; }
   void Stop();
   void SetVolume(float volume);
   void SetLoop(bool loop);
   void SetLength(int length);
   void Render(VPXRenderContext2D* const ctx, const SDL_Rect& destRect);

private:
   void Run();
   AVCodecContext* OpenStream(AVFormatContext* pInputFormatContext, int stream);
   void HandleAudioFrame(AVFrame* pFrame);
   void HandleVideoFrame(AVFrame* pFrame);

   string m_filename;
   uint64_t m_startTimestamp = 0; // timestamp in ms when the play command was called
   bool m_loop = false;
   int m_playIndex = 0;
   float m_volume = 100.f;
   int m_length = 0;
   int m_priority = -1;

   bool m_paused = false;
   double m_pauseTimestamp = 0.0;

   AVFormatContext* m_pFormatContext = nullptr;

   int m_videoStream = -1;
   AVCodecContext* m_pVideoContext = nullptr;

   SwsContext* m_swsContext = nullptr;
   int m_nRgbFrames = 0; // Circular buffer of m_nRgbFrames frames, ready to be rendered if framePTS >= playPTS
   int m_activeRgbFrame = 0;
   AVFrame** m_rgbFrames = nullptr;
   uint8_t** m_rgbFrameBuffers = nullptr;

   VPXTexture m_videoTexture = nullptr;
   unsigned int m_videoTextureId = 0xFFFFFF;

   int m_audioStream = -1;
   AVCodecContext* m_pAudioContext = nullptr;
   struct SwrContext* m_pAudioConversionContext = nullptr;
   AVSampleFormat m_audioFormat = AV_SAMPLE_FMT_NONE;
   //Sound* m_pSound = nullptr;

   std::mutex m_mutex;
   std::thread m_thread;
   bool m_running = false;

   const LibAV& m_libAv;
};

}
