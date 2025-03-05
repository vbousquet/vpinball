#include "core/stdafx.h"

#include "LEDAreaInfo.h"

namespace B2S
{

LEDAreaInfo::LEDAreaInfo(const SDL_Rect& rect, bool isOnDMD)
{
   m_rect = rect;
   m_onDMD = isOnDMD;
}

}