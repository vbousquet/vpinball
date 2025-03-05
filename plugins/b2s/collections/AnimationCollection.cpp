#include "core/stdafx.h"

#include "../classes/AnimationInfo.h"
#include "AnimationCollection.h"

namespace B2S
{

void AnimationCollection::Add(int key, AnimationInfo* pAnimationInfo) { (*this)[key].push_back(pAnimationInfo); }

}