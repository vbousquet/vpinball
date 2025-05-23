/*
 * This code was derived from notes at:
 *
 * https://nailbuster.com/pup/PUPDMD_Reference_v13.zip
 */

#include "core/stdafx.h"

#include "PUPLabel.h"
#include "PUPScreen.h"
#include "PUPPlaylist.h"
#include "PUPManager.h"

#include "RSJparser/RSJparser.tcc"

#include <filesystem>

PUPLabel::PUPLabel(PUPManager* pManager, const string& szName, const string& szFont, float size, LONG color, float angle, PUP_LABEL_XALIGN xAlign, PUP_LABEL_YALIGN yAlign, float xPos, float yPos, int pagenum, bool visible)
{
   m_pManager = pManager;
   m_szName = szName;

   if (!szFont.empty()) {
      TTF_Font* pFont = m_pManager->GetFont(szFont);
      if (!pFont) {
         PLOGE.printf("Font not found: label=%s, font=%s", szName.c_str(), szFont.c_str());
      }
      m_pFont = pFont;
   }
   else
      m_pFont = nullptr;

   m_size = size;
   m_color = color;
   m_angle = angle;
   m_xAlign = xAlign;
   m_yAlign = yAlign;
   m_xPos = xPos;
   m_yPos = yPos;
   m_visible = visible;
   m_pagenum = pagenum;
   m_shadowColor = 0;
   m_shadowState = 0;
   m_xoffset = 0;
   m_yoffset = 0;
   m_outline = false;
   m_dirty = true;
   m_pScreen = nullptr;
   m_pTexture = NULL;
   m_width = 0;
   m_height = 0;
   m_anigif = 0;
   m_pAnimation = NULL;
}

PUPLabel::~PUPLabel()
{
   if (m_pTexture)
      SDL_DestroyTexture(m_pTexture);

   if (m_pAnimation)
      IMG_FreeAnimation(m_pAnimation);
}

void PUPLabel::SetCaption(const string& szCaption)
{
   if (szCaption.empty() && m_type != PUP_LABEL_TYPE_TEXT)
      return;

   if (szCaption == "`u`")
      return;

   string szText = szCaption;
   std::ranges::replace(szText.begin(), szText.end(), '~', '\n');
   szText = string_replace_all(szText, "\\r"s, "\n"s);

   {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_szCaption != szText)
      {
         m_type = PUP_LABEL_TYPE_TEXT;
         m_szPath.clear();

         const string szExt = extension_from_path(szText);
         if (szExt == "gif" || szExt == "png" || szExt == "apng" || szExt == "bmp" || szExt == "jpg")
         {
            std::filesystem::path fs_path(normalize_path_separators(szText));
            string playlistFolder = fs_path.parent_path().string();
            PUPPlaylist* pPlaylist = m_pScreen->GetPlaylist(playlistFolder);
            if (pPlaylist)
            {
               string szPath = pPlaylist->GetPlayFilePath(fs_path.filename().string());
               if (!szPath.empty())
               {
                  m_szPath = szPath;
                  m_type = (szExt == "gif") ? PUP_LABEL_TYPE_GIF : PUP_LABEL_TYPE_IMAGE;
               }
               else
               {
                  PLOGW.printf("Image not found: screen=%d, label=%s, path=%s", m_pScreen->GetScreenNum(), m_szName.c_str(), szText.c_str());
                  // we need to set a path otherwise the caption will be used as text
                  m_szPath = szText;
               }
            }
            else
            {
               PLOGE.printf("Image playlist not found: screen=%d, label=%s, path=%s, playlist=%s", m_pScreen->GetScreenNum(), m_szName.c_str(), szText.c_str(), playlistFolder.c_str());
            }
         }

         m_szCaption = szText;
         m_dirty = true;
      }
   }
}

void PUPLabel::SetVisible(bool visible)
{
   std::lock_guard<std::mutex> lock(m_mutex);
   m_visible = visible;
}

void PUPLabel::SetSpecial(const string& szSpecial)
{
   PLOGD.printf("PUPLabel::SetSpecial: name=%s, caption=%s, json=%s", m_szName.c_str(), m_szCaption.c_str(), szSpecial.c_str());

   RSJresource json(szSpecial);
   switch (json["mt"s].as<int>(0))
   {
   case 2:
   {
      if (json["zback"s].exists() && json["zback"s].as<int>() == 1)
         m_pScreen->SendLabelToBack(this);

      if (json["ztop"s].exists() && json["ztop"s].as<int>() == 1)
         m_pScreen->SendLabelToFront(this);

      {
         std::lock_guard<std::mutex> lock(m_mutex);

         if (json["size"s].exists())
         {
            m_size = std::stof(json["size"s].as_str());
            m_dirty = true;
         }

         if (json["xpos"s].exists())
         {
            m_xPos = std::stof(json["xpos"s].as_str());
            m_dirty = true;
         }

         if (json["ypos"s].exists())
         {
            m_yPos = std::stof(json["ypos"s].as_str());
            m_dirty = true;
         }

         if (json["fname"s].exists())
         {
            string szFont = json["fname"s].as_str();
               TTF_Font* pFont = m_pManager->GetFont(szFont);
               if (!pFont)
               {
                  PLOGE.printf("Label font not found: name=%s, font=%s", m_szName.c_str(), szFont.c_str());
               }
               m_pFont = pFont;
            m_dirty = true;
         }

         if (json["fonth"s].exists())
         {
            m_size = std::stof(json["fonth"s].as_str());
            m_dirty = true;
         }

         if (json["color"s].exists())
         {
            m_color = json["color"s].as<int>();
            m_dirty = true;
         }

         if (json["xalign"s].exists())
         {
            m_xAlign = (PUP_LABEL_XALIGN)json["xalign"s].as<int>();
            m_dirty = true;
         }

         if (json["yalign"s].exists())
         {
            m_yAlign = (PUP_LABEL_YALIGN)json["yalign"s].as<int>();
            m_dirty = true;
         }

         if (json["pagenum"s].exists())
         {
            m_pagenum = json["pagenum"s].as<int>();
            m_dirty = true;
         }

         if (json["stopani"s].exists())
         {
            // stop any pup animations on label/image (zoom/flash/pulse).  this is not about animated gifs
            PLOGW.printf("stopani not implemented");
            m_dirty = true;
         }

         if (json["rotate"s].exists())
         {
            // in tenths.  so 900 is 90 degrees. rotate support for images too.  note images must be aligned center to rotate properly(default)
            m_angle = std::stof(json["rotate"s].as_str());
            m_dirty = true;
         }

         if (json["zoom"s].exists())
         {
            // 120 for 120% of current height, 80% etc...
            PLOGW.printf("zoom not implemented");
            m_dirty = true;
         }

         if (json["alpha"s].exists())
         {
            // '0-255  255=full, 0=blank
            PLOGW.printf("alpha not implemented");
            m_dirty = true;
         }

         if (json["gradstate"s].exists() && json["gradcolor"s].exists())
         {
            // color=gradcolor, gradstate = 0 (gradstate is percent)
            PLOGW.printf("gradstate/gradcolor not implemented");
            m_dirty = true;
         }

         if (json["grayscale"s].exists())
         {
            // only on image objects.  will show as grayscale.  1=gray filter on 0=off normal mode
            PLOGW.printf("filter not implemented");
            m_dirty = true;
         }

         if (json["filter"s].exists())
         {
            // fmode 1-5 (invertRGB, invert,grayscale,invertalpha,clear),blur)
            PLOGW.printf("filter not implemented");
            m_dirty = true;
         }

         if (json["shadowcolor"s].exists())
         {
            m_shadowColor = json["shadowcolor"s].as<int>();
            m_dirty = true;
         }

         if (json["shadowtype"s].exists())
         {
            // ST = 1 (Shadow), ST = 2 (Border)
            PLOGW.printf("shadowtype not implemented");
            m_dirty = true;
         }

         if (json["xoffset"s].exists())
         {
            m_xoffset = std::stof(json["xoffset"s].as_str());
            m_dirty = true;
         }

         if (json["yoffset"s].exists())
         {
            m_yoffset = std::stof(json["yoffset"s].as_str());
            m_dirty = true;
         }

         if (json["anigif"s].exists())
         {
            m_anigif = json["anigif"s].as<int>();
            m_dirty = true;
         }

         if (json["width"s].exists())
         {
            m_width = std::stof(json["width"s].as_str());
            m_dirty = true;
         }

         if (json["height"s].exists())
         {
            m_height = std::stof(json["height"s].as_str());
            m_dirty = true;
         }

         if (json["autow"s].exists())
         {
            PLOGW.printf("autow not implemented");
            m_dirty = true;
         }

         if (json["autoh"s].exists())
         {
            PLOGW.printf("autoh not implemented");
            m_dirty = true;
         }

         if (json["shadowstate"s].exists())
         {
            m_shadowState = json["shadowstate"s].as<int>();
            m_dirty = true;
         }

         if (json["outline"s].exists())
         {
            m_outline = (json["outline"s].as<int>() == 1);
            m_dirty = true;
         }
      }
   }
   break;

   case 1:
   {
      /*
            Animate
            at = animate type (1=flashing, 2=motion)
            fq = when flashing its the frequency of flashing
            len = length in ms of animation
            fc = foreground color of text during animation
            PLOGW << "Label animation not implemented";
         */
   }
   break;

   default: break;
   }
}

void PUPLabel::Render(SDL_Renderer* pRenderer, SDL_Rect& rect, int pagenum)
{
   std::lock_guard<std::mutex> lock(m_mutex);

   if (!m_visible || pagenum != m_pagenum || m_szCaption.empty())
      return;

   if (m_dirty) {
      if (!m_szPath.empty()) {
         if (m_pTexture) {
            SDL_DestroyTexture(m_pTexture);
            m_pTexture = NULL;
         }
         if (m_pAnimation) {
            IMG_FreeAnimation(m_pAnimation);
            m_pAnimation = nullptr;
         }
         if (m_type == PUP_LABEL_TYPE_IMAGE) {
            m_pTexture = IMG_LoadTexture(pRenderer, m_szPath.c_str());
            if (m_pTexture)
               m_dirty = false;
            else {
               PLOGE.printf("Unable to load image: %s", m_szPath.c_str());
            }
         }
         else if (m_type == PUP_LABEL_TYPE_GIF) {
            m_pAnimation = IMG_LoadAnimation(m_szPath.c_str());
            if (m_pAnimation) {
               m_animationFrame = -1;
               m_animationStart = SDL_GetTicks();
               m_pTexture = SDL_CreateTextureFromSurface(pRenderer, m_pAnimation->frames[0]);
               m_dirty = false;
            }
            else {
               PLOGE.printf("Unable to load animation: %s", m_szPath.c_str());
            }
         }
      }
      else {
         if (m_pFont)
            UpdateLabelTexture(pRenderer, rect);
      }
   }

   if (!m_pTexture)
      return;

   if (m_pAnimation) {
      int expectedFrame = static_cast<int>(static_cast<float>(SDL_GetTicks() - m_animationStart) * 60.f / 1000.f) % m_pAnimation->count;
      if (expectedFrame != m_animationFrame) {
         m_animationFrame = expectedFrame;
         SDL_UpdateTexture(m_pTexture, nullptr, m_pAnimation->frames[m_animationFrame]->pixels, m_pAnimation->frames[m_animationFrame]->pitch);
      }
   }

   float width;
   float height;

   if (m_type == PUP_LABEL_TYPE_TEXT) {
      width = m_width;
      height = m_height;
   }
   else {
      width = (m_width / 100.0f) * rect.w;
      height = (m_height / 100.0f) * rect.h;
   }

   SDL_FRect dest = { static_cast<float>(rect.x), static_cast<float>(rect.y), width, height };

   if (m_xPos > 0.f)
   {
      dest.x += ((m_xPos / 100.0f) * static_cast<float>(rect.w));
      if (m_xAlign == PUP_LABEL_XALIGN_CENTER)
         dest.x -= (width / 2.f);
      else if (m_xAlign == PUP_LABEL_XALIGN_RIGHT)
         dest.x -= width;
   }
   else
   {
      if (m_xAlign == PUP_LABEL_XALIGN_CENTER)
         dest.x += ((static_cast<float>(rect.w) - width) / 2.f);
      else if (m_xAlign == PUP_LABEL_XALIGN_RIGHT)
         dest.x += (static_cast<float>(rect.w) - width);
   }

   if (m_yPos > 0.f)
   {
      dest.y += ((m_yPos / 100.0f) * static_cast<float>(rect.h));
      if (m_yAlign == PUP_LABEL_YALIGN_CENTER)
         dest.y -= (height / 2.f);
      else if (m_yAlign == PUP_LABEL_YALIGN_BOTTOM)
         dest.y -= height;
   }
   else
   {
      if (m_yAlign == PUP_LABEL_YALIGN_CENTER)
         dest.y += ((static_cast<float>(rect.h) - height) / 2.f);
      else if (m_yAlign == PUP_LABEL_YALIGN_BOTTOM)
         dest.y += (static_cast<float>(rect.h) - height);
   }

   SDL_FPoint center = { height / 2.0f, 0 };
   SDL_RenderTextureRotated(pRenderer, m_pTexture, NULL, &dest, -m_angle / 10.0f, &center, SDL_FLIP_NONE);
}

void PUPLabel::UpdateLabelTexture(SDL_Renderer* pRenderer, SDL_Rect& rect)
{
   static ankerl::unordered_dense::map<string, ankerl::unordered_dense::set<string>> warnedLabels;

   if (m_pTexture) {
      SDL_DestroyTexture(m_pTexture);
      m_pTexture = NULL;
   }

   int height = (int)((m_size / 100.0) * rect.h);
   TTF_SetFontSize(m_pFont, height);
   TTF_SetFontOutline(m_pFont, 0);

   SDL_Color textColor = { GetRValue(m_color), GetGValue(m_color), GetBValue(m_color) };
   SDL_Surface* pTextSurface = TTF_RenderText_Blended_Wrapped(m_pFont, m_szCaption.c_str(), m_szCaption.length(), textColor, 0);
   if (!pTextSurface) {
      string szError = SDL_GetError();
      if (warnedLabels[szError].find(m_szName) == warnedLabels[szError].end()) {
         PLOGW.printf("Unable to render text: label=%s, error=%s", m_szName.c_str(), szError.c_str());
         warnedLabels[szError].insert(m_szName);
      }
      return;
   }

   SDL_Surface* pMergedSurface = nullptr;

   if (m_shadowState) {
      SDL_Color shadowColor = { GetRValue(m_shadowColor), GetGValue(m_shadowColor), GetBValue(m_shadowColor) };
      SDL_Surface* pShadowSurface = TTF_RenderText_Blended_Wrapped(m_pFont, m_szCaption.c_str(), m_szCaption.length(), shadowColor, 0);
      if (!pShadowSurface) {
         string szError = SDL_GetError();
         if (warnedLabels[szError].find(m_szName) == warnedLabels[szError].end()) {
            PLOGW.printf("Unable to render text: label=%s, error=%s", m_szName.c_str(), szError.c_str());
            warnedLabels[szError].insert(m_szName);
         }
         delete pTextSurface;
         return;
      }

      int xoffset = (int) (((abs(m_xoffset) / 100.0f) * height) / 2.f);
      int yoffset = (int) (((abs(m_yoffset) / 100.0f) * height) / 2.f);

      pMergedSurface = SDL_CreateSurface(pTextSurface->w + xoffset, pTextSurface->h + yoffset, SDL_PIXELFORMAT_RGBA32);
      if (pMergedSurface)
      {
         //SDL_FillSurfaceRect(pMergedSurface, NULL, SDL_MapRGBA(pMergedSurface->format, 255, 255, 0, 255));
         if (!m_outline)
         {
            SDL_Rect shadowRect = { (m_xoffset < 0) ? 0 : xoffset, (m_yoffset < 0) ? 0 : yoffset, pShadowSurface->w, pShadowSurface->h };
            SDL_BlitSurface(pShadowSurface, NULL, pMergedSurface, &shadowRect);

            SDL_Rect textRect = { (m_xoffset > 0) ? 0 : xoffset, (m_yoffset > 0) ? 0 : yoffset, pTextSurface->w, pTextSurface->h };
            SDL_BlitSurface(pTextSurface, NULL, pMergedSurface, &textRect);
         }
         else
         {
            SDL_Rect shadowRects[8] = {
               { 0,           0,           pShadowSurface->w, pShadowSurface->h },
               { xoffset / 2, 0,           pShadowSurface->w, pShadowSurface->h },
               { xoffset,     0,           pShadowSurface->w, pShadowSurface->h },

               { 0,           yoffset / 2, pShadowSurface->w, pShadowSurface->h },
               { xoffset,     yoffset / 2, pShadowSurface->w, pShadowSurface->h },

               { 0,           yoffset,     pShadowSurface->w, pShadowSurface->h },
               { xoffset / 2, yoffset,     pShadowSurface->w, pShadowSurface->h },
               { xoffset,     yoffset,     pShadowSurface->w, pShadowSurface->h }
            };

            for (int i = 0; i < 8; ++i)
               SDL_BlitSurface(pShadowSurface, NULL, pMergedSurface, &shadowRects[i]);

            SDL_Rect textRect = { xoffset / 2, yoffset / 2, pTextSurface->w, pTextSurface->h };
            SDL_BlitSurface(pTextSurface, NULL, pMergedSurface, &textRect);
         }
      }

      SDL_DestroySurface(pShadowSurface);
   }
   else
   {
      pMergedSurface = SDL_CreateSurface(pTextSurface->w, pTextSurface->h, SDL_PIXELFORMAT_RGBA32);
      if (pMergedSurface)
      {
         //SDL_FillSurfaceRect(pMergedSurface, NULL, SDL_MapRGBA(pMergedSurface->format, 255, 255, 0, 255));
         SDL_BlitSurface(pTextSurface, NULL, pMergedSurface, NULL);
      }
   }

   if (pMergedSurface)
   {
      m_pTexture = SDL_CreateTextureFromSurface(pRenderer, pMergedSurface);
      if (m_pTexture) {
         m_width = pMergedSurface->w;
         m_height = pMergedSurface->h;

         m_dirty = false;
      }
      SDL_DestroySurface(pMergedSurface);
   }

   SDL_DestroySurface(pTextSurface);
}

string PUPLabel::ToString() const
{
   return "name=" + m_szName +
      ", caption=" + m_szCaption +
      ", visible=" + (m_visible ? "true" : "false") +
      ", size=" + std::to_string(m_size) +
      ", color=" + std::to_string(m_color) +
      ", angle=" + std::to_string(m_angle) +
      ", xAlign=" + (m_xAlign == PUP_LABEL_XALIGN_LEFT  ? "LEFT" : m_xAlign == PUP_LABEL_XALIGN_CENTER ? "CENTER" : "RIGHT") +
      ", yAlign=" + (m_yAlign == PUP_LABEL_YALIGN_TOP ? "TOP" : m_yAlign == PUP_LABEL_YALIGN_CENTER ? "CENTER" : "BOTTOM") +
      ", xPos=" + std::to_string(m_xPos) +
      ", yPos=" + std::to_string(m_yPos) +
      ", pagenum=" + std::to_string(m_pagenum) +
      ", szPath=" + m_szPath;
}
