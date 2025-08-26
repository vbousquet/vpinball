// license:GPLv3+

#include "core/stdafx.h"

#include "InGameUI.h"
#include "LiveUI.h"

#include "ingameui/HomePage.h"
#include "ingameui/TableOptionsPage.h"
#include "ingameui/TableRulesPage.h"
#include "ingameui/AudioSettingsPage.h"
#include "ingameui/MiscSettingsPage.h"
#include "ingameui/NudgeSettingsPage.h"
#include "ingameui/PointOfViewSettingsPage.h"
#include "ingameui/VRSettingsPage.h"


namespace VPX::InGameUI
{

InGameUI::InGameUI(LiveUI &liveUI)
   : m_liveUI(liveUI)
{
   m_app = g_pvp;
   m_player = g_pplayer;
   m_table = m_player->m_pEditorTable;
   m_live_table = m_player->m_ptable;
   m_pininput = &(m_player->m_pininput);
   m_renderer = m_player->m_renderer;

   AddPage(std::make_unique<HomePage>());
   AddPage(std::make_unique<TableOptionsPage>());
   AddPage(std::make_unique<TableRulesPage>());
   AddPage(std::make_unique<AudioSettingsPage>());
   AddPage(std::make_unique<MiscSettingsPage>());
   AddPage(std::make_unique<NudgeSettingsPage>());
   AddPage(std::make_unique<PointOfViewSettingsPage>());
   if (m_player->m_vrDevice)
      AddPage(std::make_unique<VRSettingsPage>());
}

InGameUI::~InGameUI() { }

void InGameUI::AddPage(std::unique_ptr<InGameUIPage> page) { m_pages[page->GetPath()] = std::move(page); }

void InGameUI::Navigate(const string &path)
{
   assert(IsOpened());
   if (m_activePage)
   {
      m_navigationHistory.push_back(m_activePage->GetPath());
      m_activePage->Close();
   }
   m_activePage = m_pages[path].get();
   if (m_activePage)
      m_activePage->Open();
   else
      Close();
}

void InGameUI::NavigateBack()
{
   assert(IsOpened());
   if (m_navigationHistory.empty())
      Close();
   else
   {
      string path = m_navigationHistory.back();
      Navigate(path);
      m_navigationHistory.pop_back();
      m_navigationHistory.pop_back();
   }
}

void InGameUI::Open()
{
   assert(!IsOpened());
   m_isOpened = true;
   Navigate("homepage");
}

void InGameUI::Close()
{
   assert(IsOpened());
   m_isOpened = false;
   if (m_playerPaused)
      m_player->SetPlayState(true);
   m_playerPaused = false;
   m_live_table->FireOptionEvent(3); // Tweak mode closed event
   if (m_activePage)
      m_activePage->Close();
}

void InGameUI::HandleTweakInput()
{
   /* const uint32_t now = msec();
   static uint32_t lastHandle = now;
   const uint32_t sinceLastInputHandleMs = now - lastHandle;
   lastHandle = now;

   BackdropSetting activeTweakSetting = m_tweakPageOptions[m_activeTweakIndex];
   PinTable *const table = m_live_table;

   // Legacy leyboard fly camera when in ingame option. Remove ?
   if (m_live_table->m_settings.LoadValueBool(Settings::Player, "EnableCameraModeFlyAround"s))
   {
      if (!ImGui::IsKeyDown(ImGuiKey_LeftAlt) && !ImGui::IsKeyDown(ImGuiKey_RightAlt))
      {
         if (ImGui::IsKeyDown(ImGuiKey_LeftArrow))
            m_renderer->m_cam.x += 10.0f;
         if (ImGui::IsKeyDown(ImGuiKey_RightArrow))
            m_renderer->m_cam.x -= 10.0f;
         if (ImGui::IsKeyDown(ImGuiKey_UpArrow))
            m_renderer->m_cam.y += 10.0f;
         if (ImGui::IsKeyDown(ImGuiKey_DownArrow))
            m_renderer->m_cam.y -= 10.0f;
      }
      else
      {
         if (ImGui::IsKeyDown(ImGuiKey_UpArrow))
            m_renderer->m_cam.z += 10.0f;
         if (ImGui::IsKeyDown(ImGuiKey_DownArrow))
            m_renderer->m_cam.z -= 10.0f;
         if (ImGui::IsKeyDown(ImGuiKey_LeftArrow))
            m_renderer->m_inc += 0.01f;
         if (ImGui::IsKeyDown(ImGuiKey_RightArrow))
            m_renderer->m_inc -= 0.01f;
      }
   }

   const PinInput::InputState state = m_player->m_pininput.GetInputState();
   for (int i = 0; i < eCKeys; i++)
   {
      const EnumAssignKeys keycode = static_cast<EnumAssignKeys>(i);
      int keyEvent;
      if (state.IsKeyPressed(keycode, m_prevInputState))
         keyEvent = 1;
      else if (state.IsKeyReleased(keycode, m_prevInputState))
         keyEvent = 2;
      else if (state.IsKeyDown(keycode))
         keyEvent = 0;
      else
         continue;

      if (keycode == eEscape && keyEvent == 2)
         Close();

      if (keycode == eLeftFlipperKey || keycode == eRightFlipperKey)
      {
         static uint32_t startOfPress = 0;
         static float floatFraction = 1.0f;
         if (keyEvent != 0)
         {
            startOfPress = now;
            floatFraction = 1.0f;
         }
         if (keyEvent == 2) // Do not react on key up (only key down or long press)
            continue;
         const bool up = keycode == eRightFlipperKey;
         const float step = up ? 1.f : -1.f;
         const float absIncSpeed = (float)sinceLastInputHandleMs * 0.001f * min(50.f, 0.75f + (float)(now - startOfPress) / 300.0f);
         const float incSpeed = up ? absIncSpeed : -absIncSpeed;

         // Since we need less than 1 int per frame for eg volume, we need to keep track of the float value
         // and step every n frames.
         floatFraction += absIncSpeed * 10.f;
         int absIntStep = 0;
         if (floatFraction >= 1.f)
         {
            absIntStep = static_cast<int>(floatFraction);
            floatFraction = floatFraction - (float)absIntStep;
         }
         const int intStep = up ? absIntStep : -absIntStep;

         ViewSetup &viewSetup = table->mViewSetups[table->m_BG_current_set];
         const bool isWindow = viewSetup.mMode == VLM_WINDOW;
         bool modified = true;
         if (activeTweakSetting >= BS_Custom)
         {
            const vector<Settings::OptionDef> &customOptions
               = m_tweakPages[m_activeTweakPageIndex] == TP_TableOption ? m_live_table->m_settings.GetTableSettings() : Settings::GetPluginSettings();
            if (activeTweakSetting < BS_Custom + (int)customOptions.size())
            {
               const auto &opt = customOptions[activeTweakSetting - BS_Custom];
               float nTotalSteps = (opt.maxValue - opt.minValue) / opt.step;
               int nMsecPerStep = nTotalSteps < 20.f ? 500 : max(5, 250 - (int)(now - startOfPress) / 10); // discrete vs continuous sliding
               int nSteps = (now - m_lastTweakKeyDown) / nMsecPerStep;
               if (keyEvent == 1)
               {
                  nSteps = 1;
                  m_lastTweakKeyDown = now - nSteps * nMsecPerStep;
               }
               if (nSteps > 0)
               {
                  m_lastTweakKeyDown += nSteps * nMsecPerStep;
                  float value = m_live_table->m_settings.LoadValueWithDefault(opt.section, opt.id, opt.defaultValue);
                  if (!opt.literals.empty())
                  {
                     value += (float)nSteps * opt.step * step;
                     while (value < opt.minValue)
                        value += opt.maxValue - opt.minValue + 1;
                     while (value > opt.maxValue)
                        value -= opt.maxValue - opt.minValue + 1;
                  }
                  else
                     value = clamp(value + (float)nSteps * opt.step * step, opt.minValue, opt.maxValue);
                  table->m_settings.SaveValue(opt.section, opt.id, value);
                  if (opt.section == Settings::TableOption)
                     m_live_table->FireOptionEvent(1); // Table option changed event
                  else
                     VPXPluginAPIImpl::GetInstance().BroadcastVPXMsg(VPXPluginAPIImpl::GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_SETTINGS_CHANGED), nullptr);
               }
               else
                  modified = false;
            }
         }
         else
         {
            assert(false);
            break;
         }
         m_tweakState[activeTweakSetting] |= modified ? 1 : 0;
      }
      else if (keyEvent == 1) // Key down
      {
         if (keycode == eLeftTiltKey && m_live_table->m_settings.LoadValueBool(Settings::Player, "EnableCameraModeFlyAround"s))
            m_live_table->mViewSetups[m_live_table->m_BG_current_set].mViewportRotation -= 1.0f;
         else if (keycode == eRightTiltKey && m_live_table->m_settings.LoadValueBool(Settings::Player, "EnableCameraModeFlyAround"s))
            m_live_table->mViewSetups[m_live_table->m_BG_current_set].mViewportRotation += 1.0f;
         else if (keycode == eStartGameKey) // Save tweak page
         {
            string iniFileName = m_live_table->GetSettingsFileName();
            string message;
            if (m_tweakPages[m_activeTweakPageIndex] == TP_TableOption)
            {
               // Custom table/plugin options
               const vector<Settings::OptionDef> &customOptions
                  = m_tweakPages[m_activeTweakPageIndex] == TP_TableOption ? m_live_table->m_settings.GetTableSettings() : Settings::GetPluginSettings();
               message = m_tweakPages[m_activeTweakPageIndex] > TP_TableOption ? "Plugin options"s : "Table options"s;
               const int nOptions = (int)customOptions.size();
               for (int i2 = 0; i2 < nOptions; i2++)
               {
                  const auto &opt = customOptions[i2];
                  if ((opt.section == Settings::TableOption && m_tweakPages[m_activeTweakPageIndex] == TP_TableOption)
                     || (opt.section > Settings::TableOption
                        && m_tweakPages[m_activeTweakPageIndex] == static_cast<int>(TP_Plugin00) + static_cast<int>(opt.section) - static_cast<int>(Settings::Plugin00)))
                  {
                     if (m_tweakState[BS_Custom + i2] == 2)
                        m_table->m_settings.DeleteValue(opt.section, opt.id);
                     else
                        m_table->m_settings.SaveValue(opt.section, opt.id, m_live_table->m_settings.LoadValueWithDefault(opt.section, opt.name, opt.defaultValue));
                     m_tweakState[BS_Custom + i2] = 0;
                  }
               }
            }
            if (m_table->m_filename.empty() || !FileExists(m_table->m_filename))
            {
               m_liveUI.PushNotification("You need to save your table before exporting user settings"s, 5000);
            }
            else
            {
               m_table->m_settings.SaveToFile(iniFileName);
               m_liveUI.PushNotification(message + " exported to " + iniFileName, 5000);
            }
         }
         else if (keycode == ePlungerKey) // Reset tweak page
         {
            // Reset custom table/plugin options
            if (m_tweakPages[m_activeTweakPageIndex] >= TP_TableOption)
            {
               if (m_tweakPages[m_activeTweakPageIndex] > TP_TableOption)
                  m_liveUI.PushNotification("Plugin options reset to default values"s, 5000);
               else
                  m_liveUI.PushNotification("Table options reset to default values"s, 5000);
               const vector<Settings::OptionDef> &customOptions
                  = m_tweakPages[m_activeTweakPageIndex] == TP_TableOption ? m_live_table->m_settings.GetTableSettings() : Settings::GetPluginSettings();
               const int nOptions = (int)customOptions.size();
               for (int i2 = 0; i2 < nOptions; i2++)
               {
                  const auto &opt = customOptions[i2];
                  if ((opt.section == Settings::TableOption && m_tweakPages[m_activeTweakPageIndex] == TP_TableOption)
                     || (opt.section > Settings::TableOption
                        && m_tweakPages[m_activeTweakPageIndex] == static_cast<int>(TP_Plugin00) + static_cast<int>(opt.section) - static_cast<int>(Settings::Plugin00)))
                  {
                     if (m_tweakState[BS_Custom + i2] == 2)
                        m_table->m_settings.DeleteValue(opt.section, opt.id);
                     else
                        m_table->m_settings.SaveValue(opt.section, opt.id, m_live_table->m_settings.LoadValueWithDefault(opt.section, opt.id, opt.defaultValue));
                     m_tweakState[BS_Custom + i2] = 0;
                  }
               }
               if (m_tweakPages[m_activeTweakPageIndex] > TP_TableOption)
                  VPXPluginAPIImpl::GetInstance().BroadcastVPXMsg(VPXPluginAPIImpl::GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_SETTINGS_CHANGED), nullptr);
               else
                  m_live_table->FireOptionEvent(2); // custom option resetted event
            }
         }
      }
      else if (keyEvent == 0) // Continuous keypress
      {
         if (keycode == eLeftTiltKey && m_live_table->m_settings.LoadValueBool(Settings::Player, "EnableCameraModeFlyAround"s))
            m_live_table->mViewSetups[m_live_table->m_BG_current_set].mViewportRotation -= 1.0f;
         else if (keycode == eRightTiltKey && m_live_table->m_settings.LoadValueBool(Settings::Player, "EnableCameraModeFlyAround"s))
            m_live_table->mViewSetups[m_live_table->m_BG_current_set].mViewportRotation += 1.0f;
      }
   }
   m_prevInputState = state; */
}

void InGameUI::Update()
{
   if (!m_isOpened)
      return;

   // Only pause player if balls are moving to keep attract mode if possible
   if (!m_playerPaused)
   {
      bool ballMoving = false;
      for (const auto &ball : m_player->m_vball)
      {
         if (ball->m_d.m_vel.LengthSquared() > 0.25f)
         {
            ballMoving = true;
            break;
         }
      }
      if (ballMoving)
      {
         m_player->SetPlayState(false);
         m_playerPaused = true;
      }
   }

   PinInput::InputState state = m_player->m_pininput.GetInputState();
   // Enable keyboard shortcut if no control editing is in progress
   // FIXME: we should only disable keyboard shortcut, not gamepad, VR controller,...
   if (!ImGui::IsAnyItemActive())
   {
      if (state.IsKeyPressed(eLeftMagnaSave, m_prevInputState))
      {
         m_useFlipperNav = true;
         m_activePage->SelectPrevItem();
      }
      if (state.IsKeyPressed(eRightMagnaSave, m_prevInputState))
      {
         m_useFlipperNav = true;
         m_activePage->SelectNextItem();
      }
      if (state.IsKeyPressed(eLeftFlipperKey, m_prevInputState))
      {
         m_useFlipperNav = true;
         m_activePage->AdjustItem(-1, true);
      }
      else if (state.IsKeyDown(eLeftFlipperKey))
      {
         m_useFlipperNav = true;
         m_activePage->AdjustItem(-1, false);
      }
      if (state.IsKeyPressed(eRightFlipperKey, m_prevInputState))
      {
         m_useFlipperNav = true;
         m_activePage->AdjustItem(1, true);
      }
      else if (state.IsKeyDown(eRightFlipperKey))
      {
         m_useFlipperNav = true;
         m_activePage->AdjustItem(1, false);
      }
      if (state.IsKeyPressed(ePlungerKey, m_prevInputState))
         m_activePage->ResetToDefaults();
      if (state.IsKeyPressed(eAddCreditKey, m_prevInputState))
      {
         if (g_pvp->m_povEdit)
            g_pvp->QuitPlayer(Player::CloseState::CS_CLOSE_APP);
         else
            m_activePage->ResetToInitialValues();
      }
      if (state.IsKeyPressed(eStartGameKey, m_prevInputState))
         m_activePage->Save();
      if (state.IsKeyReleased(eEscape, m_prevInputState))
         Close(); // FIXME should a navigate back, up to InGameUI close, applied on key release as this is the way it is handle for the main splash (to be changed ?)
   }
   m_prevInputState = state;

   m_activePage->Render();

   /*
   const vector<Settings::OptionDef> &customOptions
      = m_tweakPages[m_activeTweakPageIndex] == TP_TableOption ? m_live_table->m_settings.GetTableSettings() : Settings::GetPluginSettings();
   if (setting - BS_Custom >= (int)customOptions.size())
      continue;
   const Settings::OptionDef &opt = customOptions[setting - BS_Custom];
   const float value = table->m_settings.LoadValueWithDefault(opt.section, opt.id, opt.defaultValue);
   const string label = opt.name + ": ";
   if (!opt.literals.empty()) // List of values
   {
      int index = (int)(value - opt.minValue);
      if (index < 0 || index >= (int)opt.literals.size())
         index = (int)(opt.defaultValue - opt.minValue);
      CM_ROW(setting, label.c_str(), "%s", opt.literals[index].c_str(), "");
   }
   else if (opt.unit == Settings::OT_PERCENT) // Percent value
   {
      CM_ROW(setting, label.c_str(), "%.1f", 100.f * value, "%");
   }
   else // OT_NONE
   {
      CM_ROW(setting, label.c_str(), "%.1f", value, "");
   }
   */
}

};