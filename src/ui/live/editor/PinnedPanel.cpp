// license:GPLv3+

#include "core/stdafx.h"
#include "PinnedPanel.h"
#include "IconsLucide.h"
#include "EditorTheme.h"

#include "core/player.h"
#include "parts/Material.h"
#include "parts/primitive.h"
#include "physics/PhysicsEngine.h"
#include "ui/live/EditorUI.h"
#include "ui/live/LiveUI.h"

#include "PropertyPane.h"

#include <nlohmann/json.hpp>
#include <set>

namespace VPX::EditorUI
{

// Field groups pinned by the see-through quick set (property pane separators)
static const string kRefractionGroup = "Refractions"s;
static const string kSeeThroughGroup = "See-Through Surface"s;
static const string kBulbGroup = "Traced Insert Bulb"s;

static bool IsTracedInsert(const IEditable *part)
{
   return part != nullptr && part->GetItemType() == eItemPrimitive && static_cast<const Primitive *>(part)->IsTracedInsert();
}

static const char *PinKindName(PinKind kind)
{
   switch (kind)
   {
   case PinKind::Table: return "table";
   case PinKind::Material: return "material";
   default: return "part";
   }
}

static bool PinKindFromName(const string &name, PinKind &kind)
{
   if (name == "table")
      kind = PinKind::Table;
   else if (name == "material")
      kind = PinKind::Material;
   else if (name == "part")
      kind = PinKind::Part;
   else
      return false;
   return true;
}

static string ObjectId(PinKind kind, const string &name) { return string(PinKindName(kind)) + '\x1f' + name; }

string PinKey::Id() const { return ObjectId(kind, name) + '\x1f' + prop; }

PinnedPanel::PinnedPanel(EditorUI &editor)
   : m_editor(editor)
{
   ResetWorkspaces();
}

PinnedPanel::~PinnedPanel() = default;

bool PinnedPanel::IsPinned(const PinKey &key) const { return std::ranges::find(m_pins, key) != m_pins.end(); }

void PinnedPanel::Pin(const PinKey &key)
{
   if (!IsPinned(key))
      m_pins.push_back(key);
}

void PinnedPanel::Unpin(const PinKey &key) { std::erase(m_pins, key); }

bool PinnedPanel::NeedsDef(const string &id, const void *owner) const
{
   const auto it = m_defs.find(id);
   // Only part definitions depend on their owner (they may capture the part's EditorUIPart); table and material panes may be
   // drawn on the startup copy or the live table of an inspection session with the same field definitions
   return it == m_defs.end() || (it->second.key.kind == PinKind::Part && it->second.owner != owner);
}

void PinnedPanel::SetDef(const PinKey &key, const string &label, const string &group, const string &section, const void *owner, PinDrawFn &&draw, PinCopyFn &&copy, bool common)
{
   Def &def = m_defs[key.Id()];
   def.key = key;
   def.label = label;
   def.group = group;
   def.section = section;
   def.owner = owner;
   def.draw = std::move(draw);
   def.copy = std::move(copy);
   def.common = common;
   // Only a harvest records which fields an object shows: a panel registration (e.g. from the startup copy in an inspection
   // session, whose owner differs) keeps the def in the object's curated sections
   if (m_harvestStamp != 0)
   {
      def.stamp = m_harvestStamp;
      def.seq = m_harvestSeq++;
   }
}

unsigned PinnedPanel::EnsureHarvested(PinKind kind, const string &name, void **resolved)
{
   const void *owner = nullptr;
   void *const obj = Resolve(kind, name, &owner);
   if (resolved != nullptr)
      *resolved = obj;
   if (obj == nullptr || owner == nullptr)
      return 0;
   const string objId = ObjectId(kind, name);
   const auto harvested = m_harvested.find(objId);
   if (harvested == m_harvested.end() || harvested->second != owner)
      Harvest(kind, name);
   const auto stamp = m_harvestStamps.find(objId);
   return stamp == m_harvestStamps.end() ? 0 : stamp->second;
}

void PinnedPanel::InvalidateHarvest(PinKind kind, const string &name) { m_harvested.erase(ObjectId(kind, name)); }

void PinnedPanel::CollectDefs(PinKind kind, const string &name, unsigned stamp, vector<const Def *> &out) const
{
   out.clear();
   if (stamp == 0)
      return;
   const string prefix = ObjectId(kind, name) + '\x1f';
   for (auto it = m_defs.lower_bound(prefix); it != m_defs.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it)
      if (it->second.stamp == stamp && it->second.draw)
         out.push_back(&it->second);
   std::ranges::sort(out, [](const Def *a, const Def *b) { return a->seq < b->seq; });
}

bool PinnedPanel::HasBulbSet(const IEditable *part) const { return IsTracedInsert(part); }

void PinnedPanel::CollectCopyTargets(IEditable *source, const PinCopyFn &copy, vector<std::shared_ptr<EditorUIPart>> &out) const
{
   out.clear();
   if (source == nullptr || !copy || m_editor.IsInspectMode())
      return;
   for (const std::shared_ptr<EditorUIPart> &part : m_editor.m_multiSel)
   {
      IEditable *const target = part->GetEditable();
      if (target != source && copy(static_cast<void *>(source), static_cast<void *>(target), false))
         out.push_back(part);
   }
}

bool PinnedPanel::CanCopyToSelected(const PinKey &key) const
{
   if (key.kind != PinKind::Part || m_editor.m_multiSel.size() < 2)
      return false;
   const Def *const def = FindDef(key);
   const void *owner = nullptr;
   IEditable *const source = static_cast<IEditable *>(Resolve(PinKind::Part, key.name, &owner));
   if (def == nullptr || !def->copy || source == nullptr)
      return false;
   vector<std::shared_ptr<EditorUIPart>> targets;
   CollectCopyTargets(source, def->copy, targets);
   return !targets.empty();
}

bool PinnedPanel::CopyToSelected(const PinKey &key)
{
   // Called from a field's menu while a pane is drawn (inside the panel's per-frame undo record): runs later, see FlushPendingCopy
   if (!CanCopyToSelected(key))
      return false;
   m_pendingCopy = key;
   m_hasPendingCopy = true;
   return true;
}

void PinnedPanel::ApplyBulbToAllTraced(Primitive *source, bool startup)
{
   EditorUI &editor = m_editor;
   vector<Primitive *> targets;
   for (IEditable *const other : editor.m_table->GetParts())
      if (other != source && IsTracedInsert(other))
         targets.push_back(static_cast<Primitive *>(other));
   if (startup) // The pane showed the startup table: the startup counterparts are the ones to read and write (what 'Save' writes to disk)
   {
      source = static_cast<Primitive *>(editor.m_table->GetStartupFromLive<IEditable>(source));
      for (Primitive *&target : targets)
         target = static_cast<Primitive *>(editor.m_table->GetStartupFromLive<IEditable>(target));
      std::erase(targets, nullptr);
      if (source == nullptr)
         return;
   }
   if (targets.empty())
   {
      editor.m_liveUI.PushNotification("No other traced inserts in this table"s, 2500);
      return;
   }
   const bool inspect = editor.IsInspectMode() || startup; // Inspection edits are not undoable: they are tracked for 'Save' like the other live edits
   if (!inspect)
   {
      editor.m_undo.BeginUndo();
      for (Primitive *const target : targets)
         editor.m_undo.MarkForUndo(target);
   }
   for (Primitive *const target : targets)
   {
      target->m_d.m_bulbDepth = source->m_d.m_bulbDepth;
      target->m_d.m_bulbScale = source->m_d.m_bulbScale;
      target->m_d.m_bulbBrightness = source->m_d.m_bulbBrightness;
      target->m_d.m_bulbStretchX = source->m_d.m_bulbStretchX;
      target->m_d.m_bulbStretchY = source->m_d.m_bulbStretchY;
      target->m_d.m_bulbOffsetX = source->m_d.m_bulbOffsetX;
      target->m_d.m_bulbOffsetY = source->m_d.m_bulbOffsetY;
   }
   const string label = "Apply bulb to " + std::to_string(targets.size()) + " traced insert" + (targets.size() == 1 ? ""s : "s"s);
   if (!inspect)
   {
      editor.m_undo.SetLabel("Apply Bulb: " + std::to_string(targets.size()) + " traced insert" + (targets.size() == 1 ? ""s : "s"s));
      editor.m_undo.EndUndo(); // Outside of any record: this one is kept and marks the table dirty
      editor.m_lastUndoPart = nullptr; // The next field edit is a step of its own
      editor.m_lastUndoId = 0;
   }
   for (Primitive *const target : targets)
   {
      if (startup)
         continue; // Startup objects are not rendered
      editor.m_renderer->ReinitRenderable(target->GetIRenderable()); // Read at draw time, so no physics update
      if (inspect)
         editor.m_liveEditedParts.insert(target);
   }
   editor.m_liveUI.PushNotification(label, 2500);
}

void PinnedPanel::FlushPendingCopy()
{
   // Undo / redo steps of materials deleted since (not undoable) are skipped, never applied to a material that reuses the name or address
   if (const int skipped = MaterialEditUndo::ConsumeSkipped(); skipped > 0)
      m_editor.m_liveUI.PushNotification(skipped == 1 ? "Undo/redo skipped: the material was deleted"s : "Undo/redo skipped for " + std::to_string(skipped) + " deleted materials", 3500);
   if (m_bulbSource != nullptr)
   {
      IEditable *const source = m_bulbSource;
      m_bulbSource = nullptr;
      ApplyBulbToAllTraced(static_cast<Primitive *>(source), m_bulbStartup);
   }
   if (!m_hasPendingCopy)
      return;
   m_hasPendingCopy = false;
   const PinKey key = m_pendingCopy;
   EditorUI &editor = m_editor;
   const Def *const def = FindDef(key);
   const void *owner = nullptr;
   IEditable *const source = key.kind == PinKind::Part ? static_cast<IEditable *>(Resolve(PinKind::Part, key.name, &owner)) : nullptr;
   if (def == nullptr || !def->copy || source == nullptr)
      return;
   vector<std::shared_ptr<EditorUIPart>> targets;
   CollectCopyTargets(source, def->copy, targets);
   if (targets.empty())
      return;
   // One undo step for all the parts: they are snapshotted before the value is written
   editor.m_undo.BeginUndo();
   for (const std::shared_ptr<EditorUIPart> &part : targets)
      editor.m_undo.MarkForUndo(part->GetEditable());
   for (const std::shared_ptr<EditorUIPart> &part : targets)
      def->copy(static_cast<void *>(source), static_cast<void *>(part->GetEditable()), true);
   editor.m_undo.SetFieldLabel("Copy to Selected: " + (def->label.empty() ? key.prop : def->label));
   editor.m_undo.EndUndo(); // Outside of any record: this one is kept and marks the table dirty
   editor.m_lastUndoPart = nullptr; // The next field edit is a step of its own
   editor.m_lastUndoId = 0;
   for (const std::shared_ptr<EditorUIPart> &part : targets)
   {
      editor.m_renderer->ReinitRenderable(part->GetEditable()->GetIRenderable());
      editor.m_player->m_physics->Update(part->GetEditable());
      part->SyncVisibilityOverrides();
   }
   editor.m_liveUI.PushNotification("Copied '"s + (def->label.empty() ? key.prop : def->label) + "' to " + std::to_string(targets.size()) + (targets.size() == 1 ? " part"s : " parts"s), 2500);
}

void PinnedPanel::ApplyToSelected(IEditable *source, const PinCopyFn &copy)
{
   EditorUI &editor = m_editor;
   if (!m_applyArmed || source == nullptr || !copy || editor.m_multiSel.size() < 2 || editor.IsInspectMode())
      return;
   vector<std::shared_ptr<EditorUIPart>> targets;
   CollectCopyTargets(source, copy, targets);
   for (const std::shared_ptr<EditorUIPart> &part : targets)
   {
      copy(static_cast<void *>(source), static_cast<void *>(part->GetEditable()), true);
      editor.m_renderer->ReinitRenderable(part->GetEditable()->GetIRenderable());
      editor.m_player->m_physics->Update(part->GetEditable());
      part->SyncVisibilityOverrides();
   }
}

bool PinnedPanel::HasSeeThroughSet(const IEditable *part) const
{
   if (part == nullptr || part->GetItemType() != eItemPrimitive)
      return false;
   if (IsTracedInsert(part)) // The quick set of a traced insert also holds its bulb fields
      return true;
   const Primitive *const prim = static_cast<const Primitive *>(part);
   if (!prim->m_d.m_szRefractionProbe.empty())
      return true;
   const Material *const mat = prim->m_d.m_szMaterial.empty() ? nullptr : m_editor.m_table->GetMaterial(prim->m_d.m_szMaterial);
   return mat != nullptr && !m_editor.m_table->IsDummyMaterial(mat) && mat->m_type == Material::PBR;
}

void PinnedPanel::PinSeeThroughSet(const IEditable *part)
{
   if (!HasSeeThroughSet(part))
      return;
   const Primitive *const prim = static_cast<const Primitive *>(part);
   m_groupRequests.push_back({ PinKind::Part, part->GetName(), kRefractionGroup });
   if (prim->IsTracedInsert())
      m_groupRequests.push_back({ PinKind::Part, part->GetName(), kBulbGroup });
   if (!prim->m_d.m_szMaterial.empty())
      PinMaterialSeeThrough(prim->m_d.m_szMaterial);
}

void PinnedPanel::PinBulbSet(const IEditable *part)
{
   if (!HasBulbSet(part))
      return;
   m_groupRequests.push_back({ PinKind::Part, part->GetName(), kBulbGroup });
}

void PinnedPanel::PinMaterialSeeThrough(const string &materialName)
{
   const Material *const mat = m_editor.m_table->GetMaterial(materialName);
   if (mat == nullptr || m_editor.m_table->IsDummyMaterial(mat) || mat->m_type != Material::PBR)
      return;
   m_groupRequests.push_back({ PinKind::Material, materialName, kSeeThroughGroup });
}

void PinnedPanel::SaveTo(PinTable *table) const
{
   if (table == nullptr)
      return;
   table->m_editorPins.clear();
   for (const PinKey &pin : m_pins)
      table->m_editorPins.push_back({ PinKindName(pin.kind), pin.name, pin.prop });
   if (m_activeWorkspace < 0)
      return; // Workspaces never used with this table: leave its data as it is
   std::array<Workspace, WorkspaceCount> workspaces = m_workspaces;
   CaptureWorkspace(workspaces[m_activeWorkspace]);
   if (m_workspacesPristine)
   {
      // Layout is only shown as active: it becomes the table's workspace as soon as the user changed the areas (editor types, widths,
      // visibility, sidebar), otherwise the table data is left as it is
      static const AreaLayout defaultLayout;
      const Workspace &now = workspaces[m_activeWorkspace];
      const bool untouched = now.layout == defaultLayout && !now.sidebar && now.sidebarTab == static_cast<int>(EditorUI::SidebarTab::Item)
         && now.outlinerMode == 0 && now.outlinerTypes == static_cast<int64_t>(OutlinerPanel::AllTypesMask) && now.sidebarWidth == 300.f;
      if (untouched)
         return;
   }
   // A width of 0 means 'keep the current one' when loading; what is written is the real width
   for (Workspace &ws : workspaces)
      if (ws.sidebarWidth <= 0.f)
         ws.sidebarWidth = m_editor.m_sidebarWidth;
   nlohmann::ordered_json list = nlohmann::ordered_json::array();
   for (const Workspace &ws : workspaces)
   {
      nlohmann::ordered_json pins = nlohmann::ordered_json::array();
      for (const PinKey &pin : ws.pins)
         pins.push_back({ { "kind", PinKindName(pin.kind) }, { "name", pin.name }, { "prop", pin.prop } });
      static constexpr const char *tabs[] = { "Item", "Favorites", "View" };
      // The layout tree, then the legacy keys derived from it for the older builds (they only know a left and a right area)
      bool outliner = true, properties = true;
      float outlinerWidth = AreaLayout::kDefaultOutlinerWidth, propertiesWidth = AreaLayout::kDefaultPropertiesWidth;
      int leftEditor = 0, rightEditor = 1;
      ws.layout.ToLegacy(outliner, properties, outlinerWidth, propertiesWidth, leftEditor, rightEditor);
      nlohmann::ordered_json entry = { { "name", ws.name }, { "sidebar", ws.sidebar }, { "sidebar_tab", tabs[clamp(ws.sidebarTab, 0, 2)] }, { "sidebar_width", ws.sidebarWidth },
         { "outliner_mode", ws.outlinerMode }, { "outliner_types", ws.outlinerTypes }, { "layout", nlohmann::ordered_json::parse(ws.layout.ToJson().dump()) }, { "outliner", outliner },
         { "properties", properties }, { "outliner_width", outlinerWidth }, { "properties_width", propertiesWidth }, { "left_editor", leftEditor },
         { "right_editor", rightEditor }, { "pins", pins } };
      if (ws.presetSizes)
      {
         // Never visited: written without sizes (no tree, widths 0), so that it still follows the current sizes when it is first applied
         entry.erase("layout");
         entry["outliner_width"] = 0.f;
         entry["properties_width"] = 0.f;
      }
      list.push_back(std::move(entry));
   }
   nlohmann::ordered_json doc = { { "active", workspaces[m_activeWorkspace].name }, { "workspaces", list } };
   table->m_editorWorkspaces = doc.dump();
}

void PinnedPanel::ResetWorkspaces()
{
   // Look-dev is the default tree without the Outliner (hidden), with the N-panel on its Favorites tab
   m_workspaces[0] = Workspace { "Layout"s, false, static_cast<int>(EditorUI::SidebarTab::Item) };
   m_workspaces[1] = Workspace { "Look-dev"s, true, static_cast<int>(EditorUI::SidebarTab::Favorites) };
   m_workspaces[1].layout = AreaLayout::FromLegacy(false, true, 0.f, 0.f, static_cast<int>(AreaType::Outliner), static_cast<int>(AreaType::Properties));
   m_workspaces[2] = Workspace { "Wiring"s, false, static_cast<int>(EditorUI::SidebarTab::Item) };
   // (the Wiring preset below, when it is a tree of its own, has sizes of its own)
   // Wiring: with kWiringBesideScript the script sits under the Properties editor, beside the fields being wired (Ben's call: the
   // recommended preset of the layout spec), otherwise it is Layout, as it was before the layout tree
   static constexpr bool kWiringBesideScript = true; // Ben 11-02: on
   if constexpr (kWiringBesideScript)
   {
      const nlohmann::json wiring = nlohmann::json::parse(R"({ "split": "vertical", "fixed": "first", "size": 200, "children": [ { "editor": "Outliner" },
         { "split": "vertical", "fixed": "second", "size": 360, "children": [ { "editor": "Viewport" },
            { "split": "horizontal", "fixed": "second", "size": 380, "children": [ { "editor": "Properties" }, { "editor": "Script" } ] } ] } ] })");
      AreaLayout layout;
      if (layout.FromJson(wiring))
      {
         m_workspaces[2].layout = layout;
         m_workspaces[2].presetSizes = false;
      }
   }
}

void PinnedPanel::CaptureWorkspace(Workspace &ws) const
{
   const EditorUI &editor = m_editor;
   ws.sidebar = editor.m_showSidebar;
   ws.sidebarTab = static_cast<int>(editor.m_sidebarTab);
   ws.sidebarWidth = editor.m_sidebarWidth;
   ws.outlinerMode = editor.m_outliner.GetDisplayMode();
   ws.outlinerTypes = editor.m_outliner.GetTypeMask();
   ws.layout = editor.m_areas;
   ws.presetSizes = false;
   ws.pins = m_pins;
}

void PinnedPanel::ApplyWorkspace(int index)
{
   if (index < 0 || index >= WorkspaceCount)
      return;
   if (m_workspacesPristine)
   {
      // First pick with this table (Layout was shown as active by default): the current areas and favorites become the Layout workspace,
      // and a target without its own favorites starts from them (nothing added so far is lost)
      CaptureWorkspace(m_workspaces[m_activeWorkspace]);
      if (m_workspaces[index].pins.empty())
         m_workspaces[index].pins = m_pins;
      m_workspacesPristine = false;
   }
   else if (m_activeWorkspace >= 0)
      CaptureWorkspace(m_workspaces[m_activeWorkspace]);
   else if (m_activeWorkspace == -1)
   {
      // First use with this table: the current areas and favorites become the Layout workspace, and a target without its own
      // favorites starts from them (nothing added so far is lost)
      CaptureWorkspace(m_workspaces[0]);
      if (m_workspaces[index].pins.empty())
         m_workspaces[index].pins = m_pins;
   }
   m_activeWorkspace = index;
   const Workspace &ws = m_workspaces[index];
   EditorUI &editor = m_editor;
   m_pins = ws.pins;
   editor.m_showSidebar = ws.sidebar;
   editor.m_sidebarTab = static_cast<EditorUI::SidebarTab>(clamp(ws.sidebarTab, 0, 2));
   if (ws.sidebarWidth > 0.f)
      editor.m_sidebarWidth = ws.sidebarWidth;
   if (ws.outlinerMode >= 0)
      editor.m_outliner.SetDisplayMode(ws.outlinerMode);
   if (ws.outlinerTypes >= 0)
      editor.m_outliner.SetTypeMask(static_cast<uint32_t>(ws.outlinerTypes));
   if (ws.presetSizes)
   {
      // A preset that was never visited keeps the sizes the user has now (widening Properties in Layout must not snap back in Look-dev): the
      // preset's visibility and editor types, on the current outliner and properties widths
      bool outliner = true, properties = true, curOutliner = true, curProperties = true;
      float outlinerWidth = 0.f, propertiesWidth = 0.f, curOutlinerWidth = 0.f, curPropertiesWidth = 0.f;
      int leftEditor = 0, rightEditor = 1, curLeft = 0, curRight = 1;
      ws.layout.ToLegacy(outliner, properties, outlinerWidth, propertiesWidth, leftEditor, rightEditor);
      editor.m_areas.ToLegacy(curOutliner, curProperties, curOutlinerWidth, curPropertiesWidth, curLeft, curRight);
      m_workspaces[index].layout = AreaLayout::FromLegacy(outliner, properties, curOutlinerWidth, curPropertiesWidth, leftEditor, rightEditor);
      m_workspaces[index].presetSizes = false;
   }
   editor.SetAreaLayout(m_workspaces[index].layout); // A copy: the workspace keeps its own tree until it is captured again
}

// Favorites of fields that no longer exist: the "Apply to all traced inserts" session toggle became a button on 11-02
static bool IsRetiredPin(const string &prop) { return prop == "Apply to all traced inserts"; }

void PinnedPanel::LoadWorkspaces(const string &json)
{
   ResetWorkspaces();
   m_activeWorkspace = -1;
   if (json.empty())
      return;
   const nlohmann::json doc = nlohmann::json::parse(json, nullptr, false);
   if (!doc.is_object())
      return;
   try // Hand edited files may hold values of the wrong type (nlohmann's value() throws then)
   {
      const string active = doc.value("active", ""s);
      if (const auto list = doc.find("workspaces"); list != doc.end() && list->is_array())
         for (const auto &entry : *list)
         {
            if (!entry.is_object())
               continue;
            const string name = entry.value("name", ""s);
            for (int i = 0; i < WorkspaceCount; i++)
            {
               Workspace &ws = m_workspaces[i];
               if (ws.name != name)
                  continue;
               ws.sidebar = entry.value("sidebar", entry.value("pinned", ws.sidebar));
               const string tab = entry.value("sidebar_tab", ""s);
               if (!tab.empty())
                  ws.sidebarTab = (tab == "Favorites" || tab == "Look") ? 1 : tab == "View" ? 2 : 0; // "Look": the tab's name before 11-02
               ws.sidebarWidth = entry.value("sidebar_width", ws.sidebarWidth);
               ws.outlinerMode = entry.value("outliner_mode", ws.outlinerMode);
               ws.outlinerTypes = entry.value("outliner_types", ws.outlinerTypes);
               // The "layout" tree when it is there and valid, otherwise the tree equivalent to the legacy keys (tables saved before the layout
               // tree: visibility, widths and editor types of the left and right areas, the defaults of the workspace for the missing ones)
               bool haveLayout = false;
               if (const auto layout = entry.find("layout"); layout != entry.end())
               {
                  AreaLayout parsed;
                  if (parsed.FromJson(*layout) && parsed.Validate()) // Logs the reason when it is rejected
                  {
                     ws.layout = parsed;
                     ws.presetSizes = false;
                     haveLayout = true;
                  }
               }
               // Legacy keys that only say "outliner and properties shown", with no widths or editor types, are the untouched pre-tree default: the
               // workspace keeps its current default tree (Wiring's script beside the Properties editor)
               // (builds before 11-02 also wrote widths 0 and the default editor types for a workspace never visited)
               const bool legacyDefault = !haveLayout && entry.value("outliner", true) && entry.value("properties", true) && entry.value("outliner_width", 0.f) <= 0.f
                  && entry.value("properties_width", 0.f) <= 0.f && entry.value("left_editor", 0) == 0 && entry.value("right_editor", 1) == 1;
               if (!haveLayout && !legacyDefault)
               {
                  bool outliner = true, properties = true;
                  float outlinerWidth = 0.f, propertiesWidth = 0.f;
                  int leftEditor = 0, rightEditor = 1;
                  ws.layout.ToLegacy(outliner, properties, outlinerWidth, propertiesWidth, leftEditor, rightEditor);
                  outliner = entry.value("outliner", outliner);
                  properties = entry.value("properties", properties);
                  outlinerWidth = entry.value("outliner_width", 0.f); // 0 or missing: the default width
                  propertiesWidth = entry.value("properties_width", 0.f);
                  leftEditor = entry.value("left_editor", leftEditor); // Missing in tables saved before the editor types: the defaults
                  rightEditor = entry.value("right_editor", rightEditor);
                  ws.layout = AreaLayout::FromLegacy(outliner, properties, outlinerWidth, propertiesWidth, leftEditor, rightEditor);
                  // Legacy keys without widths (0 or missing): the sizes are still unset
                  ws.presetSizes = outlinerWidth <= 0.f && propertiesWidth <= 0.f;
               }
               ws.pins.clear();
               if (const auto pins = entry.find("pins"); pins != entry.end() && pins->is_array())
                  for (const auto &pin : *pins)
                  {
                     PinKey key;
                     if (!pin.is_object() || !PinKindFromName(pin.value("kind", ""s), key.kind))
                        continue;
                     key.name = pin.value("name", ""s);
                     key.prop = pin.value("prop", ""s);
                     if (!key.prop.empty() && !IsRetiredPin(key.prop) && std::ranges::find(ws.pins, key) == ws.pins.end())
                        ws.pins.push_back(key);
                  }
               if (name == active)
                  m_activeWorkspace = i;
            }
         }
   }
   catch (const nlohmann::json::exception &)
   {
      ResetWorkspaces();
      m_activeWorkspace = -1;
   }
}

void PinnedPanel::SyncWithTable()
{
   // Pins belong to the base (startup) table: they are kept across the switch to its live copy in an inspection session
   const PinTable *const base = m_editor.m_table->m_liveBaseTable ? m_editor.m_table->m_liveBaseTable : m_editor.m_table;
   if (base == m_sourceTable)
      return;
   m_sourceTable = base;
   m_pins.clear();
   // The Properties editor's tab lists point into the definitions: drop them with the definitions
   PropertiesPanel::Curation &curation = m_editor.m_properties.m_curation;
   curation.active = false;
   curation.stamp = 0;
   for (auto &list : curation.tabs)
      list.clear();
   curation.look.clear();
   curation.transform.clear();
   curation.all.clear();
   m_editor.m_properties.m_materialCuration = PropertiesPanel::Curation();
   m_defs.clear();
   m_harvested.clear();
   m_groupRequests.clear();
   for (const PinTable::EditorPin &pin : base->m_editorPins)
   {
      PinKey key;
      if (!PinKindFromName(pin.kind, key.kind) || pin.prop.empty() || IsRetiredPin(pin.prop))
         continue;
      key.name = pin.name;
      key.prop = pin.prop;
      if (!IsPinned(key))
         m_pins.push_back(key);
   }
   // The active workspace's panels (its pins are the table's current pin list loaded above)
   LoadWorkspaces(base->m_editorWorkspaces);
   // Areas of a table without workspaces: the defaults; with workspaces: the active one's (its favorites are the current list
   // loaded above)
   m_editor.m_showSidebar = false;
   m_editor.m_sidebarTab = EditorUI::SidebarTab::Item;
   m_editor.m_sidebarWidth = 300.f;
   m_editor.SetAreaLayout(AreaLayout());
   m_editor.m_outliner.SetDisplayMode(0); // Outliner display mode and type filter of a table without saved workspaces: the defaults
   m_editor.m_outliner.SetTypeMask(OutlinerPanel::AllTypesMask);
   m_workspacesPristine = false;
   if (m_activeWorkspace == -1)
   {
      m_activeWorkspace = 0; // A table without saved workspaces starts on Layout (the defaults above), highlighted in the top bar
      m_workspacesPristine = true;
   }
   else if (m_activeWorkspace >= 0)
   {
      const vector<PinKey> pins = m_pins;
      const int active = m_activeWorkspace;
      m_activeWorkspace = -2; // Apply without capturing the previous table's areas
      m_workspaces[active].pins = pins;
      ApplyWorkspace(active);
   }
}

// Resolves a pinned object by name on the displayed table (the live copy in an inspection session, like the panel's 'Live' tab)
void *PinnedPanel::Resolve(PinKind kind, const string &name, const void **owner) const
{
   *owner = nullptr;
   PinTable *const table = m_editor.m_table;
   switch (kind)
   {
   case PinKind::Table: *owner = table; return static_cast<void *>(table);
   case PinKind::Material:
      for (Material *const mat : table->m_materials)
         if (mat->m_name == name)
         {
            *owner = mat;
            return static_cast<void *>(mat);
         }
      return nullptr;
   case PinKind::Part:
      for (IEditable *const part : table->GetParts())
         if (part->GetName() == name)
         {
            const auto it = m_editor.m_editableMap.find(part);
            *owner = it != m_editor.m_editableMap.end() ? it->second.get() : nullptr;
            return static_cast<void *>(part);
         }
      return nullptr;
   }
   return nullptr;
}

// Registers the field definitions of an object by drawing its property pane in a hidden window
bool PinnedPanel::Harvest(PinKind kind, const string &name)
{
   const void *owner = nullptr;
   void *const obj = Resolve(kind, name, &owner);
   m_harvested[ObjectId(kind, name)] = owner;
   if (obj == nullptr || owner == nullptr)
      return false;
   m_harvestStamp = ++m_harvestCounter;
   m_harvestSeq = 0;
   m_harvestStamps[ObjectId(kind, name)] = m_harvestStamp;
   PropertyPane props(m_editor.m_table);
   ApplyUnits(props);
   props.SetPinning(this, PropertyPane::PinMode::Harvest, kind == PinKind::Part ? owner : nullptr);
   ImGui::SetNextWindowPos(ImVec2(-100000.f, -100000.f));
   ImGui::SetNextWindowSize(ImVec2(PropertiesPanel::PaneWidth * m_editor.m_liveUI.GetDPI(), 400.f));
   ImGui::Begin("##PinHarvest", nullptr,
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
         | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground);
   switch (kind)
   {
   case PinKind::Table: m_editor.m_properties.TableProperties(props); break;
   case PinKind::Material: m_editor.m_properties.MaterialProperties(props, static_cast<Material *>(obj)); break;
   case PinKind::Part: static_cast<EditorUIPart *>(const_cast<void *>(owner))->UpdatePropertyPane(props); break;
   }
   ImGui::End();
   m_harvestStamp = 0;
   return true;
}

void PinnedPanel::ProcessGroupRequests()
{
   if (m_groupRequests.empty())
      return;
   const vector<GroupRequest> requests = std::move(m_groupRequests);
   m_groupRequests.clear();
   int found = 0;
   for (const GroupRequest &req : requests)
   {
      // Always refresh the object's definitions: the group may have fields never drawn yet (other material type, collapsed section,...)
      Harvest(req.kind, req.name);
      for (const auto &[id, def] : m_defs)
         if (def.key.kind == req.kind && def.key.name == req.name && def.group == req.group)
         {
            if (!IsPinned(def.key))
               m_pins.push_back(def.key);
            found++;
         }
   }
   if (found == 0)
      m_editor.m_liveUI.PushNotification("No fields found to pin"s, 4000);
}

void PinnedPanel::ApplyUnits(PropertyPane &props) const
{
   switch (m_editor.m_units)
   {
   case EditorUI::Units::VPX: props.SetLengthUnit(PropertyPane::Unit::VPLength); break;
   case EditorUI::Units::Metric: props.SetLengthUnit(PropertyPane::Unit::Millimeters); break;
   case EditorUI::Units::Imperial: props.SetLengthUnit(PropertyPane::Unit::Inches); break;
   }
}

void PinnedPanel::PinSelectionSeeThrough()
{
   const Selection &sel = m_editor.m_selection;
   if (sel.GetType() == Selection::S_EDITABLE)
      PinSeeThroughSet(sel.GetPart()->GetEditable());
   else if (sel.GetType() == Selection::S_MATERIAL && sel.GetMaterial() != nullptr)
      PinMaterialSeeThrough(sel.GetMaterial()->m_name);
}

// One log line per pin and reason, so an unresolved pin is diagnosable without flooding the log every frame
static void LogUnresolved(const PinKey &pin, const string &reason)
{
   static std::set<string> logged;
   if (logged.insert(pin.Id() + '\x1e' + reason).second)
      PLOGI << "Pinned property '" << pin.prop << "' of " << PinKindName(pin.kind) << " '" << pin.name << "' unresolved: " << reason;
}

void PinnedPanel::LoadFavoriteDefs()
{
   ProcessGroupRequests();
   // Objects of the pinned fields, in order of first appearance
   vector<std::pair<PinKind, string>> &objects = m_objects;
   objects.clear();
   for (const PinKey &pin : m_pins)
      if (std::ranges::find(objects, std::make_pair(pin.kind, pin.name)) == objects.end())
         objects.emplace_back(pin.kind, pin.name);

   // Load the missing or stale definitions (part re-init, live copy of an inspection session), one object per frame
   for (const auto &[kind, name] : objects)
   {
      const void *owner = nullptr;
      if (Resolve(kind, name, &owner) == nullptr || owner == nullptr)
         continue;
      const bool stale = std::ranges::any_of(m_pins,
         [&, kind = kind, &name = name](const PinKey &pin)
         {
            if (pin.kind != kind || pin.name != name)
               return false;
            const auto it = m_defs.find(pin.Id());
            return it == m_defs.end() || (kind == PinKind::Part && it->second.owner != owner);
         });
      if (!stale)
         continue;
      const auto harvested = m_harvested.find(ObjectId(kind, name));
      if (harvested != m_harvested.end() && harvested->second == owner)
         continue; // Already harvested from this owner: the remaining fields are not available (e.g. hidden for this material type)
      Harvest(kind, name);
      break;
   }

}

void PinnedPanel::RenderFavoriteRows(bool hovered)
{
   EditorUI &editor = m_editor;
   const vector<std::pair<PinKind, string>> &objects = m_objects;
   if (m_pins.empty())
   {
      static const char *const hints[] = { "Right-click a field in the Properties editor", "and pick \u201cAdd to Quick Favorites\u201d" };
      ImGui::Dummy(ImVec2(0.f, 8.f));
      for (const char *hint : hints)
      {
         ImGui::SetCursorPosX(ImGui::GetCursorPosX() + max(0.f, (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(hint).x) * 0.5f));
         ImGui::TextDisabled("%s", hint);
      }
   }

   PropertyPane props(editor.m_table);
   ApplyUnits(props);
   props.SetPinning(this, PropertyPane::PinMode::Overlay);
   props.SetLookSliders(true); // The overlay is a dial surface
   const vector<PinKey> pins = m_pins; // Rows may be removed while drawn
   vector<PinKey> &keys = m_itemKeys;
   for (const auto &[kind, name] : objects)
   {
      keys.clear();
      for (const PinKey &pin : pins)
         if (pin.kind == kind && pin.name == name)
            keys.push_back(pin);
      RenderObjectRows(props, kind, name, keys, true, hovered);
   }
}

void PinnedPanel::RenderObjectRows(PropertyPane &props, PinKind kind, const string &name, const vector<PinKey> &keys, bool removable, bool hovered)
{
   EditorUI &editor = m_editor;
   const bool inspect = editor.IsInspectMode();
   const void *owner = nullptr;
   void *const obj = Resolve(kind, name, &owner);
   ImGui::PushID(ObjectId(kind, name).c_str());
   const string header = (kind == PinKind::Table ? "Table"s : name) + (kind == PinKind::Material ? " (material)"s : string()) + "###obj";
   if (!props.BeginSection(header))
   {
      ImGui::PopID();
      return;
   }
   IEditable *const part = (kind == PinKind::Part && obj != nullptr) ? static_cast<IEditable *>(obj) : nullptr;
   // Undo of part edits outside of an inspection session, recorded like the properties panel (only while interacting, as it snapshots the part)
   const bool withUndo = part != nullptr && !inspect && hovered;
   // Alt held while editing: the fields also write to the other selected parts, which are snapshotted with the edited one
   const bool altApply = withUndo && ImGui::GetIO().KeyAlt && editor.m_multiSel.size() > 1 && (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsAnyItemActive());
   if (withUndo)
   {
      editor.m_undo.BeginUndo();
      editor.m_undo.MarkForUndo(part);
      if (altApply)
         for (const std::shared_ptr<EditorUIPart> &selected : editor.m_multiSel)
            editor.m_undo.MarkForUndo(selected->GetEditable());
   }
   m_applyArmed = altApply;
   // Material edits are undoable too (outside of an inspection session): the material is snapshotted before the rows are drawn
   MaterialEditUndo materialUndo(editor.m_undo, editor.m_table, (kind == PinKind::Material && !inspect) ? static_cast<Material *>(obj) : nullptr, MaterialEditUndo::SourceNPanel);
   props.SetPinOwner(owner);
   const int modifiedBefore = props.GetModifiedField();
   for (const PinKey &pin : keys)
   {
      props.FlushRow(); // Below the previous row when its label wrapped
      ImGui::PushID(pin.prop.c_str());
      if (removable)
      {
         // Small muted icon overlaid in the label's left margin (not a flow item, the row starts at the edge), shown while the row
         // is hovered or the button focused (the button itself is always submitted, so keyboard navigation still reaches it)
         const ImVec2 rowPos = ImGui::GetCursorScreenPos();
         const float frameHeight = ImGui::GetFrameHeight();
         const float buttonWidth = ImGui::GetFontSize() * 1.2f; // ~18 px reserved
         const bool rowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)
            && ImGui::IsMouseHoveringRect(rowPos, ImVec2(rowPos.x + ImGui::GetContentRegionAvail().x, rowPos.y + frameHeight), false);
         if (rowHovered) // Faint row highlight: the hover-remove icon visibly belongs to its row
            ImGui::GetWindowDrawList()->AddRectFilled(rowPos, ImVec2(rowPos.x + ImGui::GetContentRegionAvail().x, rowPos.y + frameHeight), EditorTheme::RowHover);
         const bool remove = ImGui::InvisibleButton("##Unpin", ImVec2(buttonWidth, frameHeight));
         if (remove)
            Unpin(pin);
         const bool itemHovered = ImGui::IsItemHovered();
         if (rowHovered || itemHovered || ImGui::IsItemFocused())
         {
            const ImVec2 iconSize = ImGui::CalcTextSize(ICON_LC_X);
            ImGui::GetWindowDrawList()->AddText(ImVec2(rowPos.x + (buttonWidth - iconSize.x) * 0.5f, rowPos.y + (frameHeight - iconSize.y) * 0.5f),
               itemHovered ? EditorTheme::Text : EditorTheme::IconMuted, ICON_LC_X);
         }
         ImGui::SetItemTooltip("Remove from Quick Favorites");
         ImGui::SetCursorScreenPos(rowPos);
      }
      const auto it = m_defs.find(pin.Id());
      if (obj == nullptr)
      {
         LogUnresolved(pin, "object not found on the displayed table"s);
         ImGui::TextDisabled("%s (object not found)", PropertyPane::PinKeyLabel(pin.prop).c_str());
      }
      else if (it == m_defs.end() || !it->second.draw || (kind == PinKind::Part && it->second.owner != owner))
      {
         LogUnresolved(pin,
            owner == nullptr ? "object found but has no editor UI part (a part without a part group is a live object in full edit mode)"s
            : it == m_defs.end() ? "object found, field not found in its harvested property pane (label mismatch or field not drawn for this part)"s
                                 : "field definition is stale (owner changed)"s);
         ImGui::TextDisabled("%s (not available)", PropertyPane::PinKeyLabel(pin.prop).c_str());
      }
      else
         it->second.draw(props, obj);
      ImGui::PopID();
   }
   props.EndSection();
   m_applyArmed = false;
   const int modified = props.GetModifiedField() != modifiedBefore ? props.GetModifiedField() : 0;
   if (materialUndo.Commit(modified, props.GetModifiedLabel()))
   {
      editor.m_lastUndoPart = nullptr; // The next part edit is a step of its own
      editor.m_lastUndoId = 0;
   }
   if (withUndo)
   {
      if (modified > 0)
         editor.m_undo.SetFieldLabel(props.GetModifiedLabel());
      editor.m_undo.EndUndo();
      // One step per drag / typed value (see PropertiesPanel); Alt editing has its own key, and the chain ends with the interaction
      const unsigned int editId = (altApply ? 0x800000u : 0u) | 0x4000u | static_cast<unsigned int>(modified);
      const bool continues = modified > 0 && editor.m_lastUndoPart == part && editor.m_lastUndoId == editId;
      const auto endChain = [&editor]()
      {
         if (editor.m_lastUndoId != 0x1000) // The gizmo's own marker is not ours to clear
         {
            editor.m_lastUndoPart = nullptr;
            editor.m_lastUndoId = 0;
         }
      };
      if (modified <= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsAnyItemActive())
         endChain();
      if (modified > 0 && !continues)
      {
         editor.m_lastUndoPart = part;
         editor.m_lastUndoId = editId;
         m_editRecordId = editor.m_undo.TopRecordId();
      }
      else
      {
         editor.m_undo.Discard();
         // Esc cancelled the edit: its step is dropped, if it is still the latest record
         if (continues && PropertyPane::ConsumeEditCancel() && editor.m_undo.HasUndo() && editor.m_undo.TopRecordId() == m_editRecordId)
         {
            editor.m_undo.Discard();
            endChain();
         }
      }
      const ImGuiID activeId = ImGui::GetActiveID();
      if (activeId != m_prevActiveId) // Another item took the interaction: a new edit
         endChain();
      m_prevActiveId = activeId;
   }
   if (modified > 0)
   {
      if (part != nullptr)
      {
         editor.m_renderer->ReinitRenderable(part->GetIRenderable());
         editor.m_player->m_physics->Update(part);
      }
      if (inspect)
      {
         // Same tracking as the panel's 'Live' tab: 'Save' copies these edits back to the startup table
         if (kind == PinKind::Material)
            editor.m_liveEditedMaterials.insert(static_cast<Material *>(obj));
         else if (part != nullptr)
            editor.m_liveEditedParts.insert(part);
      }
      else if (part == nullptr && kind != PinKind::Material) // Material edits are undo steps, which mark the table dirty
         editor.m_table->SetNonUndoableDirty(SaveDirtyState::Dirty);
   }
   ImGui::PopID();
}

void PinnedPanel::RenderFavoritesToolbar()
{
   const Selection &sel = m_editor.m_selection;
   const bool canPinSel = (sel.GetType() == Selection::S_EDITABLE && HasSeeThroughSet(sel.GetPart()->GetEditable()))
      || (sel.GetType() == Selection::S_MATERIAL && sel.GetMaterial() != nullptr && sel.GetMaterial()->m_type == Material::PBR);
   const float dpi = m_editor.m_liveUI.GetDPI();
   const float buttonHeight = 24.f * dpi;
   const float spacing = ImGui::GetStyle().ItemSpacing.x;
   ImGui::BeginDisabled(!canPinSel);
   if (ImGui::Button(ICON_LC_DROPLET " Add see-through set", ImVec2(max(1.f, ImGui::GetContentRegionAvail().x - buttonHeight - spacing), buttonHeight)))
      PinSelectionSeeThrough();
   ImGui::EndDisabled();
   ImGui::SetItemTooltip("Add the selection's see-through set to Quick Favorites (refraction fields of the part, its Traced Insert Bulb fields if it is a traced insert, and the See-Through Surface fields of its material)");
   ImGui::SameLine();
   ImGui::BeginDisabled(m_pins.empty());
   ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::V(EditorTheme::IconMuted));
   if (ImGui::Button(ICON_LC_TRASH_2 "##FavClear", ImVec2(buttonHeight, buttonHeight)))
      ImGui::OpenPopup("##PinClearAll");
   ImGui::PopStyleColor();
   ImGui::EndDisabled();
   ImGui::SetItemTooltip("Remove all Quick Favorites");
   if (ImGui::BeginPopup("##PinClearAll"))
   {
      ImGui::Text("Remove all %d Quick Favorites?", static_cast<int>(m_pins.size()));
      if (ImGui::Button("Remove all"))
      {
         ClearPins();
         ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel"))
         ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
   }
   ImGui::Dummy(ImVec2(0.f, max(0.f, 6.f * dpi - ImGui::GetStyle().ItemSpacing.y)));
}

void PinnedPanel::RenderItemTab(bool hovered)
{
   // The selection's Transform fields (the Properties editor's Transform tab, drawn from the same definitions)
   EditorUI &editor = m_editor;
   const PropertiesPanel &properties = editor.m_properties;
   // Nothing selected: an empty tab (like Blender's Item tab), not a second copy of the table's Look fields shown in Properties
   const auto &defs = properties.m_curation.tabs[static_cast<int>(PropertiesPanel::Tab::Transform)];
   if (!properties.m_curation.active || defs.empty() || properties.m_curation.obj == nullptr)
   {
      // Centred in the panel: icon above a dim message
      const char *const message = editor.m_selection.GetType() == Selection::S_NONE ? "Nothing selected" : "No transform for this selection";
      const ImVec2 avail = ImGui::GetContentRegionAvail();
      const ImVec2 iconSize = ImGui::CalcTextSize(ICON_LC_BOX);
      const ImVec2 textSize = ImGui::CalcTextSize(message);
      const float blockHeight = iconSize.y + ImGui::GetStyle().ItemSpacing.y + textSize.y;
      const float x0 = ImGui::GetCursorPosX();
      const float y0 = ImGui::GetCursorPosY();
      ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::V(EditorTheme::TextDim));
      ImGui::SetCursorPos(ImVec2(x0 + max(0.f, (avail.x - iconSize.x) * 0.5f), y0 + max(0.f, (avail.y - blockHeight) * 0.4f)));
      ImGui::TextUnformatted(ICON_LC_BOX);
      ImGui::SetCursorPosX(x0 + max(0.f, (avail.x - textSize.x) * 0.5f));
      ImGui::TextUnformatted(message);
      ImGui::PopStyleColor();
      if (editor.m_selection.GetType() == Selection::S_NONE)
      {
         ImGui::Dummy(ImVec2(0.f, 6.f));
         if (ImGui::Button("Frame All", ImVec2(ImGui::GetContentRegionAvail().x, 0.f)))
            editor.FrameAll();
         ImGui::SetItemTooltip("Home");
      }
      return;
   }
   ImGui::Dummy(ImVec2(0.f, 2.f)); // The name is the header of the section drawn below
   vector<PinKey> &keys = m_itemKeys;
   keys.clear();
   for (const Def *def : defs)
      keys.push_back(def->key);
   PropertyPane props(editor.m_table);
   ApplyUnits(props);
   props.SetPinning(this, PropertyPane::PinMode::Overlay);
   props.SetLookSliders(true);
   RenderObjectRows(props, properties.m_curation.kind, properties.m_curation.name, keys, false, hovered);
}

void PinnedPanel::RenderViewTab()
{
   EditorUI &editor = m_editor;
   PropertyPane props(editor.m_table);
   if (!props.BeginSection("View"s))
      return;
   static constexpr const char *cameras[] = { "Editor", "Desktop", "Cabinet", "Full Single Screen" };
   static constexpr int overrides[] = { BG_DESKTOP, BG_FULLSCREEN, BG_FSS };
   int current = -1;
   if (editor.m_camMode == ViewMode::EditorCam)
      current = 0;
   else
      for (int i = 0; i < 3; i++)
         if (editor.m_selection == Selection::Camera(i))
            current = i + 1;
   props.PropertyLabel("Camera"s);
   if (PropertyPane::BeginFlatCombo("##ViewCamera", current >= 0 ? cameras[current] : "Preview"))
   {
      for (int i = 0; i < 4; i++)
         if (ImGui::Selectable(cameras[i], i == current))
         {
            if (i == 0)
            {
               editor.m_table->SetViewSetupOverride(BG_DESKTOP);
               editor.m_table->SetViewSetupOverride(BG_INVALID);
               editor.m_camMode = ViewMode::EditorCam;
            }
            else
            {
               editor.SetSelection(Selection::Camera(i - 1));
               editor.m_camMode = ViewMode::PreviewCam;
               editor.m_table->SetViewSetupOverride(static_cast<ViewSetupID>(overrides[i - 1]));
            }
         }
      ImGui::EndCombo();
   }
   props.PropertyLabel("Perspective"s);
   PropertyPane::FlatCheckbox("##ViewPerspective", &editor.m_perspectiveCam);
   ImGui::SetItemTooltip("Numpad 5");
   props.PropertyLabel("Navigation Cube"s);
   PropertyPane::FlatCheckbox("##ViewNavCube", &editor.m_showNavCube);
   ImGui::Dummy(ImVec2(0.f, 4.f));
   const float width = ImGui::GetContentRegionAvail().x;
   ImGui::BeginDisabled(!editor.CanFrameSelection());
   if (ImGui::Button("Frame Selected", ImVec2(width, 0.f)))
      editor.FrameSelection();
   ImGui::EndDisabled();
   if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("%s", editor.CanFrameSelection() ? "F / Numpad ." : "Nothing selected");
   if (ImGui::Button("Frame All", ImVec2(width, 0.f)))
      editor.FrameAll();
   ImGui::SetItemTooltip("Home");
   props.EndSection();
}

void PinnedPanel::Render()
{
   EditorUI &editor = m_editor;
   if (editor.m_table->IsLocked())
      return;
   PropertyPane::ConsumeEditCancel(); // A cancelled edit nobody handled
   SyncWithTable();
   ProcessGroupRequests(); // Quick sets added from the Properties editor, whether or not the favorites are shown
   const bool lookShown = editor.m_showSidebar && editor.m_sidebarTab == EditorUI::SidebarTab::Favorites;
   if (lookShown || m_quickOpen || editor.m_openQuickFavorites)
      LoadFavoriteDefs(); // May harvest (hidden window): before the sidebar window is begun
   if (!editor.m_showSidebar || !editor.IsViewportVisible())
      return; // The N-panel belongs to the viewport area: not with a panel maximized over it
   // The Item tab shows the Properties editor's Transform list: keep it current when the Properties editor is hidden
   if (!editor.IsAreaEditorShown(EditorUI::AreaEditor::Properties) && editor.m_sidebarTab == EditorUI::SidebarTab::Item)
      editor.m_properties.PrepareCuration();

   // N-panel: a fixed area along the right edge of the viewport area, resized by its splitter
   const float dpi = editor.m_liveUI.GetDPI();
   const AreaRect &leaf = editor.m_viewportRect; // The viewport area, the N-panel being at its right edge
   const float width = editor.m_sidebarWidth * dpi;
   ImGui::SetNextWindowPos(ImVec2(leaf.x1 - width, leaf.y0));
   ImGui::SetNextWindowSize(ImVec2(width, leaf.Height()));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.f * dpi, 4.f * dpi));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
   ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
   ImGui::Begin("SIDEBAR", nullptr,
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus
         | ImGuiWindowFlags_NoNavFocus);
   const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem | ImGuiHoveredFlags_AllowWhenBlockedByPopup)
      || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsAnyItemActive());
   // Muted chip tabs: the active one is a raised grey, no overline, no bar border
   ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 4.f);
   ImGui::PushStyleVar(ImGuiStyleVar_TabBarBorderSize, 0.f);
   ImGui::PushStyleVar(ImGuiStyleVar_TabBarOverlineSize, 0.f);
   ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.f, 0.f, 0.f, 0.f));
   ImGui::PushStyleColor(ImGuiCol_TabHovered, EditorTheme::V(EditorTheme::TabHover));
   ImGui::PushStyleColor(ImGuiCol_TabSelected, EditorTheme::V(EditorTheme::TabActive));
   ImGui::PushStyleColor(ImGuiCol_TabDimmed, ImVec4(0.f, 0.f, 0.f, 0.f));
   ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, EditorTheme::V(EditorTheme::TabActive));
   ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, ImVec4(0.f, 0.f, 0.f, 0.f));
   ImGui::PushStyleColor(ImGuiCol_TabDimmedSelectedOverline, ImVec4(0.f, 0.f, 0.f, 0.f));
   const bool tabBar = ImGui::BeginTabBar("##SidebarTabs");
   if (tabBar)
   {
      static constexpr const char *names[] = { "Item", "Favorites", "View" };
      // A tab picked from elsewhere (search, workspace) is brought to front (ImGui applies it on the next frame); otherwise the
      // tab shown is the user's pick
      const bool external = m_shownTab != static_cast<int>(editor.m_sidebarTab);
      int shown = -1;
      for (int t = 0; t < 3; t++)
      {
         const EditorUI::SidebarTab tab = static_cast<EditorUI::SidebarTab>(t);
         // The tab label is drawn by BeginTabItem: active tab white, the others dim
         ImGui::PushStyleColor(ImGuiCol_Text, (editor.m_sidebarTab == tab) ? IM_COL32_WHITE : EditorTheme::TextDim);
         const bool tabOpen = ImGui::BeginTabItem(names[t], nullptr, (external && editor.m_sidebarTab == tab) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None);
         ImGui::PopStyleColor();
         if (tabOpen)
         {
            shown = t;
            ImGui::BeginChild("##SidebarContent");
            switch (tab)
            {
            case EditorUI::SidebarTab::Item: RenderItemTab(hovered); break;
            case EditorUI::SidebarTab::Favorites:
               RenderFavoritesToolbar();
               RenderFavoriteRows(hovered);
               break;
            case EditorUI::SidebarTab::View: RenderViewTab(); break;
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
         }
      }
      ImGui::EndTabBar();
      if (!external && shown >= 0)
         editor.m_sidebarTab = static_cast<EditorUI::SidebarTab>(shown);
      if (shown == static_cast<int>(editor.m_sidebarTab))
         m_shownTab = shown;
   }
   ImGui::PopStyleColor(7);
   ImGui::PopStyleVar(3);
   ImGui::End();
   ImGui::PopStyleVar(3);
}

bool PinnedPanel::ConsumedEscape()
{
   if (!m_escPending)
      return false;
   if (!ImGui::IsKeyDown(ImGuiKey_Escape))
      m_escPending = false; // Released: this is the frame of the release, still consumed
   return true;
}

void PinnedPanel::RenderQuickFavorites()
{
   // Q: Blender's Quick Favorites, here the favorite fields themselves, editable in the popup
   EditorUI &editor = m_editor;
   // Opens at the mouse; fixed width, height capped at 75% of the work area, kept 8 px inside it
   static ImVec2 s_openPos;
   static float s_lastHeight = 0.f;
   if (editor.m_openQuickFavorites)
   {
      editor.m_openQuickFavorites = false;
      s_openPos = ImGui::GetMousePos();
      s_lastHeight = 0.f;
      ImGui::OpenPopup("Quick Favorites##QuickFavorites");
   }
   const float dpi = editor.m_liveUI.GetDPI();
   const ImGuiViewport *const viewport = ImGui::GetMainViewport();
   const float width = 320.f * dpi;
   const float margin = 8.f * dpi;
   const float maxHeight = viewport->WorkSize.y * 0.75f;
   const float height = s_lastHeight > 0.f ? s_lastHeight : maxHeight; // Until measured, assume the worst
   ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.f), ImVec2(width, maxHeight));
   ImGui::SetNextWindowPos(ImVec2(clamp(s_openPos.x, viewport->WorkPos.x + margin, max(viewport->WorkPos.x + margin, viewport->WorkPos.x + viewport->WorkSize.x - width - margin)),
      clamp(s_openPos.y, viewport->WorkPos.y + margin, max(viewport->WorkPos.y + margin, viewport->WorkPos.y + viewport->WorkSize.y - height - margin))));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.f * dpi, 8.f * dpi));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f * dpi);
   m_quickOpen = ImGui::BeginPopup("Quick Favorites##QuickFavorites");
   ImGui::PopStyleVar(2);
   if (!m_quickOpen)
      return;
   s_lastHeight = ImGui::GetWindowSize().y;
   {
      // Section-style title: accent rule on the left, normal text
      ImGui::TextUnformatted("Quick Favorites");
      const ImVec2 titleMin = ImGui::GetItemRectMin(), titleMax = ImGui::GetItemRectMax();
      ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(titleMin.x - 6.f * dpi, titleMin.y), ImVec2(titleMin.x - 4.f * dpi, titleMax.y), EditorTheme::Accent);
   }
   ImGui::Separator();
   ImGui::Dummy(ImVec2(0.f, max(0.f, 6.f * dpi - ImGui::GetStyle().ItemSpacing.y)));
   if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
   {
      ImGui::CloseCurrentPopup();
      m_escPending = true;
   }
   // Undo snapshots only while interacting with the popup (as the sidebar)
   RenderFavoriteRows(ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsAnyItemActive()));
   if (ImGui::GetScrollMaxY() > 0.f && ImGui::GetScrollY() < ImGui::GetScrollMaxY() - 1.f)
   {
      // More rows below: fade the bottom edge
      const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
      const float fade = 28.f * dpi;
      const ImU32 bg = ImGui::GetColorU32(ImGuiCol_PopupBg), clear = bg & ~IM_COL32_A_MASK;
      ImGui::GetWindowDrawList()->AddRectFilledMultiColor(ImVec2(wp.x + 1.f, wp.y + ws.y - fade - 1.f), ImVec2(wp.x + ws.x - 1.f, wp.y + ws.y - 1.f), clear, clear, bg, bg);
   }
   ImGui::EndPopup();
}

}
