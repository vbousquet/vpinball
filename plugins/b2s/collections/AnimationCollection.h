#pragma once

#include <map>

namespace B2S
{

class AnimationInfo;

class AnimationCollection : public std::map<int, vector<AnimationInfo*>>
{
public:
   void Add(int key, AnimationInfo* pAnimationInfo);
};

}