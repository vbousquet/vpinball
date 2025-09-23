// license:GPLv3+

#include "core/stdafx.h"
#include "core/VPXPluginAPIImpl.h"
#include "renderer/VRDevice.h"

#include "ScanCodes.h"

#ifdef __LIBVPINBALL__
   #include "standalone/VPinballLib.h"
#endif

#ifdef _WIN32
   #include "input/DirectInputMouseHandler.h"
   #include "input/DirectInputJoystickHandler.h"
#endif

#ifdef ENABLE_XINPUT
   #include "input/XInputJoystickHandler.h"
#endif

#include "input/SDLInputHandler.h"

#ifndef __LIBVPINBALL__
   #include "input/OpenPinDevHandler.h"
#endif




PinInput::PinInput()
   : m_joypmcancel(SDL_GAMEPAD_BUTTON_NORTH + 1) 
   , m_onActionEventMsgId(VPXPluginAPIImpl::GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_ACTION_CHANGED))
{
   const Settings& settings = g_pvp->m_settings;

   auto addKeyAction = [this](const string& settingId, const string& label, const string& defaultMappings)
   {
      auto newAction = AddAction(std::make_unique<InputAction>(this, settingId, label, defaultMappings,
         [](const InputAction& action, bool, bool isPressed)
         {
            if (g_pplayer->m_liveUI->IsTweakMode() || g_pplayer->m_liveUI->HasKeyboardCapture())
               return;
            CComVariant rgvar[1] = { CComVariant(0x10000 | action.m_actionId) };
            DISPPARAMS dispparams = { rgvar, nullptr, 1, 0 };
            g_pplayer->m_ptable->FireDispID(isPressed ? DISPID_GameEvents_KeyDown : DISPID_GameEvents_KeyUp, &dispparams);
         }));
      return newAction->m_actionId;
   };

   auto addFlipperKeyAction = [this](const string& settingId, const string& label, const string& defaultMappings)
   {
      auto newAction = AddAction(std::make_unique<InputAction>(this, settingId, label, defaultMappings,
         [this](const InputAction& action, bool, bool isPressed)
         {
            if (g_pplayer->m_liveUI->IsTweakMode() || g_pplayer->m_liveUI->HasKeyboardCapture())
               return;
            if (isPressed)
            {
               g_pplayer->m_pininput.PlayRumble(0.f, 0.2f, 150);
               // Debug only, for testing parts of the flipper input lag (note that it excludes device lag, device to computer lag, OS lag, and VPX polling lag)
               m_leftkey_down_usec = usec();
               m_leftkey_down_frame = g_pplayer->m_overall_frames;
            }
            CComVariant rgvar[1] = { CComVariant(0x10000 | action.m_actionId) };
            DISPPARAMS dispparams = { rgvar, nullptr, 1, 0 };
            g_pplayer->m_ptable->FireDispID(isPressed ? DISPID_GameEvents_KeyDown : DISPID_GameEvents_KeyUp, &dispparams);
         }));
      return newAction->m_actionId;
   };

   m_leftFlipperActionId = addFlipperKeyAction("LeftFlipper", "Left Flipper", "K225"); // SDL_SCANCODE_LSHIFT
   assert(VPXAction::VPXACTION_LeftFlipper == m_leftFlipperActionId);
   m_rightFlipperActionId = addFlipperKeyAction("RightFlipper", "Right Flipper", "K229"); // SDL_SCANCODE_RSHIFT
   assert(VPXAction::VPXACTION_RightFlipper == m_rightFlipperActionId);
   m_stagedLeftFlipperActionId = addFlipperKeyAction("LeftStagedFlipper", "Left Staged Flipper", "K227"); // SDL_SCANCODE_LGUI
   assert(VPXAction::VPXACTION_StagedLeftFlipper == m_stagedLeftFlipperActionId);
   m_stagedRightFlipperActionId = addFlipperKeyAction("RightStagedFlipper", "Right Staged Flipper", "K230"); // SDL_SCANCODE_RALT
   assert(VPXAction::VPXACTION_StagedLeftFlipper == m_stagedLeftFlipperActionId);
   m_leftNudgeActionId = addKeyAction("LeftNudge", "Left Nudge", "K29"); // SDL_SCANCODE_Z
   assert(VPXAction::VPXACTION_LeftNudge == m_leftNudgeActionId);
   m_rightNudgeActionId = addKeyAction("RightNudge", "Right Nudge", "K56"); // SDL_SCANCODE_SLASH
   assert(VPXAction::VPXACTION_RightNudge == m_rightNudgeActionId);
   m_centerNudgeActionId = addKeyAction("CenterNudge", "Center Nudge", "K44"); // SDL_SCANCODE_SPACE
   assert(VPXAction::VPXACTION_CenterNudge == m_centerNudgeActionId);
   m_tiltActionId = addKeyAction("Tilt", "Tilt", "K23"); // SDL_SCANCODE_T
   assert(VPXAction::VPXACTION_Tilt == m_tiltActionId);
   m_plungerActionId = addKeyAction("Plunger", "Plunger", "K40"); // SDL_SCANCODE_RETURN
   assert(VPXAction::VPXACTION_Plunger == m_plungerActionId);
   m_addCreditActionId = addKeyAction("Credit", "Credit", "K34"); // SDL_SCANCODE_5
   assert(VPXAction::VPXACTION_AddCredit == m_addCreditActionId);
   m_addCredit2ActionId = addKeyAction("Credit2", "Credit (2)", "K33"); // SDL_SCANCODE_4
   assert(VPXAction::VPXACTION_AddCredit2 == m_addCredit2ActionId);
   m_startActionId = addKeyAction("Start", "Start", "K30"); // SDL_SCANCODE_1
   assert(VPXAction::VPXACTION_StartGame == m_startActionId);
   m_leftMagnaActionId = addKeyAction("LeftMagna", "Left Magna", "K224"); // SDL_SCANCODE_LCTRL
   assert(VPXAction::VPXACTION_LeftMagnaSave == m_leftMagnaActionId);
   m_rightMagnaActionId = addKeyAction("RightMagna", "Right Magna", "K228"); // SDL_SCANCODE_RCTRL
   assert(VPXAction::VPXACTION_RightMagnaSave == m_rightMagnaActionId);
   m_lockbarActionId = addKeyAction("Lockbar", "Lockbar", "K226"); // SDL_SCANCODE_LALT
   assert(VPXAction::VPXACTION_Lockbar == m_lockbarActionId);

   auto pause = AddAction(std::make_unique<InputAction>(this, "Pause", "Pause Game", "K19", // SDL_SCANCODE_P
      [](const InputAction&, bool, bool isPressed)
      {
         if (g_pplayer->m_liveUI->IsTweakMode() || g_pplayer->m_liveUI->HasKeyboardCapture() || !isPressed)
            return;
         g_pplayer->SetPlayState(!g_pplayer->IsPlaying());
      }));
   assert(VPXAction::VPXACTION_Pause == pause->m_actionId);

   auto perfOverlay = AddAction(std::make_unique<InputAction>(this, "PerfOverlay", "Toggle Perf. Overlay", "K68", // SDL_SCANCODE_F11
      [](const InputAction&, bool, bool isPressed)
      {
         if (g_pplayer->m_liveUI->IsTweakMode() || g_pplayer->m_liveUI->HasKeyboardCapture() || !isPressed)
            return;
         g_pplayer->m_liveUI->ToggleFPS();
      }));
   assert(VPXAction::VPXACTION_PerfOverlay == perfOverlay->m_actionId);

   m_disable_esc = settings.LoadValueBool(Settings::Player, "DisableESC"s); // FIXME Why do we add this setting instead of just letting the user remove all input bindings ?
   auto exitAction = AddAction(std::make_unique<InputAction>(this, "ExitInteractive", "Interactive Exit", "K41", // SDL_SCANCODE_ESCAPE
      [this](const InputAction&, bool wasPressed, bool isPressed)
      {
         if (m_disable_esc // Interactive exit disabled ?
            || !g_pplayer->m_playfieldWnd->IsFocused() // Focus lost
            || g_pplayer->m_liveUI->IsOpened() // Inside LiveUI ?
            || g_pplayer->m_liveUI->HasKeyboardCapture()) // LiveUI or any control being active ?
         {
            // Discard long/short press
            m_exitPressTimestamp = 0;
         }
         else if (wasPressed != isPressed)
         {
            // Open UI on key up (instead of key down) since a long press should not trigger the UI but directly exit from the app
            m_gameStartedOnce = true; // Disable autostart as player has requested close
            if (isPressed)
               m_exitPressTimestamp = msec();
            else
               g_pplayer->SetCloseState(Player::CS_USER_INPUT);
         }
         else if (isPressed // Exit button is pressed
            && m_exitPressTimestamp // Exit has not been discarded
            && (g_pplayer->m_time_msec > 1000) // Game has been played at least 1 second
            && ((msec() - m_exitPressTimestamp) > m_exitAppPressLengthMs)) // Exit button has been pressed continuously long enough
         {
            // Directly exit on long press (without showing UI)
            g_pvp->QuitPlayer(Player::CloseState::CS_CLOSE_APP);
         }
      }));
   exitAction->SetRepeatPeriod(0);
   assert(VPXAction::VPXACTION_ExitInteractive == exitAction->m_actionId);

   m_exitGameActionId = AddAction(
      std::make_unique<InputAction>(this, "ExitGame", "Exit Game", "K20", // SDL_SCANCODE_Q
      [](const InputAction& action, bool, bool isPressed)
      {
         if (g_pplayer->m_liveUI->IsTweakMode() || g_pplayer->m_liveUI->HasKeyboardCapture())
            return;
         CComVariant rgvar[1] = { CComVariant(0x10000 | action.m_actionId) };
         DISPPARAMS dispparams = { rgvar, nullptr, 1, 0 };
         g_pplayer->m_ptable->FireDispID(isPressed ? DISPID_GameEvents_KeyDown : DISPID_GameEvents_KeyUp, &dispparams);
         #ifdef __STANDALONE__
            g_pplayer->SetCloseState(Player::CS_CLOSE_APP);
         #else
            g_pplayer->SetCloseState(Player::CS_STOP_PLAY);
         #endif
      }))->m_actionId;
   assert(VPXAction::VPXACTION_ExitGame == m_exitGameActionId);

   auto inGameUI = AddAction(std::make_unique<InputAction>(this, "InGameUI", "Toggle InGame UI", "K69", // SDL_SCANCODE_F12
      [](const InputAction&, bool, bool isPressed)
      {
         if (g_pplayer->m_liveUI->IsTweakMode() || g_pplayer->m_liveUI->HasKeyboardCapture() || !isPressed)
            return;
         if (g_pplayer->m_liveUI->IsTweakMode())
            g_pplayer->m_liveUI->HideUI();
         else
            g_pplayer->m_liveUI->OpenTweakMode();
      }));
   assert(VPXAction::VPXACTION_InGameUI == inGameUI->m_actionId);

   auto volumeDown = AddAction(std::make_unique<InputAction>(this, "VolumeDown", "Volume Down", "K45", // SDL_SCANCODE_MINUS
      [this](const InputAction&, bool, bool isPressed)
      {
         if (!isPressed)
            return;
         g_pplayer->m_MusicVolume = clamp(g_pplayer->m_MusicVolume - 1, 0, 100);
         g_pplayer->m_SoundVolume = clamp(g_pplayer->m_SoundVolume - 1, 0, 100);
         g_pplayer->UpdateVolume();
         m_volumeNotificationId = g_pplayer->m_liveUI->PushNotification("Volume: " + std::to_string(g_pplayer->m_MusicVolume) + '%', 500, m_volumeNotificationId);
      }));
   volumeDown->SetRepeatPeriod(75);
   assert(VPXAction::VPXACTION_VolumeDown == volumeDown->m_actionId);

   auto volumeUp = AddAction(std::make_unique<InputAction>(this, "VolumeUp", "Volume Up", "K46", // SDL_SCANCODE_EQUALS
      [this](const InputAction&, bool, bool isPressed)
      {
         if (!isPressed)
            return;
         g_pplayer->m_MusicVolume = clamp(g_pplayer->m_MusicVolume + 1, 0, 100);
         g_pplayer->m_SoundVolume = clamp(g_pplayer->m_SoundVolume + 1, 0, 100);
         g_pplayer->UpdateVolume();
         m_volumeNotificationId = g_pplayer->m_liveUI->PushNotification("Volume: " + std::to_string(g_pplayer->m_MusicVolume) + '%', 500, m_volumeNotificationId);
      }));
   volumeUp->SetRepeatPeriod(75);
   assert(VPXAction::VPXACTION_VolumeUp == volumeUp->m_actionId);

   auto vrCenter = AddAction(std::make_unique<InputAction>(this, "VRCenter", "Align VR view", "K93", // SDL_SCANCODE_KP_5
      [](const InputAction&, bool, bool isPressed)
      {
         if (g_pplayer->m_liveUI->IsTweakMode() || g_pplayer->m_liveUI->HasKeyboardCapture() || !isPressed)
            return;
         if (g_pplayer->m_vrDevice)
            g_pplayer->m_vrDevice->RecenterTable();
      }));
   assert(VPXAction::VPXACTION_VRRecenter == vrCenter->m_actionId);

   auto vrUp = AddAction(std::make_unique<InputAction>(this, "VRUp", "Move VR view up", "K96", // SDL_SCANCODE_KP_8
   [](const InputAction&, bool, bool isPressed)
      {
         if (g_pplayer->m_liveUI->IsTweakMode() || g_pplayer->m_liveUI->HasKeyboardCapture() || !isPressed)
            return;
         #if defined(ENABLE_VR)
         if (g_pplayer->m_vrDevice)
               g_pplayer->m_vrDevice->TableUp();
         #endif
      }));
   assert(VPXAction::VPXACTION_VRUp == vrUp->m_actionId);

   auto vrDown = AddAction(std::make_unique<InputAction>(this, "VRDown", "Move VR view up", "K90", // SDL_SCANCODE_KP_2
      [](const InputAction&, bool, bool isPressed)
      {
         if (g_pplayer->m_liveUI->IsTweakMode() || g_pplayer->m_liveUI->HasKeyboardCapture() || !isPressed)
            return;
         #if defined(ENABLE_VR)
         if (g_pplayer->m_vrDevice)
            g_pplayer->m_vrDevice->TableDown();
         #endif
      }));
   assert(VPXAction::VPXACTION_VRDown == vrDown->m_actionId);

   AddAction(std::make_unique<InputAction>(this, "GenTournament", "Create Tournament File", "K226&K30", // SDL_SCANCODE_LALT & SDL_SCANCODE_1
      [](const InputAction&, bool, bool isPressed)
      {
         if (g_pplayer->m_liveUI->IsTweakMode() || g_pplayer->m_liveUI->HasKeyboardCapture() || !isPressed)
            return;
         if (g_pvp->m_ptableActive->TournamentModePossible())
            g_pvp->GenerateTournamentFile();
      }));

   AddAction(std::make_unique<InputAction>(this, "DebugBalls", "Debug Balls", "K18", // SDL_SCANCODE_O
      [](const InputAction&, bool, bool isPressed)
      {
         if (g_pplayer->m_liveUI->IsTweakMode() || g_pplayer->m_liveUI->HasKeyboardCapture() || !isPressed)
            return;
         g_pplayer->m_debugBalls = !g_pplayer->m_debugBalls;
      }));

   AddAction(std::make_unique<InputAction>(this, "Debugger", "Open Debugger", "K7", // SDL_SCANCODE_D
      [this](const InputAction&, bool, bool isPressed)
      {
         m_gameStartedOnce = true; // disable autostart as player as requested debugger instead
         if (g_pplayer->m_liveUI->IsTweakMode() || g_pplayer->m_liveUI->HasKeyboardCapture() || !isPressed)
            return;
         g_pplayer->m_showDebugger = true;
      }));

   AddAction(std::make_unique<InputAction>(this, "ToggleStereo", "Select Stereo Mode", "K67", // SDL_SCANCODE_F10
      [this](const InputAction&, bool, bool isPressed)
      {
         if (g_pplayer->m_liveUI->IsTweakMode() || g_pplayer->m_liveUI->HasKeyboardCapture() || !isPressed)
            return;
         if (IsAnaglyphStereoMode(g_pplayer->m_renderer->m_stereo3D))
         {
            // Select next glasses or toggle stereo on/off
            int glassesIndex = g_pplayer->m_renderer->m_stereo3D - STEREO_ANAGLYPH_1;
            if (!g_pplayer->m_renderer->m_stereo3Denabled && glassesIndex != 0)
            {
               g_pplayer->m_liveUI->PushNotification("Stereo enabled"s, 2000);
               g_pplayer->m_renderer->m_stereo3Denabled = true;
            }
            else
            {
               const int dir = (m_inputState.IsKeyDown(eLeftFlipperKey) || m_inputState.IsKeyDown(eRightFlipperKey)) ? -1 : 1;
               // Loop back with shift pressed
               if (!g_pplayer->m_renderer->m_stereo3Denabled && glassesIndex <= 0 && dir == -1)
               {
                  g_pplayer->m_renderer->m_stereo3Denabled = true;
                  glassesIndex = 9;
               }
               else if (g_pplayer->m_renderer->m_stereo3Denabled && glassesIndex <= 0 && dir == -1)
               {
                  g_pplayer->m_liveUI->PushNotification("Stereo disabled"s, 2000);
                  g_pplayer->m_renderer->m_stereo3Denabled = false;
               }
               // Loop forward
               else if (!g_pplayer->m_renderer->m_stereo3Denabled)
               {
                  g_pplayer->m_liveUI->PushNotification("Stereo enabled"s, 2000);
                  g_pplayer->m_renderer->m_stereo3Denabled = true;
               }
               else if (glassesIndex >= 9 && dir == 1)
               {
                  g_pplayer->m_liveUI->PushNotification("Stereo disabled"s, 2000);
                  glassesIndex = 0;
                  g_pplayer->m_renderer->m_stereo3Denabled = false;
               }
               else
               {
                  glassesIndex += dir;
               }
               g_pplayer->m_renderer->m_stereo3D = (StereoMode)(STEREO_ANAGLYPH_1 + glassesIndex);
               if (g_pplayer->m_renderer->m_stereo3Denabled)
               {
                  string name;
                  static const string defaultNames[]
                     = { "Red/Cyan"s, "Green/Magenta"s, "Blue/Amber"s, "Cyan/Red"s, "Magenta/Green"s, "Amber/Blue"s, "Custom 1"s, "Custom 2"s, "Custom 3"s, "Custom 4"s };
                  if (!g_pvp->m_settings.LoadValue(Settings::Player, "Anaglyph"s.append(std::to_string(glassesIndex + 1)).append("Name"s), name))
                     name = defaultNames[glassesIndex];
                  g_pplayer->m_liveUI->PushNotification("Profile #"s.append(std::to_string(glassesIndex + 1)).append(" '"s).append(name).append("' activated"s), 2000);
               }
            }
         }
         else if (Is3DTVStereoMode(g_pplayer->m_renderer->m_stereo3D))
         {
            // Toggle stereo on/off
            g_pplayer->m_renderer->m_stereo3Denabled = !g_pplayer->m_renderer->m_stereo3Denabled;
         }
         else if (g_pplayer->m_renderer->m_stereo3D == STEREO_VR)
         {
            g_pplayer->m_renderer->m_vrPreview = (VRPreviewMode)((g_pplayer->m_renderer->m_vrPreview + 1) % (VRPREVIEW_BOTH + 1));
            g_pplayer->m_liveUI->PushNotification(g_pplayer->m_renderer->m_vrPreview == VRPREVIEW_DISABLED ? "Preview disabled"s // Will only display in headset
                  : g_pplayer->m_renderer->m_vrPreview == VRPREVIEW_LEFT                                   ? "Preview switched to left eye"s
                  : g_pplayer->m_renderer->m_vrPreview == VRPREVIEW_RIGHT                                  ? "Preview switched to right eye"s
                                                                                                           : "Preview switched to both eyes"s,
               2000);
         }
         g_pvp->m_settings.SaveValue(Settings::Player, "Stereo3DEnabled"s, g_pplayer->m_renderer->m_stereo3Denabled);
         g_pplayer->m_renderer->InitLayout();
         g_pplayer->m_renderer->UpdateStereoShaderState();
      }));


   for (const auto& action : m_actions)
      action->LoadMappings(settings);

   m_exitPressTimestamp = 0;
   m_exitAppPressLengthMs = settings.LoadValueInt(Settings::Player, "Exitconfirm"s) * 1000 / 60;

   m_override_default_buttons = settings.LoadValueBool(Settings::Player, "PBWDefaultLayout"s);

   m_joypmbuyin = settings.LoadValueWithDefault(Settings::Player, "JoyPMBuyIn"s, m_joypmbuyin);
   m_joypmcoin3 = settings.LoadValueWithDefault(Settings::Player, "JoyPMCoin3"s, m_joypmcoin3);
   m_joypmcoin4 = settings.LoadValueWithDefault(Settings::Player, "JoyPMCoin4"s, m_joypmcoin4);
   m_joypmcoindoor = settings.LoadValueWithDefault(Settings::Player, "JoyPMCoinDoor"s, m_joypmcoindoor);
   m_joypmcancel = settings.LoadValueWithDefault(Settings::Player, "JoyPMCancel"s, m_joypmcancel);
   m_joypmdown = settings.LoadValueWithDefault(Settings::Player, "JoyPMDown"s, m_joypmdown);
   m_joypmup = settings.LoadValueWithDefault(Settings::Player, "JoyPMUp"s, m_joypmup);
   m_joypmenter = settings.LoadValueWithDefault(Settings::Player, "JoyPMEnter"s, m_joypmenter);

   m_joycustom1 = settings.LoadValueWithDefault(Settings::Player, "JoyCustom1"s, m_joycustom1);
   m_joycustom1key = GetSDLScancodeFromDirectInputKey(settings.LoadValueWithDefault(Settings::Player, "JoyCustom1Key"s, m_joycustom1key));
   m_joycustom2 = settings.LoadValueWithDefault(Settings::Player, "JoyCustom2"s, m_joycustom2);
   m_joycustom2key = GetSDLScancodeFromDirectInputKey(settings.LoadValueWithDefault(Settings::Player, "JoyCustom2Key"s, m_joycustom2key));
   m_joycustom3 = settings.LoadValueWithDefault(Settings::Player, "JoyCustom3"s, m_joycustom3);
   m_joycustom3key = GetSDLScancodeFromDirectInputKey(settings.LoadValueWithDefault(Settings::Player, "JoyCustom3Key"s, m_joycustom3key));
   m_joycustom4 = settings.LoadValueWithDefault(Settings::Player, "JoyCustom4"s, m_joycustom4);
   m_joycustom4key = GetSDLScancodeFromDirectInputKey(settings.LoadValueWithDefault(Settings::Player, "JoyCustom4Key"s, m_joycustom4key));

   MapActionToMouse(eLeftFlipperKey, settings.LoadValueInt(Settings::Player, "JoyLFlipKey"s), true);
   MapActionToMouse(eRightFlipperKey, settings.LoadValueInt(Settings::Player, "JoyRFlipKey"s), true);
   MapActionToMouse(ePlungerKey, settings.LoadValueInt(Settings::Player, "JoyPlungerKey"s), true);
   MapActionToMouse(eLeftTiltKey, settings.LoadValueInt(Settings::Player, "JoyLTiltKey"s), true);
   MapActionToMouse(eRightTiltKey, settings.LoadValueInt(Settings::Player, "JoyCTiltKey"s), true);
   MapActionToMouse(eCenterTiltKey, settings.LoadValueInt(Settings::Player, "JoyRTiltKey"s), true);

   memset(&m_inputState, 0, sizeof(m_inputState));
   m_rumbleMode = g_pvp->m_settings.LoadValueWithDefault(Settings::Player, "RumbleMode"s, 3);

   // Initialize device handlers

   auto inputAPI = static_cast<InputAPI>(g_pvp->m_settings.LoadValueWithDefault(Settings::Player, "InputApi"s, PI_SDL));

   // We always have an SDL handler as keyboard is always handled by SDL
   m_inputHandlers.push_back(std::make_unique<SDLInputHandler>(*this));
   m_sdlHandler = static_cast<SDLInputHandler*>(m_inputHandlers.back().get());
   m_useSDLJoyAPI = (inputAPI == PI_SDL);

   if (inputAPI == PI_XINPUT)
   #ifdef ENABLE_XINPUT
      m_inputHandlers.push_back(std::make_unique<XInputJoystickHandler>(*this, m_focusHWnd));
   #else
      inputAPI = PI_DIRECTINPUT;
   #endif

   #ifdef _WIN32
      // Cache the initial state of sticky keys
      m_startupStickyKeys.cbSize = sizeof(STICKYKEYS);
      SystemParametersInfo(SPI_GETSTICKYKEYS, sizeof(STICKYKEYS), &m_startupStickyKeys, 0);

      if (inputAPI == PI_DIRECTINPUT)
      {
         m_inputHandlers.push_back(std::make_unique<DirectInputJoystickHandler>(*this, m_focusHWnd));
         m_joystickDIHandler = static_cast<DirectInputJoystickHandler*>(m_inputHandlers.back().get());
         m_rumbleMode = 0;
      }

      if (settings.LoadValueWithDefault(Settings::Player, "EnableMouseInPlayer"s, true))
         m_inputHandlers.push_back(std::make_unique<DirectInputMouseHandler>(*this, m_focusHWnd));

      // Disable Sticky Keys
      STICKYKEYS newStickyKeys = {};
      newStickyKeys.cbSize = sizeof(STICKYKEYS);
      newStickyKeys.dwFlags = 0;
      SystemParametersInfo(SPI_SETSTICKYKEYS, sizeof(STICKYKEYS), &newStickyKeys, SPIF_SENDCHANGE);
   #endif

   #ifndef __LIBVPINBALL__
      m_inputHandlers.push_back(std::make_unique<OpenPinDevHandler>(*this));
   #endif

   ReloadNudgeAndPlungerSettings();

   // Apply initial keyboard state (if any key is already in a pressed state when the object is created)
   int nSDLKeys;
   const bool* sdlKeyStates = SDL_GetKeyboardState(&nSDLKeys);
   for (const auto& [sdlScancode, mappings] : m_keyMappings)
      if (sdlScancode < nSDLKeys)
         for (auto& mapping : mappings)
            mapping->SetPressed(sdlKeyStates[sdlScancode]);
}

PinInput::~PinInput()
{
   m_actionMappings.clear();
   m_analogActionMappings.clear();
   m_inputHandlers.clear();

   m_sdlHandler = nullptr;

   #ifdef _WIN32
      // restore the state of the sticky keys
      SystemParametersInfo(SPI_SETSTICKYKEYS, sizeof(STICKYKEYS), &m_startupStickyKeys, SPIF_SENDCHANGE);
      m_joystickDIHandler = nullptr;
   #endif

   VPXPluginAPIImpl::ReleaseMsgID(m_onActionEventMsgId);
}

InputAction* PinInput::AddAction(std::unique_ptr<InputAction>&& action)
{
   action->m_actionId = static_cast<int>(m_actions.size());
   m_actions.push_back(std::move(action));
   return m_actions.back().get();
}

bool PinInput::IsPressed(int actionId) const
{
   assert(0 <= actionId && actionId < static_cast<int>(m_actions.size()));
   return m_actions[actionId]->IsPressed();
}

void PinInput::Register(DigitalMapping* mapping)
{
   switch (mapping->m_type)
   {
   case DigitalMapping::Type::Keyboard:
      {
         auto it = m_keyMappings.find(mapping->m_sdlScanCode);
         if (it != m_keyMappings.end())
         {
            assert(std::ranges::find(it->second, mapping) == it->second.end());
            it->second.push_back(mapping);
         }
         else
            m_keyMappings.emplace(mapping->m_sdlScanCode, vector { mapping });
      }
      break;

   }
}

void PinInput::Unregister(DigitalMapping* mapping)
{
   switch (mapping->m_type)
   {
   case DigitalMapping::Type::Keyboard:
      {
         auto it = m_keyMappings.find(mapping->m_sdlScanCode);
         if (it != m_keyMappings.end())
            std::erase(it->second, mapping);
      }
      break;

   }

}

void PinInput::OnInputActionStateChanged(InputAction* action)
{
   // Allow plugins to react to action event, filter, ...
   // FIXME this supposes that actions are created in the same order than in the VPXAction enum
   VPXActionEvent event { static_cast<VPXAction>(action->m_actionId), action->IsPressed() };
   VPXPluginAPIImpl::GetInstance().BroadcastVPXMsg(m_onActionEventMsgId, &event);
   if (static_cast<bool>(event.isPressed) != action->IsPressed())
   {
      // FIXME override action state by plugin
      // isPressed = event.isPressed;
   }

   // Update input state
   /*
   if (isPressed == m_inputState.IsKeyDown(action))
      return; // Action has been discarded by a plugin
   else if (isPressed)
      m_inputState.SetPressed(action);
   else
      m_inputState.SetReleased(action);
      */
}

void PinInput::RegisterOnUpdate(InputAction* action)
{
   m_onUpdateActions.push_back(action);
}

void PinInput::UnregisterOnUpdate(InputAction* action)
{
   std::erase(m_onUpdateActions, action);
}


#ifdef _WIN32
void PinInput::SetFocusWindow(HWND focusWnd)
{
   m_focusHWnd = focusWnd;
}
#endif

void PinInput::ReloadNudgeAndPlungerSettings()
{
   const Settings& settings = g_pvp->m_settings;

   m_deadz = settings.LoadValueWithDefault(Settings::Player, "DeadZone"s, 0) * JOYRANGEMX / 100;

   m_linearPlunger = false;
   m_plungerPosDirty = true;
   m_plungerSpeedDirty = true;
   m_plunger_retract = settings.LoadValueWithDefault(Settings::Player, "PlungerRetract"s, m_plunger_retract);

   m_accelerometerDirty = true;
   m_accelerometerEnabled = settings.LoadValueWithDefault(Settings::Player, "PBWEnabled"s, true); // true if electronic accelerometer enabled
   m_accelerometerFaceUp = settings.LoadValueWithDefault(Settings::Player, "PBWNormalMount"s, true); // true is normal mounting (left hand coordinates)
   m_accelerometerAngle = 0.0f; // 0 degrees rotated counterclockwise (GUI is lefthand coordinates)
   const bool accel = settings.LoadValueWithDefault(Settings::Player, "PBWRotationCB"s, false);
   if (accel)
      m_accelerometerAngle = (float)settings.LoadValueWithDefault(Settings::Player, "PBWRotationValue"s, 0);
   m_accelerometerSensitivity = clamp((float)settings.LoadValueWithDefault(Settings::Player, "NudgeSensitivity"s, 500) * (float)(1.0 / 1000.0), 0.f, 1.f);
   m_accelerometerMax.x = static_cast<float>(settings.LoadValueWithDefault(Settings::Player, "PBWAccelMaxX"s, 100) * JOYRANGEMX) / 100.f;
   m_accelerometerMax.y = static_cast<float>(settings.LoadValueWithDefault(Settings::Player, "PBWAccelMaxY"s, 100) * JOYRANGEMX) / 100.f;
   m_accelerometerGain.x = dequantizeUnsignedPercentNoClamp(settings.LoadValueWithDefault(Settings::Player, "PBWAccelGainX"s, 150));
   m_accelerometerGain.y = dequantizeUnsignedPercentNoClamp(settings.LoadValueWithDefault(Settings::Player, "PBWAccelGainY"s, 150));
}


void PinInput::UnmapJoy(uint64_t joyId)
{
   std::erase_if(m_actionMappings, [joyId](const ActionMapping& am) { return (am.type == ActionMapping::AM_Joystick) && (am.joystickId == joyId); });
   std::erase_if(m_analogActionMappings, [joyId](const AnalogActionMapping& am) { return am.joystickId == joyId; });
}

void PinInput::MapActionToMouse(EnumPlayerActions action, int button, bool replace)
{
   if (replace)
      std::erase_if(m_actionMappings, [action](const ActionMapping& am) { return (am.action == action) && (am.type == ActionMapping::AM_Mouse); });
   ActionMapping mapping;
   mapping.action = action;
   mapping.type = ActionMapping::AM_Mouse;
   mapping.buttonId = button;
   m_actionMappings.push_back(mapping);
}

void PinInput::MapActionToKeyboard(EnumPlayerActions action, SDL_Scancode scancode, bool replace)
{
   if (replace)
      std::erase_if(m_actionMappings, [action](const ActionMapping& am) { return (am.action == action) && (am.type == ActionMapping::AM_Keyboard); });
   const auto& it = std::ranges::find_if(m_actionMappings.begin(), m_actionMappings.end(),
      [scancode](const ActionMapping& mapping) { return (mapping.type == ActionMapping::AM_Keyboard) && (mapping.scancode == scancode); });
   if (it != m_actionMappings.end())
   {
      // We do not support mapping multiple actions to the same input
      const string msg = "Input mapping conflict: at least 2 different actions are mapped to the same key '"s + SDL_GetScancodeName(scancode) + '\'';
      PLOGE << msg;
      if (g_pplayer && g_pplayer->m_liveUI)
         g_pplayer->m_liveUI->PushNotification(msg, 3000);
      return;
   }
   ActionMapping mapping;
   mapping.action = action;
   mapping.type = ActionMapping::AM_Keyboard;
   mapping.scancode = scancode;
   m_actionMappings.push_back(mapping);
}

void PinInput::MapActionToJoystick(EnumPlayerActions action, uint64_t joystickId, int buttonId, bool replace)
{
   if (replace)
      std::erase_if(m_actionMappings, [action](const ActionMapping& am) { return (am.action == action) && (am.type == ActionMapping::AM_Joystick); });
   ActionMapping mapping;
   mapping.action = action;
   mapping.type = ActionMapping::AM_Joystick;
   mapping.joystickId = joystickId;
   mapping.buttonId = buttonId;
   m_actionMappings.push_back(mapping);
}

void PinInput::MapAnalogActionToJoystick(AnalogAction output, uint64_t joystickId, int axisId, bool revert, bool replace)
{
   if (replace)
      std::erase_if(m_analogActionMappings, [output](const AnalogActionMapping& am) { return (am.output == output); });
   AnalogActionMapping mapping;
   mapping.joystickId = joystickId;
   mapping.axisId = axisId;
   mapping.revert = revert;
   mapping.output = output;
   m_analogActionMappings.push_back(mapping);
}


// Since input event processing is single threaded on the main game logic thread, we do not queue events but directly process them

void PinInput::PushActionEvent(EnumPlayerActions action, bool isPressed)
{
   InputEvent e;
   e.type = InputEvent::Type::Action;
   e.action = action;
   e.isPressed = isPressed;
   ProcessEvent(e);
}

void PinInput::PushMouseEvent(int button, bool isPressed)
{
   InputEvent e;
   e.type = InputEvent::Type::Mouse;
   e.buttonId = button;
   e.isPressed = isPressed;
   ProcessEvent(e);
}

void PinInput::PushKeyboardEvent(SDL_Keycode keycode, SDL_Scancode scancode, bool isPressed)
{
   InputEvent e;
   e.type = InputEvent::Type::Keyboard;
   e.keycode = keycode;
   e.scancode = scancode;
   e.isPressed = isPressed;
   ProcessEvent(e);
}

void PinInput::PushJoystickButtonEvent(uint64_t joystickId, unsigned int buttonId, bool isPressed)
{
   InputEvent e;
   e.type = InputEvent::Type::JoyButton;
   e.joystickId = joystickId;
   e.buttonId = buttonId + 1;
   e.isPressed = isPressed;
   ProcessEvent(e);
}

void PinInput::PushJoystickAxisEvent(uint64_t joystickId, int axisId, float value)
{
   assert(-1.f <= value && value <= 1.f);
   InputEvent e;
   e.type = InputEvent::Type::JoyAxis;
   e.joystickId = joystickId;
   e.axisId = axisId;
   e.value = static_cast<int>(value * JOYRANGEMX);
   ProcessEvent(e);
}

void PinInput::HandleSDLEvent(SDL_Event &e)
{
   m_sdlHandler->HandleSDLEvent(e, m_useSDLJoyAPI);
}

#if defined(_WIN32)
DirectInputJoystickHandler* PinInput::GetDirectInputJoystickHandler() const
{
   return m_joystickDIHandler;
}
#endif

const PinInput::InputState& PinInput::GetInputState() const
{
   return m_inputState;
}
 
void PinInput::SetInputState(const InputState& state)
{
   const uint64_t changes = state.actionState ^ m_inputState.actionState;
   uint64_t mask = 1ull;
   for (int i = 0; i < eActionCount; i++, mask <<= 1)
      if (changes & mask)
         FireActionEvent(static_cast<EnumPlayerActions>(i), (state.actionState & mask) != 0);
   m_inputState = state;
}

const Vertex2D& PinInput::GetNudge() const
{
   if (m_accelerometerDirty)
   {
      // Accumulate over all accelerometer devices
      m_accelerometer.SetZero();
      for (const auto& aam : m_analogActionMappings)
      {
         if (aam.output == AnalogAction::AM_NudgeX)
            m_accelerometer.x += clamp(aam.value, -m_accelerometerMax.x, m_accelerometerMax.x);
         if (aam.output == AnalogAction::AM_NudgeY)
            m_accelerometer.y += clamp(aam.value, -m_accelerometerMax.y, m_accelerometerMax.y);
      }

      // Scale to normalized float range, -1.0f..+1.0f
      float dx = m_accelerometer.x / static_cast<float>(JOYRANGEMX);
      const float dy = m_accelerometer.y / static_cast<float>(JOYRANGEMX);

      // Apply table mirroring
      if (g_pplayer->m_ptable->m_tblMirrorEnabled)
         dx = -dx;

      // Rotate to match hardware mounting orientation, including left or right coordinates (suppose same orientation of all hardwares, which could be improved)
      const float a = ANGTORAD(m_accelerometerAngle);
      const float cna = cosf(a);
      const float sna = sinf(a);
      m_accelerometer.x = m_accelerometerGain.x * (dx * cna + dy * sna) * (1.0f - m_accelerometerSensitivity); // calc Green's transform component for X
      const float nugY   = m_accelerometerGain.y * (dy * cna - dx * sna) * (1.0f - m_accelerometerSensitivity); // calc Green's transform component for Y
      m_accelerometer.y = m_accelerometerFaceUp ? nugY : -nugY; // add as left or right hand coordinate system

      m_accelerometerDirty = false;
   }
   return m_accelerometer;
}

void PinInput::SetNudge(const Vertex2D& nudge)
{
   m_accelerometer = nudge;
   m_accelerometerDirty = false;
}


bool PinInput::HasMechPlunger() const
{
   const auto& it = std::ranges::find_if(
      m_analogActionMappings.begin(), m_analogActionMappings.end(), [](const AnalogActionMapping& mapping) { return (mapping.output == AnalogAction::AM_PlungerPos); });
   return it != m_analogActionMappings.end();
}

float PinInput::GetPlungerPos() const
{
   if (m_plungerPosDirty)
   {
      m_plungerPos = 0.f;
      for (const auto& aam : m_analogActionMappings)
      {
         if (aam.output == AnalogAction::AM_PlungerPos)
            m_plungerPos += aam.value;
      }
      m_plungerPosDirty = false;
      //PLOGD << "Plunger pos  : " << m_plungerPos / static_cast<float>(JOYRANGEMX);
   }
   return m_plungerPos;
}

void PinInput::SetPlungerPos(float pos)
{
   constexpr uint64_t extPlungerId = 0xF00000000ull;
   m_plungerPosDirty = true;
   for (auto& aam : m_analogActionMappings)
      if (aam.output == AnalogAction::AM_PlungerPos && aam.joystickId == extPlungerId)
      {
         aam.value = pos;
         return;
      }
   MapAnalogActionToJoystick(AnalogAction::AM_PlungerPos, extPlungerId, 0, false, false);
   SetPlungerPos(pos);
}

bool PinInput::HasMechPlungerSpeed() const
{
   const auto& it = std::ranges::find_if(
      m_analogActionMappings.begin(), m_analogActionMappings.end(), [](const AnalogActionMapping& mapping) { return (mapping.output == AnalogAction::AM_PlungerSpeed); });
   return it != m_analogActionMappings.end();
}

float PinInput::GetPlungerSpeed() const
{
   if (m_plungerSpeedDirty)
   {
      m_plungerSpeed = 0.f;
      for (const auto& aam : m_analogActionMappings)
      {
         if (aam.output == AnalogAction::AM_PlungerSpeed)
            m_plungerSpeed += aam.value;
      }
      m_plungerSpeedDirty = false;
      //PLOGD << "Plunger speed: " << m_plungerSpeed / static_cast<float>(JOYRANGEMX);
   }
   return m_plungerSpeed;
}

void PinInput::SetPlungerSpeed(float speed)
{
   constexpr uint64_t extPlungerId = 0xF00000000ull;
   m_plungerPosDirty = true;
   for (auto& aam : m_analogActionMappings)
      if (aam.output == AnalogAction::AM_PlungerSpeed && aam.joystickId == extPlungerId)
      {
         aam.value = speed;
         return;
      }
   MapAnalogActionToJoystick(AnalogAction::AM_PlungerSpeed, extPlungerId, 1, false, false);
   SetPlungerPos(speed);
}

void PinInput::PlayRumble(const float lowFrequencySpeed, const float highFrequencySpeed, const int ms_duration)
{
   if (m_rumbleMode == 0)
      return;

   for (const auto& handler : m_inputHandlers)
      handler->PlayRumble(lowFrequencySpeed, highFrequencySpeed, ms_duration);

   #ifdef __LIBVPINBALL__
      VPinballLib::RumbleData rumbleData = {
         (uint16_t)(saturate(lowFrequencySpeed) * 65535.f),
         (uint16_t)(saturate(highFrequencySpeed) * 65535.f),
         (uint32_t)ms_duration
      };
      VPinballLib::VPinball::SendEvent(VPinballLib::Event::Rumble, &rumbleData);
   #endif
}

void PinInput::FireGenericKeyEvent(SDL_Scancode scancode, bool isPressed)
{
   // Check if we are mirrored & swap (some) left & right input.
   if (g_pplayer->m_ptable->m_tblMirrorEnabled)
   {
      switch (scancode)
      {
      case SDL_SCANCODE_LSHIFT: scancode = SDL_SCANCODE_RSHIFT; break;
      case SDL_SCANCODE_RSHIFT: scancode = SDL_SCANCODE_LSHIFT; break;
      case SDL_SCANCODE_LCTRL: scancode = SDL_SCANCODE_RCTRL; break;
      case SDL_SCANCODE_RCTRL: scancode = SDL_SCANCODE_LCTRL; break;
      case SDL_SCANCODE_RIGHT: scancode = SDL_SCANCODE_LEFT; break;
      case SDL_SCANCODE_LEFT: scancode = SDL_SCANCODE_RIGHT; break;
      case SDL_SCANCODE_LGUI: scancode = SDL_SCANCODE_RGUI; break;
      case SDL_SCANCODE_RGUI: scancode = SDL_SCANCODE_LGUI; break;
      default: break;
      }
   }
   const unsigned char dik = GetDirectInputKeyFromSDLScancode(scancode);
   if (dik == 0)
   {
      PLOGE << "No DirectInput mapping for SDL scancode '" << (int)scancode << "'. Key event was dropped.";
      return;
   }
   CComVariant rgvar[1] = { CComVariant(dik) };
   DISPPARAMS dispparams = { rgvar, nullptr, 1, 0 };
   g_pplayer->m_ptable->FireDispID(isPressed ? DISPID_GameEvents_KeyDown : DISPID_GameEvents_KeyUp, &dispparams);
}

void PinInput::FireActionEvent(EnumPlayerActions action, bool isPressed)
{
   // Allow plugins to react to action event, filter, ...

   VPXActionEvent event { static_cast<VPXAction>(action), isPressed };
   VPXPluginAPIImpl::GetInstance().BroadcastVPXMsg(m_onActionEventMsgId, &event);
   isPressed = event.isPressed;

   // Update input state
 
   if (isPressed == m_inputState.IsKeyDown(action))
      return; // Action has been discarded by a plugin
   else if (isPressed)
      m_inputState.SetPressed(action);
   else
      m_inputState.SetReleased(action);

   // Process action

   if (!g_pplayer->m_liveUI->IsTweakMode() && isPressed && (action == eLeftFlipperKey || action == eRightFlipperKey || action == eStagedLeftFlipperKey || action == eStagedRightFlipperKey))
   {
      g_pplayer->m_pininput.PlayRumble(0.f, 0.2f, 150);
      // Debug only, for testing parts of the flipper input lag
      m_leftkey_down_usec = usec();
      m_leftkey_down_frame = g_pplayer->m_overall_frames;
   }
}

void PinInput::Autostart(const uint32_t initialDelayMs, const uint32_t retryDelayMs)
{
   // Don't perform autostart if a game has been started once.
   // Note that this is hacky/buggy as it rely on the table to create ball only on game start
   // while lots of tables do create balls on startup instead.
   if (m_gameStartedOnce)
      return;
   if (!g_pplayer->m_vball.empty())
   {
      m_gameStartedOnce = true;
      return;
   }

   const uint32_t now = msec();
   if (m_autoStartTimestamp == 0)
   {
      m_autoStartTimestamp = now;
      return;
   }

   const uint32_t elapsed = now - m_autoStartTimestamp;
   if (m_autoStartPressed // Start button is down.
      && (elapsed > 100)) // Start button has been down for at least 0.10 seconds.
   {
      // Release start.
      m_autoStartTimestamp = now;
      m_autoStartPressed = false;
      FireActionEvent(eStartGameKey, false);
      PLOGD << "Autostart: Release";
   }
   else if (!m_autoStartPressed                                       // Start button is up.
       && (    ( m_autoStartDoneOnce && (elapsed > retryDelayMs))     // Not started and last attempt was at least AutoStartRetry seconds ago.
            || (!m_autoStartDoneOnce && (elapsed > initialDelayMs)))) // Never attempted and autostart time has elapsed.
   {
      // Press start.
      m_autoStartTimestamp = now;
      m_autoStartPressed = true;
      m_autoStartDoneOnce = true;
      FireActionEvent(eStartGameKey, true);
      PLOGD << "Autostart: Press";
   }
}

// Setup a hardware device button and analog input mapping
// For the time being, an action may only be bound to one button as we do not handle combination of multiple sources
// For analog input, multiple source are supported, averaging for nudge and summing for plunger (assuming there is only one non 0)
void PinInput::SetupJoyMapping(uint64_t joystickId, InputLayout inputLayout)
{
   const int lr_axis = g_pvp->m_settings.LoadValueWithDefault(Settings::Player, "LRAxis"s, 1);
   const int ud_axis = g_pvp->m_settings.LoadValueWithDefault(Settings::Player, "UDAxis"s, 2);
   const bool lr_axis_reverse = g_pvp->m_settings.LoadValueWithDefault(Settings::Player, "LRAxisFlip"s, false);
   const bool ud_axis_reverse = g_pvp->m_settings.LoadValueWithDefault(Settings::Player, "UDAxisFlip"s, false);
   const int plunger_axis = g_pvp->m_settings.LoadValueWithDefault(Settings::Player, "PlungerAxis"s, 3);
   const int plunger_speed_axis = g_pvp->m_settings.LoadValueWithDefault(Settings::Player, "PlungerSpeedAxis"s, 0);
   const bool plunger_reverse = g_pvp->m_settings.LoadValueWithDefault(Settings::Player, "ReversePlungerAxis"s, false);

   switch (inputLayout)
   {
   case InputLayout::PBWizard:
      SetupJoyMapping(joystickId, InputLayout::Generic);
      if (!m_override_default_buttons)
      {
         MapActionToJoystick(ePlungerKey, joystickId, 0, true);
         MapActionToJoystick(eRightFlipperKey, joystickId, 1, true);
         MapActionToJoystick(eRightMagnaSave, joystickId, 2, true);
         MapActionToJoystick(eVolumeDown, joystickId, 3, true);
         MapActionToJoystick(eVolumeUp, joystickId, 4, true);
         // Button 5 is not mapped
         MapActionToJoystick(eEscape, joystickId, 6, true);
         MapActionToJoystick(eExitGame, joystickId, 7, true);
         MapActionToJoystick(eStartGameKey, joystickId, 8, true);
         MapActionToJoystick(eLeftFlipperKey, joystickId, 9, true);
         MapActionToJoystick(eLeftMagnaSave, joystickId, 10, true);
         MapActionToJoystick(eAddCreditKey, joystickId, 11, true);
         MapActionToJoystick(eAddCreditKey2, joystickId, 12, true);
      }

      if (lr_axis != 0)
         MapAnalogActionToJoystick(AnalogAction::AM_NudgeX, joystickId, 1, true, false);
      if (ud_axis != 0)
         MapAnalogActionToJoystick(AnalogAction::AM_NudgeY, joystickId, 2, false, false);
      if (plunger_axis != 0)
      { // This can be overriden and assigned to Rz instead of Z axis
         if (m_override_default_buttons && (plunger_axis == 6))
            MapAnalogActionToJoystick(AnalogAction::AM_PlungerPos, joystickId, 6, false, false);
         else
            MapAnalogActionToJoystick(AnalogAction::AM_PlungerPos, joystickId, 3, true, false);
      }
      break;

   case InputLayout::UltraCade:
      SetupJoyMapping(joystickId, InputLayout::Generic);
      if (!m_override_default_buttons)
      {
         MapActionToJoystick(eAddCreditKey, joystickId, 11, true);
         MapActionToJoystick(eAddCreditKey2, joystickId, 12, true);
         MapActionToJoystick(eRightMagnaSave, joystickId, 2, true);
         // Button 3 is not mapped
         // Button 4 is not mapped
         MapActionToJoystick(eVolumeUp, joystickId, 5, true);
         MapActionToJoystick(eVolumeDown, joystickId, 6, true);
         // Button 7 is not mapped
         MapActionToJoystick(eLeftFlipperKey, joystickId, 8, true);
         // Button 9 is not mapped
         MapActionToJoystick(eRightFlipperKey, joystickId, 10, true);
         // Button 11 is not mapped
         MapActionToJoystick(eStartGameKey, joystickId, 12, true);
         MapActionToJoystick(ePlungerKey, joystickId, 13, true);
         MapActionToJoystick(eExitGame, joystickId, 14, true);
      }

      if (lr_axis != 0)
         MapAnalogActionToJoystick(AnalogAction::AM_NudgeX, joystickId, 2, true, false);
      if (ud_axis != 0)
         MapAnalogActionToJoystick(AnalogAction::AM_NudgeY, joystickId, 1, true, false);
      if (plunger_axis != 0)
         MapAnalogActionToJoystick(AnalogAction::AM_PlungerPos, joystickId, 3, false, false);
      break;

   case InputLayout::Sidewinder:
      SetupJoyMapping(joystickId, InputLayout::Generic);

      if (lr_axis != 0)
         MapAnalogActionToJoystick(AnalogAction::AM_NudgeX, joystickId, 1, lr_axis_reverse, false);
      if (ud_axis != 0)
         MapAnalogActionToJoystick(AnalogAction::AM_NudgeY, joystickId, 2, ud_axis_reverse, false);
      if (plunger_axis != 0)
         MapAnalogActionToJoystick(AnalogAction::AM_PlungerPos, joystickId, 7, !plunger_reverse, false);
      break;

   case InputLayout::VirtuaPin:
      SetupJoyMapping(joystickId, InputLayout::Generic);
      if (!m_override_default_buttons)
      {
         MapActionToJoystick(ePlungerKey, joystickId, 0, true);
         MapActionToJoystick(eRightFlipperKey, joystickId, 1, true);
         MapActionToJoystick(eRightMagnaSave, joystickId, 2, true);
         MapActionToJoystick(eVolumeDown, joystickId, 3, true);
         MapActionToJoystick(eVolumeUp, joystickId, 4, true);
         // Button 5 is not mapped
         MapActionToJoystick(eEscape, joystickId, 6, true);
         MapActionToJoystick(eExitGame, joystickId, 7, true);
         MapActionToJoystick(eStartGameKey, joystickId, 8, true);
         MapActionToJoystick(eLeftFlipperKey, joystickId, 9, true);
         MapActionToJoystick(eLeftMagnaSave, joystickId, 10, true);
         MapActionToJoystick(eAddCreditKey, joystickId, 11, true);
         MapActionToJoystick(eAddCreditKey2, joystickId, 12, true);
      }

      if (lr_axis != 0)
         MapAnalogActionToJoystick(AnalogAction::AM_NudgeX, joystickId, 1, true, false);
      if (ud_axis != 0)
         MapAnalogActionToJoystick(AnalogAction::AM_NudgeY, joystickId, 2, false, false);
      if (plunger_axis != 0)
         MapAnalogActionToJoystick(AnalogAction::AM_PlungerPos, joystickId, 3, true, false);
      break;

   case InputLayout::OpenPinDev:
      SetupJoyMapping(joystickId, InputLayout::Generic);

      // OpenPinDev does not conform to the normal axis mapping. It adds itself to the list of axes
      // we hack this by using virtual axis 10..13 for this device. THis is not clean and should be
      // removed in favor of handling these devices like all others.
      m_analogActionMappings.clear();
      if (lr_axis == 9)
         MapAnalogActionToJoystick(AnalogAction::AM_NudgeX, joystickId, 10, lr_axis_reverse, false);
      if (ud_axis == 9)
         MapAnalogActionToJoystick(AnalogAction::AM_NudgeY, joystickId, 11, ud_axis_reverse, false);
      if (plunger_axis == 9)
         MapAnalogActionToJoystick(AnalogAction::AM_PlungerPos, joystickId, 12, plunger_reverse, false);
      if (plunger_speed_axis == 9)
         MapAnalogActionToJoystick(AnalogAction::AM_PlungerSpeed, joystickId, 13, false, false);
      break;

   case InputLayout::Generic:
   default:
      {
         const Settings& settings = g_pvp->m_settings;
         MapActionToJoystick(eLeftFlipperKey, joystickId, settings.LoadValueInt(Settings::Player, "JoyLFlipKey"s), true);
         MapActionToJoystick(eRightFlipperKey, joystickId, settings.LoadValueInt(Settings::Player, "JoyRFlipKey"s), true);
         MapActionToJoystick(eStagedLeftFlipperKey, joystickId, settings.LoadValueInt(Settings::Player, "JoyStagedLFlipKey"s), true);
         MapActionToJoystick(eStagedRightFlipperKey, joystickId, settings.LoadValueInt(Settings::Player, "JoyStagedRFlipKey"s), true);
         MapActionToJoystick(eLeftTiltKey, joystickId, settings.LoadValueInt(Settings::Player, "JoyLTiltKey"s), true);
         MapActionToJoystick(eRightTiltKey, joystickId, settings.LoadValueInt(Settings::Player, "JoyRTiltKey"s), true);
         MapActionToJoystick(eCenterTiltKey, joystickId, settings.LoadValueInt(Settings::Player, "JoyCTiltKey"s), true);
         MapActionToJoystick(ePlungerKey, joystickId, settings.LoadValueInt(Settings::Player, "JoyPlungerKey"s), true);
         MapActionToJoystick(eFrameCount, joystickId, settings.LoadValueInt(Settings::Player, "JoyFrameCount"s), true);
         MapActionToJoystick(eDBGBalls, joystickId, settings.LoadValueInt(Settings::Player, "JoyDebugKey"s), true);
         MapActionToJoystick(eDebugger, joystickId, settings.LoadValueInt(Settings::Player, "JoyDebuggerKey"s), true);
         MapActionToJoystick(eAddCreditKey, joystickId, settings.LoadValueInt(Settings::Player, "JoyAddCreditKey"s), true);
         MapActionToJoystick(eAddCreditKey2, joystickId, settings.LoadValueInt(Settings::Player, "JoyAddCredit2Key"s), true);
         MapActionToJoystick(eStartGameKey, joystickId, settings.LoadValueInt(Settings::Player, "JoyStartGameKey"s), true);
         MapActionToJoystick(eMechanicalTilt, joystickId, settings.LoadValueInt(Settings::Player, "JoyMechTiltKey"s), true);
         MapActionToJoystick(eRightMagnaSave, joystickId, settings.LoadValueInt(Settings::Player, "JoyRMagnaSave"s), true);
         MapActionToJoystick(eLeftMagnaSave, joystickId, settings.LoadValueInt(Settings::Player, "JoyLMagnaSave"s), true);
         MapActionToJoystick(eExitGame, joystickId, settings.LoadValueInt(Settings::Player, "JoyExitGameKey"s), true);
         MapActionToJoystick(eVolumeUp, joystickId, settings.LoadValueInt(Settings::Player, "JoyVolumeUp"s), true);
         MapActionToJoystick(eVolumeDown, joystickId, settings.LoadValueInt(Settings::Player, "JoyVolumeDown"s), true);
         MapActionToJoystick(eLockbarKey, joystickId, settings.LoadValueInt(Settings::Player, "JoyLockbarKey"s), true);
         // eEnable3D (no joystick mapping)
         MapActionToJoystick(eTableRecenter, joystickId, settings.LoadValueInt(Settings::Player, "JoyTableRecenterKey"s), true);
         MapActionToJoystick(eTableUp, joystickId, settings.LoadValueInt(Settings::Player, "JoyTableUpKey"s), true);
         MapActionToJoystick(eTableDown, joystickId, settings.LoadValueInt(Settings::Player, "JoyTableDownKey"s), true);
         // eEscape (no joystick mapping)
         MapActionToJoystick(ePause, joystickId, settings.LoadValueInt(Settings::Player, "JoyPauseKey"s), true);
         MapActionToJoystick(eTweak, joystickId, settings.LoadValueInt(Settings::Player, "JoyTweakKey"s), true);

         // TODO map to corresponding GenericKey (or define actions for these keys)
         // MapActionToJoystick(, joystickId, settings.LoadValueWithDefault(Settings::Player, "JoyPMBuyIn"s, 0), true); 2
         // MapActionToJoystick(, joystickId, settings.LoadValueWithDefault(Settings::Player, "JoyPMCoin3"s, 0), true); 5
         // MapActionToJoystick(, joystickId, settings.LoadValueWithDefault(Settings::Player, "JoyPMCoin4"s, 0), true); 6
         // MapActionToJoystick(, joystickId, settings.LoadValueWithDefault(Settings::Player, "JoyPMCoinDoor"s, 0), true); END
         // MapActionToJoystick(, joystickId, settings.LoadValueWithDefault(Settings::Player, "JoyPMCancel"s, 0), true); 7
         // MapActionToJoystick(, joystickId, settings.LoadValueWithDefault(Settings::Player, "JoyPMDown"s, 0), true); 8
         // MapActionToJoystick(, joystickId, settings.LoadValueWithDefault(Settings::Player, "JoyPMUp"s, 0), true); 9
         // MapActionToJoystick(, joystickId, settings.LoadValueWithDefault(Settings::Player, "JoyPMEnter"s, 0), true); 0

         // TODO map to corresponding GenericKey
         // MapActionToJoystick(, joystickId, settings.LoadValueWithDefault(Settings::Player, "JoyCustom1"s, 0), true);
         // MapActionToJoystick(, joystickId, settings.LoadValueWithDefault(Settings::Player, "JoyCustom1Key"s, 0), true);
         // MapActionToJoystick(, joystickId, settings.LoadValueWithDefault(Settings::Player, "JoyCustom2"s, 0), true);
         // MapActionToJoystick(, joystickId, settings.LoadValueWithDefault(Settings::Player, "JoyCustom2Key"s, 0), true);
         // MapActionToJoystick(, joystickId, settings.LoadValueWithDefault(Settings::Player, "JoyCustom3"s, 0), true);
         // MapActionToJoystick(, joystickId, settings.LoadValueWithDefault(Settings::Player, "JoyCustom3Key"s, 0), true);
         // MapActionToJoystick(, joystickId, settings.LoadValueWithDefault(Settings::Player, "JoyCustom4"s, 0), true);
         // MapActionToJoystick(, joystickId, settings.LoadValueWithDefault(Settings::Player, "JoyCustom4Key"s, 0), true);

         if (lr_axis != 0)
            MapAnalogActionToJoystick(AnalogAction::AM_NudgeX, joystickId, lr_axis, lr_axis_reverse, false);
         if (ud_axis != 0)
            MapAnalogActionToJoystick(AnalogAction::AM_NudgeY, joystickId, ud_axis, ud_axis_reverse, false);
         if (plunger_axis != 0)
            MapAnalogActionToJoystick(AnalogAction::AM_PlungerPos, joystickId, plunger_axis, plunger_reverse, false);
         if (plunger_speed_axis != 0)
            MapAnalogActionToJoystick(AnalogAction::AM_PlungerSpeed, joystickId, plunger_speed_axis, false, false);
      }
      break;
   }
}

void PinInput::ProcessInput()
{
   if (!g_pplayer || !g_pplayer->m_ptable) return; // only if player is running
   g_pplayer->m_logicProfiler.OnProcessInput();

   const uint32_t now = msec();

   // Gather input from all handlers
   #ifdef _WIN32
   const HWND foregroundWindow = GetForegroundWindow();
   #else
   constexpr HWND foregroundWindow = NULL;
   #endif
   for (const auto& handler : m_inputHandlers)
      handler->Update(foregroundWindow);

   // Wipe action state if we're not the foreground window as we miss key-up events
   #ifdef _WIN32
   if (m_focusHWnd != foregroundWindow)
      memset(&m_inputState, 0, sizeof(m_inputState));
   #endif

   // Handle automatic start
   if (g_pplayer->m_ptable->m_tblAutoStartEnabled)
      Autostart(g_pplayer->m_ptable->m_tblAutoStart, g_pplayer->m_ptable->m_tblAutoStartRetry);
   if (m_autoStartTimestamp == 0) // Check if we've been initialized.
      m_autoStartTimestamp = now;

   for (const auto action : m_onUpdateActions)
      action->OnUpdate();
}

void PinInput::ProcessEvent(const InputEvent& event)
{
   if (event.type == InputEvent::Type::Mouse && !g_pplayer->m_liveUI->HasMouseCapture())
   {
      const auto& it = std::ranges::find_if(m_actionMappings.begin(), m_actionMappings.end(),
         [&event](const ActionMapping& mapping) { return (mapping.type == ActionMapping::AM_Mouse) && (mapping.buttonId == event.buttonId); });
      if (it != m_actionMappings.end())
         FireActionEvent(it->action, event.isPressed);
   }
   else if (event.type == InputEvent::Type::Keyboard)
   {
      if (auto it = m_keyMappings.find(event.scancode); it != m_keyMappings.end())
         for (auto mapping : it->second)
            mapping->SetPressed(event.isPressed);

      if (!g_pplayer->m_liveUI->IsTweakMode() && !g_pplayer->m_liveUI->HasKeyboardCapture())
         FireGenericKeyEvent(event.scancode, event.isPressed);
   }
   else if (event.type == InputEvent::Type::Action)
   {
      FireActionEvent(event.action, event.isPressed);
   }
   else if (event.type == InputEvent::Type::JoyButton)
   {
      const auto& it = std::ranges::find_if(m_actionMappings.begin(), m_actionMappings.end(),
         [&event](const ActionMapping& mapping) { return (mapping.type == ActionMapping::AM_Joystick) && (mapping.joystickId == event.joystickId) && (mapping.buttonId == event.buttonId); });
      if (it != m_actionMappings.end())
         FireActionEvent(it->action, event.isPressed);
      else
      {
         if (m_joycustom1 == event.buttonId)
            FireGenericKeyEvent(m_joycustom1key, event.isPressed);
         else if (m_joycustom2 == event.buttonId)
            FireGenericKeyEvent(m_joycustom2key, event.isPressed);
         else if (m_joycustom3 == event.buttonId)
            FireGenericKeyEvent(m_joycustom3key, event.isPressed);
         else if (m_joycustom4 == event.buttonId)
            FireGenericKeyEvent(m_joycustom4key, event.isPressed);
         else if (m_joypmbuyin == event.buttonId)
            FireGenericKeyEvent(SDL_SCANCODE_2, event.isPressed);
         else if (m_joypmcoin3 == event.buttonId)
            FireGenericKeyEvent(SDL_SCANCODE_5, event.isPressed);
         else if (m_joypmcoin4 == event.buttonId)
            FireGenericKeyEvent(SDL_SCANCODE_6, event.isPressed);
         else if (m_joypmcoindoor == event.buttonId)
            FireGenericKeyEvent(SDL_SCANCODE_END, event.isPressed);
         else if (m_joypmcancel == event.buttonId)
            FireGenericKeyEvent(SDL_SCANCODE_7, event.isPressed);
         else if (m_joypmdown == event.buttonId)
            FireGenericKeyEvent(SDL_SCANCODE_8, event.isPressed);
         else if (m_joypmup == event.buttonId)
            FireGenericKeyEvent(SDL_SCANCODE_9, event.isPressed);
         else if (m_joypmenter == event.buttonId)
            FireGenericKeyEvent(SDL_SCANCODE_0, event.isPressed);
      }
   }
   else if (event.type == InputEvent::Type::JoyAxis)
   {
      const auto& it = std::ranges::find_if(m_analogActionMappings.begin(), m_analogActionMappings.end(),
         [&event](const AnalogActionMapping& mapping) { return (mapping.joystickId == event.joystickId) && (mapping.axisId == event.axisId); });
      if (it != m_analogActionMappings.end())
      {
         auto newValue = static_cast<float>(it->revert ? -event.value : event.value);
         switch (it->output)
         {
         case AnalogAction::AM_NudgeX:
         case AnalogAction::AM_NudgeY:
            if (newValue < -m_deadz)
               newValue = (newValue + static_cast<float>(m_deadz)) * static_cast<float>(JOYRANGEMX) / static_cast<float>(JOYRANGEMX - m_deadz);
            else if (newValue > m_deadz)
               newValue = (newValue - static_cast<float>(m_deadz)) * static_cast<float>(JOYRANGEMX) / static_cast<float>(JOYRANGEMX - m_deadz);
            else
               newValue = 0.f;
            m_accelerometerDirty |= (newValue != it->value);
            break;

         case AnalogAction::AM_PlungerSpeed:
            m_plungerSpeedDirty |= (newValue != it->value);
            break;

         case AnalogAction::AM_PlungerPos:
            m_plungerPosDirty |= (newValue != it->value);
            break;
         }
         it->value = newValue;
      }
   }
}
