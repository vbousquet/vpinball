#include "core/stdafx.h"

#include "AnimationInfo.h"

namespace B2S
{

AnimationInfo::AnimationInfo(const string& szAnimationName, bool inverted)
{
   m_szAnimationName = szAnimationName;
   m_inverted = inverted;
}

}