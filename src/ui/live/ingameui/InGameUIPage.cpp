// license:GPLv3+

#include "core/stdafx.h"

#include "InGameUIPage.h"

#include "fonts/IconsForkAwesome.h"

namespace VPX::InGameUI
{

InGameUIPage::InGameUIPage(const string& path, const string& title, const string& info)
   : m_player(g_pplayer)
   , m_path(path)
   , m_title(title)
   , m_info(info)
{
   assert(m_player);
}

void InGameUIPage::Open()
{
   m_selectedItem = -1;
   m_useFlipperNav = false;
}

void InGameUIPage::Close() { }

void InGameUIPage::ClearItems() { m_items.clear(); }

void InGameUIPage::AddItem(std::unique_ptr<InGameUIItem>& item) { m_items.push_back(std::move(item)); }

Settings& InGameUIPage::GetSettings() { return m_player->m_ptable->m_settings; }

bool InGameUIPage::IsAdjustable() const
{
   for (const auto& item : m_items)
      if (item->IsAdjustable())
         return true;
   return false;
}

bool InGameUIPage::IsDefaults() const
{
   for (const auto& item : m_items)
      if (!item->IsDefaultValue())
         return false;
   return true;
}

bool InGameUIPage::IsModified() const
{
   for (const auto& item : m_items)
      if (item->IsModified())
         return true;
   return false;
}

void InGameUIPage::ResetToInitialValues()
{
   if (!IsModified())
      return;
   // Note that changing the value of items may result in changing the content of m_items (page rebuilding)
   for (int i = 0; i < m_items.size(); i++)
      m_items[i]->ResetToInitialValue();
   g_pplayer->m_liveUI->PushNotification("Changes were undoed"s, 5000);
   m_selectedItem = -1;
}

void InGameUIPage::ResetToDefaults()
{
   if (IsDefaults())
      return;
   // Note that changing the value of items may result in changing the content of m_items (page rebuilding)
   for (int i = 0; i < m_items.size(); i++)
      m_items[i]->ResetToDefault();
   g_pplayer->m_liveUI->PushNotification("Settings reseted to defaults"s, 5000);
   m_selectedItem = -1;
}

void InGameUIPage::Save()
{
   if (!IsModified())
      return;
   // FIXME disable save on table that do not have a filename (not yet saved) and only save to table ini
   // FIXME implement, letting pages select if they save to app settings, table setting overrides, or let the user choose one of both
   Settings& settings = GetSettings();
   const bool isTableOverride = true;
   for (const auto& item : m_items)
      item->Save(settings, isTableOverride);
   m_selectedItem = -1;
}

void InGameUIPage::SelectNextItem()
{
   const bool hasSave = IsModified();
   const bool hasUndo = hasSave;
   const bool hasDefaults = !IsDefaults();
   const int nStockItems = 1 + (hasSave ? 1 : 0) + (hasUndo ? 1 : 0) + (hasDefaults ? 1 : 0);
   const int nItems = nStockItems + static_cast<int>(m_items.size());
   do
      m_selectedItem = (nStockItems + m_selectedItem + 1) % nItems - nStockItems;
   while (m_selectedItem > 0 && !m_items[m_selectedItem - 1]->IsInteractive());
   m_useFlipperNav = true;
}

void InGameUIPage::SelectPrevItem()
{
   const bool hasSave = IsModified();
   const bool hasUndo = hasSave;
   const bool hasDefaults = !IsDefaults();
   const int nStockItems = 1 + (hasSave ? 1 : 0) + (hasUndo ? 1 : 0) + (hasDefaults ? 1 : 0);
   const int nItems = nStockItems + static_cast<int>(m_items.size());
   do
      m_selectedItem = (nStockItems + m_selectedItem + nItems - 1) % nItems - nStockItems;
   while (m_selectedItem > 0 && !m_items[m_selectedItem - 1]->IsInteractive());
   m_useFlipperNav = true;
}

void InGameUIPage::AdjustItem(float direction, bool isInitialPress)
{
   switch (m_selectedItem)
   {
   case -4: // Defaults
      if (isInitialPress)
         ResetToDefaults();
      break;

   case -3: // Undo
      if (isInitialPress)
         ResetToInitialValues();
      break;

   case -2: // Save
      if (isInitialPress)
         Save();
      break;

   case -1: // Back
      if (isInitialPress)
         m_player->m_liveUI->m_inGameUI.NavigateBack();
      break;

   default:
      if (m_selectedItem < 0 || m_selectedItem > m_items.size())
         return;

      {
         const uint32_t now = msec();
         if (isInitialPress)
            m_pressStartMs = m_lastUpdateMs = now;
         const float elapsed = static_cast<float>(now - m_lastUpdateMs) / 1000.f;
         m_lastUpdateMs = now;
         const uint32_t elapsedSincePress = now - m_pressStartMs;
         const float speedFactor = elapsedSincePress < 250 ? 1.0f : elapsedSincePress < 500 ? 2.f : elapsedSincePress < 1000 ? 4.f : elapsedSincePress < 1500 ? 8.f : 16.f;

         const auto& item = m_items[m_selectedItem];
         switch (item->m_type)
         {
         case InGameUIItem::Type::Navigation: m_player->m_liveUI->m_inGameUI.Navigate(item->m_path); break;

         case InGameUIItem::Type::EnumValue:
            if (isInitialPress)
            {
               if (direction < 0.f)
                  item->SetValue((item->GetIntValue() + static_cast<int>(item->m_enum.size()) - 1) % static_cast<int>(item->m_enum.size()));
               else
                  item->SetValue((item->GetIntValue() + 1) % static_cast<int>(item->m_enum.size()));
            }
            break;

         case InGameUIItem::Type::FloatValue:
         case InGameUIItem::Type::IntValue:
            if (isInitialPress)
               m_adjustedValue = item->GetFloatValue();
            m_adjustedValue += direction * speedFactor * elapsed * (item->m_maxValue - item->m_minValue) / 32.f;
            item->SetValue(m_adjustedValue);
            break;

         default: break;
         }
      }
   }
}

void InGameUIPage::Render()
{
   const ImGuiIO& io = ImGui::GetIO();
   const ImGuiStyle& style = ImGui::GetStyle();
   ImGui::SetNextWindowBgAlpha(0.5f);
   if (m_player->m_vrDevice)
   {
      const float size = min(0.25f * io.DisplaySize.x, 0.25f * io.DisplaySize.y);
      ImGui::SetNextWindowSize(ImVec2(size, size));
      ImGui::SetNextWindowPos(ImVec2(0.5f * io.DisplaySize.x, 0.5f * io.DisplaySize.y), 0, ImVec2(0.5f, 0.5f));
   }
   else
   {
      ImGui::SetNextWindowSize(ImVec2(min(0.5f * io.DisplaySize.x, 0.5f * io.DisplaySize.y), min(0.5f * io.DisplaySize.x, 0.3f * io.DisplaySize.y)));
      ImGui::SetNextWindowPos(ImVec2(0.5f * io.DisplaySize.x, 0.8f * io.DisplaySize.y), 0, ImVec2(0.5f, 1.f));
   }
   ImGui::Begin("InGameUI", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);

   ImGui::PushStyleColor(ImGuiCol_Separator, style.Colors[ImGuiCol_Text]);

   const bool isAdjustable = IsAdjustable();

   // Header
   ImGui::SeparatorText(m_title.c_str());
   float buttonWidth = ImGui ::CalcTextSize(ICON_FK_REPLY, nullptr, true).x + style.FramePadding.x * 2.0f + style.ItemSpacing.x;
   if (isAdjustable)
   {
      buttonWidth += ImGui::CalcTextSize(ICON_FK_UNDO, nullptr, true).x + style.FramePadding.x * 2.0f;
      buttonWidth += style.ItemSpacing.x;
      buttonWidth += ImGui::CalcTextSize(ICON_FK_HEART, nullptr, true).x + style.FramePadding.x * 2.0f;
      buttonWidth += style.ItemSpacing.x;
      buttonWidth += ImGui::CalcTextSize(ICON_FK_FLOPPY_O, nullptr, true).x + style.FramePadding.x * 2.0f;
      buttonWidth += style.ItemSpacing.x;
   }
   ImGui::SameLine(ImGui::GetWindowSize().x - buttonWidth);
   bool undoHoovered = false;
   bool defaultHoovered = false;
   bool saveHoovered = false;
   if (isAdjustable)
   {
      // Reset to defaults
      ImGui::BeginDisabled(IsDefaults());
      bool highlighted = m_useFlipperNav && m_selectedItem == -4;
      if (highlighted)
         ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 255, 0, 255));
      if (ImGui::Button(ICON_FK_HEART))
      {
         m_selectedItem = -4;
         AdjustItem(1.f, true);
      }
      if (highlighted)
         ImGui::PopStyleColor();
      defaultHoovered = ImGui::IsItemHovered();
      ImGui::EndDisabled();

      ImGui::SameLine();

      // Undo changes
      ImGui::BeginDisabled(!IsModified());
      highlighted = m_useFlipperNav && m_selectedItem == -3;
      if (highlighted)
         ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 255, 0, 255));
      if (ImGui::Button(ICON_FK_UNDO))
      {
         m_selectedItem = -3;
         AdjustItem(1.f, true);
      }
      if (highlighted)
         ImGui::PopStyleColor();
      undoHoovered = ImGui::IsItemHovered();

      ImGui::SameLine();

      // Save changes
      highlighted = m_useFlipperNav && m_selectedItem == -2;
      if (highlighted)
         ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 255, 0, 255));
      if (ImGui::Button(ICON_FK_FLOPPY_O))
      {
         m_selectedItem = -2;
         AdjustItem(1.f, true);
      }
      if (highlighted)
         ImGui::PopStyleColor();
      saveHoovered = ImGui::IsItemHovered();
      ImGui::EndDisabled();
      ImGui::SameLine();
   }
   // Get back to previous page or to game
   bool highlighted = m_useFlipperNav && m_selectedItem == -1;
   if (highlighted)
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 255, 0, 255));
   if (ImGui::Button(ICON_FK_REPLY))
   {
      m_selectedItem = -1;
      AdjustItem(1.f, true);
   }
   if (highlighted)
      ImGui::PopStyleColor();
   const bool backHoovered = ImGui::IsItemHovered();

   // Page items
   // Note that items may trigger state change which in turn may trigger a rebuild of the page (changing m_items)
   InGameUIItem* hooveredItem = nullptr;
   const ImVec2 itemPadding = style.ItemSpacing;
   ImGui::BeginChild("PageItems", ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeight() * 3.f - itemPadding.y * 2.f), ImGuiChildFlags_None,
      ImGuiWindowFlags_NoBackground);
   float labelEndScreenX = 0.f;
   for (const auto& item : m_items)
      if (item->IsAdjustable())
         labelEndScreenX = max(labelEndScreenX, ImGui::CalcTextSize(item->m_label.c_str()).x);
   labelEndScreenX = ImGui::GetCursorScreenPos().x + labelEndScreenX + style.ItemSpacing.x * 2.0f + 30.f;
   const float itemEndScreenX = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("*", nullptr, true).x - itemPadding.x;
   for (int i = 0; i < m_items.size(); i++)
   {
      const auto& item = m_items[i];
      const bool isMouseOver = (ImGui::IsWindowHovered()) && (ImGui::GetMousePos().y >= ImGui::GetCursorScreenPos().y - itemPadding.y - 1.f)
         && (ImGui::GetMousePos().y <= ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight() + itemPadding.y);
      const bool hoovered = (m_useFlipperNav && i == m_selectedItem) || (!m_useFlipperNav && isMouseOver && item->IsInteractive());
      if (hoovered)
      {
         hooveredItem = item.get();
         ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 255, 0, 255));
         ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetCursorScreenPos() - itemPadding,
            ImGui::GetCursorScreenPos() + ImVec2(itemPadding.x, itemPadding.y * 2.f) + ImVec2(itemEndScreenX - ImGui::GetCursorScreenPos().x + itemPadding.x, ImGui::GetTextLineHeight()),
            IM_COL32(0, 255, 0, 50));
         if (m_useFlipperNav)
            ImGui::SetScrollHereY(0.5f);
      }

      switch (item->m_type)
      {
      case InGameUIItem::Type::Info:
         ImGui::SetCursorScreenPos(ImGui::GetCursorScreenPos() + ImVec2(0.f, itemPadding.y));
         ImGui::Text("%s", item->m_label.c_str());
         ImGui::SetCursorScreenPos(ImGui::GetCursorScreenPos() + ImVec2(0.f, itemPadding.y));
         break;

      case InGameUIItem::Type::Navigation:
         ImGui::SetCursorScreenPos(ImGui::GetCursorScreenPos() + ImVec2(0.f, itemPadding.y));
         ImGui::Text(ICON_FK_ANGLE_DOUBLE_RIGHT);
         ImGui::SameLine(0.f, 10.f);
         ImGui::Text("%s", item->m_label.c_str());
         ImGui::SetCursorScreenPos(ImGui::GetCursorScreenPos() + ImVec2(0.f, itemPadding.y));
         if (isMouseOver && ImGui::IsMouseClicked(ImGuiMouseButton_::ImGuiMouseButton_Left))
         {
            m_selectedItem = i;
            AdjustItem(1.f, true);
         }
         break;

      case InGameUIItem::Type::Toggle:
         // FIXME implement
         break;

      case InGameUIItem::Type::EnumValue:
      {
         ImGui::Text("%s", item->m_label.c_str());
         ImGui::SameLine(labelEndScreenX - ImGui::GetCursorScreenPos().x);
         int v = item->GetIntValue();
         ImGui::SetNextItemWidth(itemEndScreenX - ImGui::GetCursorScreenPos().x);
         ImGui::Combo(("##" + item->m_label).c_str(), &v,
            [](void* data, int idx, const char** out_text)
            {
               const auto* vec = static_cast<const vector<string>*>(data);
               if (idx < 0 || idx >= (int)vec->size())
                  return false;
               *out_text = vec->at(idx).c_str();
               return true;
            },
            (void*)&item->m_enum, (int)item->m_enum.size());
         item->SetValue(v);
         if (item->IsModified())
         {
            ImGui::SameLine(itemEndScreenX - ImGui::GetCursorScreenPos().x);
            ImGui::Text(ICON_FK_PENCIL);
         }
         break;
      }

      case InGameUIItem::Type::FloatValue:
      {
         ImGui::Text("%s", item->m_label.c_str());
         ImGui::SameLine(labelEndScreenX - ImGui::GetCursorScreenPos().x);
         float v = item->GetFloatValue();
         ImGui::SetNextItemWidth(itemEndScreenX - ImGui::GetCursorScreenPos().x);
         ImGui::SliderFloat(("##" + item->m_label).c_str(), &v, item->m_minValue, item->m_maxValue, item->m_format.c_str(), ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoRoundToFormat);
         item->SetValue(v);
         if (item->IsModified())
         {
            ImGui::SameLine(itemEndScreenX - ImGui::GetCursorScreenPos().x);
            ImGui::Text(ICON_FK_PENCIL);
         }
         break;
      }

      case InGameUIItem::Type::IntValue:
      {
         ImGui::Text("%s", item->m_label.c_str());
         ImGui::SameLine(labelEndScreenX - ImGui::GetCursorScreenPos().x);
         int v = item->GetIntValue();
         ImGui::SetNextItemWidth(itemEndScreenX - ImGui::GetCursorScreenPos().x);
         ImGui::SliderInt(("##" + item->m_label).c_str(), &v, static_cast<int>(item->m_minValue), static_cast<int>(item->m_maxValue), item->m_format.c_str(),
            ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoRoundToFormat);
         item->SetValue(v);
         if (item->IsModified())
         {
            ImGui::SameLine(itemEndScreenX - ImGui::GetCursorScreenPos().x);
            ImGui::Text(ICON_FK_PENCIL);
         }
         break;
      }
      }

      if (hoovered)
         ImGui::PopStyleColor();
   }
   ImGui::Dummy(ImVec2(0, 0));
   ImGui::EndChild();

   ImGui::Separator();

   ImGui::BeginChild("Info", ImVec2(), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
   if (hooveredItem && std::ranges::find_if(m_items, [hooveredItem](auto& item) { return item.get() == hooveredItem; }) != m_items.end() && !hooveredItem->m_tooltip.empty())
      ImGui::TextWrapped("%s", hooveredItem->m_tooltip.c_str());
   else if (undoHoovered)
      ImGui::TextWrapped("%s", "Undo changes\n[Input shortcut: Credit Button]");
   else if (defaultHoovered)
      ImGui::TextWrapped("%s", "Reset page to defaults\n[Input shortcut: Launch Button]");
   else if (saveHoovered)
      ImGui::TextWrapped("%s", "Save changes\n[Input shortcut: Start Button]");
   else if (backHoovered)
      ImGui::TextWrapped("%s", "Get back\n[Input shortcut: Quit Button]");
   else if (!m_info.empty())
      ImGui::TextWrapped("%s", m_info.c_str());
   ImGui::EndChild();

   ImGui::PopStyleColor();
   ImGui::End();
}


};