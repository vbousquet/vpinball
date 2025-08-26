// license:GPLv3+

#include "core/stdafx.h"

#include "HomePage.h"

namespace VPX::InGameUI
{

HomePage::HomePage()
   : InGameUIPage("homepage", "Visual Pinball X", "")
{
}

void HomePage::Open()
{
   ClearItems();

   if (!m_player->m_ptable->m_rules.empty())
   {
      auto tableRules = std::make_unique<InGameUIItem>("Table Rules"s, ""s, "table/rules"s);
      AddItem(tableRules);
   }

   if (!m_player->m_ptable->m_settings.GetTableSettings().empty())
   {
      auto tableOptions = std::make_unique<InGameUIItem>("Table Options"s, ""s, "table/options"s);
      AddItem(tableOptions);
   }

   if (m_player->m_vrDevice)
   {
      #ifdef ENABLE_XR
         // Legacy OpenVR does not support dynamic repositioning through LiveUI (especially overall scale, this would need to be rewritten but not done as this is planned for deprecation)
         auto vrSettings = std::make_unique<InGameUIItem>("VR Settings"s, ""s, "settings/vr"s);
         AddItem(vrSettings);
      #endif
   }
   else
   {
      auto povSettings = std::make_unique<InGameUIItem>("Point Of View"s, ""s, "settings/pov"s);
      AddItem(povSettings);
   }

   auto audioSettings = std::make_unique<InGameUIItem>("Sound Settings"s, ""s, "settings/audio"s);
   AddItem(audioSettings);

   auto miscSettings = std::make_unique<InGameUIItem>("Miscellaneous Settings"s, ""s, "settings/misc"s);
   AddItem(miscSettings);

   /*auto graphicSettings = std::make_unique<InGameUIItem>("Graphic Settings"s, ""s, "settings/graphic"s);
   AddItem(graphicSettings);

   auto displaySettings = std::make_unique<InGameUIItem>("Display Settings"s, ""s, "settings/display"s);
   AddItem(displaySettings);

   auto inputSettings = std::make_unique<InGameUIItem>("Input Settings"s, ""s, "settings/input"s);
   AddItem(inputSettings);

   auto nudgeSettings = std::make_unique<InGameUIItem>("Nudge & Tilt Settings"s, ""s, "settings/nudge"s);
   AddItem(nudgeSettings);*/

   
   /* FIXME port to new UI
   for (int j = 0; j < Settings::GetNPluginSections(); j++)
   {
      int nOptions = 0;
      const int nCustomOptions = (int)Settings::GetPluginSettings().size();
      for (int i = 0; i < nCustomOptions; i++)
         if ((Settings::GetPluginSettings()[i].section == Settings::Plugin00 + j) && (Settings::GetPluginSettings()[i].showMask & VPX_OPT_SHOW_TWEAK))
            nOptions++;
      if (nOptions > 0)
         m_tweakPages.push_back((TweakPage)(TP_Plugin00 + j));
   }
   
   const int nCustomOptions = (int)Settings::GetPluginSettings().size();
   for (int i = 0; i < nCustomOptions; i++)
      if (Settings::GetPluginSettings()[i].section == Settings::Plugin00 + (m_tweakPages[m_activeTweakPageIndex] - TP_Plugin00)
         && (Settings::GetPluginSettings()[i].showMask & VPX_OPT_SHOW_TWEAK))
         m_tweakPageOptions.push_back((BackdropSetting)(BS_Custom + i));
   
   */

   InGameUIPage::Open();
}

};