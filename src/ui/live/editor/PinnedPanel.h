// license:GPLv3+

#pragma once

#include "AreaLayout.h"

#include <array>
#include <functional>
#include <map>
#include <memory>

class IEditable;
class PinTable;

namespace VPX::EditorUI
{

class EditorUI;
class EditorUIPart;
class PropertyPane;

// Object a pinned property belongs to
enum class PinKind
{
   Table,
   Material,
   Part
};

// A pinned property: the object it belongs to (by kind and name, resolved on the displayed table each frame) and its
// property pane label. Names are used instead of pointers so that pins survive selection changes, part re-inits and
// the switch between the edited table and its live copy in an inspection session.
struct PinKey
{
   PinKind kind = PinKind::Part;
   string name; // Empty for the table
   string prop; // Property pane label of the field
   string Id() const;
   bool operator==(const PinKey &other) const { return kind == other.kind && name == other.name && prop == other.prop; }
};

// Draws a pinned field with a property pane on the resolved object (Material*, PinTable* or IEditable*)
using PinDrawFn = std::function<void(PropertyPane &, void *)>;

// Copies a field value between two parts (IEditable* passed as void*): tells whether the destination has the field (it is of the type the field
// belongs to) and, when `apply` is set, writes the source value to it. Used by "Copy to Selected" and the Alt apply-to-all
using PinCopyFn = std::function<bool(void *src, void *dst, bool apply)>;

// Quick Favorites (Blender's Q menu, here favorite fields) and the N-panel sidebar (Item / Favorites / View) that shows them in its
// Favorites tab. Favorites are added from the Properties editor (right click on a field), and drawn with the same property pane
// widgets and getter/setter definitions as the panel. The definitions are registered by the
// property pane when it draws a field (and cached by key); for objects never shown in the panel this session (pins
// loaded with the table, quick sets), they are harvested by running the object's pane in a hidden window.
class PinnedPanel
{
public:
   explicit PinnedPanel(EditorUI &editor);
   ~PinnedPanel();

   void Render(); // The N-panel sidebar, when shown (at the right edge of the viewport area)
   void RenderQuickFavorites(); // The Q popup, when opened
   bool ConsumedEscape(); // The Esc that closed the Q popup is not an editor Esc

   bool IsPinned(const PinKey &key) const;
   void Pin(const PinKey &key);
   void Unpin(const PinKey &key);
   void ClearPins() { m_pins.clear(); }
   bool HasPins() const { return !m_pins.empty(); }

   // Field definitions, registered by the property panes (owner identifies the definition's provider, see PropertyPane::SetPinning)
   struct Def
   {
      PinKey key;
      string label; // Label shown in the UI (key.prop is the legacy-stable storage key)
      string group; // Section or separator under which the field is drawn in the panel
      string section; // Section (collapsing header) of the field in the full sheet
      const void *owner = nullptr; // Part: its EditorUIPart (definitions may capture it), otherwise the object itself
      PinDrawFn draw;
      PinCopyFn copy; // Part fields: copy of the value to another part (null for the fields that cannot be copied)
      bool common = false; // Registered while the pane's tier was Common (PropertyPane::SetTier)
      int seq = 0; // Draw order in the harvest that registered it
      unsigned stamp = 0; // Harvest that registered it (0: registered by the panel outside of a harvest)
   };
   bool NeedsDef(const string &id, const void *owner) const;
   void SetDef(const PinKey &key, const string &label, const string &group, const string &section, const void *owner, PinDrawFn &&draw, PinCopyFn &&copy, bool common);
   bool IsHarvesting() const { return m_harvestStamp != 0; }

   // Curated (Look / Transform) sections of the properties panel: the fields of an object, as registered by its latest harvest
   // Harvests the object if it was never harvested from its current owner; returns the stamp of its latest harvest (0 if unresolved)
   unsigned EnsureHarvested(PinKind kind, const string &name, void **resolved = nullptr);
   void InvalidateHarvest(PinKind kind, const string &name);
   // Definitions registered by the given harvest of an object, in draw order
   void CollectDefs(PinKind kind, const string &name, unsigned stamp, vector<const Def *> &out) const;
   // Every known definition (search palette index)
   const std::map<string, Def> &GetDefs() const { return m_defs; }
   const Def *FindDef(const PinKey &key) const
   {
      const auto it = m_defs.find(key.Id());
      return it == m_defs.end() ? nullptr : &it->second;
   }

   // Copy to Selected (Blender's field context menu): writes the value of a part field to the other selected parts having the same field (same
   // field of a compatible part type), as one undo step. Not available in inspection sessions
   bool CanCopyToSelected(const PinKey &key) const;
   bool CopyToSelected(const PinKey &key); // Queues the copy (the menu is drawn inside the panels' undo records)
   void FlushPendingCopy(); // Runs the queued copy (and bulb apply), as one undo step: to be called outside of any undo record (after the areas are rendered)
   // "Apply to all traced inserts" button: queues the copy of the traced insert's bulb values to the other traced inserts (run by FlushPendingCopy)
   void RequestApplyBulb(IEditable *source, bool startup)
   {
      m_bulbSource = source;
      m_bulbStartup = startup;
   }
   // Alt apply-to-all: a part field was just changed through its pane, copy its new value to the other selected parts. Only acts while armed by the
   // caller, which has begun an undo record and marked all selected parts in it (a snapshot is taken before the pane changes the field)
   void ArmApplyToSelected(bool armed) { m_applyArmed = armed; }
   void ApplyToSelected(IEditable *source, const PinCopyFn &copy);

   // "See-through set": a primitive's refraction fields and its material's See-Through Surface fields
   bool HasSeeThroughSet(const IEditable *part) const;
   void PinSeeThroughSet(const IEditable *part);
   void PinMaterialSeeThrough(const string &materialName);
   // "Bulb set": the Traced Insert Bulb fields (sliders) of a traced insert; also part of its see-through set
   bool HasBulbSet(const IEditable *part) const;
   void PinBulbSet(const IEditable *part);

   // Persistence: the pin list is stored on the (startup) table, written to its table.json
   void SaveTo(PinTable *table) const;

   // Workspaces (Blender's top bar tabs): Layout, Look-dev and Wiring presets of the areas (the area layout tree: editor types, widths and
   // visibility of the areas; N-panel and its tab, outliner display mode and type filter) with their own Quick Favorites, saved with the table
   // ("editor_workspaces" in table.json, beside "editor_pins" which stays the current favorites list)
   struct Workspace
   {
      string name;
      bool sidebar = false;
      int sidebarTab = 0; // EditorUI::SidebarTab
      float sidebarWidth = 0.f; // 0: keep the current width
      int outlinerMode = -1; // -1: keep the current one
      int64_t outlinerTypes = -1;
      AreaLayout layout; // Saved as "layout", and as the legacy outliner / properties / widths / left_editor / right_editor keys (see AreaLayout::ToLegacy)
      vector<PinKey> pins;
      bool presetSizes = true; // The layout is still a preset of the legacy shape (never visited, nothing saved): its split sizes follow the current ones when it is applied
   };
   static constexpr int WorkspaceCount = 3;
   const Workspace &GetWorkspace(int index) const { return m_workspaces[index]; }
   int GetActiveWorkspace() const { return m_activeWorkspace; }
   // Stores the current panels and pins in the active workspace, then switches to the given one
   void ApplyWorkspace(int index);

private:
   struct GroupRequest
   {
      PinKind kind;
      string name;
      string group;
   };

   void SyncWithTable();
   void *Resolve(PinKind kind, const string &name, const void **owner) const;
   bool Harvest(PinKind kind, const string &name);
   void ProcessGroupRequests();
   void LoadFavoriteDefs();
   void RenderFavoriteRows(bool hovered);
   void RenderObjectRows(PropertyPane &props, PinKind kind, const string &name, const vector<PinKey> &keys, bool removable, bool hovered);
   void RenderFavoritesToolbar();
   void RenderItemTab(bool hovered);
   void RenderViewTab();
   void ApplyUnits(PropertyPane &props) const;
   void PinSelectionSeeThrough();
   // Selected parts (other than the source) that have the field the copy function handles
   void CollectCopyTargets(IEditable *source, const PinCopyFn &copy, vector<std::shared_ptr<EditorUIPart>> &out) const;
   void ResetWorkspaces();
   void CaptureWorkspace(Workspace &ws) const;
   void LoadWorkspaces(const string &json);

   EditorUI &m_editor;
   vector<std::pair<PinKind, string>> m_objects; // Objects of the favorites, in order of first appearance
   vector<PinKey> m_itemKeys;
   int m_shownTab = -1; // Sidebar tab shown last frame
   bool m_quickOpen = false; // Q popup open
   bool m_escPending = false;
   PinKey m_pendingCopy;
   bool m_hasPendingCopy = false;
   void ApplyBulbToAllTraced(Primitive *source, bool startup);
   bool m_bulbStartup = false; // The pane showed the startup table (inspection Startup tab): the values go to the startup inserts
   IEditable *m_bulbSource = nullptr; // Traced insert whose bulb values are to be applied to all the others
   uint64_t m_editRecordId = 0; // Undo record kept for the field edit in progress (see TopRecordId)
   unsigned int m_prevActiveId = 0;
   bool m_applyArmed = false; // ApplyToSelected may act (see ArmApplyToSelected)
   vector<PinKey> m_pins; // In pinning order (rows are grouped by object, in order of first appearance)
   std::map<string, Def> m_defs; // By PinKey::Id()
   vector<GroupRequest> m_groupRequests; // Quick set requests, applied once the object's definitions are known
   std::map<string, const void *> m_harvested; // Objects already harvested (by kind + name) with the owner they were harvested from
   std::map<string, unsigned> m_harvestStamps; // Stamp of the latest harvest of each object (by kind + name)
   unsigned m_harvestCounter = 0;
   unsigned m_harvestStamp = 0; // Stamp of the harvest in progress (0 when not harvesting)
   int m_harvestSeq = 0;
   const PinTable *m_sourceTable = nullptr; // Base table the pin list was loaded from
   std::array<Workspace, WorkspaceCount> m_workspaces;
   bool m_workspacesPristine = false; // Layout is shown as active but the table has no saved workspaces: nothing is written to it until a workspace is picked or edited
   int m_activeWorkspace = -1; // None until one is picked or the table has saved workspaces (the table data stays untouched until then)
};

}
