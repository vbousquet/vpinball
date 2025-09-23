// license:GPLv3+

#include "core/stdafx.h"
#include "InputAction.h"

#include <sstream>

DigitalMapping DigitalMapping::Parse(InputEventManager* eventManager, MappingHandler* mappingHandler, string mappingDef)
{
   mappingDef = trim_string(mappingDef);
   if (mappingDef.length() < 2)
      return DigitalMapping(eventManager, mappingHandler, DigitalMapping::Type::Keyboard, SDL_SCANCODE_UNKNOWN);
   switch (mappingDef[0])
   {
   case 'K': // Keyboard mapping: Kxx where xx is the SDL scancode
      if (int scancode; try_parse_int(mappingDef.substr(1), scancode))
         return DigitalMapping(eventManager, mappingHandler, DigitalMapping::Type::Keyboard, static_cast<SDL_Scancode>(scancode));
      break;
   case 'J': // Joystick button mapping: Jxx;yy where xx is the joystick GUID and yy is the button number (see SDL_GetJoystickGUIDForID)
      break;
   case 'L': // Joystick axis mapping: Jxx;yy;zz;kk where xx is the joystick GUID, yy is the axis number, zz is the pressed threshold, kk is the released threshold
      break;
   case 'M': // Mouse button: Mxx
      if (int button; try_parse_int(mappingDef.substr(1), button))
         return DigitalMapping(eventManager, mappingHandler, DigitalMapping::Type::MouseButton, button);
      break;
   case 'N': // Mouse axis: Mxx;yy;zz
      break;
   case 'T': // Touchscreen area: Txx;yy;zz;kk
      break;
   case 'V': // VR controller button mapping: Jxx;yy where xx is the controller GUID and yy is the button number
      break;
   case 'W': // VR controller axis mapping: Jxx;yy;zz;kk where xx is the controller GUID, yy is the axis number, zz is the pressed threshold, kk is the released threshold
      break;
   default: break;
   }
   return DigitalMapping(eventManager, mappingHandler, DigitalMapping::Type::Keyboard, SDL_SCANCODE_UNKNOWN);
}

void DigitalMapping::Serialize(std::stringstream& result) const
{
   switch (m_type)
   {
   case DigitalMapping::Type::Keyboard: result << 'K' << static_cast<int>(m_sdlScanCode); break;

   case DigitalMapping::Type::JoystickButton:
      result << 'J' << std::hex;
      for (int i = 0; i < 16; i++)
         result << m_sdlJoyGUID.data[i] << (i == 15 ? ';' : '.');
      result << std::dec << m_button;
      break;

   case DigitalMapping::Type::JoystickAxis:
      result << 'L' << std::hex;
      for (int i = 0; i < 16; i++)
         result << m_sdlJoyGUID.data[i] << (i == 15 ? ';' : '.');
      result << std::dec << m_axis << ';' << f2sz(m_pressedThreshold) << ';' << f2sz(m_releasedThreshold);
      break;

   default: assert(false); break;
   }
}


void InputAction::ClearMappings()
{
   m_inputMappings.clear();
}

void InputAction::LoadMappings(const Settings& settings)
{
   ClearMappings();

   const string mappingString = settings.LoadValueWithDefault(Settings::Section::Input, "Mapping."s + m_settingId, m_defaultMappings);

   // Split by '|' which corresponds to different key bindings (any of them triggere the action, so it is a 'or')
   std::istringstream outerStream(mappingString);
   std::string outerToken;
   while (std::getline(outerStream, outerToken, '|'))
   {
      // Split by '&' which corresponds to the key that must be pressed together to fullfill the binding
      std::istringstream innerStream(outerToken);
      std::vector<DigitalMapping> innerVector;
      std::string innerToken;
      while (std::getline(innerStream, innerToken, '&'))
      {
         DigitalMapping mapping = DigitalMapping::Parse(m_eventManager, this, innerToken);
         if (mapping.m_type == DigitalMapping::Type::Keyboard && mapping.m_sdlScanCode == SDL_SCANCODE_UNKNOWN)
         {
            PLOGE << "Invalid input mapping type: " << innerToken;
         }
         else
         {
            innerVector.push_back(std::move(mapping));
         }
      }
      if (!innerVector.empty())
         m_inputMappings.push_back(std::move(innerVector));
   }
}

void InputAction::SaveMappings(Settings& settings) const
{
   std::stringstream result;
   bool firstOr = true;
   for (const auto& mappings : m_inputMappings)
   {
      if (!firstOr)
         result << " | ";
      firstOr = false;
      bool firstAnd = true;
      for (const DigitalMapping& mapping : mappings)
      {
         if (!firstAnd)
            result << '&';
         firstAnd = false;
         mapping.Serialize(result);
      }
   }
   settings.SaveValue(Settings::Section::Input, "Mapping."s + m_settingId, result.str());
}

void InputAction::SetEnabled(bool enabled)
{
   m_enabled = enabled;
}

void InputAction::OnInputChanged(DigitalMapping*)
{
   const bool wasPressed = m_isPressed;
   m_isPressed = false;
   for (const auto& mappings : m_inputMappings)
   {
      m_isPressed = true;
      for (const auto& mapping : mappings)
      {
         m_isPressed &= mapping.IsPressed();
         if (!m_isPressed)
            break;
      }
      if (m_isPressed)
         break;
   }
   if (m_isPressed != wasPressed)
   {
      m_eventManager->OnInputActionStateChanged(this);
      m_lastOnChangeMs = msec();
      if (m_enabled)
         m_onStateChange(*this, wasPressed, m_isPressed);
      if (m_isPressed && m_repeatPeriodMs >= 0)
         m_eventManager->RegisterOnUpdate(this);
      else if (m_repeatPeriodMs >= 0)
         m_eventManager->UnregisterOnUpdate(this);
   }
}

void InputAction::OnUpdate()
{
   if (m_isPressed)
   {
      int now = msec();
      if (now >= m_lastOnChangeMs + m_repeatPeriodMs)
      {
         m_lastOnChangeMs = now;
         if (m_enabled)
            m_onStateChange(*this, m_isPressed, m_isPressed);
      }
   }
}

void InputAction::SetRepeatPeriod(int delayMs)
{
   if (m_isPressed && m_repeatPeriodMs >= 0)
      m_eventManager->UnregisterOnUpdate(this);
   m_repeatPeriodMs = delayMs;
   if (m_isPressed && m_repeatPeriodMs >= 0)
      m_eventManager->RegisterOnUpdate(this);
}
