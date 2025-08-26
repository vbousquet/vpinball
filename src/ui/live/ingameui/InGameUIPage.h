// license:GPLv3+

#pragma once

#include "InGameUIItem.h"

namespace VPX::InGameUI
{

class InGameUIPage
{
public:
   InGameUIPage(const string &path, const string &title, const string &info);
   
   Settings& GetSettings();

   void ClearItems();
   void AddItem(std::unique_ptr<InGameUIItem>& item);
   
   virtual void Open();
   virtual void Close();
   virtual void Save();
   virtual void ResetToInitialValues();
   virtual void ResetToDefaults();
   virtual void Render();
   void SelectNextItem();
   void SelectPrevItem();
   void AdjustItem(float direction, bool isInitialPress);
   bool IsAdjustable() const;
   bool IsDefaults() const;
   bool IsModified() const;

   const string &GetPath() const { return m_path; }

protected:
   Player* const m_player;
   int m_selectedItem = -1;

private:
   const string m_path;
   const string m_title;
   const string m_info;
   bool m_useFlipperNav = false;
   float m_adjustedValue = 0.f;
   uint32_t m_lastUpdateMs = 0;
   uint32_t m_pressStartMs = 0;
   vector<std::unique_ptr<InGameUIItem>> m_items;
};

};