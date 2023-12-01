#include "stdafx.h"
#include "HeadTracking.h"
#include "BAM/BAMView.h"

HeadTracking::HeadTracking(Player* player)
   : m_player(player)
{
}

void HeadTracking::Setup()
{
   // Check if BAM headtracker is running
   m_bamTracker = new BAM_Tracker::BAM_Tracker_Client();
   if (!m_bamTracker->IsBAMTrackerPresent())
   {
      delete m_bamTracker;
      m_bamTracker = nullptr;
   }

   // Custom data exchange protocol
}

void HeadTracking::Update()
{
   // #ravarcade: UpdateBAMHeadTracking will set proj/view matrix to add BAM view and head tracking
   if (m_bamTracker && m_bamTracker->IsBAMTrackerPresent())
   {
      Matrix3D m_matView;
      Matrix3D m_matProj[2];
      BAMView::createProjectionAndViewMatrix(&m_matProj[0]._11, &m_matView._11);
      ModelViewProj& mvp = m_player->m_pin3d.GetMVP();
      mvp.SetView(m_matView);
      for (unsigned int eye = 0; eye < mvp.m_nEyes; eye++)
         mvp.SetProj(eye, m_matProj[eye]);
   }
}

void HeadTracking::Release()
{
   delete m_bamTracker;
   m_bamTracker = nullptr;
}
