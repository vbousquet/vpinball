// license:GPLv3+

#pragma once

#include "common.h"
#include <mutex>
#include <atomic>

namespace PUP {

class PUPImage
{
public:
   PUPImage();
   ~PUPImage();

   const std::filesystem::path& GetFile() const { return m_file; }

   void Load(const std::filesystem::path& szFile);
   void Clear();
   void Render(VPXRenderContext2D* const ctx, const SDL_Rect& rect, float alpha = 1.f);

   // Invoked from the loader thread once an image has been decoded, with its
   // native size (used to size advertised display surfaces).
   void SetOnLoadedCallback(std::function<void(int width, int height)> onLoaded) { m_onLoaded = std::move(onLoaded); }

private:
   std::filesystem::path m_file;
   std::function<void(int, int)> m_onLoaded;
   std::unique_ptr<SDL_Surface, void (*)(SDL_Surface*)> m_pSurface;
   VPXTexture m_pTexture = nullptr;

   std::mutex m_loadMutex;
   std::unique_ptr<SDL_Surface, void (*)(SDL_Surface*)> m_pendingSurface;
   std::atomic<bool> m_loading { false };
};

}
