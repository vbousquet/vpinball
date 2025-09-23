// license:GPLv3+

#pragma once


// Input event manager that manages input event source and dispatch them
class InputEventManager
{
public:
   virtual ~InputEventManager() = default;
   virtual void OnInputActionStateChanged(class InputAction* action) = 0;
   virtual void Register(class DigitalMapping* mapping) = 0;
   virtual void Unregister(class DigitalMapping* mapping) = 0;
   virtual void RegisterOnUpdate(class InputAction* action) = 0;
   virtual void UnregisterOnUpdate(class InputAction* action) = 0;
};


// Mapping handler that maps a group of input events to an action
class MappingHandler
{
public:
   virtual ~MappingHandler() = default;
   virtual void OnInputChanged(class DigitalMapping* mapping) = 0;
};


// Individual input event mapping
class DigitalMapping
{
public:
   enum class Type
   {
      Keyboard, JoystickButton, JoystickAxis, MouseButton, MouseAxis, VRControllerButton, VRControllerAxis
   };

   DigitalMapping(InputEventManager* eventManager, MappingHandler* mappingHandler, Type type, SDL_Scancode sdlScanCode)
      : m_type(type)
      , m_sdlScanCode(sdlScanCode)
      // Unused
      , m_button(0)
      , m_axis(0)
      , m_pressedThreshold(0.f)
      , m_releasedThreshold(0.f)
      , m_sdlJoyGUID()
      // Events
      , m_eventManager(eventManager)
      , m_mappingHandler(mappingHandler)
   {
      m_eventManager->Register(this);
   }

   DigitalMapping(InputEventManager* eventManager, MappingHandler* mappingHandler, Type type, int button)
      : m_type(type)
      , m_button(button)
      // Unused
      , m_sdlScanCode(SDL_SCANCODE_UNKNOWN)
      , m_axis(0)
      , m_pressedThreshold(0.f)
      , m_releasedThreshold(0.f)
      , m_sdlJoyGUID()
      // Events
      , m_eventManager(eventManager)
      , m_mappingHandler(mappingHandler)
   {
      m_eventManager->Register(this);
   }

   DigitalMapping(DigitalMapping&& other) noexcept
      : m_type(other.m_type)
      , m_sdlScanCode(other.m_sdlScanCode)
      , m_sdlJoyGUID(other.m_sdlJoyGUID)
      , m_button(other.m_button)
      , m_axis(other.m_axis)
      , m_pressedThreshold(other.m_pressedThreshold)
      , m_releasedThreshold(other.m_releasedThreshold)
      , m_isPressed(other.m_isPressed)
      , m_eventManager(other.m_eventManager)
      , m_mappingHandler(other.m_mappingHandler)
   {
      m_eventManager->Register(this);
   }

   ~DigitalMapping()
   {
      m_eventManager->Unregister(this);
   }

   static DigitalMapping Parse(InputEventManager* eventManager, MappingHandler* mappingHandler, string mappingDef);
   void Serialize(std::stringstream& result) const;

   const Type m_type;
   const SDL_Scancode m_sdlScanCode;
   const int m_button;
   const int m_axis;
   const float m_pressedThreshold;
   const float m_releasedThreshold;
   const SDL_GUID m_sdlJoyGUID;

   void SetPressed(bool pressed)
   {
      if (pressed != m_isPressed)
      {
         m_isPressed = pressed;
         m_mappingHandler->OnInputChanged(this);
      }
   }
   bool IsPressed() const { return m_isPressed; }

private:
   bool m_isPressed = false;
   InputEventManager* const m_eventManager;
   MappingHandler* const m_mappingHandler;
};


// Inputs to digital action mapping handler
class InputAction final : public MappingHandler
{
public:
   InputAction(InputEventManager* eventManager, const string& settingId, const string& label, const string& defaultMappings, std::function<void(const InputAction&, bool, bool)> onChange)
      : m_settingId(settingId)
      , m_label(label)
      , m_defaultMappings(defaultMappings)
      , m_onStateChange(onChange)
      , m_eventManager(eventManager)
   {
   }
   InputAction(InputAction&& other) = delete;
   ~InputAction() override = default;

   void ClearMappings();
   void LoadMappings(const Settings& settings);
   void SaveMappings(Settings& settings) const;

   void SetEnabled(bool enabled);
   void OnInputChanged(DigitalMapping* mapping) override;
   bool IsPressed() const { return m_isPressed; }

   void OnUpdate();
   void SetRepeatPeriod(int delayMs);

   int m_actionId = -1;

private:
   const string m_settingId;
   const string m_label;
   const string m_defaultMappings;
   const std::function<void(const InputAction&, bool, bool)> m_onStateChange;
   std::function<void(void)> m_onUpdateCallback;

   InputEventManager* const m_eventManager;
   vector<vector<DigitalMapping>> m_inputMappings;
   bool m_isPressed = false;
   bool m_enabled = true;
   int m_lastOnChangeMs = 0;
   int m_repeatPeriodMs = -1;
};
