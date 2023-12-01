#pragma once
#include "BAM/BAM_Tracker.h"

class Player;

class HeadTracking final
{
public:
   HeadTracking(Player* player);

   void Setup();
   void Update();
   void Release();

private:
   Player* const m_player = nullptr;

   // Support for BAM head tracking
   BAM_Tracker::BAM_Tracker_Client* m_bamTracker = nullptr;

   // Support for custom VPX headtracking
};
