// license:GPLv3+

#include "core/stdafx.h"
#include "PropertiesPanel.h"
#include "IconsLucide.h"
#include "EditorTheme.h"

#include "audio/AudioPlayer.h"
#include "core/editablereg.h"
#include "core/player.h"
#include "core/SettingsService.h"
#include "parts/Collection.h"
#include "parts/Material.h"
#include "parts/Sound.h"
#include "parts/bumper.h"
#include "parts/decal.h"
#include "parts/flipper.h"
#include "parts/gate.h"
#include "parts/hittarget.h"
#include "parts/kicker.h"
#include "parts/plunger.h"
#include "parts/primitive.h"
#include "parts/ramp.h"
#include "parts/rubber.h"
#include "parts/spinner.h"
#include "parts/surface.h"
#include "parts/trigger.h"
#include "physics/PhysicsEngine.h"
#include "renderer/Texture.h"
#include "ui/live/EditorUI.h"
#include "ui/live/LiveUI.h"
#include "utils/color.h"

#include "PropertyPane.h"

namespace VPX::EditorUI
{

// Curation rules: which fields of a type make its Look and Transform sections. A field is Look when the pane registered it under
// PropertyPane::Tier::Common, or when its group (section or separator) or label is listed below; the group lists are the pin
// quick sets (see-through, bulb, dome). Everything stays in the full sheet under "Advanced".
struct CurationRules
{
   std::initializer_list<const char *> lookGroups;
   std::initializer_list<const char *> lookLabels;
   std::initializer_list<const char *> transformLabels;
   std::initializer_list<const char *> tracedInsertHiddenGroups; // Groups left out of the Look of a traced insert (fields that do not apply to it)
};

static const CurationRules *GetCurationRules(PinKind kind, int itemType)
{
   static const CurationRules primitive { { "Refractions", "Traced Insert Bulb", "Flasher Dome" }, { "Visible", "Material", "Image", "Reflection Strength", "Modulate Opacity", "Modulate Color" },
      { "Position", "Orientation", "Scale" }, { "Refractions" } }; // A traced insert has no refraction: its block is all zeros
   // The light pane marks its Light Settings section Common (PropertyPane::SetTier)
   static const CurationRules light { {}, { "Type", "State", "Show Bulb", "Transmit", "Modulate" }, { "Center", "Position", "Height", "Surface" } };
   static const CurationRules flasher { {}, { "Visible", "Color", "Image A", "Image B", "Mix", "Mix Factor (%)", "Opacity (%)", "Additive Blend", "Modulate" }, { "Position", "Rotation" } };
   static const CurationRules material { { "See-Through Surface" },
      { "Type", "Color", "Metallic", "Roughness", "Occlusion", "Emission Intensity", "Clear Coat", "Clear Coat Roughness", "Edge Brightness", "Enable Transparency", "Opacity", "Edge Opacity",
         "Thickness" },
      {} };
   static const CurationRules table { { "Environment Lighting" },
      { "Light Em. Scale", "Light Height", "Light Range", "Scene Lighting Scale", "Bloom Strength", "Tonemapping", "Exposure", "Enable Ambient Occlusion", "Ambient Occlusion Scale",
         "Enable Sc. Sp. Reflections", "Screen Space Reflection Scale" },
      {} };
   switch (kind)
   {
   case PinKind::Table: return &table;
   case PinKind::Material: return &material;
   case PinKind::Part:
      switch (itemType)
      {
      case eItemPrimitive: return &primitive;
      case eItemLight: return &light;
      case eItemFlasher: return &flasher;
      default: return nullptr;
      }
   }
   return nullptr;
}

static bool Listed(std::initializer_list<const char *> list, const string &value)
{
   return std::ranges::any_of(list, [&value](const char *item) { return value == item; });
}

// The "Apply to all traced inserts" button at the end of the Look of a traced insert (the full sheet has it under the bulb fields)
static void DrawApplyBulbButton(PropertyPane &props, PinKind kind, void *obj)
{
   if (kind != PinKind::Part || props.IsMultiEdit() || obj == nullptr)
      return;
   IEditable *const part = static_cast<IEditable *>(obj);
   if (part->GetItemType() == eItemPrimitive && static_cast<const Primitive *>(part)->IsTracedInsert())
      props.ApplyBulbToAllButton(part);
}

string PropertiesPanel::PartMaterial(const IEditable *part)
{
   switch (part->GetItemType())
   {
   case eItemPrimitive: return static_cast<const Primitive *>(part)->m_d.m_szMaterial;
   case eItemRamp: return static_cast<const Ramp *>(part)->m_d.m_szMaterial;
   case eItemSurface: return static_cast<const Surface *>(part)->m_d.m_szTopMaterial;
   case eItemDecal: return static_cast<const Decal *>(part)->m_d.m_szMaterial;
   case eItemFlipper: return static_cast<const Flipper *>(part)->m_d.m_szMaterial;
   case eItemHitTarget: return static_cast<const HitTarget *>(part)->m_d.m_szMaterial;
   case eItemPlunger: return static_cast<const Plunger *>(part)->m_d.m_szMaterial;
   case eItemRubber: return static_cast<const Rubber *>(part)->m_d.m_szMaterial;
   case eItemSpinner: return static_cast<const Spinner *>(part)->m_d.m_szMaterial;
   case eItemKicker: return static_cast<const Kicker *>(part)->m_d.m_szMaterial;
   case eItemTrigger: return static_cast<const Trigger *>(part)->m_d.m_szMaterial;
   case eItemGate: return static_cast<const Gate *>(part)->m_d.m_szMaterial;
   case eItemBumper: return static_cast<const Bumper *>(part)->m_d.m_szCapMaterial;
   default: return string();
   }
}

void PropertiesPanel::PrepareCuration()
{
   EditorUI &editor = m_editor;
   PinKind kind = PinKind::Table;
   string name;
   int itemType = -1;
   string partMaterial;
   switch (editor.m_selection.GetType())
   {
   case Selection::S_NONE: break;
   case Selection::S_MATERIAL:
      kind = PinKind::Material;
      name = editor.m_selection.GetMaterial()->m_name;
      break;
   case Selection::S_EDITABLE:
      kind = PinKind::Part;
      name = editor.m_selection.GetPart()->GetEditable()->GetName();
      itemType = editor.m_selection.GetPart()->GetEditable()->GetItemType();
      partMaterial = PartMaterial(editor.m_selection.GetPart()->GetEditable());
      break;
   default:
      m_curation.active = false;
      m_materialCuration.active = false;
      return;
   }
   // Multi selections keep the full sheet (fields of other types, mixed values)
   const bool single = editor.m_multiSel.size() <= 1;
   m_curation.tabbed = single;
   PrepareCurationFor(kind, name, itemType, single, m_curation);
   m_hasMaterialTab = single && kind == PinKind::Part && !partMaterial.empty() && editor.m_table->GetMaterial(partMaterial) != nullptr;
   // Tab memory per object type; a search palette jump goes to the tab holding the field
   if (m_curation.active && m_curation.tabbed)
   {
      if (const auto it = m_tabMemory.find(TabKey()); it != m_tabMemory.end())
         m_tab = it->second;
      else
         m_tab = HasTab(Tab::Look) ? Tab::Look : Tab::Object; // First visit of this type: its look
      if (PropertyPane::IsFocusPending() && PropertyPane::GetFocusKey().kind == kind && PropertyPane::GetFocusKey().name == name)
      {
         m_tab = Tab::Advanced;
         for (int t = 0; t < static_cast<int>(Tab::Count); t++)
            if (std::ranges::any_of(m_curation.tabs[t], [](const PinnedPanel::Def *def) { return def->key == PropertyPane::GetFocusKey(); }))
            {
               m_tab = static_cast<Tab>(t);
               break;
            }
         m_tabMemory[TabKey()] = m_tab;
      }
      if (!HasTab(m_tab))
         m_tab = HasTab(Tab::Look) ? Tab::Look : HasTab(Tab::Object) ? Tab::Object : Tab::Advanced;
   }
   const bool hasMaterial = single && !partMaterial.empty() && editor.m_table->GetMaterial(partMaterial) != nullptr;
   PrepareCurationFor(PinKind::Material, partMaterial, -1, hasMaterial, m_materialCuration);
}

bool PropertiesPanel::IsSelectionAlive(const Selection &sel) const
{
   const EditorUI &editor = m_editor;
   const PinTable *const table = editor.m_table;
   switch (sel.GetType())
   {
   case Selection::S_EDITABLE: return std::ranges::find(editor.m_editables, sel.GetPart()) != editor.m_editables.end();
   case Selection::S_MATERIAL: return std::ranges::find(table->m_materials, sel.GetMaterial()) != table->m_materials.end();
   case Selection::S_IMAGE: return std::ranges::find(table->m_vimage, sel.GetImage()) != table->m_vimage.end();
   case Selection::S_SOUND: return std::ranges::find(table->m_vsound, sel.GetSound()) != table->m_vsound.end();
   case Selection::S_RENDERPROBE: return std::ranges::find(table->m_vrenderprobe, sel.GetProbe()) != table->m_vrenderprobe.end();
   case Selection::S_COLLECTION: return std::ranges::find(table->GetCollections(), sel.GetCollection()) != table->GetCollections().end();
   default: return true;
   }
}

int PropertiesPanel::TabKey() const
{
   return m_curation.kind == PinKind::Table ? -1 : m_curation.kind == PinKind::Material ? -2 : static_cast<IEditable *>(m_curation.obj)->GetItemType();
}

bool PropertiesPanel::HasTab(Tab tab) const
{
   switch (tab)
   {
   case Tab::Advanced: return true;
   case Tab::Object:
      // Skipped when there is nothing to show (e.g. a light has no object settings and no material)
      return !m_curation.tabs[static_cast<int>(Tab::Object)].empty()
         || (m_curation.kind == PinKind::Part && m_curation.obj != nullptr && !PartMaterial(static_cast<const IEditable *>(m_curation.obj)).empty());
   case Tab::Material: return m_hasMaterialTab;
   default: return !m_curation.tabs[static_cast<int>(tab)].empty();
   }
}

void PropertiesPanel::DrawTabStrip()
{
   static constexpr const char *icons[] = { ICON_LC_BOX, ICON_LC_MOVE_3D, ICON_LC_CIRCLE_DOT, ICON_LC_SUN, ICON_LC_ATOM, ICON_LC_CODE, ICON_LC_SLIDERS_HORIZONTAL };
   static constexpr const char *names[] = { "Object", "Transform", "Material", "Look", "Physics", "Script & Events", "Advanced (every field)" };
   const float dpi = m_editor.m_liveUI.GetDPI();
   const float stripWidth = 32.f * dpi;
   const ImVec2 size(28.f * dpi, 28.f * dpi);
   // Strip background is the child's (ChildBg); 1 px border on its right edge
   {
      const ImVec2 pos = ImGui::GetWindowPos();
      const ImVec2 winSize = ImGui::GetWindowSize();
      ImGui::GetWindowDrawList()->AddLine(ImVec2(pos.x + winSize.x - 1.f, pos.y), ImVec2(pos.x + winSize.x - 1.f, pos.y + winSize.y), EditorTheme::Border);
   }
   ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
   ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 4.f * dpi));
   ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::V(EditorTheme::Hover));
   ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::V(EditorTheme::Hover));
   const auto tabButton = [&](const char *label, bool active) -> bool
   {
      ImGui::SetCursorPosX((stripWidth - size.x) * 0.5f);
      ImGui::PushStyleColor(ImGuiCol_Button, active ? EditorTheme::V(EditorTheme::Raised) : ImVec4(0.f, 0.f, 0.f, 0.f));
      ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::V(active ? EditorTheme::Accent : EditorTheme::TextDim));
      const bool clicked = ImGui::Button(label, size);
      ImGui::PopStyleColor(2);
      return clicked;
   };
   ImGui::SetCursorPosY(4.f * dpi);
   // Pin ID: lock the editor on the current object
   if (tabButton(ICON_LC_PIN "##PinID", m_locked))
   {
      m_locked = !m_locked;
      m_lockedSel = m_editor.m_selection;
   }
   ImGui::SetItemTooltip(m_locked ? "Unpin: follow the selection again" : "Pin: keep showing this object while selecting others");
   ImGui::Dummy(ImVec2(0.f, 2.f * dpi)); // 10 px between the pin and the tabs (with the item spacings)
   for (int t = 0; t < static_cast<int>(Tab::Count); t++)
   {
      if (!HasTab(static_cast<Tab>(t)))
         continue;
      const bool active = m_tab == static_cast<Tab>(t) && !IsSearching(); // A search spans every tab: none is highlighted
      ImGui::PushID(t);
      if (tabButton(icons[t], active))
      {
         m_search.clear(); // Picking a tab leaves the search
         m_searchTerms.clear();
         m_tab = static_cast<Tab>(t);
         m_tabMemory[TabKey()] = m_tab;
      }
      ImGui::PopID();
      ImGui::SetItemTooltip("%s", names[t]);
   }
   ImGui::PopStyleColor(2);
   ImGui::PopStyleVar(2);
}

void PropertiesPanel::DrawTabContent(PropertyPane &props)
{
   EditorUI &editor = m_editor;
   const bool tabbed = m_curation.active && m_curation.tabbed;
   // Object: the part's material chip (a click selects the material) is the first row of the Object section, drawn below
   string objectMaterial;
   if ((!tabbed || m_tab == Tab::Object) && !IsSearching() && editor.m_selection.GetType() == Selection::S_EDITABLE && !props.IsMultiEdit())
      objectMaterial = PartMaterial(props.GetEditedPart(editor.m_selection.GetPart()->GetEditable()));
   if (tabbed && IsSearching())
   {
      const int shown = DrawSearchResults(props, m_curation, "");
      if (shown == 0 && CountSearchMatches(m_materialCuration) == 0)
      {
         ImGui::Indent(ImGui::GetStyle().FramePadding.x * 2.f);
         ImGui::TextDisabled("No fields match \"%s\"", m_search.c_str());
         ImGui::Unindent(ImGui::GetStyle().FramePadding.x * 2.f);
      }
      props.SkipRest(true);
      return;
   }
   if (!tabbed)
   {
      if (!objectMaterial.empty())
      {
         props.PropertyLabel("Material"s);
         if (ImGui::Button((objectMaterial + "  " ICON_LC_ARROW_RIGHT "###MaterialChip").c_str(), ImVec2(ImGui::CalcItemWidth(), 0.f)))
            m_pendingMaterialSel = editor.m_table->GetMaterial(objectMaterial);
         ImGui::SetItemTooltip("Select this material");
         props.FlushRow();
      }
      return;
   }
   static constexpr const char *titles[] = { "Object", "Transform", "Material", "Look", "Physics", "Script & Events", "Advanced" };
   const vector<const PinnedPanel::Def *> &defs = m_curation.tabs[static_cast<int>(m_tab)];
   if (!defs.empty() || !objectMaterial.empty())
   {
      const int modifiedBefore = props.GetModifiedField();
      ImGui::PushID("##Tab"); // The tab rows draw the same fields as the full sheet: keep their IDs apart
      if (props.BeginSection(titles[static_cast<int>(m_tab)]))
      {
         props.SetLookSliders(true);
         if (!objectMaterial.empty())
         {
            props.PropertyLabel("Material"s);
            if (ImGui::Button((objectMaterial + "  " ICON_LC_ARROW_RIGHT "###MaterialChip").c_str(), ImVec2(ImGui::CalcItemWidth(), 0.f)))
               m_pendingMaterialSel = editor.m_table->GetMaterial(objectMaterial);
            ImGui::SetItemTooltip("Select this material");
            props.FlushRow();
         }
         // Several field groups on one tab (Transform: "Position, Rotation & Size" and "Additional Transform"): each gets its sub-header
         const bool multiGroup = m_tab == Tab::Transform && std::ranges::any_of(defs, [&defs](const PinnedPanel::Def *def) { return def->group != defs.front()->group; });
         string lastGroup;
         for (const PinnedPanel::Def *def : defs)
         {
            if (multiGroup && !def->group.empty() && def->group != lastGroup && def->group != titles[static_cast<int>(m_tab)])
               props.Separator(def->group);
            lastGroup = def->group;
            def->draw(props, m_curation.obj);
         }
         if (m_tab == Tab::Look)
            DrawApplyBulbButton(props, m_curation.kind, m_curation.obj);
         props.EndSection();
      }
      ImGui::PopID();
      if (props.GetModifiedField() != modifiedBefore)
         m_curation.reharvest = true;
   }
   props.SkipRest(m_tab != Tab::Advanced);
}

void PropertiesPanel::PrepareCurationFor(PinKind kind, const string &name, int itemType, bool enabled, Curation &cur)
{
   EditorUI &editor = m_editor;
   const CurationRules *const rules = enabled ? GetCurationRules(kind, itemType) : nullptr;
   if (!enabled || (rules == nullptr && !cur.tabbed))
   {
      cur.active = false;
      return;
   }
   // Harvest again after an edit, and every 2 s for changes made elsewhere (undo, script, other panes) that can show or hide
   // fields (material type, render mode,...); never while a field is being dragged or typed in
   const double now = ImGui::GetTime();
   if ((cur.reharvest || now - cur.harvestTime > 2.0) && !ImGui::IsAnyItemActive())
   {
      cur.reharvest = false;
      cur.harvestTime = now;
      editor.m_pinned.InvalidateHarvest(kind, name);
   }
   void *obj = nullptr;
   const unsigned stamp = editor.m_pinned.EnsureHarvested(kind, name, &obj);
   cur.active = stamp != 0 && obj != nullptr;
   if (!cur.active)
      return;
   if (stamp == cur.stamp && kind == cur.kind && name == cur.name && obj == cur.obj)
      return;
   cur.kind = kind;
   cur.name = name;
   cur.obj = obj;
   cur.stamp = stamp;
   editor.m_pinned.CollectDefs(kind, name, stamp, m_defScratch);
   cur.look.clear();
   cur.transform.clear();
   cur.all = m_defScratch;
   // The insert bulb and dome groups are only Look on the primitives they apply to (the full sheet keeps them for every primitive)
   const Primitive *const prim = (kind == PinKind::Part && itemType == eItemPrimitive) ? static_cast<const Primitive *>(static_cast<IEditable *>(obj)) : nullptr;
   const auto offLook = [prim, rules](const PinnedPanel::Def *def)
   {
      return prim != nullptr
         && ((def->group == "Traced Insert Bulb" && !prim->IsTracedInsert()) || (def->group == "Flasher Dome" && !prim->IsFlasherDome())
            || (rules != nullptr && prim->IsTracedInsert() && Listed(rules->tracedInsertHiddenGroups, def->group)));
   };
   // Types without curation rules: the visual sections make their Look tab
   static constexpr const char *visualSections[] = { "Visuals", "Render Mode", "Light Settings", "Bulb", "Transparency", "Ball Reflections & Shadows" };
   const auto isLook = [&](const PinnedPanel::Def *def)
   { return (rules == nullptr && std::ranges::any_of(visualSections, [def](const char *section) { return def->section == section; })) || def->common || Listed(rules->lookGroups, def->group) || Listed(rules->lookLabels, def->key.prop); };
   const auto isTransform = [&](const PinnedPanel::Def *def)
   { return def->section == "Position" || def->section == "Center" || (rules != nullptr && Listed(rules->transformLabels, def->key.prop)); };
   for (auto &list : cur.tabs)
      list.clear();
   for (const PinnedPanel::Def *def : m_defScratch)
   {
      // Tabs: one domain per field (fields of no domain are only in Advanced, like everything else)
      Tab tab = Tab::Advanced;
      if (def->key.prop == "Visible" || def->key.prop == "Static Rendering" || def->key.prop == "Reflection Enabled" || def->key.prop == "Show in Editor")
         tab = Tab::Object;
      else if (def->section.find("Physics") != string::npos)
         tab = Tab::Physics;
      else if (def->section == "Timer" || def->section == "States" || def->section == "Transient state" || def->key.prop == "State Link"
         || def->key.prop.find("Hit Event") != string::npos)
         tab = Tab::Script;
      else if (isTransform(def))
         tab = Tab::Transform;
      else if (!offLook(def) && isLook(def))
         tab = Tab::Look;
      if (tab != Tab::Advanced)
         cur.tabs[static_cast<int>(tab)].push_back(def);
      // Sections (the part's material pane)
      if (offLook(def))
         continue;
      if (rules != nullptr && isLook(def))
         cur.look.push_back(def);
      else if (rules != nullptr && Listed(rules->transformLabels, def->key.prop))
         cur.transform.push_back(def);
   }
   cur.advancedCount = static_cast<int>(m_defScratch.size() - cur.look.size() - cur.transform.size());
   cur.advancedLabel = "Advanced (" + std::to_string(cur.advancedCount) + " more)###Advanced";
}

void PropertiesPanel::DrawCuration(PropertyPane &props, Curation &cur)
{
   EditorUI &editor = m_editor;
   // Header material chip of a part: its material, a click selects it (its fields are also drawn under the part, see PartMaterialPane)
   if (&cur == &m_curation && editor.m_selection.GetType() == Selection::S_EDITABLE && !props.IsMultiEdit())
   {
      const string material = PartMaterial(props.GetEditedPart(editor.m_selection.GetPart()->GetEditable()));
      if (!material.empty())
      {
         props.PropertyLabel("Material"s);
         if (ImGui::Button((material + "  " ICON_LC_ARROW_RIGHT "###MaterialChip").c_str(), ImVec2(ImGui::CalcItemWidth(), 0.f)))
            m_pendingMaterialSel = editor.m_table->GetMaterial(material);
         ImGui::SetItemTooltip("Select this material");
         props.FlushRow();
      }
   }
   if (!cur.active)
      return;
   const int modifiedBefore = props.GetModifiedField();
   ImGui::PushID("##Curated"); // The curated rows draw the same fields as the full sheet below: keep their IDs apart
   const auto drawDefs = [&props, &cur](const vector<const PinnedPanel::Def *> &defs)
   {
      props.SetLookSliders(true);
      for (const PinnedPanel::Def *def : defs)
         def->draw(props, cur.obj);
   };
   if (!cur.look.empty() && props.BeginSection("Look"s))
   {
      drawDefs(cur.look);
      DrawApplyBulbButton(props, cur.kind, cur.obj);
      props.EndSection();
   }
   if (!cur.transform.empty() && props.BeginSection("Transform"s))
   {
      drawDefs(cur.transform);
      props.EndSection();
   }
   ImGui::PopID();
   if (props.GetModifiedField() != modifiedBefore)
      cur.reharvest = true;
   ImGui::Spacing();
   // Search palette focus on a field of this object: still pending here means it is not curated, so open the full sheet
   if (PropertyPane::IsFocusPending() && PropertyPane::GetFocusKey().kind == cur.kind && PropertyPane::GetFocusKey().name == cur.name)
      ImGui::SetNextItemOpen(true);
   const bool open = PropertyPane::SectionHeader(cur.advancedLabel.c_str());
   ImGui::SetItemTooltip("Every field of this object, in the full sheet layout");
   props.SkipRest(!open);
   if (open)
   {
      ImGui::Indent();
      cur.advancedIndent = true;
   }
}

void PropertiesPanel::EndCuration(PropertyPane &props, Curation &cur)
{
   props.FlushRow();
   props.SkipRest(false);
   if (cur.advancedIndent)
   {
      ImGui::Unindent();
      cur.advancedIndent = false;
   }
   if (props.GetModifiedField() > 0)
      cur.reharvest = true;
}

void PropertiesPanel::ApplyUnits(PropertyPane &props) const
{
   switch (m_editor.m_units)
   {
   case EditorUI::Units::VPX: props.SetLengthUnit(PropertyPane::Unit::VPLength); break;
   case EditorUI::Units::Metric: props.SetLengthUnit(PropertyPane::Unit::Millimeters); break;
   case EditorUI::Units::Imperial: props.SetLengthUnit(PropertyPane::Unit::Inches); break;
   }
}

void PropertiesPanel::PartMaterialPane(bool showStartup, bool isLive)
{
   EditorUI &editor = m_editor;
   if (editor.m_selection.GetType() != Selection::S_EDITABLE || editor.m_multiSel.size() > 1)
      return;
   const string name = PartMaterial(editor.m_selection.GetPart()->GetEditable());
   Material *const material = name.empty() ? nullptr : editor.m_table->GetMaterial(name);
   if (material == nullptr)
      return;
   const bool searching = IsSearching();
   if (searching && (!m_materialCuration.active || CountSearchMatches(m_materialCuration) == 0))
      return; // Search: the material pane only shows when one of its fields matches
   // Parts sharing the material (refreshed twice a second, as assignments change from any pane)
   if (m_usersFor != material || ImGui::GetFrameCount() - m_usersFrame > 30)
   {
      m_users.clear();
      editor.m_table->ShowWhereMaterialUsed(m_users, material);
      std::erase_if(m_users, [&editor](const WhereUsedInfo &w) { return w.whereUsedObjectname == editor.m_table->m_name; });
      // One entry per part (a bumper or wall can use the material for several of its surfaces)
      for (size_t i = 0; i < m_users.size(); i++)
         for (size_t j = i + 1; j < m_users.size();)
            if (m_users[j].whereUsedObjectname == m_users[i].whereUsedObjectname)
            {
               m_users[i].whereUsedPropertyName += ", " + m_users[j].whereUsedPropertyName;
               m_users.erase(m_users.begin() + j);
            }
            else
               j++;
      m_usersFor = material;
      m_usersFrame = ImGui::GetFrameCount();
   }

   ImGui::PushID("##PartMaterialPane"); // Same fields as the material's own pane: keep the IDs apart from the part's
   ImGui::Spacing();
   const bool open = PropertyPane::SectionHeader(("Material  " + name + "###PartMaterial").c_str(), ImGuiTreeNodeFlags_DefaultOpen);
   ImGui::SetItemTooltip("The material of this part, edited in place");
   if (open)
   {
      const int users = static_cast<int>(m_users.size());
      if (users > 1)
         ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f), ICON_LC_TRIANGLE_ALERT " Shared: used by %d parts", users);
      else
         ImGui::TextDisabled("Used by this part only");
      if (users > 1)
      {
         ImGui::SameLine();
         if (ImGui::SmallButton("List"))
            ImGui::OpenPopup("##MaterialUsers");
         ImGui::SetItemTooltip("Edits here change every part that uses this material");
      }
      if (ImGui::BeginPopup("##MaterialUsers"))
      {
         for (const WhereUsedInfo &where : m_users)
         {
            ImGui::PushID(&where);
            IEditable *const editable = editor.m_table->GetElementByName(where.whereUsedObjectname.c_str());
            if (ImGui::Selectable(where.whereUsedObjectname.c_str()) && editable != nullptr)
               if (const auto it = editor.m_editableMap.find(editable); it != editor.m_editableMap.end())
                  m_pendingPartSel = it->second;
            ImGui::SameLine();
            ImGui::TextDisabled("%s", where.whereUsedPropertyName.c_str());
            ImGui::PopID();
         }
         ImGui::EndPopup();
      }

      PropertyPane props(editor.m_table);
      ApplyUnits(props);
      props.SetPinning(&editor.m_pinned, PropertyPane::PinMode::Panel);
      if (editor.IsInspectMode())
         props.SetShowStartup(showStartup);
      props.SetAfterHeader(
         [this, searching](PropertyPane &p)
         {
            if (searching)
            {
               DrawSearchResults(p, m_materialCuration, "Material: ");
               p.SkipRest(true);
            }
            else
               DrawCuration(p, m_materialCuration);
         });
      m_inPartMaterialPane = true;
      MaterialEditUndo materialUndo(editor.m_undo, editor.m_table, editor.IsInspectMode() ? nullptr : material, MaterialEditUndo::SourcePartMaterial);
      MaterialProperties(props, material);
      m_inPartMaterialPane = false;
      EndCuration(props, m_materialCuration);
      if (materialUndo.Commit(props.GetModifiedField(), props.GetModifiedLabel()))
      {
         editor.m_lastUndoPart = nullptr; // The next part edit is a step of its own
         editor.m_lastUndoId = 0;
      }
      if (props.GetModifiedField() > 0)
      {
         // Same tracking as the material's own pane ('Live' tab) and the pinned overlay
         if (isLive)
            editor.m_liveEditedMaterials.insert(material);
         // Outside of an inspection session the edit is an undo step, which marks the table dirty (and clean again when undone)
      }
   }
   ImGui::PopID();
}

void PropertiesPanel::UpdateSearchTerms()
{
   if (m_search == m_searchTermsFor)
      return;
   m_searchTermsFor = m_search;
   m_searchTerms.clear();
   string lower = m_search;
   std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
   size_t pos = 0;
   while (pos < lower.size())
   {
      const size_t start = lower.find_first_not_of(" \t", pos);
      if (start == string::npos)
         break;
      const size_t end = lower.find_first_of(" \t", start);
      m_searchTerms.push_back(lower.substr(start, end == string::npos ? string::npos : end - start));
      pos = end == string::npos ? lower.size() : end;
   }
}

void PropertiesPanel::DrawSearchField()
{
   EditorUI &editor = m_editor;
   const ImGuiStyle &style = ImGui::GetStyle();
   const bool searchable = m_curation.active && m_curation.tabbed;
   ImGui::SameLine();
   ImGui::BeginDisabled(!searchable);
   ImGui::SetNextItemWidth(max(40.f, ImGui::GetContentRegionAvail().x));
   if (m_focusSearch)
   {
      if (searchable)
         ImGui::SetKeyboardFocusHere();
      m_focusSearch = false;
   }
   // Esc clears the text and leaves the field (ImGui alone would only restore the text the field had when it was entered)
   const bool escape = ImGui::GetActiveID() == ImGui::GetID("##PropSearch") && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
   ImGui::InputTextWithHint("##PropSearch", ICON_LC_SEARCH " Search fields", &m_search);
   if (escape)
   {
      m_search.clear();
      editor.m_areaEscPending = true; // The Esc is not an editor Esc
   }
   ImGui::SetItemTooltip(searchable ? "Filter the fields of all the tabs (Ctrl+F)" : "Search is available for a single selected part, material or the table");
   if (!m_search.empty())
   {
      const ImVec2 fieldMin = ImGui::GetItemRectMin();
      const ImVec2 fieldMax = ImGui::GetItemRectMax();
      const float side = fieldMax.y - fieldMin.y;
      ImGui::SetCursorScreenPos(ImVec2(fieldMax.x - side, fieldMin.y));
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
      ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::V(EditorTheme::TextDim));
      if (ImGui::Button(ICON_LC_X "##ClearPropSearch", ImVec2(side, side)))
         m_search.clear();
      ImGui::PopStyleColor(2);
      ImGui::SetItemTooltip("Clear");
   }
   ImGui::EndDisabled();
   (void)style;
   UpdateSearchTerms();
}

bool PropertiesPanel::MatchesSearch(const PinnedPanel::Def &def) const
{
   string text = def.label + ' ' + def.key.prop + ' ' + def.section + ' ' + def.group;
   std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
   return std::ranges::all_of(m_searchTerms, [&text](const string &term) { return text.find(term) != string::npos; });
}

int PropertiesPanel::CountSearchMatches(const Curation &cur) const
{
   if (!cur.active || m_searchTerms.empty())
      return 0;
   return static_cast<int>(std::ranges::count_if(cur.all, [this](const PinnedPanel::Def *def) { return MatchesSearch(*def); }));
}

int PropertiesPanel::DrawSearchResults(PropertyPane &props, Curation &cur, const char *titlePrefix)
{
   static constexpr const char *tabNames[] = { "Object", "Transform", "Material", "Look", "Physics", "Script & Events", "Advanced" };
   if (!cur.active)
      return 0;
   struct Group
   {
      string title;
      vector<const PinnedPanel::Def *> defs;
   };
   vector<Group> groups;
   int count = 0;
   for (const PinnedPanel::Def *def : cur.all)
   {
      if (!MatchesSearch(*def))
         continue;
      int tab = static_cast<int>(Tab::Advanced); // Fields of no domain live in Advanced
      for (int t = 0; t < static_cast<int>(Tab::Count); t++)
         if (std::ranges::find(cur.tabs[t], def) != cur.tabs[t].end())
         {
            tab = t;
            break;
         }
      const string title = string(titlePrefix) + tabNames[tab] + " / " + (def->section.empty() ? def->group : def->section);
      auto it = std::ranges::find_if(groups, [&title](const Group &g) { return g.title == title; });
      if (it == groups.end())
         it = groups.insert(groups.end(), Group { title, {} });
      it->defs.push_back(def);
      count++;
   }
   if (groups.empty())
      return 0;
   props.SetHighlight(&m_searchTerms);
   props.SetLookSliders(true);
   ImGui::PushID("##Search"); // The result rows draw the same fields as the tabs and the full sheet: keep their IDs apart
   for (const Group &group : groups)
      if (props.BeginSection(group.title))
      {
         for (const PinnedPanel::Def *def : group.defs)
            def->draw(props, cur.obj);
         props.EndSection();
      }
   ImGui::PopID();
   props.SetHighlight(nullptr);
   return count;
}

PropertiesPanel::PropertiesPanel(EditorUI &editor)
   : m_editor(editor)
{
}

PropertiesPanel::~PropertiesPanel() = default;

void PropertiesPanel::RenderInArea(const ImVec2 &pos, const ImVec2 &size)
{
   EditorUI &editor = m_editor;
   if (editor.m_table->IsLocked())
      return;

   ImGui::SetNextWindowPos(pos);
   ImGui::SetNextWindowSize(size);
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f * editor.m_liveUI.GetDPI(), 4.0f * editor.m_liveUI.GetDPI()));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
   ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
   // Pin ID: draw the locked object as if it were selected (the real selection is restored once the panel is drawn)
   // A search jump goes to its object: unpin (the pinned object would hide it)
   if (m_locked && PropertyPane::IsFocusPending())
      m_locked = false;
   const bool locked = m_locked && IsSelectionAlive(m_lockedSel);
   m_locked = locked;
   Selection realSel;
   vector<std::shared_ptr<EditorUIPart>> realMultiSel;
   vector<Texture *> realImages;
   vector<VPX::Sound *> realSounds;
   vector<Material *> realMaterials;
   bool swapped = false;
   if (locked && !(editor.m_selection == m_lockedSel))
   {
      swapped = true;
      realSel = editor.m_selection;
      realMultiSel = editor.m_multiSel;
      realImages = editor.m_multiSelImages;
      realSounds = editor.m_multiSelSounds;
      realMaterials = editor.m_multiSelMaterials;
      editor.SetSelection(m_lockedSel);
   }
   m_mouseOver = ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), false);
   PropertyPane::ConsumeEditCancel(); // A cancelled edit nobody handled last frame
   if (PropertyPane::IsFocusPending()) // A jump to a field (search palette) needs the normal tabs
   {
      m_search.clear();
      m_searchTerms.clear();
   }
   PrepareCuration(); // May harvest the selection's pane in a hidden window: before the panel window is begun
   ImGui::Begin("PROPERTIES", nullptr,
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus
         | ImGuiWindowFlags_NoNavFocus);

   // Area header (Blender's editor type switcher), above the tab strip and the content
   editor.RenderAreaTypeButton(EditorUI::AreaEditor::Properties);
   DrawSearchField();
   ImGui::Separator();

   // Multi-edit only applies to uniform selections: all selected parts of the same type as the active one
   const bool uniformPartSel = editor.m_selection.GetType() == Selection::S_EDITABLE && editor.m_multiSel.size() > 1
      && std::ranges::all_of(editor.m_multiSel,
         [&editor](const std::shared_ptr<EditorUIPart> &part) { return part->GetEditable()->GetItemType() == editor.m_selection.GetPart()->GetEditable()->GetItemType(); });

   if (editor.m_multiSel.size() > 1)
      ImGui::TextDisabled(uniformPartSel ? "%d parts selected" : "%d parts selected (editing the active one)", (int)editor.m_multiSel.size());
   if (editor.m_multiSelImages.size() > 1)
      ImGui::TextDisabled("%d images selected (editing the active one)", (int)editor.m_multiSelImages.size());
   if (editor.m_multiSelSounds.size() > 1)
      ImGui::TextDisabled("%d sounds selected (editing the active one)", (int)editor.m_multiSelSounds.size());
   if (editor.m_multiSelMaterials.size() > 1)
      ImGui::TextDisabled("%d materials selected (editing the active one)", (int)editor.m_multiSelMaterials.size());

   // Vertical tab strip (Blender's Properties editor) at the left, content at the right
   const bool tabbed = m_curation.active && m_curation.tabbed;
   if (tabbed)
   {
      ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::V(EditorTheme::Strip));
      ImGui::BeginChild("##PropTabs", ImVec2(32.f * editor.m_liveUI.GetDPI(), 0.f), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
      ImGui::PopStyleColor();
      DrawTabStrip();
      ImGui::EndChild();
      ImGui::SameLine(0.f, 4.f);
      ImGui::BeginChild("##PropContent", ImVec2(0.f, 0.f));
   }
   else
   {
      // Objects without tabs still offer the pin
      if (ImGui::SmallButton(m_locked ? ICON_LC_PIN " Pinned##PinID" : ICON_LC_PIN "##PinID"))
      {
         m_locked = !m_locked;
         m_lockedSel = editor.m_selection;
      }
      ImGui::SetItemTooltip(m_locked ? "Unpin: follow the selection again" : "Pin: keep showing this object while selecting others");
   }

   PropertyPane props(editor.m_table);
   props.SetAfterHeader([this](PropertyPane &p) { DrawTabContent(p); });
   // Fields register their definition for the pinned properties overlay and offer the pin context menu
   props.SetPinning(&editor.m_pinned, PropertyPane::PinMode::Panel, editor.m_selection.GetType() == Selection::S_EDITABLE ? editor.m_selection.GetPart().get() : nullptr);
   ApplyUnits(props);
   if (uniformPartSel)
   {
      vector<IEditable *> coParts;
      for (const std::shared_ptr<EditorUIPart> &part : editor.m_multiSel)
         if (part != editor.m_selection.GetPart())
            coParts.push_back(part->GetEditable());
      props.SetCoEditedParts(coParts);
   }
   if (editor.IsInspectMode() && editor.m_selection.GetType() != Selection::S_IMAGE
      && editor.m_selection.GetType() != Selection::S_SOUND) // Images & sounds are shared between live and startup instance, so they do not have 2 states
   {
      if (ImGui::BeginTabBar("Startup/Live", ImGuiTabBarFlags_NoCloseWithMiddleMouseButton))
      {
         for (int tab = 0; tab < 2; tab++)
         {
            const bool is_live = (tab == 1);
            if (ImGui::BeginTabItem(is_live ? "Live" : "Startup", nullptr, (is_live && m_selectLiveTab) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
            {
               if (is_live)
                  m_selectLiveTab = false;
               props.SetShowStartup(!is_live);
               switch (editor.m_selection.GetType())
               {
               case Selection::S_NONE: TableProperties(props); break;
               case Selection::S_EDITABLE:
                  editor.m_selection.GetPart()->UpdatePropertyPane(props);
                  if (props.GetModifiedField() > 0)
                     RefreshEditedParts(props);
                  break;
               case Selection::S_IMAGE: ImageProperties(props, editor.m_selection.GetImage()); break;
               case Selection::S_CAMERA: CameraProperties(props, editor.m_selection.GetCamera()); break;
               case Selection::S_MATERIAL: MaterialProperties(props, editor.m_selection.GetMaterial()); break;
               case Selection::S_RENDERPROBE: RenderProbeProperties(props, editor.m_selection.GetProbe()); break;
               case Selection::S_SOUND: SoundProperties(props, editor.m_selection.GetSound()); break;
               case Selection::S_COLLECTION: CollectionProperties(props, editor.m_selection.GetCollection()); break;
               }
               EndCuration(props, m_curation);
               if (!tabbed || m_tab == Tab::Material || IsSearching())
                  PartMaterialPane(!is_live, is_live);
               // Remember what was edited on the live copy: 'Save' copies it back to the startup table that is written to disk
               if (is_live && props.GetModifiedField() > 0)
               {
                  if (editor.m_selection.GetType() == Selection::S_MATERIAL)
                     editor.m_liveEditedMaterials.insert(editor.m_selection.GetMaterial());
                  else if (editor.m_selection.GetType() == Selection::S_EDITABLE)
                  {
                     if (props.IsMultiEdit())
                        for (const std::shared_ptr<EditorUIPart> &part : editor.m_multiSel)
                           editor.m_liveEditedParts.insert(part->GetEditable());
                     else
                        editor.m_liveEditedParts.insert(editor.m_selection.GetPart()->GetEditable());
                  }
               }
               ImGui::EndTabItem();
            }
         }
         ImGui::EndTabBar();
      }
   }
   else
   {
      switch (editor.m_selection.GetType())
      {
      case Selection::S_NONE: TableProperties(props); break;
      case Selection::S_EDITABLE:
      {
         IEditable *const editable = editor.m_selection.GetPart()->GetEditable();
         const string nameBefore = editable->GetName();
         editor.m_undo.BeginUndo();
         if (props.IsMultiEdit()) // Multi-edit applies the field changes to every selected part
         {
            for (const std::shared_ptr<EditorUIPart> &part : editor.m_multiSel)
               editor.m_undo.MarkForUndo(part->GetEditable());
         }
         else
            editor.m_undo.MarkForUndo(editable);
         // Alt held while editing: the fields also write to the other selected parts, snapshotted in the same record
         const bool areaActive = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
            || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsAnyItemActive());
         const bool altApply = areaActive && ImGui::GetIO().KeyAlt && !props.IsMultiEdit() && editor.m_multiSel.size() > 1
            && (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsAnyItemActive());
         if (altApply)
            for (const std::shared_ptr<EditorUIPart> &part : editor.m_multiSel)
               editor.m_undo.MarkForUndo(part->GetEditable());
         editor.m_pinned.ArmApplyToSelected(altApply);
         // The playfield mesh pane edits table fields, which the primitive's undo state does not hold
         if (editable->GetItemType() == eItemPrimitive && static_cast<const Primitive *>(editable)->IsPlayfield())
         {
            PinTable *const table = static_cast<const Primitive *>(editable)->m_ptable;
            editor.m_undo.MarkForAction(
               [table, material = table->m_playfieldMaterial, image = table->m_image, showInEditor = table->m_winEditorBackdrop, reflectionStrength = table->m_playfieldReflectionStrength]()
               {
                  table->m_playfieldMaterial = material;
                  table->m_image = image;
                  table->m_winEditorBackdrop = showInEditor;
                  table->m_playfieldReflectionStrength = reflectionStrength;
               },
               // Redo: the same four fields as they are when the step is undone
               [table]() -> std::function<void()>
               {
                  return [table, material = table->m_playfieldMaterial, image = table->m_image, showInEditor = table->m_winEditorBackdrop,
                            reflectionStrength = table->m_playfieldReflectionStrength]()
                  {
                     table->m_playfieldMaterial = material;
                     table->m_image = image;
                     table->m_winEditorBackdrop = showInEditor;
                     table->m_playfieldReflectionStrength = reflectionStrength;
                  };
               });
         }
         editor.m_selection.GetPart()->UpdatePropertyPane(props);
         editor.m_pinned.ArmApplyToSelected(false);
         EndCuration(props, m_curation);
         // A renamed surface or ramp retargets the parts placed on it, recorded in the same undo record
         editor.m_table->UpdateSurfaceReferences(editable, nameBefore, [&editor](IEditable *part) { editor.m_undo.MarkForUndo(part); });
         if (props.GetModifiedField() > 0)
            editor.m_undo.SetFieldLabel(props.GetModifiedLabel());
         editor.m_undo.EndUndo();
         {
            // The edit of one field (a drag, a typed value) is one step: its first frame keeps the record, the next ones discard theirs. Alt editing
            // (other parts in the record) has its own key, and the chain ends when the interaction does
            const bool modified = props.GetModifiedField() > 0;
            const unsigned int editId = (altApply ? 0x800000u : 0u) | 0x2000u | static_cast<unsigned int>(props.GetModifiedField());
            const bool continues = modified && editor.m_lastUndoPart == editable && editor.m_lastUndoId == editId;
            const auto endChain = [&editor]()
            {
               if (editor.m_lastUndoId != 0x1000) // The gizmo's own marker is not ours to clear
               {
                  editor.m_lastUndoPart = nullptr;
                  editor.m_lastUndoId = 0;
               }
            };
            if (!modified && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsAnyItemActive())
               endChain();
            if (modified && !continues)
            {
               editor.m_lastUndoPart = editable;
               editor.m_lastUndoId = editId;
               m_editRecordId = editor.m_undo.TopRecordId();
            }
            else
            {
               editor.m_undo.Discard();
               // Esc cancelled the drag / typed value (restored): the step it had started is dropped too, if it is still the latest record
               if (continues && PropertyPane::ConsumeEditCancel() && editor.m_undo.HasUndo() && editor.m_undo.TopRecordId() == m_editRecordId)
               {
                  editor.m_undo.Discard();
                  endChain();
               }
            }
            // Another item took the interaction (Tab between components, a click on another field): a new edit
            const ImGuiID activeId = ImGui::GetActiveID();
            if (activeId != m_prevActiveId)
               endChain();
            m_prevActiveId = activeId;
         }
         if (props.GetModifiedField() > 0)
            RefreshEditedParts(props);
         break;
      }
      case Selection::S_IMAGE: ImageProperties(props, editor.m_selection.GetImage()); break;
      case Selection::S_CAMERA: CameraProperties(props, editor.m_selection.GetCamera()); break;
      case Selection::S_MATERIAL:
      {
         // Material edits are undoable: snapshot before the pane is drawn, recorded when a field was modified
         MaterialEditUndo materialUndo(editor.m_undo, editor.m_table, editor.m_selection.GetMaterial(), MaterialEditUndo::SourceProperties);
         MaterialProperties(props, editor.m_selection.GetMaterial());
         if (materialUndo.Commit(props.GetModifiedField(), props.GetModifiedLabel()))
         {
            editor.m_lastUndoPart = nullptr; // The next part edit is a step of its own
            editor.m_lastUndoId = 0;
         }
         break;
      }
      case Selection::S_RENDERPROBE: RenderProbeProperties(props, editor.m_selection.GetProbe()); break;
      case Selection::S_SOUND: SoundProperties(props, editor.m_selection.GetSound()); break;
      case Selection::S_COLLECTION: CollectionProperties(props, editor.m_selection.GetCollection()); break;
      }
      if (editor.m_selection.GetType() != Selection::S_EDITABLE)
         EndCuration(props, m_curation);
      else if (!tabbed || m_tab == Tab::Material || IsSearching())
         PartMaterialPane(false, false);
   }
   RenderPopups(); // In the content child when tabbed: the pane code opens its popups there
   if (tabbed)
      ImGui::EndChild();

   ImGui::End();
   ImGui::PopStyleVar(3);
   // Pin ID: back to the real selection, unless the panel itself changed the selection
   if (swapped && editor.m_selection == m_lockedSel)
   {
      editor.m_selection = realSel;
      editor.m_multiSel = std::move(realMultiSel);
      editor.m_multiSelImages = std::move(realImages);
      editor.m_multiSelSounds = std::move(realSounds);
      editor.m_multiSelMaterials = std::move(realMaterials);
   }
   if (m_pendingMaterialSel != nullptr)
   {
      editor.SetSelection(Selection(m_pendingMaterialSel));
      m_pendingMaterialSel = nullptr;
   }
   if (m_pendingPartSel != nullptr)
   {
      editor.SetSelection(Selection(m_pendingPartSel));
      m_pendingPartSel = nullptr;
   }
}

void PropertiesPanel::RefreshEditedParts(PropertyPane &props)
{
   EditorUI &editor = m_editor;
   const auto refresh = [&editor](const std::shared_ptr<EditorUIPart> &part)
   {
      editor.m_renderer->ReinitRenderable(part->GetEditable()->GetIRenderable());
      editor.m_player->m_physics->Update(part->GetEditable());
      // Multi-edit writes the co-edited parts' visibility fields directly: resync their editor overrides
      // (the active part's overrides are updated by the pane's own setters, so it must be excluded)
      if (part != editor.m_selection.GetPart())
         part->SyncVisibilityOverrides();
   };
   if (props.IsMultiEdit())
      for (const std::shared_ptr<EditorUIPart> &part : editor.m_multiSel)
         refresh(part);
   else
      refresh(editor.m_selection.GetPart());
}

void PropertiesPanel::TableProperties(PropertyPane &props)
{
   EditorUI &editor = m_editor;
   PinTable *table = props.GetEditedPart<PinTable>(editor.m_table);
   props.Header("Table"s, [table]() { return table->GetName(); }, [table](const string &v) { table->SetName(v); });

   if (props.BeginSection("Visuals"s))
   {
      props.Separator("Playfield"s);
      props.MaterialCombo<PinTable>(
         table, "Material"s, //
         [](const PinTable *table) { return table->m_playfieldMaterial; }, //
         [](PinTable *table, const string &v) { table->m_playfieldMaterial = v; });
      props.ImageCombo<PinTable>(
         table, "Image"s, //
         [](const PinTable *table) { return table->m_image; }, //
         [](PinTable *table, const string &v) { table->m_image = v; });
      ImGui::BeginDisabled(table->m_image.empty());
      props.Checkbox<PinTable>(
         table, "Show in Editor"s, //
         [](const PinTable *table) { return table->m_winEditorBackdrop; }, //
         [](PinTable *table, bool v)
         {
            table->m_winEditorBackdrop = v;
            if (g_pplayer->m_implicitPlayfieldMesh)
               g_pplayer->m_implicitPlayfieldMesh->m_d.m_displayTexture = v;
         },
         "Draw the image on the playfield in the editor (does not change how it renders in game)");
      ImGui::EndDisabled();
      props.InputInt<PinTable>(
         table, "Reflection Strength"s, //
         [](const PinTable *table) { return table->GetPlayfieldReflectionStrength(); }, //
         [](PinTable *table, int v) { table->SetPlayfieldReflectionStrength(v); });

      props.Separator("Ball"s);
      props.ImageCombo<PinTable>(
         table, "Ball Image"s, //
         [](const PinTable *table) { return table->m_ballImage; }, //
         [](PinTable *table, const string &v) { table->m_ballImage = v; });
      props.Checkbox<PinTable>(
         table, "Spherical Map"s, //
         [](const PinTable *table) { return table->m_ballSphericalMapping; }, //
         [](PinTable *table, bool v) { table->m_ballSphericalMapping = v; }, "Wrap the image around the ball instead of projecting it flat");
      props.ImageCombo<PinTable>(
         table, "Ball Decal"s, //
         [](const PinTable *table) { return table->m_ballImageDecal; }, //
         [](PinTable *table, const string &v) { table->m_ballImageDecal = v; });
      props.Checkbox<PinTable>(
         table, "Logo Mode"s, //
         [](const PinTable *table) { return table->m_BallDecalMode; }, //
         [](PinTable *table, bool v) { table->m_BallDecalMode = v; }, "Draw the decal as a flat logo keeping its orientation instead of wrapping it on the ball");
      props.InputFloat<PinTable>(
         table, "Reflection of Playfield"s, //
         [](const PinTable *table) { return table->m_ballPlayfieldReflectionStrength; }, //
         [](PinTable *table, float v) { table->m_ballPlayfieldReflectionStrength = v; }, PropertyPane::Unit::None, 2, "Strength of the playfield's reflection on the ball");
      props.InputFloat<PinTable>(
         table, "Default Bulb Intensity Scale"s, //
         [](const PinTable *table) { return table->m_defaultBulbIntensityScaleOnBall; }, //
         [](PinTable *table, float v) { table->m_defaultBulbIntensityScaleOnBall = v; }, PropertyPane::Unit::None, 2, "Default scale of bulb light contribution on the ball");

      props.Separator("Backdrop"s);
      props.InputRGB<PinTable>(
         table, "Background Color"s, //
         [](const PinTable *table) { return convertColor(table->m_colorbackdrop); }, //
         [](PinTable *table, const vec3 &v) { table->m_colorbackdrop = convertColorRGB(v); });
      props.Checkbox<PinTable>(
         table, "Apply Night->Day cycle"s, //
         [](const PinTable *table) { return table->m_ImageBackdropNightDay; }, //
         [](PinTable *table, bool v) { table->m_ImageBackdropNightDay = v; }, "Tint the backdrop image according to the night/day setting");
      props.ImageCombo<PinTable>(
         table, "DT Image"s, //
         [](const PinTable *table) { return table->m_BG_image[0]; }, //
         [](PinTable *table, const string &v) { table->m_BG_image[0] = v; }, "Backdrop image in desktop view");
      props.ImageCombo<PinTable>(
         table, "FS Image"s, //
         [](const PinTable *table) { return table->m_BG_image[1]; }, //
         [](PinTable *table, const string &v) { table->m_BG_image[1] = v; }, "Backdrop image in fullscreen (cabinet) view");
      props.ImageCombo<PinTable>(
         table, "FSS Image"s, //
         [](const PinTable *table) { return table->m_BG_image[2]; }, //
         [](PinTable *table, const string &v) { table->m_BG_image[2] = v; }, "Backdrop image in full single screen view");
      props.ImageCombo<PinTable>(
         table, "Color Grading LUT"s, //
         [](const PinTable *table) { return table->m_imageColorGrade; }, //
         [](PinTable *table, const string &v) { table->m_imageColorGrade = v; }, "Lookup-table image applied to color grade the final render");
      props.Checkbox<PinTable>(
         table, "Enable EMReels"s, //
         [](const PinTable *table) { return table->m_renderEMReels; }, //
         [](PinTable *table, bool v) { table->m_renderEMReels = v; }, "Render the table's electromechanical reel displays");
      props.Checkbox<PinTable>(
         table, "Enable Decals"s, //
         [](const PinTable *table) { return table->m_renderDecals; }, //
         [](PinTable *table, bool v) { table->m_renderDecals = v; });
      props.Checkbox<PinTable>(
         table, "Enable FSS mode"s, //
         [](const PinTable *table) { return table->IsFSSEnabled(); }, //
         [](PinTable *table, bool v) { table->EnableFSS(v); }, "Full Single Screen mode: render the table and its backglass on a single display");
      props.EndSection();
   }

   if (props.BeginSection("Physics"s))
   {
      props.Separator("Physics Constants"s);
      props.InputFloat<PinTable>(
         table, "Gravity Constant"s, //
         [](const PinTable *table) { return table->GetGravity(); }, //
         [](PinTable *table, float v) { table->SetGravity(v); }, PropertyPane::Unit::None, 2);
      props.InputFloat<PinTable>(
         table, "Playfield Elasticity"s, //
         [](const PinTable *table) { return table->m_elasticity; }, //
         [](PinTable *table, float v) { table->m_elasticity = v; }, PropertyPane::Unit::None, 3);
      props.InputFloat<PinTable>(
         table, "Playfield Elasticity Falloff"s, //
         [](const PinTable *table) { return table->m_elasticityFalloff; }, //
         [](PinTable *table, float v) { table->m_elasticityFalloff = v; }, PropertyPane::Unit::None, 3, "Reduce elasticity for slower impacts (0 = constant elasticity)");
      props.InputFloat<PinTable>(
         table, "Playfield Friction"s, //
         [](const PinTable *table) { return table->m_friction; }, //
         [](PinTable *table, float v) { table->m_friction = max(v, 0.f); }, PropertyPane::Unit::None, 3);
      props.InputFloat<PinTable>(
         table, "Playfield Scatter Angle"s, //
         [](const PinTable *table) { return table->m_scatter; }, //
         [](PinTable *table, float v) { table->m_scatter = v; }, PropertyPane::Unit::Degree, 1, "Random angle added to the ball's bounce on the playfield");
      props.InputFloat<PinTable>(
         table, "Default Elements Scatter Angle"s, //
         [](const PinTable *table) { return table->m_defaultScatter; }, //
         [](PinTable *table, float v) { table->m_defaultScatter = v; }, PropertyPane::Unit::Degree, 1, "Scatter angle used by elements that don't define their own");
      props.Combo<PinTable>(
         table, "Overwrite Physics by Global Set"s, vector<string> { "Disable"s, "Set1"s, "Set2"s, "Set3"s, "Set4"s, "Set5"s, "Set6"s, "Set7"s, "Set8"s }, //
         [](const PinTable *table) { return table->m_overridePhysics; }, //
         [](PinTable *table, int v) { table->m_overridePhysics = v; }, "Apply a predefined global physics set to the whole table");
      props.Checkbox<PinTable>(
         table, "including Flipper Physics"s, //
         [](const PinTable *table) { return table->m_overridePhysicsFlipper; }, //
         [](PinTable *table, bool v) { table->m_overridePhysicsFlipper = v; }, "Also apply the physics set to flippers, which use their own physics values");
      props.InputFloat<PinTable>(
         table, "Nudge Time"s, //
         [](const PinTable *table) { return table->m_nudgeTime; }, //
         [](PinTable *table, float v) { table->m_nudgeTime = v; }, PropertyPane::Unit::None, 2, "Duration of the impulse applied to the table by a nudge");
      props.InputInt<PinTable>(
         table, "Physics Max. Loops (-1=def.)"s, //
         [](const PinTable *table) { return static_cast<int>(table->m_PhysicsMaxLoops); }, //
         [](PinTable *table, int v) { table->m_PhysicsMaxLoops = static_cast<unsigned int>(v); });

      props.Separator("Dimensions"s);
      props.InputFloat<PinTable>(
         table, "Playfield Width"s, //
         [](const PinTable *table) { return table->GetTableWidth(); }, //
         [](PinTable *table, float v) { table->SetTableWidth(v); }, PropertyPane::Unit::VPLength, 1);
      props.InputFloat<PinTable>(
         table, "Playfield Length"s, //
         [](const PinTable *table) { return table->GetHeight(); }, //
         [](PinTable *table, float v) { table->SetHeight(v); }, PropertyPane::Unit::VPLength, 1);
      props.InputFloat<PinTable>(
         table, "Top Glass Height"s, //
         [](const PinTable *table) { return table->m_glassTopHeight; }, //
         [](PinTable *table, float v) { table->m_glassTopHeight = v; }, PropertyPane::Unit::VPLength, 1);
      props.InputFloat<PinTable>(
         table, "Bottom Glass Height"s, //
         [](const PinTable *table) { return table->m_glassBottomHeight; }, //
         [](PinTable *table, float v) { table->m_glassBottomHeight = v; }, PropertyPane::Unit::VPLength, 1);
      props.InputFloat<PinTable>(
         table, "Lockbar Height"s, //
         [](const PinTable *table) { return table->m_groundToLockbarHeight; }, //
         [](PinTable *table, float v) { table->m_groundToLockbarHeight = v; }, PropertyPane::Unit::VPLength, 1);

      props.Separator("Difficulty"s);
      props.InputFloat<PinTable>(
         table, "Slope for Min. Difficulty"s, //
         [](const PinTable *table) { return table->m_angletiltMin; }, //
         [](PinTable *table, float v) { table->m_angletiltMin = v; }, PropertyPane::Unit::Degree, 2, "Table incline at minimum game difficulty");
      props.InputFloat<PinTable>(
         table, "Slope for Max. Difficulty"s, //
         [](const PinTable *table) { return table->m_angletiltMax; }, //
         [](PinTable *table, float v) { table->m_angletiltMax = v; }, PropertyPane::Unit::Degree, 2, "Table incline at maximum game difficulty");
      props.InputFloat<PinTable>(
         table, "Game Difficulty"s, //
         [](const PinTable *table) { return table->m_difficulty; }, //
         [](PinTable *table, float v) { table->m_difficulty = v; }, PropertyPane::Unit::Percent, 0, "Interpolate the table's difficulty settings between the min & max slopes");
      props.EndSection();
   }

   if (props.BeginSection("Sound"s))
   {
      props.InputInt<PinTable>(
         table, "Sound Effect Volume"s, //
         [](const PinTable *table) { return table->GetTableSoundVolume(); }, //
         [](PinTable *table, int v) { table->SetTableSoundVolume(v); });
      props.InputInt<PinTable>(
         table, "Music Volume"s, //
         [](const PinTable *table) { return table->GetTableMusicVolume(); }, //
         [](PinTable *table, int v) { table->SetTableMusicVolume(v); });
      props.EndSection();
   }

   if (props.BeginSection("Lighting"s))
   {
      props.InputRGB<PinTable>(
         table, "Ambient Color", //
         [](const PinTable *table) { return convertColor(table->m_lightAmbient); }, //
         [&editor](PinTable *table, const vec3 &v)
         {
            table->m_lightAmbient = convertColorRGB(v);
            editor.m_renderer->MarkShaderDirty(); // Needed to update shaders with new light settings
         });

      props.InputRGB<PinTable>(
         table, "Light Emission Color", //
         [](const PinTable *table) { return convertColor(table->m_Light[0].emission); }, //
         [&editor](PinTable *table, const vec3 &v)
         {
            table->m_Light[0].emission = convertColorRGB(v);
            editor.m_renderer->MarkShaderDirty(); // Needed to update shaders with new light settings
         });
      props.InputFloat<PinTable>(
         table, "Light Emission Scale"s, //
         [](const PinTable *table) { return table->m_lightEmissionScale; }, //
         [&editor](PinTable *table, float v)
         {
            table->m_lightEmissionScale = v;
            editor.m_renderer->MarkShaderDirty(); // Needed to update shaders with new light settings
         },
         PropertyPane::Unit::None, 0, "Intensity of the table's main light emission");
      props.InputFloat<PinTable>(
         table, "Light Height"s, //
         [](const PinTable *table) { return table->m_lightHeight; }, //
         [&editor](PinTable *table, float v)
         {
            table->m_lightHeight = v;
            editor.m_renderer->MarkShaderDirty(); // Needed to update shaders with new light settings
         },
         PropertyPane::Unit::VPLength, 1);
      props.InputFloat<PinTable>(
         table, "Light Range"s, //
         [](const PinTable *table) { return table->m_lightRange; }, //
         [&editor](PinTable *table, float v)
         {
            table->m_lightRange = v;
            editor.m_renderer->MarkShaderDirty(); // Needed to update shaders with new light settings
         },
         PropertyPane::Unit::VPLength, 1);
      props.InputFloat<PinTable>(
         table, "Scene Lighting Scale"s, //
         [](const PinTable *table) { return table->m_globalEmissionScale; }, //
         [&editor](PinTable *table, float v)
         {
            table->m_globalEmissionScale = v;
            editor.m_renderer->MarkShaderDirty(); // Needed to update shaders with new light settings
         },
         PropertyPane::Unit::Percent, 1, "Global scale applied to all scene lighting");

      props.Separator("Environment Lighting"s);
      props.ImageCombo<PinTable>(
         table, "Environment Image"s, //
         [](const PinTable *table) { return table->m_envImage; }, //
         [&editor](PinTable *table, const string &v)
         {
            table->m_envImage = v;
            editor.m_renderer->MarkShaderDirty(); // Needed to update shaders with new light settings
         },
         "Image used for ambient environment lighting");
      props.InputFloat<PinTable>(
         table, "Environment Emission"s, //
         [](const PinTable *table) { return table->m_envEmissionScale; }, //
         [&editor](PinTable *table, float v)
         {
            table->m_envEmissionScale = v;
            editor.m_renderer->MarkShaderDirty(); // Needed to update shaders with new light settings
         },
         PropertyPane::Unit::Percent, 3, "Intensity of the environment lighting");
      props.InputFloat<PinTable>(
         table, "Environment Reflections"s, //
         [](const PinTable *table) { return table->m_envReflectionScale; }, //
         [&editor](PinTable *table, float v)
         {
            table->m_envReflectionScale = max(v, 0.f);
            editor.m_renderer->MarkShaderDirty(); // Needed to update shaders with new light settings
         },
         PropertyPane::Unit::None, 3);

      props.Separator("Render Options"s);
      props.Checkbox<PinTable>(
         table, "Ambient Occlusion"s, //
         [](const PinTable *table) { return table->m_enableAO; }, //
         [&editor](PinTable *table, bool v)
         {
            table->m_enableAO = v;
            editor.m_renderer->MarkShaderDirty(); // Needed to update shaders with new light settings
         },
         "Darken creases and contact areas with ambient occlusion");
      props.InputFloat<PinTable>(
         table, "AO Scale"s, //
         [](const PinTable *table) { return table->m_AOScale; }, //
         [&editor](PinTable *table, float v)
         {
            table->m_AOScale = v;
            editor.m_renderer->MarkShaderDirty(); // Needed to update shaders with new light settings
         },
         PropertyPane::Unit::Percent, 1, "Strength of the ambient occlusion");
      props.Checkbox<PinTable>(
         table, "Screen Space Reflections"s, //
         [](const PinTable *table) { return table->m_enableSSR; }, //
         [&editor](PinTable *table, bool v)
         {
            table->m_enableSSR = v;
            editor.m_renderer->MarkShaderDirty(); // Needed to update shaders with new light settings
         },
         "Reflect on-screen content on glossy surfaces (screen-space reflections)");
      props.InputFloat<PinTable>(
         table, "SSR Scale"s, //
         [](const PinTable *table) { return table->m_SSRScale; }, //
         [](PinTable *table, float v) { table->m_SSRScale = v; }, PropertyPane::Unit::Percent, 1, "Strength of the screen-space reflections");
      props.InputFloat<PinTable>(
         table, "Bloom Strength"s, //
         [](const PinTable *table) { return table->m_bloom_strength; }, //
         [](PinTable *table, float v) { table->m_bloom_strength = v; }, PropertyPane::Unit::Percent, 1, "Intensity of the glow around bright areas");
      props.Combo<PinTable>(
         table, "Tonemapping"s, vector<string> { "Reinhard"s, "AgX"s, "Filmic"s, "Neutral"s }, //
         [](const PinTable *table) { return static_cast<int>(table->GetToneMapper()); }, //
         [&editor](PinTable *table, int v)
         {
            table->SetToneMapper(static_cast<ToneMapper>(v));
            editor.m_renderer->MarkShaderDirty(); // Needed to update shaders with new light settings
         },
         "How high dynamic range colors are mapped to the display");
      props.InputFloat<PinTable>(
         table, "Exposure"s, //
         [](const PinTable *table) { return table->GetExposure(); }, //
         [&editor](PinTable *table, float v)
         {
            table->SetExposure(v);
            editor.m_renderer->MarkShaderDirty(); // Needed to update shaders with new light settings
         },
         PropertyPane::Unit::None, 2);

      props.EndSection();
   }
}

void PropertiesPanel::CameraProperties(PropertyPane &props, int bgSet)
{
   EditorUI &editor = m_editor;
   ImGui::BeginDisabled(true);
   props.Header("Camera"s, [bgSet]() { return bgSet == 0 ? "Desktop"s : bgSet == 1 ? "Cabinet"s : "Full Single Screen"s; }, [](const string &) {});
   ImGui::EndDisabled();

   {
      if (ImGui::Button("Import"))
      {
         editor.m_table->ImportBackdropPOV(editor.m_table->GetSettingsFileName(), false);
         editor.m_renderer->MarkShaderDirty();
      }
      ImGui::SameLine();
      if (ImGui::Button("Export"))
         editor.m_table->ExportBackdropPOV(editor.m_table->GetSettingsFileName());
      ImGui::NewLine();
   }

   if (props.BeginSection("Visuals"s))
   {
      ViewSetup *const vs = &props.GetEditedPart<PinTable>(editor.m_table)->mViewSetups[bgSet];
      props.Combo<ViewSetup>(
         vs, "View Mode"s, vector { "Legacy"s, "Camera"s, "Window"s }, //
         [](const ViewSetup *viewSetup) { return static_cast<int>(viewSetup->mMode); }, //
         [](ViewSetup *viewSetup, int v) { viewSetup->mMode = static_cast<ViewLayoutMode>(v); }, "How the view is computed: legacy fixed projection, camera parameters, or window mapping");
      props.InputFloat<ViewSetup>(
         vs, "Field of View"s, //
         [](const ViewSetup *viewSetup) { return viewSetup->mFOV; }, //
         [](ViewSetup *viewSetup, float v) { viewSetup->mFOV = v; }, PropertyPane::Unit::Degree, 1);
      props.InputFloat<ViewSetup>(
         vs, "Layback"s, //
         [](const ViewSetup *viewSetup) { return viewSetup->mLayback; }, //
         [](ViewSetup *viewSetup, float v) { viewSetup->mLayback = v; }, PropertyPane::Unit::Degree, 1, "Fake tilt of the table toward the viewer (legacy view only)");
      props.InputFloat<ViewSetup>(
         vs, "Look At"s, //
         [](const ViewSetup *viewSetup) { return viewSetup->mLookAt; }, //
         [](ViewSetup *viewSetup, float v) { viewSetup->mLookAt = v; }, PropertyPane::Unit::None, 2, "Height along the table the camera is aimed at");
      props.InputFloat<ViewSetup>(
         vs, "X Offset"s, //
         [](const ViewSetup *viewSetup) { return viewSetup->mViewX; }, //
         [](ViewSetup *viewSetup, float v) { viewSetup->mViewX = v; }, PropertyPane::Unit::None, 0);
      props.InputFloat<ViewSetup>(
         vs, "Y Offset"s, //
         [](const ViewSetup *viewSetup) { return viewSetup->mViewY; }, //
         [](ViewSetup *viewSetup, float v) { viewSetup->mViewY = v; }, PropertyPane::Unit::None, 0);
      props.InputFloat<ViewSetup>(
         vs, "Z Offset"s, //
         [](const ViewSetup *viewSetup) { return viewSetup->mViewZ; }, //
         [](ViewSetup *viewSetup, float v) { viewSetup->mViewZ = v; }, PropertyPane::Unit::None, 0);
      props.InputFloat<ViewSetup>(
         vs, "Rotation"s, //
         [](const ViewSetup *viewSetup) { return viewSetup->mViewportRotation; }, //
         [](ViewSetup *viewSetup, float v) { viewSetup->mViewportRotation = v; }, PropertyPane::Unit::Degree, 0);
      props.InputFloat<ViewSetup>(
         vs, "View X Offset"s, //
         [](const ViewSetup *viewSetup) { return viewSetup->mViewHOfs; }, //
         [](ViewSetup *viewSetup, float v) { viewSetup->mViewHOfs = v; }, PropertyPane::Unit::None, 1);
      props.InputFloat<ViewSetup>(
         vs, "View Y Offset"s, //
         [](const ViewSetup *viewSetup) { return viewSetup->mViewVOfs; }, //
         [](ViewSetup *viewSetup, float v) { viewSetup->mViewVOfs = v; }, PropertyPane::Unit::None, 1);
      props.InputFloat<ViewSetup>(
         vs, "X Scale"s, //
         [](const ViewSetup *viewSetup) { return viewSetup->mSceneScaleX; }, //
         [](ViewSetup *viewSetup, float v) { viewSetup->mSceneScaleX = v; }, PropertyPane::Unit::Percent, 3, "Stretch the rendered scene (compensates display distortion)");
      props.InputFloat<ViewSetup>(
         vs, "Y Scale"s, //
         [](const ViewSetup *viewSetup) { return viewSetup->mSceneScaleY; }, //
         [](ViewSetup *viewSetup, float v) { viewSetup->mSceneScaleY = v; }, PropertyPane::Unit::Percent, 3);
      props.InputFloat<ViewSetup>(
         vs, "Z Scale"s, //
         [](const ViewSetup *viewSetup) { return viewSetup->mSceneScaleZ; }, //
         [](ViewSetup *viewSetup, float v) { viewSetup->mSceneScaleZ = v; }, PropertyPane::Unit::Percent, 3);
      props.EndSection();
   }
}

void PropertiesPanel::ImageProperties(PropertyPane &props, Texture *texture)
{
   EditorUI &editor = m_editor;
   ImGui::BeginDisabled(editor.m_table->m_liveBaseTable != nullptr); // Disable edition in inspection mode as images are shared between startup & inspected table

   props.Header("Image"s, [texture]() { return texture->m_name; }, [texture](const string &v) { texture->m_name = v; });

   ImageActions(props);

   ImTextureID image = editor.m_renderer->m_renderDevice->m_texMan.LoadTexture(texture, false);
   if (image)
   {
      if (props.BeginSection("Visuals"s))
      {
         std::shared_ptr<const BaseTexture> tex = texture->GetRawBitmap(false, 0);

         ImGui::BeginDisabled(tex == nullptr || !tex->HasAlpha());
         props.InputFloat<Texture>(
            texture, "Alpha Mask", //
            [](const Texture *image) { return image->m_alphaTestValue; }, //
            [](Texture *image, float v) { image->m_alphaTestValue = v; }, PropertyPane::Unit::None, 2, "Pixels with alpha below this value are discarded (cutout transparency)");
         ImGui::EndDisabled();

         const string info
            = std::to_string(image->GetWidth()) + 'x' + std::to_string(image->GetHeight()) + ' ' + ((tex != nullptr && tex->m_format) ? BaseTexture::GetFormatString(tex->m_format) : ""s);
         props.Separator(info);

         props.EndSection();

         const float w = ImGui::GetWindowWidth();
         const float scale = w / static_cast<float>(std::max(image->GetWidth(), image->GetHeight()));
         const float imgW = static_cast<float>(image->GetWidth()) * scale;
         ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w - imgW) * 0.5f);
         ImGui::Image(image, ImVec2(imgW, static_cast<float>(image->GetHeight()) * scale));
      }
   }
   else
   {
      ImGui::Text("Failed to load image");
   }

   ImGui::EndDisabled();

   vector<WhereUsedInfo> whereUsed;
   editor.m_table->ShowWhereImageUsed(whereUsed, texture);
   UsersSection(props, whereUsed);
}

void PropertiesPanel::RenderProbeProperties(PropertyPane &props, RenderProbe *probe)
{
   EditorUI &editor = m_editor;
   RenderProbe *editedProbe = props.GetEditedPart<RenderProbe>(probe);
   props.Header("Render Probe"s, [editedProbe]() { return editedProbe->GetName(); }, [editedProbe](const string &v) { editedProbe->SetName(v); });

   if (props.BeginSection("Visuals"s))
   {
      props.Combo<RenderProbe>(
         probe, "Type"s, vector { "Reflection"s, "Refraction"s }, //
         [](const RenderProbe *probe) { return static_cast<int>(probe->GetType()); }, //
         [](RenderProbe *probe, int v) { probe->SetType(static_cast<RenderProbe::ProbeType>(v)); },
         "Reflection renders the scene mirrored on a plane; Refraction renders what is behind the surface");
      props.InputFloat3<RenderProbe>(
         probe, "Normal"s, //
         [](const RenderProbe *probe) { return probe->GetReflectionPlaneNormal(); }, //
         [](RenderProbe *probe, const vec3 &v) { probe->SetReflectionPlaneNormal(v); }, PropertyPane::Unit::None, 2, "Normal of the plane the scene is reflected on");
      props.InputFloat<RenderProbe>(
         probe, "Distance"s, //
         [](const RenderProbe *probe) { return probe->GetReflectionPlaneDistance(); }, //
         [](RenderProbe *probe, float v) { probe->SetReflectionPlaneDistance(v); }, PropertyPane::Unit::VPLength, 1, "Distance of the reflection plane along its normal");
      props.InputFloat<RenderProbe>(
         probe, "Influence Range"s, //
         [](const RenderProbe *probe) { return probe->GetInfluenceRange(); }, //
         [](RenderProbe *probe, float v) { probe->SetInfluenceRange(v); }, PropertyPane::Unit::VPLength, 1, "Maximum distance at which the probe's captured content is used");
      props.EndSection();
   }

   if (PropertyPane::SectionHeader("Users", ImGuiTreeNodeFlags_DefaultOpen))
   {
      // Add a white line above
      const ImVec2 headerMin = ImGui::GetItemRectMin();
      const ImVec2 headerMax = ImGui::GetItemRectMax();
      ImDrawList *drawList = ImGui::GetWindowDrawList();
      const ImVec2 lineStart(headerMin.x, headerMin.y);
      const ImVec2 lineEnd(headerMax.x, headerMin.y);
      drawList->AddLine(lineStart, lineEnd, ImGui::GetColorU32(ImGuiCol_Text), 1.0f);

      for (const IEditable *editable : editor.m_table->GetParts())
      {
         if (editable->GetItemType() != eItemPrimitive)
            continue;
         const Primitive *const primitive = static_cast<const Primitive *>(editable);
         if ((probe->GetType() == RenderProbe::PLANE_REFLECTION) && (primitive->m_d.m_szReflectionProbe != probe->GetName()))
            continue;
         if ((probe->GetType() == RenderProbe::SCREEN_SPACE_TRANSPARENCY) && (primitive->m_d.m_szRefractionProbe != probe->GetName()))
            continue;
         const auto it = editor.m_editableMap.find(editable);
         if (it == editor.m_editableMap.end())
            continue;
         if (ImGui::Selectable(primitive->GetName().c_str()))
            editor.SetSelection(Selection(it->second));
      }
   }
}

void PropertiesPanel::MaterialProperties(PropertyPane &props, Material *material)
{
   Material *editedMaterial = props.GetEditedPart<Material>(material);
   props.Header("Material"s, [editedMaterial]() { return editedMaterial->m_name; }, [editedMaterial](const string &v) { editedMaterial->m_name = v; });

   if (!m_inPartMaterialPane) // The material list actions act on the material selection, not on a part's material
   {
      ImGui::BeginDisabled(m_editor.m_table->m_liveBaseTable != nullptr); // Material list actions are not supported in inspection mode
      MaterialActions(props);
      ImGui::EndDisabled();
   }

   if (props.BeginSection("Visuals"s))
   {
      props.Combo<Material>(
         material, "Type"s, vector { "Default"s, "Metal"s, "PBR"s }, //
         [](const Material *material) { return material->m_type; }, //
         [](Material *material, int v) { material->m_type = static_cast<Material::MaterialType>(v); }, "How the material reflects light: dielectric (default) or metal");
      props.InputRGB<Material>(
         material, "Color", //
         [](const Material *material) { return convertColor(material->m_cBase); }, //
         [](Material *material, const vec3 &v) { material->m_cBase = convertColorRGB(v); });
      // Wrap lighting is only used by the legacy (non PBR) shading path
      if (editedMaterial->m_type != Material::PBR)
         props.InputFloat<Material>(
            material, "Wrap Lighting"s, //
            [](const Material *material) { return material->m_fWrapLighting; }, //
            [](Material *material, float v) { material->m_fWrapLighting = v; }, PropertyPane::Unit::None, 2, "Wrap diffuse light around the part's edges, softening the shading");
      // The glossy specular layer is only used by the legacy default material shading
      if (editedMaterial->m_type == Material::BASIC)
      {
         props.InputRGB<Material>(
            material, "Glossy Color", //
            [](const Material *material) { return convertColor(material->m_cGlossy); }, //
            [](Material *material, const vec3 &v) { material->m_cGlossy = convertColorRGB(v); });
         props.InputFloat<Material>(
            material, "Glossy Image Lerp"s, //
            [](const Material *material) { return material->m_fGlossyImageLerp; }, //
            [](Material *material, float v) { material->m_fGlossyImageLerp = v; }, PropertyPane::Unit::None, 2,
            "How much specular highlights take the image's color instead of the glossy color");
      }
      props.InputFloat<Material>(
         material, "Roughness"s, //
         [](const Material *material) { return material->m_fRoughness; }, //
         [](Material *material, float v) { material->m_fRoughness = v; }, PropertyPane::Unit::None, 2, "Sharpness of the specular highlights (higher = smaller, brighter highlights)");
      // The clearcoat layer is not used by the PBR shading path
      if (editedMaterial->m_type != Material::PBR)
         props.InputRGB<Material>(
            material, "Clearcoat Color", //
            [](const Material *material) { return convertColor(material->m_cClearcoat); }, //
            [](Material *material, const vec3 &v) { material->m_cClearcoat = convertColorRGB(v); });
      if (editedMaterial->m_type != Material::PBR)
         props.InputFloat<Material>(
            material, "Edge Brightness"s, //
            [](const Material *material) { return material->m_fEdge; }, //
            [](Material *material, float v) { material->m_fEdge = v; }, PropertyPane::Unit::None, 2, "Extra brightness at grazing angles");
      if (editedMaterial->m_type == Material::PBR)
      {
         props.InputFloat<Material>(
            material, "Metallic"s, //
            [](const Material *material) { return material->m_fMetallic; }, //
            [](Material *material, float v) { material->m_fMetallic = v; }, PropertyPane::Unit::None, 2);
         props.InputFloat<Material>(
            material, "Occlusion"s, //
            [](const Material *material) { return material->m_fOcclusion; }, //
            [](Material *material, float v) { material->m_fOcclusion = v; }, PropertyPane::Unit::None, 2);
         props.InputRGB<Material>(
            material, "Emission Color", //
            [](const Material *material) { return convertColor(material->m_cEmission); }, //
            [](Material *material, const vec3 &v) { material->m_cEmission = convertColorRGB(v); });
         props.InputFloat<Material>(
            material, "Emission Intensity"s, //
            [](const Material *material) { return material->m_fEmissionIntensity; }, //
            [](Material *material, float v) { material->m_fEmissionIntensity = v; }, PropertyPane::Unit::None, 2);
         props.InputFloat<Material>(
            material, "Clear Coat"s, //
            [](const Material *material) { return material->m_fClearcoat; }, //
            [](Material *material, float v) { material->m_fClearcoat = v; }, PropertyPane::Unit::None, 2);
         props.InputFloat<Material>(
            material, "Clear Coat Roughness"s, //
            [](const Material *material) { return material->m_fClearcoatRoughness; }, //
            [](Material *material, float v) { material->m_fClearcoatRoughness = v; }, PropertyPane::Unit::None, 2);
         props.Separator("See-Through Surface"s);
         props.SliderFloat<Material>(
            material, "Scatter"s, //
            [](const Material *material) { return material->m_fScatter; }, //
            [](Material *material, float v) { material->m_fScatter = clamp(v, 0.f, 1.f); }, 0.f, 1.f, 3);
         props.SliderFloat<Material>(
            material, "Scatter Floor"s, //
            [](const Material *material) { return material->m_fScatterFloor; }, //
            [](Material *material, float v) { material->m_fScatterFloor = clamp(v, 0.f, 1.f); }, 0.f, 0.3f, 3);
         props.SliderFloat<Material>(
            material, "Scatter Env Share"s, //
            [](const Material *material) { return material->m_fScatterEnv; }, //
            [](Material *material, float v) { material->m_fScatterEnv = max(v, 0.f); }, 0.f, 1.f, 3);
         props.SliderFloat<Material>(
            material, "Scatter Edge Angle"s, //
            [](const Material *material) { return material->m_fScatterEdgeAngle; }, //
            [](Material *material, float v) { material->m_fScatterEdgeAngle = clamp(v, 30.f, 89.f); }, 50.f, 89.f, 1);
         props.SliderFloat<Material>(
            material, "Rim Gain"s, //
            [](const Material *material) { return material->m_fRimGain; }, //
            [](Material *material, float v) { material->m_fRimGain = max(v, 0.f); }, 0.f, 8.f, 2);
         props.SliderFloat<Material>(
            material, "Rim Width"s, //
            [](const Material *material) { return material->m_fRimWidth; }, //
            [](Material *material, float v) { material->m_fRimWidth = clamp(v, 0.05f, 1.f); }, 0.2f, 1.f, 2);
         props.SliderFloat<Material>(
            material, "Bevel Brightening"s, //
            [](const Material *material) { return material->m_fBevelBright; }, //
            [](Material *material, float v) { material->m_fBevelBright = max(v, 0.f); }, 0.f, 2.f, 2);
         props.SliderFloat<Material>(
            material, "Scatter Locality"s, //
            [](const Material *material) { return material->m_fScatterLocality; }, //
            [](Material *material, float v) { material->m_fScatterLocality = clamp(v, 0.f, 1.f); }, 0.f, 1.f, 2);
         props.SliderFloat<Material>(
            material, "Scatter Width"s, //
            [](const Material *material) { return material->m_fScatterWidth; }, //
            [](Material *material, float v) { material->m_fScatterWidth = v <= 0.f ? 0.f : clamp(v, 0.05f, 1.f); }, 0.f, 1.f, 2); // 0 = no band
         props.SliderFloat<Material>(
            material, "Scatter Saturation"s, //
            [](const Material *material) { return material->m_fScatterSaturation; }, //
            [](Material *material, float v) { material->m_fScatterSaturation = clamp(v, 0.f, 2.f); }, 0.f, 2.f, 2);
         props.SliderFloat<Material>(
            material, "Dense Edge Albedo"s, //
            [](const Material *material) { return material->m_fDenseAlbedo; }, //
            [](Material *material, float v) { material->m_fDenseAlbedo = max(v, 0.f); }, 0.f, 1.f, 3);
         props.InputRGB<Material>(
            material, "Absorption Color", //
            [](const Material *material) { return convertColor(material->m_cAbsorption); }, //
            [](Material *material, const vec3 &v) { material->m_cAbsorption = convertColorRGB(v); });
         props.SliderFloat<Material>(
            material, "Absorption"s, //
            [](const Material *material) { return material->m_fAbsorption; }, //
            [](Material *material, float v) { material->m_fAbsorption = max(v, 0.f); }, 0.f, 3.f, 3);
         props.SliderFloat<Material>(
            material, "Absorption Light Tint"s, //
            [](const Material *material) { return material->m_fAbsorptionLightTint; }, //
            [](Material *material, float v) { material->m_fAbsorptionLightTint = clamp(v, 0.f, 1.f); }, 0.f, 1.f, 2);
         props.SliderFloat<Material>(
            material, "Edge Glow"s, //
            [](const Material *material) { return material->m_fEdgeGlow; }, //
            [](Material *material, float v) { material->m_fEdgeGlow = clamp(v, 0.f, 5.f); }, 0.f, 5.f, 2);
         props.SliderFloat<Material>(
            material, "Edge Glow Tint"s, //
            [](const Material *material) { return material->m_fEdgeGlowTint; }, //
            [](Material *material, float v) { material->m_fEdgeGlowTint = clamp(v, 0.f, 1.f); }, 0.f, 1.f, 2);
      }
      props.EndSection();
   }

   if (props.BeginSection("Transparency"s))
   {
      props.Checkbox<Material>(
         material, "Enable Transparency"s, //
         [](const Material *material) { return material->m_bOpacityActive; }, //
         [](Material *material, bool v) { material->m_bOpacityActive = v; });
      props.InputFloat<Material>(
         material, "Opacity"s, //
         [](const Material *material) { return material->m_fOpacity; }, //
         [](Material *material, float v) { material->m_fOpacity = v; }, PropertyPane::Unit::None, 2);
      // Edge opacity is not used by the PBR shading path
      if (editedMaterial->m_type != Material::PBR)
         props.InputFloat<Material>(
            material, "Edge Opacity"s, //
            [](const Material *material) { return material->m_fEdgeAlpha; }, //
            [](Material *material, float v) { material->m_fEdgeAlpha = v; }, PropertyPane::Unit::None, 2, "Opacity at grazing angles");
      props.InputFloat<Material>(
         material, "Thickness"s, //
         [](const Material *material) { return material->m_fThickness; }, //
         [](Material *material, float v) { material->m_fThickness = v; }, PropertyPane::Unit::None, 2, "Simulated thickness used for refraction through transparent parts");
      props.InputRGB<Material>(
         material, "Refraction Tint", //
         [](const Material *material) { return convertColor(material->m_cRefractionTint); }, //
         [](Material *material, const vec3 &v) { material->m_cRefractionTint = convertColorRGB(v); });
      props.EndSection();
   }

   vector<WhereUsedInfo> whereUsed;
   m_editor.m_table->ShowWhereMaterialUsed(whereUsed, material);
   UsersSection(props, whereUsed);
}

void PropertiesPanel::SoundProperties(PropertyPane &props, VPX::Sound *sound)
{
   EditorUI &editor = m_editor;
   VPX::AudioPlayer *const audioPlayer = editor.m_player->m_audioPlayer.get();

   // Track the playback state of the selected sound: playback commands are dispatched asynchronously
   // so the button keeps the "stop" state until playback is actually observed, and reverts to "play"
   // when the sound reaches its end.
   if (m_playingSound != sound)
   {
      m_playingSound = nullptr;
      m_playingSoundObserved = false;
   }
   const bool playing = audioPlayer && audioPlayer->IsSoundPlaying(sound);
   m_playingSoundObserved |= playing;
   if (m_playingSoundObserved && !playing)
   {
      m_playingSound = nullptr;
      m_playingSoundObserved = false;
   }
   const bool isPlaying = playing || (m_playingSound == sound);

   props.Header(
      "Sound"s, [sound]() { return sound->GetName(); },
      [&editor, sound](const string &v)
      {
         sound->SetName(v);
         editor.m_table->SetNonUndoableDirty(SaveDirtyState::Dirty);
      });

   ImGui::BeginDisabled(editor.m_table->m_liveBaseTable != nullptr); // Sound list actions are not supported in inspection mode as sounds are shared
   SoundActions(props);
   ImGui::EndDisabled();

   ImGui::BeginDisabled(audioPlayer == nullptr);
   if (ImGui::Button(isPlaying ? (ICON_LC_SQUARE " Stop"s).c_str() : (ICON_LC_PLAY " Play"s).c_str()))
   {
      if (isPlaying)
      {
         audioPlayer->StopSound(sound);
         m_playingSound = nullptr;
         m_playingSoundObserved = false;
      }
      else
      {
         audioPlayer->PlaySound(sound, 1.0f, 0.f, 0, 0.f, 0.f, 0, false, true);
         m_playingSound = sound;
         m_playingSoundObserved = false;
      }
   }
   ImGui::EndDisabled();

   ImGui::BeginDisabled(editor.m_table->m_liveBaseTable != nullptr); // Disable edition in inspection mode as sounds are shared between startup & inspected table

   if (props.BeginSection("Audio Positioning"s))
   {
      props.Combo<VPX::Sound>(
         sound, "Output Target"s, vector { "Table"s, "Backglass"s }, //
         [](const VPX::Sound *s) { return static_cast<int>(s->GetOutputTarget()); }, //
         [&editor](VPX::Sound *s, int v)
         {
            s->SetOutputTarget(static_cast<VPX::SoundOutTypes>(v));
            editor.m_table->SetNonUndoableDirty(SaveDirtyState::Dirty);
         },
         "Play this sound on the table's or the backglass' audio output");
      props.InputFloat<VPX::Sound>(
         sound, "Volume"s, //
         [](const VPX::Sound *s) { return dequantizeSignedPercent(s->GetVolume()); }, //
         [&editor](VPX::Sound *s, float v)
         {
            s->SetVolume(quantizeSignedPercent(v));
            editor.m_table->SetNonUndoableDirty(SaveDirtyState::Dirty);
         },
         PropertyPane::Unit::None, 2);
      props.InputFloat<VPX::Sound>(
         sound, "Pan"s, //
         [](const VPX::Sound *s) { return dequantizeSignedPercent(s->GetPan()); }, //
         [&editor](VPX::Sound *s, float v)
         {
            s->SetPan(quantizeSignedPercent(v));
            editor.m_table->SetNonUndoableDirty(SaveDirtyState::Dirty);
         },
         PropertyPane::Unit::None, 2);
      props.InputFloat<VPX::Sound>(
         sound, "Front/Rear Fade"s, //
         [](const VPX::Sound *s) { return dequantizeSignedPercent(s->GetFrontRearFade()); }, //
         [&editor](VPX::Sound *s, float v)
         {
            s->SetFrontRearFade(quantizeSignedPercent(v));
            editor.m_table->SetNonUndoableDirty(SaveDirtyState::Dirty);
         },
         PropertyPane::Unit::None, 2, "Balance between front and rear speakers (surround setups)");
      props.EndSection();
   }

   if (props.BeginSection("File"s))
   {
      if (m_soundInfoFor != sound)
      {
         m_soundInfoFor = sound;
         m_soundInfo = audioPlayer ? audioPlayer->GetSoundInformations(sound) : std::optional<VPX::SoundSpec>();
      }
      ImGui::TextWrapped("%s", PathToUTF8(sound->GetImportPath()).c_str());
      ImGui::TextDisabled("Size: %s", SizeToReadable(sound->GetFileSize()).c_str()); //
      if (m_soundInfo)
         ImGui::TextDisabled("%.2f s, %u Hz, %u channel%s", m_soundInfo->lengthInSeconds, m_soundInfo->sampleFrequency, m_soundInfo->nChannels, m_soundInfo->nChannels > 1 ? "s" : "");
      props.EndSection();
   }

   ImGui::EndDisabled();
}

void PropertiesPanel::CollectionProperties(PropertyPane &props, Collection *collection)
{
   EditorUI &editor = m_editor;
   PinTable *const table = editor.m_table;
   // The displayed collection: when inspecting a live table, this is the live collection on the
   // 'Live' tab and its startup version on the 'Startup' tab (they are distinct objects)
   Collection *const edited = props.GetEditedPart<Collection>(collection);
   PinTable *const editedTable = (edited == collection) ? table : table->m_liveBaseTable;

   props.Header(
      "Collection"s, [edited]() { return edited->m_name; },
      [editedTable, edited](const string &v)
      {
         // Same uniqueness handling as the Win32 collection manager (names are script identifiers)
         string name = TruncateToUTF16Length(v, MAXNAMEBUFFER - 1);
         if (name.empty() || editedTable == nullptr)
            return;
         if (lowerCase(name) != lowerCase(edited->m_name) && !editedTable->IsNameUnique(name))
            name = editedTable->GetUniqueName(name);
         editedTable->RenameCollection(edited, name);
         editedTable->SetNonUndoableDirty(SaveDirtyState::Dirty);
      });

   // Actions on the table's collection list are only available when editing the base table
   // (the inspected table is a live copy whose changes would be lost when the play session ends)
   ImGui::BeginDisabled(table->m_liveBaseTable != nullptr);
   if (props.BeginSection("Actions"s))
   {
      if (ImGui::Button("New"))
         editor.CreateCollection(false);
      ImGui::SameLine();
      const auto &collections = table->GetCollections();
      int colIndex = -1;
      for (size_t i = 0; i < collections.size(); i++)
         if (collections[i] == collection)
            colIndex = static_cast<int>(i);
      ImGui::BeginDisabled(colIndex <= 0);
      if (ImGui::Button("Move Up"))
      {
         table->MoveCollectionUp(collections[colIndex]);
         table->SetNonUndoableDirty(SaveDirtyState::Dirty);
      }
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Move the collection up in the table's collection list");
      ImGui::SameLine();
      ImGui::BeginDisabled(colIndex < 0 || colIndex >= static_cast<int>(collections.size()) - 1);
      if (ImGui::Button("Move Down"))
      {
         table->MoveCollectionDown(collections[colIndex]);
         table->SetNonUndoableDirty(SaveDirtyState::Dirty);
      }
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Move the collection down in the table's collection list");
      ImGui::SameLine();
      if (ImGui::Button("Delete"))
      {
         RequestConfirm("Delete the selected collection?"s,
            [this, collection]()
            {
               EditorUI &editor = m_editor;
               for (CComObject<Collection> *const col : editor.m_table->GetCollections())
                  if (col == collection)
                  {
                     editor.m_table->RemoveCollection(col);
                     break;
                  }
               editor.m_selection = Selection();
               editor.m_table->SetNonUndoableDirty(SaveDirtyState::Dirty);
            });
      }
      props.EndSection();
   }
   ImGui::EndDisabled();

   if (props.BeginSection("Options"s))
   {
      // Fields are synchronizable between the live and startup versions of the collection: the
      // setter may receive either one, so the table to flag as dirty is resolved from the object
      const auto markDirty = [&editor](Collection *col)
      {
         PinTable *dirtyTable = editor.m_table;
         if (dirtyTable->m_liveBaseTable && dirtyTable->GetLiveFromStartup<Collection>(col) != nullptr)
            dirtyTable = dirtyTable->m_liveBaseTable;
         dirtyTable->SetNonUndoableDirty(SaveDirtyState::Dirty);
      };
      props.Checkbox<Collection>(
         collection, "Fire events for this collection"s, //
         [](const Collection *col) { return col->m_fireEvents; }, //
         [&markDirty](Collection *col, bool v)
         {
            col->m_fireEvents = v;
            markDirty(col);
         },
         "Fire a Collection event when a member is hit, on top of the member's own event");
      props.Checkbox<Collection>(
         collection, "Suppress individual events for each member"s, //
         [](const Collection *col) { return col->m_stopSingleEvents; }, //
         [&markDirty](Collection *col, bool v)
         {
            col->m_stopSingleEvents = v;
            markDirty(col);
         },
         "Only fire the collection's event, not each member's own event");
      props.Checkbox<Collection>(
         collection, "Group elements together"s, //
         [](const Collection *col) { return col->m_groupElements; }, //
         [&markDirty](Collection *col, bool v)
         {
            col->m_groupElements = v;
            markDirty(col);
         },
         "Show this collection's members as a group in the editor");
      props.EndSection();
   }

   if (props.BeginSection("Content"s))
   {
      const vector<IEditable *> &members = edited->GetParts();

      // Member picker listing the table's scriptable parts that are not members yet
      props.PropertyLabel("Add part"s);
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (ImGui::BeginCombo("##AddPart", "Select a part to add"))
      {
         vector<IEditable *> candidates;
         for (IEditable *const part : editedTable->GetParts())
            if (part->GetIScriptable() != nullptr && std::ranges::find(members, part) == members.end())
               candidates.push_back(part);
         std::ranges::sort(candidates, [](const IEditable *a, const IEditable *b) { return StrLessNoCase(a->GetName(), b->GetName()); });
         for (IEditable *const part : candidates)
            if (ImGui::Selectable(part->GetName().c_str()))
            {
               vector<IEditable *> newContent(members);
               newContent.push_back(part);
               editedTable->SetCollectionContent(edited, newContent);
               editedTable->SetNonUndoableDirty(SaveDirtyState::Dirty);
            }
         ImGui::EndCombo();
      }

      // Members, in the collection's order which is the order exposed to scripts
      if (members.empty())
         ImGui::TextDisabled("Empty collection");
      const float buttonWidth = ImGui::CalcTextSize(ICON_LC_ARROW_UP).x + ImGui::GetStyle().FramePadding.x * 2.0f;
      const float buttonsX = ImGui::GetContentRegionAvail().x - 3.f * buttonWidth - 2.f * ImGui::GetStyle().ItemSpacing.x;
      int moveIndex = -1, removeIndex = -1;
      for (int i = 0; i < static_cast<int>(members.size()); i++)
      {
         IEditable *const member = members[i];
         ImGui::PushID(i);
         if (ImGui::Selectable(member->GetName().c_str()))
         {
            // Select the part in the editor (a startup part resolves to its live counterpart)
            IEditable *const target = (edited == collection) ? member : table->GetLiveFromStartup<IEditable>(member);
            if (target != nullptr)
               if (const auto it = editor.m_editableMap.find(target); it != editor.m_editableMap.end())
                  editor.SetSelection(Selection(it->second));
         }
         if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", LocalString(EditableRegistry::GetTypeNameStringID(member->GetItemType())).m_szbuffer);
         ImGui::SameLine(buttonsX);
         ImGui::BeginDisabled(i == 0);
         if (ImGui::SmallButton(ICON_LC_ARROW_UP))
            moveIndex = i - 1;
         ImGui::EndDisabled();
         if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Move up");
         ImGui::SameLine();
         ImGui::BeginDisabled(i == static_cast<int>(members.size()) - 1);
         if (ImGui::SmallButton(ICON_LC_ARROW_DOWN))
            moveIndex = i;
         ImGui::EndDisabled();
         if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Move down");
         ImGui::SameLine();
         if (ImGui::SmallButton(ICON_LC_X))
            removeIndex = i;
         if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Remove from collection");
         ImGui::PopID();
      }
      if (moveIndex != -1 || removeIndex != -1)
      {
         vector<IEditable *> newContent(members);
         if (moveIndex != -1)
            std::swap(newContent[moveIndex], newContent[moveIndex + 1]);
         else
            newContent.erase(newContent.begin() + removeIndex);
         editedTable->SetCollectionContent(edited, newContent);
         editedTable->SetNonUndoableDirty(SaveDirtyState::Dirty);
      }
      props.EndSection();
   }
}

void PropertiesPanel::RequestConfirm(const string &message, const std::function<void()> &action)
{
   m_confirmMessage = message;
   m_confirmAction = action;
   ImGui::OpenPopup("Confirm Action");
}

void PropertiesPanel::UsersSection(PropertyPane &props, const vector<WhereUsedInfo> &whereUsed)
{
   EditorUI &editor = m_editor;
   if (props.BeginSection("Users"s))
   {
      if (whereUsed.empty())
         ImGui::TextDisabled("Not used by any part");
      for (const WhereUsedInfo &where : whereUsed)
      {
         IEditable * editable = editor.m_table->GetElementByName(where.whereUsedObjectname.c_str());
         if (editable == nullptr && where.whereUsedObjectname == m_editor.m_table->m_name)
            editable = m_editor.m_table;
         ImGui::PushID(&where);
         // Clicking an entry selects the using part in the editor, like the Win32 dialog's 'Edit Object' button
         if (ImGui::Selectable(where.whereUsedObjectname.c_str()))
            if (editable != nullptr)
               if (const auto it = editor.m_editableMap.find(editable); it != editor.m_editableMap.end())
                  editor.SetSelection(Selection(it->second));
         ImGui::PopID();
         ImGui::SameLine();
         string type = "";
         if (editable != nullptr && editable->GetItemType() == eItemTable)
            type = "Table";
         else if (editable != nullptr)
            type = LocalString(EditableRegistry::GetTypeNameStringID(editable->GetItemType())).m_szbuffer;
         ImGui::TextDisabled("[%s - %s]", type.c_str(), where.whereUsedPropertyName.c_str());
      }
      props.EndSection();
   }
}

void PropertiesPanel::RenderPopups()
{
   // Confirmation popup for destructive resource actions (OpenPopup is called from this window's scope)
   if (ImGui::BeginPopupModal("Confirm Action", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
   {
      ImGui::TextWrapped("%s", m_confirmMessage.c_str());
      ImGui::Separator();
      if (ImGui::Button("OK"))
      {
         if (m_confirmAction)
            m_confirmAction();
         m_confirmAction = nullptr;
         ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel") || PropertyPane::ModalEscapePressed())
      {
         m_confirmAction = nullptr;
         ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
   }
}

void PropertiesPanel::ImageActions(PropertyPane &props)
{
   EditorUI &editor = m_editor;
   PinTable *const table = editor.m_table;
   vector<Texture *> &sel = editor.m_multiSelImages;

   // Update the selection when a reimport replaced an image (the old object is deleted)
   const auto replaceImage = [&editor](Texture *oldImage, Texture *newImage)
   {
      std::replace(editor.m_multiSelImages.begin(), editor.m_multiSelImages.end(), oldImage, newImage);
      if (editor.m_selection == Selection(oldImage))
         editor.m_selection = (newImage != nullptr) ? Selection(newImage) : Selection();
      if (editor.m_outlinerImageAnchor == oldImage)
         editor.m_outlinerImageAnchor = newImage;
   };

   // Apply asynchronous file dialog results
   if (m_pendingImageImport && !m_pendingImageImport->empty())
   {
      vector<Texture *> imported;
      for (const string &file : *m_pendingImageImport)
         if (Texture *const tex = table->ImportImage(PathFromUTF8(file), ""s))
            imported.push_back(tex);
      g_settingsService.GetAppSettings().SetRecentDir_ImageDir(PathToString(PathFromUTF8(m_pendingImageImport->front()).parent_path()), false);
      m_pendingImageImport = nullptr;
      if (!imported.empty())
      {
         sel = imported;
         editor.m_selection = Selection(imported.back());
         editor.m_outlinerImageAnchor = imported.back();
         table->SetNonUndoableDirty(SaveDirtyState::Dirty);
      }
      else
         m_actionStatus = "Failed to import the selected file(s)"s;
   }
   if (m_pendingImageReimport && !m_pendingImageReimport->empty())
   {
      const string file = *m_pendingImageReimport;
      m_pendingImageReimport = nullptr;
      Texture *const old = m_pendingImageReimportTarget;
      m_pendingImageReimportTarget = nullptr;
      if (old != nullptr)
      {
         // ImportImage replaces and deletes the image that already uses this name
         if (Texture *const tex = table->ImportImage(PathFromUTF8(file), old->m_name))
         {
            replaceImage(old, tex);
            table->SetNonUndoableDirty(SaveDirtyState::Dirty);
         }
         else
            m_actionStatus = "Failed to reimport '"s + file + '\'';
         g_settingsService.GetAppSettings().SetRecentDir_ImageDir(PathToString(PathFromUTF8(file).parent_path()), false);
      }
   }
   if (m_pendingImageExport && !m_pendingImageExport->empty())
   {
      const std::filesystem::path path = PathFromUTF8(*m_pendingImageExport);
      m_pendingImageExport = nullptr;
      // Like the Win32 dialog: with multiple selected images, only the folder of the picked file is used
      int failed = 0;
      for (Texture *tex : m_pendingImageExportSel)
      {
         const std::filesystem::path file = (m_pendingImageExportSel.size() > 1)
            ? path.parent_path() / (m_exportUseNames ? (PathFromString(tex->m_name) += tex->GetFilePath().extension()) : tex->GetFilePath().filename())
            : path;
         if (!tex->SaveFile(file))
            failed++;
      }
      m_pendingImageExportSel.clear();
      if (failed > 0)
         m_actionStatus = "Failed to export "s + std::to_string(failed) + " image(s)"s;
      g_settingsService.GetAppSettings().SetRecentDir_ImageDir(PathToString(path.parent_path()), false);
   }

   if (props.BeginSection("Actions"s))
   {
      const bool canImport = editor.m_player->m_playfieldWnd != nullptr;
      ImGui::BeginDisabled(!canImport);
      if (ImGui::Button("Import"))
      {
         m_pendingImageImport = std::make_shared<vector<string>>();
         const SDL_DialogFileFilter filters[] = { { "Image Files", "bmp;jpg;jpeg;png;tga;webp;exr;hdr" } };
         const string dir = PathToUTF8(PathFromString(g_settingsService.GetAppSettings().GetRecentDir_ImageDir())); // SDL expects UTF-8
         SDL_ShowOpenFileDialog(
            [](void *userdata, const char *const *filelist, int filter)
            {
               auto *res = static_cast<std::shared_ptr<vector<string>> *>(userdata);
               if (filelist != nullptr)
                  for (int i = 0; filelist[i] != nullptr; i++)
                     (**res).push_back(filelist[i]);
               delete res;
            },
            new std::shared_ptr<vector<string>>(m_pendingImageImport), //
            editor.m_player->m_playfieldWnd->GetCore(), filters, 1, dir.empty() ? nullptr : dir.c_str(), true);
      }
      ImGui::EndDisabled();

      ImGui::SameLine();
      ImGui::BeginDisabled(sel.empty() || !canImport);
      if (ImGui::Button("Export"))
      {
         m_pendingImageExportSel = sel;
         m_pendingImageExport = std::make_shared<string>();
         const SDL_DialogFileFilter filters[] = { { "Image Files", "bmp;jpg;jpeg;png;tga;webp;exr;hdr" } };
         const string dir = PathToUTF8(PathFromString(g_settingsService.GetAppSettings().GetRecentDir_ImageDir())); // SDL expects UTF-8
         SDL_ShowSaveFileDialog(
            [](void *userdata, const char *const *filelist, int filter)
            {
               auto *res = static_cast<std::shared_ptr<string> *>(userdata);
               if (filelist != nullptr && filelist[0] != nullptr)
                  **res = filelist[0];
               delete res;
            },
            new std::shared_ptr<string>(m_pendingImageExport), //
            editor.m_player->m_playfieldWnd->GetCore(), filters, 1, dir.empty() ? nullptr : dir.c_str());
      }
      ImGui::EndDisabled();

      ImGui::SameLine();
      ImGui::BeginDisabled(sel.empty());
      if (ImGui::Button("Delete"))
      {
         RequestConfirm("Delete the selected image(s)?"s,
            [this]()
            {
               EditorUI &editor = m_editor;
               for (Texture *const tex : editor.m_multiSelImages)
                  editor.m_table->RemoveImage(tex);
               editor.m_multiSelImages.clear();
               editor.m_outlinerImageAnchor = nullptr;
               editor.m_selection = Selection();
               editor.m_table->SetNonUndoableDirty(SaveDirtyState::Dirty);
            });
      }
      ImGui::EndDisabled();

      // --

      ImGui::BeginDisabled(sel.empty());

      if (ImGui::Button("Reimport"))
      {
         RequestConfirm("Reimport the selected image(s) using their existing file path(s)?"s,
            [this, replaceImage]()
            {
               EditorUI &editor = m_editor;
               const vector<Texture *> sel = editor.m_multiSelImages;
               int missing = 0;
               for (Texture *const old : sel)
                  if (!std::filesystem::exists(old->GetFilePath()))
                     missing++;
                  else if (Texture *const tex = m_editor.m_table->ImportImage(old->GetFilePath(), old->m_name)) // On failure, the old image is kept
                     replaceImage(old, tex);
                  else
                     missing++;
               if (missing > 0)
                  m_actionStatus = std::to_string(missing) + " file(s) not found or failed to load";
               m_editor.m_table->SetNonUndoableDirty(SaveDirtyState::Dirty);
            });
      }

      ImGui::SameLine();
      ImGui::BeginDisabled(sel.size() != 1);
      if (ImGui::Button("Reimport From"))
      {
         RequestConfirm("Reimport the selected image using a different file path?"s,
            [this]()
            {
               if (m_editor.m_player->m_playfieldWnd == nullptr)
                  return;
               m_pendingImageReimportTarget = m_editor.m_multiSelImages.empty() ? nullptr : m_editor.m_multiSelImages.back();
               m_pendingImageReimport = std::make_shared<string>();
               const SDL_DialogFileFilter filters[] = { { "Image Files", "bmp;jpg;jpeg;png;tga;webp;exr;hdr" } };
               const string dir = PathToUTF8(PathFromString(g_settingsService.GetAppSettings().GetRecentDir_ImageDir())); // SDL expects UTF-8
               SDL_ShowOpenFileDialog(
                  [](void *userdata, const char *const *filelist, int filter)
                  {
                     auto *res = static_cast<std::shared_ptr<string> *>(userdata);
                     if (filelist != nullptr && filelist[0] != nullptr)
                        **res = filelist[0];
                     delete res;
                  },
                  new std::shared_ptr<string>(m_pendingImageReimport), //
                  m_editor.m_player->m_playfieldWnd->GetCore(), filters, 1, dir.empty() ? nullptr : dir.c_str(), false);
            });
      }
      ImGui::EndDisabled();

      ImGui::SameLine();
      ImGui::BeginDisabled(table->m_vimage.empty());
      if (ImGui::Button("Reimport All"))
      {
         const vector<Texture *> all = table->m_vimage;
         int missing = 0;
         for (Texture *const old : all)
            if (!std::filesystem::exists(old->GetFilePath()))
               missing++;
            else if (Texture *const tex = table->ImportImage(old->GetFilePath(), old->m_name)) // On failure, the old image is kept
               replaceImage(old, tex);
            else
               missing++;
         if (missing > 0)
            m_actionStatus = std::to_string(missing) + " file(s) not found or failed to load";
         table->SetNonUndoableDirty(SaveDirtyState::Dirty);
      }
      ImGui::EndDisabled();

      ImGui::EndDisabled();

      // --
      ImGui::Checkbox("Use names on export", &m_exportUseNames);
      if (!m_actionStatus.empty())
         ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", m_actionStatus.c_str());
      props.EndSection();
   }
}

void PropertiesPanel::SoundActions(PropertyPane &props)
{
   EditorUI &editor = m_editor;
   PinTable *const table = editor.m_table;
   vector<VPX::Sound *> &sel = editor.m_multiSelSounds;

   // Apply asynchronous file dialog results
   if (m_pendingSoundImport && !m_pendingSoundImport->empty())
   {
      vector<VPX::Sound *> imported;
      for (const string &file : *m_pendingSoundImport)
         if (VPX::Sound *const sound = table->ImportSound(PathFromUTF8(file)))
            imported.push_back(sound);
      g_settingsService.GetAppSettings().SetRecentDir_SoundDir(PathToString(PathFromUTF8(m_pendingSoundImport->front()).parent_path()), false);
      m_pendingSoundImport = nullptr;
      if (!imported.empty())
      {
         sel = imported;
         editor.m_selection = Selection(imported.back());
         editor.m_outlinerSoundAnchor = imported.back();
         table->SetNonUndoableDirty(SaveDirtyState::Dirty);
      }
      else
         m_actionStatus = "Failed to import the selected file(s)"s;
   }
   if (m_pendingSoundReimport && !m_pendingSoundReimport->empty())
   {
      const string file = *m_pendingSoundReimport;
      m_pendingSoundReimport = nullptr;
      if (m_pendingSoundReimportTarget != nullptr)
      {
         table->ReImportSound(m_pendingSoundReimportTarget, PathFromUTF8(file));
         table->SetNonUndoableDirty(SaveDirtyState::Dirty);
      }
      m_pendingSoundReimportTarget = nullptr;
      g_settingsService.GetAppSettings().SetRecentDir_SoundDir(PathToString(PathFromUTF8(file).parent_path()), false);
   }
   if (m_pendingSoundExport && !m_pendingSoundExport->empty())
   {
      const std::filesystem::path path = PathFromUTF8(*m_pendingSoundExport);
      m_pendingSoundExport = nullptr;
      // Like the Win32 dialog: with multiple selected sounds, only the folder of the picked file is used
      int failed = 0;
      for (VPX::Sound *sound : m_pendingSoundExportSel)
      {
         const std::filesystem::path file = (m_pendingSoundExportSel.size() > 1)
            ? path.parent_path() / (m_exportUseNames ? (PathFromString(sound->GetName()) += sound->GetImportPath().extension()) : sound->GetImportPath().filename())
            : path;
         if (!table->ExportSound(sound, file))
            failed++;
      }
      m_pendingSoundExportSel.clear();
      if (failed > 0)
         m_actionStatus = "Failed to export "s + std::to_string(failed) + " sound(s)"s;
      g_settingsService.GetAppSettings().SetRecentDir_SoundDir(PathToString(path.parent_path()), false);
   }

   if (props.BeginSection("Actions"s))
   {
      const bool canImport = editor.m_player->m_playfieldWnd != nullptr;
      ImGui::BeginDisabled(!canImport);
      if (ImGui::Button("Import"))
      {
         m_pendingSoundImport = std::make_shared<vector<string>>();
         const SDL_DialogFileFilter filters[] = { { "Sound Files", "wav;ogg;opus;mp3;flac" } };
         const string dir = PathToUTF8(PathFromString(g_settingsService.GetAppSettings().GetRecentDir_SoundDir())); // SDL expects UTF-8
         SDL_ShowOpenFileDialog(
            [](void *userdata, const char *const *filelist, int filter)
            {
               auto *res = static_cast<std::shared_ptr<vector<string>> *>(userdata);
               if (filelist != nullptr)
                  for (int i = 0; filelist[i] != nullptr; i++)
                     (**res).push_back(filelist[i]);
               delete res;
            },
            new std::shared_ptr<vector<string>>(m_pendingSoundImport), //
            editor.m_player->m_playfieldWnd->GetCore(), filters, 1, dir.empty() ? nullptr : dir.c_str(), true);
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(sel.empty());
      if (ImGui::Button("Reimport"))
      {
         RequestConfirm("Reimport the selected sound(s) using their existing file path(s)?"s,
            [this]()
            {
               int missing = 0;
               for (VPX::Sound *const sound : m_editor.m_multiSelSounds)
                  if (std::filesystem::exists(sound->GetImportPath()))
                     m_editor.m_table->ReImportSound(sound, sound->GetImportPath());
                  else
                     missing++;
               if (missing > 0)
                  m_actionStatus = std::to_string(missing) + " file(s) not found"s;
               m_editor.m_table->SetNonUndoableDirty(SaveDirtyState::Dirty);
            });
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(sel.size() != 1);
      if (ImGui::Button("Reimport From"))
      {
         RequestConfirm("Reimport the selected sound using a different file path?"s,
            [this]()
            {
               if (m_editor.m_player->m_playfieldWnd == nullptr)
                  return;
               m_pendingSoundReimportTarget = m_editor.m_multiSelSounds.empty() ? nullptr : m_editor.m_multiSelSounds.back();
               m_pendingSoundReimport = std::make_shared<string>();
               const SDL_DialogFileFilter filters[] = { { "Sound Files", "wav;ogg;opus;mp3;flac" } };
               const string dir = PathToUTF8(PathFromString(g_settingsService.GetAppSettings().GetRecentDir_SoundDir())); // SDL expects UTF-8
               SDL_ShowOpenFileDialog(
                  [](void *userdata, const char *const *filelist, int filter)
                  {
                     auto *res = static_cast<std::shared_ptr<string> *>(userdata);
                     if (filelist != nullptr && filelist[0] != nullptr)
                        **res = filelist[0];
                     delete res;
                  },
                  new std::shared_ptr<string>(m_pendingSoundReimport), //
                  m_editor.m_player->m_playfieldWnd->GetCore(), filters, 1, dir.empty() ? nullptr : dir.c_str(), false);
            });
      }
      ImGui::EndDisabled();

      ImGui::BeginDisabled(sel.empty() || !canImport);
      if (ImGui::Button("Export"))
      {
         m_pendingSoundExportSel = sel;
         m_pendingSoundExport = std::make_shared<string>();
         const SDL_DialogFileFilter filters[] = { { "Sound Files", "wav;ogg;opus;mp3;flac" } };
         const string dir = PathToUTF8(PathFromString(g_settingsService.GetAppSettings().GetRecentDir_SoundDir())); // SDL expects UTF-8
         SDL_ShowSaveFileDialog(
            [](void *userdata, const char *const *filelist, int filter)
            {
               auto *res = static_cast<std::shared_ptr<string> *>(userdata);
               if (filelist != nullptr && filelist[0] != nullptr)
                  **res = filelist[0];
               delete res;
            },
            new std::shared_ptr<string>(m_pendingSoundExport), //
            editor.m_player->m_playfieldWnd->GetCore(), filters, 1, dir.empty() ? nullptr : dir.c_str());
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(sel.empty());
      if (ImGui::Button("Toggle Table/Backglass"))
      {
         for (VPX::Sound *const sound : sel)
            sound->SetOutputTarget((sound->GetOutputTarget() != VPX::SNDOUT_BACKGLASS) ? VPX::SNDOUT_BACKGLASS : VPX::SNDOUT_TABLE);
         table->SetNonUndoableDirty(SaveDirtyState::Dirty);
      }
      ImGui::SameLine();
      if (ImGui::Button("Delete"))
      {
         RequestConfirm("Delete the selected sound(s)?"s,
            [this]()
            {
               EditorUI &editor = m_editor;
               for (VPX::Sound *const sound : editor.m_multiSelSounds)
                  editor.m_table->RemoveSound(sound);
               editor.m_multiSelSounds.clear();
               editor.m_outlinerSoundAnchor = nullptr;
               editor.m_selection = Selection();
               editor.m_table->SetNonUndoableDirty(SaveDirtyState::Dirty);
            });
      }
      ImGui::EndDisabled();

      ImGui::Checkbox("Use names on export", &m_exportUseNames);
      if (!m_actionStatus.empty())
         ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", m_actionStatus.c_str());
      props.EndSection();
   }
}

void PropertiesPanel::MaterialActions(PropertyPane &props)
{
   EditorUI &editor = m_editor;
   PinTable *const table = editor.m_table;
   vector<Material *> &sel = editor.m_multiSelMaterials;

   // Apply asynchronous file dialog results
   if (m_pendingMaterialImport && !m_pendingMaterialImport->empty())
   {
      const string file = *m_pendingMaterialImport;
      m_pendingMaterialImport = nullptr;
      std::ifstream f(PathFromUTF8(file), std::ios::binary);
      int version = 0, count = 0;
      f.read(reinterpret_cast<char *>(&version), sizeof(version));
      if (!f || version != MATERIAL_VERSION)
         m_actionStatus = "Materials are not compatible with this version!"s;
      else
      {
         f.read(reinterpret_cast<char *>(&count), sizeof(count));
         for (int i = 0; i < count && f; i++)
         {
            SaveMaterial mat;
            float elasticity = 0.f, elasticityFalloff = 0.f, friction = 0.f, scatterAngle = 0.f;
            f.read(reinterpret_cast<char *>(&mat), sizeof(SaveMaterial));
            f.read(reinterpret_cast<char *>(&elasticity), sizeof(float));
            f.read(reinterpret_cast<char *>(&elasticityFalloff), sizeof(float));
            f.read(reinterpret_cast<char *>(&friction), sizeof(float));
            f.read(reinterpret_cast<char *>(&scatterAngle), sizeof(float));
            if (!f)
               break;
            Material *const pmat = new Material(mat.bIsMetal ? Material::METAL : Material::BASIC, mat.fWrapLighting, mat.fRoughness, dequantizeUnsigned<8>(mat.fGlossyImageLerp),
               dequantizeUnsigned<8>(mat.fThickness), mat.fEdge, dequantizeUnsigned<7>(mat.bOpacityActive_fEdgeAlpha >> 1), mat.fOpacity, mat.cBase, mat.cGlossy, mat.cClearcoat,
               !!(mat.bOpacityActive_fEdgeAlpha & 1), 0.f, 1.f, 0, 1.f, elasticity, elasticityFalloff, friction, scatterAngle, 0xFFFFFFFF);
            pmat->m_name = string_from_utf8_or_cp1252(mat.szName, strnlen(mat.szName, std::size(mat.szName))); // May not be null terminated in a corrupted file
            table->AddMaterial(pmat);
            sel.push_back(pmat);
            editor.m_selection = Selection(pmat);
            editor.m_outlinerMaterialAnchor = pmat;
         }
         table->SetNonUndoableDirty(SaveDirtyState::Dirty);
         g_settingsService.GetAppSettings().SetRecentDir_MaterialDir(PathToString(PathFromUTF8(file).parent_path()), false);
      }
   }
   if (m_pendingMaterialExport && !m_pendingMaterialExport->empty())
   {
      const string file = *m_pendingMaterialExport;
      m_pendingMaterialExport = nullptr;
      std::ofstream f(PathFromUTF8(file), std::ios::binary | std::ios::trunc);
      if (!f)
         m_actionStatus = "Failed to export materials"s;
      else
      {
         constexpr int mv = MATERIAL_VERSION;
         const int count = static_cast<int>(m_pendingMaterialExportSel.size());
         f.write(reinterpret_cast<const char *>(&mv), sizeof(mv));
         f.write(reinterpret_cast<const char *>(&count), sizeof(count));
         for (const Material *const pmat : m_pendingMaterialExportSel)
         {
            SaveMaterial mat;
            mat.cBase = pmat->m_cBase;
            mat.cGlossy = pmat->m_cGlossy;
            mat.cClearcoat = pmat->m_cClearcoat;
            mat.fRoughness = pmat->m_fRoughness;
            mat.fGlossyImageLerp = quantizeUnsigned<8>(clamp(pmat->m_fGlossyImageLerp, 0.f, 1.f));
            mat.fThickness = quantizeUnsigned<8>(clamp(pmat->m_fThickness, 0.f, 1.f));
            mat.fEdge = pmat->m_fEdge;
            mat.fWrapLighting = pmat->m_fWrapLighting;
            mat.bIsMetal = pmat->m_type == Material::METAL;
            mat.fOpacity = pmat->m_fOpacity;
            mat.bOpacityActive_fEdgeAlpha = pmat->m_bOpacityActive ? 1 : 0;
            mat.bOpacityActive_fEdgeAlpha |= quantizeUnsigned<7>(clamp(pmat->m_fEdgeAlpha, 0.f, 1.f)) << 1;
            strncpy_s(mat.szName, std::size(mat.szName), TruncateToUTF8Length(pmat->m_name, std::size(mat.szName) - 1).c_str()); // Do not split a UTF-8 character
            f.write(reinterpret_cast<const char *>(&mat), sizeof(SaveMaterial));
            f.write(reinterpret_cast<const char *>(&pmat->m_fElasticity), sizeof(float));
            f.write(reinterpret_cast<const char *>(&pmat->m_fElasticityFalloff), sizeof(float));
            f.write(reinterpret_cast<const char *>(&pmat->m_fFriction), sizeof(float));
            f.write(reinterpret_cast<const char *>(&pmat->m_fScatterAngle), sizeof(float));
         }
         m_pendingMaterialExportSel.clear();
         g_settingsService.GetAppSettings().SetRecentDir_MaterialDir(PathToString(PathFromUTF8(file).parent_path()), false);
      }
   }

   if (props.BeginSection("Actions"s))
   {
      if (ImGui::Button("Add"))
      {
         Material *const pmat = new Material();
         table->AddMaterial(pmat);
         sel.clear();
         sel.push_back(pmat);
         editor.m_selection = Selection(pmat);
         editor.m_outlinerMaterialAnchor = pmat;
         table->SetNonUndoableDirty(SaveDirtyState::Dirty);
      }

      ImGui::SameLine();
      ImGui::BeginDisabled(sel.empty());
      if (ImGui::Button("Clone"))
      {
         vector<Material *> clones;
         for (const Material *const mat : sel)
         {
            Material *const pmat = new Material(mat);
            table->AddMaterial(pmat);
            clones.push_back(pmat);
         }
         sel = clones;
         editor.m_selection = Selection(clones.back());
         editor.m_outlinerMaterialAnchor = clones.back();
         table->SetNonUndoableDirty(SaveDirtyState::Dirty);
      }
      ImGui::EndDisabled();

      ImGui::SameLine();
      ImGui::BeginDisabled(editor.m_player->m_playfieldWnd == nullptr);
      if (ImGui::Button("Import"))
      {
         m_pendingMaterialImport = std::make_shared<string>();
         const SDL_DialogFileFilter filters[] = { { "Material Files", "mat" } };
         const string dir = PathToUTF8(PathFromString(g_settingsService.GetAppSettings().GetRecentDir_MaterialDir())); // SDL expects UTF-8
         SDL_ShowOpenFileDialog(
            [](void *userdata, const char *const *filelist, int filter)
            {
               auto *res = static_cast<std::shared_ptr<string> *>(userdata);
               if (filelist != nullptr && filelist[0] != nullptr)
                  **res = filelist[0];
               delete res;
            },
            new std::shared_ptr<string>(m_pendingMaterialImport), //
            editor.m_player->m_playfieldWnd->GetCore(), filters, 1, dir.empty() ? nullptr : dir.c_str(), false);
      }
      ImGui::EndDisabled();

      ImGui::SameLine();
      ImGui::BeginDisabled(sel.empty() || editor.m_player->m_playfieldWnd == nullptr);
      if (ImGui::Button("Export"))
      {
         m_pendingMaterialExportSel = sel;
         m_pendingMaterialExport = std::make_shared<string>();
         const SDL_DialogFileFilter filters[] = { { "Material Files", "mat" } };
         const string dir = PathToUTF8(PathFromString(g_settingsService.GetAppSettings().GetRecentDir_MaterialDir())); // SDL expects UTF-8
         SDL_ShowSaveFileDialog(
            [](void *userdata, const char *const *filelist, int filter)
            {
               auto *res = static_cast<std::shared_ptr<string> *>(userdata);
               if (filelist != nullptr && filelist[0] != nullptr)
                  **res = filelist[0];
               delete res;
            },
            new std::shared_ptr<string>(m_pendingMaterialExport), //
            editor.m_player->m_playfieldWnd->GetCore(), filters, 1, dir.empty() ? nullptr : dir.c_str());
      }
      ImGui::EndDisabled();

      ImGui::SameLine();
      ImGui::BeginDisabled(sel.empty());
      if (ImGui::Button("Delete"))
      {
         RequestConfirm("Delete the selected material(s)?"s,
            [this]()
            {
               EditorUI &editor = m_editor;
               for (Material *const mat : editor.m_multiSelMaterials)
                  editor.m_table->RemoveMaterial(mat);
               editor.m_multiSelMaterials.clear();
               editor.m_outlinerMaterialAnchor = nullptr;
               editor.m_selection = Selection();
               editor.m_table->SetNonUndoableDirty(SaveDirtyState::Dirty);
            });
      }
      ImGui::EndDisabled();

      if (!m_actionStatus.empty())
         ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", m_actionStatus.c_str());
      props.EndSection();
   }
}
}
