// license:GPLv3+

#pragma once

#include "InGameUIPage.h"

namespace VPX::InGameUI
{

class PointOfViewSettingPage : public InGameUIPage
{
public:
   PointOfViewSettingPage();
   
   void Open() override;
   void Close() override;
   void Save() override;

private:
   void BuildPage();
   void OnPointOfViewChanged();

   bool m_opened = false;
   ViewSetup m_initialViewSetup;
   vec3 m_playerPos;
   vec3 m_initialPlayerPos;
   bool m_staticPrepassDisabled = false;
};

};