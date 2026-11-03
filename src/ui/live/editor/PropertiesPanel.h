// license:GPLv3+

#pragma once

#include "imgui/imgui.h"

#include <optional>

#include "audio/AudioPlayer.h"
#include "PinnedPanel.h"
#include "Selection.h"
#include <map>

class Collection;
class IEditable;
class Material;
class Texture;
class RenderProbe;
struct WhereUsedInfo;
namespace VPX
{
class Sound;
}

namespace VPX::EditorUI
{

class EditorUI;
class EditorUIPart;
class PropertyPane;

// Right side panel displaying the properties of the current selection (table,
// camera view setup, image, material, render probe, sound or editable part).
class PropertiesPanel
{
   friend class PinnedPanel; // Harvests the field definitions of pinned objects through the panes below

public:
   explicit PropertiesPanel(EditorUI &editor);
   ~PropertiesPanel();

   void RenderInArea(const ImVec2 &pos, const ImVec2 &size); // Window filling the given area rect (header with the editor type button, then the content)

   // Search field of the header (Blender's Properties search): focuses it. Ctrl+F over the area calls this
   void FocusSearch() { m_focusSearch = true; }
   // The mouse was over the area when it was last drawn
   bool IsMouseOver() const { return m_mouseOver; }
   void ClearMouseOver() { m_mouseOver = false; } // Not drawn this frame (not hosted by a visible area)
   // The search applies to the current selection (a single part, material or the table; not a multi selection, image, sound,...)
   bool CanSearch() const { return m_curation.active && m_curation.tabbed; }

   // Width of the panel in logical pixels (scaled by the UI DPI)
   static constexpr float PaneWidth = 360.f;

   // Main visual material of a part (the one its header chip opens), empty if none
   static string PartMaterial(const IEditable *part);

private:
   void TableProperties(PropertyPane &props);
   void ImageProperties(PropertyPane &props, Texture *image);
   void RenderProbeProperties(PropertyPane &props, RenderProbe *probe);
   void CameraProperties(PropertyPane &props, int bgSet);
   void MaterialProperties(PropertyPane &props, Material *material);
   void SoundProperties(PropertyPane &props, VPX::Sound *sound);
   void CollectionProperties(PropertyPane &props, Collection *collection);

   // Rebuilds renderables & physics of the parts modified through the pane: the edited one, plus all
   // co-edited parts of a multi-edit selection (also resyncing their editor visibility overrides)
   void RefreshEditedParts(PropertyPane &props);

   // Resource actions offered at the top of the image/sound/material property panes,
   // matching the actions of the corresponding Win32 manager dialogs
   void ImageActions(PropertyPane &props);
   void SoundActions(PropertyPane &props);
   void MaterialActions(PropertyPane &props);
   void RequestConfirm(const string &message, const std::function<void()> &action);
   void UsersSection(PropertyPane &props, const vector<WhereUsedInfo> &whereUsed);
   void RenderPopups();

   // Curated sections: the object's Look and Transform fields (drawn from the pin field registry, filled by a harvest of its pane)
   // right after the header, then the full sheet collapsed under an "Advanced" header. Only for the object types with curation rules
   // Properties editor tabs (Blender's vertical domain tabs). Each tab draws its fields from the pin field registry, so it is
   // short by construction; Advanced is the complete original sheet
   enum class Tab
   {
      Object,
      Transform,
      Material,
      Look,
      Physics,
      Script,
      Advanced,
      Count
   };
   struct Curation
   {
      PinKind kind = PinKind::Table;
      string name;
      void *obj = nullptr; // Object resolved for the definitions (PinTable*, Material* or IEditable*)
      unsigned stamp = 0; // Harvest the lists below were built from
      vector<const PinnedPanel::Def *> look;
      vector<const PinnedPanel::Def *> transform;
      vector<const PinnedPanel::Def *> all; // Every field of the latest harvest, in draw order (search)
      int advancedCount = 0; // Fields of the full sheet not in the curated sections
      bool active = false;
      bool reharvest = false; // An edit may have changed the fields the pane shows (e.g. material type): harvest again once idle
      bool advancedIndent = false;
      double harvestTime = 0.0;
      string advancedLabel;
      vector<const PinnedPanel::Def *> tabs[static_cast<int>(Tab::Count)]; // Tabbed curation (the selection)
      bool tabbed = false;
   };
   // Search across all the tabs: the matching fields of the object (and of its material), grouped by tab and section
   void DrawSearchField();
   void UpdateSearchTerms();
   bool IsSearching() const { return !m_searchTerms.empty() && m_curation.active && m_curation.tabbed; }
   bool MatchesSearch(const PinnedPanel::Def &def) const;
   int CountSearchMatches(const Curation &cur) const;
   int DrawSearchResults(PropertyPane &props, Curation &cur, const char *titlePrefix);
   string m_search;
   string m_searchTermsFor;
   vector<string> m_searchTerms; // Lower case words of m_search
   bool m_focusSearch = false;
   uint64_t m_editRecordId = 0; // Undo record kept for the field edit in progress (see TopRecordId)
   ImGuiID m_prevActiveId = 0;
   bool m_mouseOver = false;
   void DrawTabStrip();
   void DrawTabContent(PropertyPane &props);
   bool HasTab(Tab tab) const;
   int TabKey() const; // Tab memory per object type
   bool IsSelectionAlive(const Selection &sel) const;
   Tab m_tab = Tab::Object;
   std::map<int, Tab> m_tabMemory;
   bool m_hasMaterialTab = false;
   // Pin ID (Blender's Properties editor pin): the panel stays on the locked object while other objects get selected
   bool m_locked = false;
   Selection m_lockedSel;
   void PrepareCuration();
   void PrepareCurationFor(PinKind kind, const string &name, int itemType, bool enabled, Curation &cur);
   void DrawCuration(PropertyPane &props, Curation &cur);
   void EndCuration(PropertyPane &props, Curation &cur);
   void ApplyUnits(PropertyPane &props) const;
   Curation m_curation; // The selection
   Curation m_materialCuration; // The material of the selected part
   vector<const PinnedPanel::Def *> m_defScratch;
   Material *m_pendingMaterialSel = nullptr; // Header material chip clicked: selected once the panel is drawn
   std::shared_ptr<EditorUIPart> m_pendingPartSel; // Material users list clicked: selected once the panel is drawn

   // The selected part's material, drawn under the part's pane (with its own Look section), and the parts sharing it
   void PartMaterialPane(bool showStartup, bool isLive);
   bool m_inPartMaterialPane = false;
   Material *m_usersFor = nullptr;
   int m_usersFrame = 0;
   vector<WhereUsedInfo> m_users;

   EditorUI &m_editor;
   bool m_selectLiveTab = true;
   ImGuiID m_prevActiveId = 0; // Item active on the previous frame (a different item taking the interaction ends the undo chain)
   size_t m_editChainRecord = 0; // Undo count just after the current interaction's record was kept (it is still the latest record while equal)

   // Results of the asynchronous SDL file dialogs used by the resource actions. Since the dialogs are
   // asynchronous, the item selection at the time the action was requested is captured along the way.
   std::shared_ptr<vector<string>> m_pendingImageImport;
   std::shared_ptr<string> m_pendingImageReimport;
   Texture *m_pendingImageReimportTarget = nullptr;
   std::shared_ptr<string> m_pendingImageExport;
   vector<Texture *> m_pendingImageExportSel;
   std::shared_ptr<vector<string>> m_pendingSoundImport;
   std::shared_ptr<string> m_pendingSoundReimport;
   VPX::Sound *m_pendingSoundReimportTarget = nullptr;
   std::shared_ptr<string> m_pendingSoundExport;
   vector<VPX::Sound *> m_pendingSoundExportSel;
   std::shared_ptr<string> m_pendingMaterialImport;
   std::shared_ptr<string> m_pendingMaterialExport;
   vector<Material *> m_pendingMaterialExportSel;

   // Pending destructive action confirmation popup
   string m_confirmMessage;
   std::function<void()> m_confirmAction;

   // When enabled, exported resource files are named after the object name instead of the import file name
   bool m_exportUseNames = false;
   // Last resource action result displayed in the actions section (e.g. files not found on reimport)
   string m_actionStatus;

   // Sound preview state: the sound whose playback was started from the pane, and whether
   // playback was actually observed (PlaySound commands are dispatched asynchronously)
   VPX::Sound *m_playingSound = nullptr;
   bool m_playingSoundObserved = false;
   VPX::Sound *m_soundInfoFor = nullptr;
   std::optional<VPX::SoundSpec> m_soundInfo;
};

}
