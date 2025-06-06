#pragma once

#include "Actor.h"
#include "VPXPlugin.h"
#include <cassert>

namespace Flex {

class AnimatedActor : public Actor
{
public:
   AnimatedActor(FlexDMD* pFlexDMD, const string& name)
      : Actor(pFlexDMD, name) { }
   ~AnimatedActor() override = default;

   ActorType GetType() const override { return AT_AnimatedActor; }

   void Update(float delta) override;
   virtual void Seek(float posInSeconds);
   virtual void Advance(float delta);
   virtual void Rewind();
   virtual void ReadNextFrame() = 0;

   float GetFrameTime() const { return m_frameTime; }
   void SetFrameTime(float frameTime) { m_frameTime = frameTime; }
   float GetFrameDuration() const { return m_frameDuration; }
   void SetFrameDuration(float frameDuration) { assert(frameDuration > 0.f); m_frameDuration = frameDuration; }
   void SetTime(float time) { m_time = time; }
   void SetEndOfAnimation(bool endOfAnimation) { m_endOfAnimation = endOfAnimation; }
   Scaling GetScaling() const { return m_scaling; }
   void SetScaling(Scaling scaling) { m_scaling = scaling; }
   Alignment GetAlignment() const { return m_alignment; }
   void SetAlignment(Alignment alignment) { m_alignment = alignment; }
   virtual float GetLength() const = 0;
   bool GetLoop() const { return m_loop; }
   void SetLoop(bool loop) { m_loop = loop; }
   bool GetPaused() const { return m_paused; }
   void SetPaused(bool paused) { m_paused = paused; }
   float GetPlaySpeed() const { return m_playSpeed; }
   void SetPlaySpeed(float playSpeed) { m_playSpeed = playSpeed; }

private:
   float m_frameTime = 0.f;
   float m_frameDuration = (float)(1. / 60.);
   float m_time = 0.f;
   bool m_endOfAnimation = false;

   Scaling m_scaling = Scaling_Stretch;
   Alignment m_alignment = Alignment_Center;
   bool m_paused = false;
   bool m_loop = true;
   float m_playSpeed = 1.0f;
};

}