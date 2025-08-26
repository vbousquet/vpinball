// license:GPLv3+

#pragma once

namespace VPX::InGameUI
{

class InGameUIItem
{
public:
   InGameUIItem(string label)
      : m_type(Type::Info) // Common
      , m_label(std::move(label))
      , m_path(""s) // Unused
      , m_minValue(0.f)
      , m_maxValue(0.f)
      , m_value(0.f)
      , m_step(0.f)
      , m_defValue(0.f)
      , m_enum()
   {
      m_initialValue = m_value;
      Validate();
   }

   InGameUIItem(string label, string tooltip, string path)
      : m_type(Type::Navigation) // Common
      , m_label(std::move(label))
      , m_tooltip(std::move(tooltip))
      , m_path(std::move(path)) // Item
      , m_minValue(0.f) // Unused
      , m_maxValue(0.f)
      , m_value(0.f)
      , m_step(0.f)
      , m_defValue(0.f)
      , m_enum()
   {
      m_initialValue = m_value;
      Validate();
   }

   InGameUIItem(
      string label, string tooltip, int min, int max, int value, int defValue, string format, std::function<void(int)> onChange, std::function<void(int, Settings&, bool)> onSave)
      : m_type(Type::IntValue) // Common
      , m_label(std::move(label))
      , m_tooltip(std::move(tooltip))
      , m_minValue(static_cast<float>(min)) // Item
      , m_maxValue(static_cast<float>(max))
      , m_value(static_cast<float>(value))
      , m_step(1.f)
      , m_defValue(GetStepAlignedValue(static_cast<float>(defValue)))
      , m_format(format)
      , m_onChangeInt(onChange)
      , m_onSaveInt(onSave)
      , m_path(""s) // Unused
      , m_enum()
   {
      m_initialValue = m_value;
      Validate();
   }

   InGameUIItem(string label, string tooltip, float min, float max, float step, float value, float defValue, string format, std::function<void(float)> onChange, std::function<void(float, Settings&, bool)> onSave)
      : m_type(Type::FloatValue) // Common
      , m_label(std::move(label))
      , m_tooltip(std::move(tooltip))
      , m_minValue(min) // Item
      , m_maxValue(max)
      , m_value(value)
      , m_step(step)
      , m_defValue(GetStepAlignedValue(defValue))
      , m_format(format)
      , m_onChangeFloat(onChange)
      , m_onSaveFloat(onSave)
      , m_path(""s) // Unused
      , m_enum()
   {
      m_initialValue = m_value;
      Validate();
   }

   InGameUIItem(string label, string tooltip, std::initializer_list<string> values, int value, int defValue, std::function<void(int)> onChange, std::function<void(int, Settings&, bool)> onSave)
      : m_type(Type::EnumValue) // Common
      , m_label(std::move(label))
      , m_tooltip(std::move(tooltip))
      , m_enum(values) // Item
      , m_minValue(0.f)
      , m_maxValue(static_cast<float>(values.size()))
      , m_value(static_cast<float>(value))
      , m_step(1.f)
      , m_defValue(GetStepAlignedValue(static_cast<float>(defValue)))
      , m_onChangeInt(onChange)
      , m_onSaveInt(onSave)
      , m_path(""s) // Unused
      , m_format(""s)
   {
      m_initialValue = m_value;
      Validate();
   }

   InGameUIItem(string label, string tooltip, bool value, bool defValue, string format, std::function<void(bool)> onChange, std::function<void(bool, Settings&, bool)> onSave)
      : m_type(Type::Toggle) // Common
      , m_label(std::move(label))
      , m_tooltip(std::move(tooltip))
      , m_minValue(0.f) // Item
      , m_maxValue(1.f)
      , m_value(value ? 1.f : 0.f)
      , m_step(1.f)
      , m_defValue(defValue ? 1.f : 0.f)
      , m_format(format)
      , m_onChangeBool(onChange)
      , m_onSaveBool(onSave)
      , m_path(""s) // Unused
      , m_enum()
   {
      m_initialValue = m_value;
      Validate();
   }

   void SetInitialValue(bool v);
   void SetInitialValue(int v);
   void SetInitialValue(float v);
   bool IsModified() const;
   bool IsDefaultValue() const;

   bool IsInteractive() { return m_type != Type::Info; }
   bool IsAdjustable() { return m_type != Type::Info && m_type != Type::Navigation; }

   void ResetToInitialValue();
   void ResetToDefault();
   void Save(Settings& settings, bool isTableOverride);

   float GetFloatValue() const { return m_value; }
   int GetIntValue() const { return static_cast<int>(m_value); }
   void SetValue(float value);
   void SetValue(int value);
   void SetValue(bool value);

   enum class Type
   {
      Info,
      Navigation,
      FloatValue,
      IntValue,
      EnumValue,
      Toggle
   };

   const Type m_type;
   const string m_label;
   const string m_tooltip;

   // Navigation item
   const string m_path;

   // Ranged value item
   const float m_minValue;
   const float m_maxValue;
   const float m_step;

   // Enum value item
   vector<string> m_enum;

   // Properties shared by value items
   const float m_defValue;
   const string m_format;

private:
   void Validate();
   float GetStepAlignedValue(float v) const { return m_minValue + static_cast<int>((v - m_minValue) / m_step) * m_step; }

   float m_value;
   float m_initialValue;

   const std::function<void(int)> m_onChangeInt;
   const std::function<void(bool)> m_onChangeBool;
   const std::function<void(float)> m_onChangeFloat;
   const std::function<void(int, Settings&, bool)> m_onSaveInt;
   const std::function<void(bool, Settings&, bool)> m_onSaveBool;
   const std::function<void(float, Settings&, bool)> m_onSaveFloat;
};

};