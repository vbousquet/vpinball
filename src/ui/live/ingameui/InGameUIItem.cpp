// license:GPLv3+

#include "core/stdafx.h"

#include "InGameUIItem.h"

namespace VPX::InGameUI
{

void InGameUIItem::Validate()
{
   switch (m_type)
   {
   case Type::FloatValue:
      assert(m_minValue < m_maxValue);
      assert(m_step > 0.f);
      m_value = clamp(m_value, m_minValue, m_maxValue);
      m_value = GetStepAlignedValue(m_value);
      m_initialValue = clamp(m_initialValue, m_minValue, m_maxValue);
      m_initialValue = GetStepAlignedValue(m_initialValue);
      assert(m_defValue == m_minValue + static_cast<int>((m_defValue - m_minValue) / m_step) * m_step);
      assert(m_minValue <= m_initialValue && m_initialValue <= m_maxValue);
      assert(m_minValue <= m_defValue && m_defValue <= m_maxValue);
      assert(m_minValue <= m_value && m_value <= m_maxValue);
      break;

   case Type::IntValue: 
      assert(m_minValue < m_maxValue);
      assert(m_step > 0.f);
      m_value = clamp(m_value, m_minValue, m_maxValue);
      m_value = GetStepAlignedValue(m_value);
      m_initialValue = clamp(m_initialValue, m_minValue, m_maxValue);
      m_initialValue = GetStepAlignedValue(m_initialValue);
      assert(m_defValue == m_minValue + static_cast<int>((m_defValue - m_minValue) / m_step) * m_step);
      assert(m_minValue <= m_initialValue && m_initialValue <= m_maxValue);
      assert(m_minValue <= m_defValue && m_defValue <= m_maxValue);
      assert(m_minValue <= m_value && m_value <= m_maxValue);
      break;

   default:
      break;
   }
}

void InGameUIItem::SetInitialValue(bool v)
{
   m_initialValue = v ? 1.f : 0.f;
   Validate();
}

void InGameUIItem::SetInitialValue(int v)
{
   m_initialValue = static_cast<float>(v);
   Validate();
}

void InGameUIItem::SetInitialValue(float v)
{
   m_initialValue = v;
   Validate();
}

bool InGameUIItem::IsModified() const
{
   switch (m_type)
   {
   case Type::FloatValue: return m_value != m_initialValue;
   case Type::IntValue: return m_value != m_initialValue;
   case Type::EnumValue: return m_value != m_initialValue;
   case Type::Toggle: return m_value != m_initialValue;
   default: return false;
   }
}

bool InGameUIItem::IsDefaultValue() const
{
   switch (m_type)
   {
   case Type::FloatValue: return m_value == m_defValue;
   case Type::IntValue: return m_value == m_defValue;
   case Type::EnumValue: return m_value == m_defValue;
   case Type::Toggle: return m_value == m_defValue;
   default: return true;
   }
}

void InGameUIItem::ResetToInitialValue()
{
   switch (m_type)
   {
   case Type::FloatValue: SetValue(m_initialValue); break;
   case Type::IntValue: SetValue(static_cast<int>(m_initialValue)); break;
   case Type::EnumValue: SetValue(static_cast<int>(m_initialValue)); break;
   case Type::Toggle: SetValue(m_initialValue != 0.f); break;
   default: break;
   }
}

void InGameUIItem::ResetToDefault()
{
   switch (m_type)
   {
   case Type::FloatValue: SetValue(m_defValue); break;
   case Type::IntValue: SetValue(static_cast<int>(m_defValue)); break;
   case Type::EnumValue: SetValue(static_cast<int>(m_defValue)); break;
   case Type::Toggle: SetValue(m_defValue != 0.f); break;
   default: break;
   }
}

void InGameUIItem::Save(Settings& settings, bool isTableOverride)
{
   switch (m_type)
   {
   case Type::FloatValue:
      m_initialValue = m_value;
      m_onSaveFloat(m_value, settings, isTableOverride);
      break;

   case Type::IntValue:
   case Type::EnumValue:
      m_initialValue = m_value;
      m_onSaveInt(static_cast<int>(m_value), settings, isTableOverride);
      break;

   case Type::Toggle:
      m_initialValue = m_value;
      m_onSaveBool(m_value != 0.f, settings, isTableOverride);
      break;

   default: break;
   }
}

void InGameUIItem::SetValue(float value)
{
   value = clamp(value, m_minValue, m_maxValue);
   value = m_minValue + static_cast<int>((value - m_minValue) / m_step) * m_step;
   if (m_value != value)
   {
      m_value = value;
      m_onChangeFloat(value);
   }
}

void InGameUIItem::SetValue(int value)
{
   value = clamp(value, m_minValue, m_maxValue);
   value = static_cast<int>(m_minValue + static_cast<int>((value - m_minValue) / m_step) * m_step);
   if (m_value != static_cast<float>(value))
   {
      m_value = static_cast<float>(value);
      m_onChangeInt(value);
   }
}

void InGameUIItem::SetValue(bool value)
{
   float newValue = value ? 1.f : 0.f;
   if (m_value != newValue)
   {
      m_value = newValue;
      m_onChangeBool(value);
   }
}

};