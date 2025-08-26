// license:GPLv3+

#include "core/stdafx.h"

#include "HomePage.h"

namespace VPX::InGameUI
{

HomePage::HomePage()
   : InGameUIPage("homepage", "Visual Pinball X", "")
{
   auto tableRules = std::make_unique<InGameUIItem>("Table Rules"s, ""s, "table/rules"s);
   AddItem(tableRules);

   auto tableOptions = std::make_unique<InGameUIItem>("Table Options"s, ""s, "table/options"s);
   AddItem(tableOptions);

   auto povSettings = std::make_unique<InGameUIItem>("Point Of View"s, ""s, "settings/pov"s);
   AddItem(povSettings);

   auto audioSettings = std::make_unique<InGameUIItem>("Sound Settings"s, ""s, "settings/audio"s);
   AddItem(audioSettings);

   auto graphicSettings = std::make_unique<InGameUIItem>("Graphic Settings"s, ""s, "settings/graphic"s);
   AddItem(graphicSettings);

   auto displaySettings = std::make_unique<InGameUIItem>("Display Settings"s, ""s, "settings/display"s);
   AddItem(displaySettings);

   auto inputSettings = std::make_unique<InGameUIItem>("Input Settings"s, ""s, "settings/input"s);
   AddItem(inputSettings);

   auto nudgeSettings = std::make_unique<InGameUIItem>("Nudge & Tilt Settings"s, ""s, "settings/nudge"s);
   AddItem(nudgeSettings);
}

};