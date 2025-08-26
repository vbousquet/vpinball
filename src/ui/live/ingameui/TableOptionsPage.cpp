// license:GPLv3+

#include "core/stdafx.h"

#include "TableOptionsPage.h"

namespace VPX::InGameUI
{

TableOptionsPage::TableOptionsPage()
   : InGameUIPage("table/options", "Table Options", "")
{
}

void TableOptionsPage::Open()
{
   ClearItems();

   for (auto& opt : m_player->m_ptable->m_settings.GetTableSettings())
   {
      if (!opt.literals.empty())
      {
         // TODO detect & implement On/Off or True/False as a toggle ?
         auto item = std::make_unique<InGameUIItem>(
            opt.name, ""s, // Missing option description
            opt.literals, static_cast<int>(opt.defaultValue),
            [this, opt]() { return static_cast<int>(m_player->m_ptable->m_settings.LoadValueWithDefault(Settings::TableOption, opt.name, opt.defaultValue)); },
            [this, opt](int prev, int v)
            {
               GetItem(opt.name)->SetInitialValue(v);
               m_player->m_ptable->m_settings.SaveValue(Settings::TableOption, opt.name, v);
               m_player->m_ptable->FireOptionEvent(1); // Table option changed event
            },
            [opt](int v, Settings& settings, bool isTableOverride) { /* Nothing to do as value are always saved */ });
         AddItem(item);
      }
      else if (round(opt.step) == 1.f && round(opt.minValue) == opt.minValue)
      {
         const float scale = opt.unit == Settings::OT_PERCENT ? 100.f : 1.f;
         string format;
         switch (opt.unit)
         {
         case Settings::OT_PERCENT: format = "%3d %%"; break;
         default: format = "%d"; break;
         }
         auto item = std::make_unique<InGameUIItem>(
            opt.name, ""s, // Missing option description
            static_cast<int>(opt.minValue * scale), static_cast<int>(opt.maxValue * scale), static_cast<int>(opt.defaultValue * scale), format,
            [this, opt, scale]() { return static_cast<int>(m_player->m_ptable->m_settings.LoadValueWithDefault(Settings::TableOption, opt.name, opt.defaultValue) * scale); },
            [this, opt, scale](int prev, int v)
            {
               GetItem(opt.name)->SetInitialValue(v);
               m_player->m_ptable->m_settings.SaveValue(Settings::TableOption, opt.name, v / scale);
               m_player->m_ptable->FireOptionEvent(1); // Table option changed event
            },
            [](int v, Settings& settings, bool isTableOverride) { /* Nothing to do as value are always saved */ });
         AddItem(item);
      }
      else
      {
         const float scale = opt.unit == Settings::OT_PERCENT ? 100.f : 1.f;
         string format;
         switch (opt.unit)
         {
         case Settings::OT_PERCENT: format = "%4.1f %%"; break;
         default: format = "%4.1f"; break;
         }
         auto item = std::make_unique<InGameUIItem>(
            opt.name, ""s, // Missing option description
            opt.minValue * scale, opt.maxValue * scale, opt.step * scale, opt.defaultValue * scale, format,
            [this, opt, scale]() { return m_player->m_ptable->m_settings.LoadValueWithDefault(Settings::TableOption, opt.name, opt.defaultValue) * scale; },
            [this, opt, scale](float prev, float v)
            {
               GetItem(opt.name)->SetInitialValue(v);
               m_player->m_ptable->m_settings.SaveValue(Settings::TableOption, opt.name, v / scale);
               m_player->m_ptable->FireOptionEvent(1); // Table option changed event
            },
            [](float v, Settings& settings, bool isTableOverride) { /* Nothing to do as value are always saved */ });
         AddItem(item);
      }
   }
}

};