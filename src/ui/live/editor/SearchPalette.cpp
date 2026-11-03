// license:GPLv3+

#include "core/stdafx.h"
#include "SearchPalette.h"

#include "core/editablereg.h"
#include "parts/Material.h"
#include "ui/live/EditorUI.h"
#include "ui/live/LiveUI.h"

#include "EditorTheme.h"
#include "EditorUIPart.h"
#include "IconsLucide.h"
#include "PropertyPane.h"
#include "Selection.h"
#include "SetOrigin.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace VPX::EditorUI
{

namespace
{

constexpr const char *kSep = "\xE2\x80\xBA"; // Single right-pointing angle quotation mark
constexpr const char *kDot = "\xC2\xB7"; // Middle dot
#ifdef __APPLE__
constexpr const char *kFooter = "Enter Run \xC2\xB7 Cmd+Enter Add to Quick Favorites \xC2\xB7 Esc Close";
#else
constexpr const char *kFooter = "Enter Run \xC2\xB7 Ctrl+Enter Add to Quick Favorites \xC2\xB7 Esc Close";
#endif
constexpr size_t kMaxResults = 60;
constexpr size_t kMaxRecent = 8;
constexpr size_t kMaxWords = 8;
constexpr int kHarvestsPerFrame = 3;

string Lower(const string &s)
{
   string r = s;
   for (char &c : r)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
   return r;
}

// 0: no match, 1: substring, 2: substring starting a word (both lowercase)
int MatchWord(const string &hay, const string &word)
{
   size_t pos = hay.find(word);
   if (pos == string::npos)
      return 0;
   while (pos != string::npos)
   {
      if (pos == 0 || !std::isalnum(static_cast<unsigned char>(hay[pos - 1])))
         return 2;
      pos = hay.find(word, pos + 1);
   }
   return 1;
}

}

SearchPalette::SearchPalette(EditorUI &editor)
   : m_editor(editor)
{
}

SearchPalette::~SearchPalette() = default;

void SearchPalette::Open()
{
   m_open = true;
   m_ctrlHeldFromOpen = ImGui::GetIO().KeyCtrl;
   m_focusInput = true;
   m_firstFrame = true;
   m_indexStale = true;
   m_dirty = true;
   m_harvestSelection = true;
   m_scrollToSel = true;
   m_hasPending = false;
   m_sel = 0;
   m_query.clear();
   m_lastQuery.clear();
   BuildOperators();
   // Retry types whose harvest failed earlier
   m_queue.clear();
   for (TypeInfo &type : m_types)
      type.queued = 0;
}

// Menu commands; rebuilt on open since workspace names are editable
void SearchPalette::BuildOperators()
{
   m_ops.clear();
   EditorUI &ed = m_editor;
   // Same guards as the menu bar: table edits are off for locked tables, and the File/Tools menus are hidden when inspecting
   const auto unlocked = [&ed]() { return !ed.m_table->IsLocked(); };
   const auto notInspect = [&ed]() { return !ed.IsInspectMode(); };
   const auto editable = [&ed]() { return !ed.m_table->IsLocked() && !ed.IsInspectMode(); };
   const auto editablePart = [&ed]() { return ed.m_selection.GetType() == Selection::S_EDITABLE && !ed.m_table->IsLocked() && !ed.IsInspectMode(); };
   const auto hasPart = [&ed]() { return ed.m_selection.GetType() == Selection::S_EDITABLE; };
   const auto add = [this](const string &path, const char *shortcut, std::function<bool()> enabled, std::function<void()> action)
   {
      Operator op;
      op.path = path;
      const size_t cut = path.rfind(kSep);
      op.label = cut == string::npos ? path : path.substr(cut + strlen(kSep) + 1);
      op.shortcut = shortcut;
      op.labelLower = Lower(op.label);
      op.pathLower = Lower(path);
      op.enabled = std::move(enabled);
      op.action = std::move(action);
      m_ops.push_back(std::move(op));
   };
   const string sep = string(" ") + kSep + " ";
   // Labels and shortcuts below are copied from EditorChrome::RenderMenuBar: keep them identical
   add("File" + sep + "New" + sep + "New Table", "Ctrl+N", notInspect, [&ed]() { ed.NewTable(EditorUI::NewTableTemplate::Blank); });
   add("File" + sep + "New" + sep + "Completely Blank Table", "", notInspect, [&ed]() { ed.NewTable(EditorUI::NewTableTemplate::Stripped); });
   add("File" + sep + "New" + sep + "Full Example Table", "", notInspect, [&ed]() { ed.NewTable(EditorUI::NewTableTemplate::Example); });
   add("File" + sep + "New" + sep + "Light Sequence Demo Table", "", notInspect, [&ed]() { ed.NewTable(EditorUI::NewTableTemplate::LightSeq); });
   add("File" + sep + "Open...", "Ctrl+O", editable, [&ed]() { ed.LoadTable(); });
   add("File" + sep + "Open Pack Folder...", "Ctrl+Shift+O", editable, [&ed]() { ed.LoadTableFolder(); });
   add("File" + sep + "Save", "Ctrl+S", editable, [&ed]() { ed.SaveTable(); });
   add("File" + sep + "Save As...", "", editable, [&ed]() { ed.SaveTableAs(); });
   add("File" + sep + "Save As Pack Folder...", "", editable, [&ed]() { ed.SaveTableAsPackFolder(); });
   add("File" + sep + "Play", "F5", editable, [&ed]() { ed.PlayTest(); });
   add("File" + sep + "Part Library", "", unlocked, [&ed]() { ed.OpenPartLibrary(); });
   // View menu: camera and view commands (same keys as the menu; with numpad emulation the top row digits also work)
   using PV = EditorUI::PredefinedView;
   add("View" + sep + "Top", "Numpad 7", nullptr, [&ed]() { ed.SetView(PV::Top); });
   add("View" + sep + "Bottom", "Ctrl+Numpad 7", nullptr, [&ed]() { ed.SetView(PV::Bottom); });
   add("View" + sep + "Front", "Numpad 1", nullptr, [&ed]() { ed.SetView(PV::Front); });
   add("View" + sep + "Back", "Ctrl+Numpad 1", nullptr, [&ed]() { ed.SetView(PV::Back); });
   add("View" + sep + "Left", "Numpad 3", nullptr, [&ed]() { ed.SetView(PV::Left); });
   add("View" + sep + "Right", "Ctrl+Numpad 3", nullptr, [&ed]() { ed.SetView(PV::Right); });
   add("View" + sep + "Perspective / Orthographic", "Numpad 5", nullptr, [&ed]() { ed.m_camMode = ViewMode::EditorCam; ed.m_perspectiveCam = !ed.m_perspectiveCam; });
   add("View" + sep + "Cycle Camera (Preview / Editor / Backdrop)", "Numpad 0", nullptr, [&ed]() { ed.CycleCamera(); });
   add("View" + sep + "Lock Orbit", "Numpad /", nullptr, [&ed]() { ed.m_orbitLock = !ed.m_orbitLock; });
   add("View" + sep + "Orbit Left", "Numpad 4", nullptr, [&ed]() { ed.OrbitStep(true, 1.f); });
   add("View" + sep + "Orbit Right", "Numpad 6", nullptr, [&ed]() { ed.OrbitStep(true, -1.f); });
   add("View" + sep + "Orbit Up", "Numpad 8", nullptr, [&ed]() { ed.OrbitStep(false, 1.f); });
   add("View" + sep + "Orbit Down", "Numpad 2", nullptr, [&ed]() { ed.OrbitStep(false, -1.f); });
   add("View" + sep + "Frame Selected", "F", hasPart, [&ed]() { ed.FrameSelection(); });
   add("View" + sep + "Frame All", "Home", nullptr, [&ed]() { ed.FrameAll(); });
   add("View" + sep + "Sidebar", "N", nullptr, [&ed]() { ed.ToggleSidebar(); });
   add("View" + sep + "Sidebar" + sep + "Favorites", "", nullptr, [&ed]() { ed.ShowSidebar(EditorUI::SidebarTab::Favorites); });
   add("View" + sep + "Outliner", "", nullptr, [&ed]() { ed.ToggleAreaEditor(EditorUI::AreaEditor::Outliner); });
   add("View" + sep + "Properties", "", nullptr, [&ed]() { ed.ToggleAreaEditor(EditorUI::AreaEditor::Properties); });
   // Reveals an editor, hosting it in its preferred area if no area shows it
   for (int i = 0; i < kPanelTypeCount; ++i)
   {
      const EditorUI::AreaEditor type = static_cast<EditorUI::AreaEditor>(i);
      add("View" + sep + "Show " + EditorUI::AreaEditorName(type), "", nullptr, [&ed, type]() { ed.ShowAreaEditor(type); });
   }
   add("View" + sep + "Toggle Maximize Area", "Ctrl+Space", nullptr, [&ed]() { ed.ToggleMaximizeArea(); });
   add("View" + sep + "Tool Shelf", "", nullptr, [&ed]() { ed.m_chrome.ToggleToolShelf(); });
   add("View" + sep + "Navigation Cube", "", nullptr, [&ed]() { ed.m_showNavCube = !ed.m_showNavCube; });
   add("View" + sep + "Quick Favorites", "Q", nullptr, [&ed]() { ed.OpenQuickFavorites(); });
   add("View" + sep + "Menu Search...", "F3", nullptr, [&ed]() { ed.m_palette.Open(); });
   // Edit menu
   add("Edit" + sep + "Undo", "Ctrl+Z", [&ed]() { return ed.CanUndo(); }, [&ed]() { ed.UndoSteps(1); });
   add("Edit" + sep + "Redo", "Ctrl+Shift+Z", [&ed]() { return ed.CanRedo(); }, [&ed]() { ed.RedoSteps(1); });
   add("Edit" + sep + "Undo History...", "", [&ed]() { return ed.m_table->m_liveBaseTable == nullptr && (ed.m_undo.HasUndo() || ed.m_undo.HasRedo()); }, [&ed]() { ed.m_chrome.OpenUndoHistory(); });
   add("Edit" + sep + "Copy", "Ctrl+C", [&ed]() { return ed.CanCopy(); }, [&ed]() { ed.CopySelection(); });
   add("Edit" + sep + "Paste", "Ctrl+V", [&ed]() { return ed.CanPaste(); },
      [&ed]()
      {
         ed.PasteSelection(ed.GetViewportContentCenter());
      });
   add("Edit" + sep + "Select All", "A", [&ed]() { return ed.CanSelect(); }, [&ed]() { ed.SelectAllAction(); });
   add("Edit" + sep + "Deselect", "Alt+A", [&ed]() { return ed.CanSelect() && ed.m_selection.GetType() != Selection::S_NONE; }, [&ed]() { ed.ClearSelection(); });
   add("Edit" + sep + "Invert Selection", "Ctrl+I", [&ed]() { return ed.CanSelect(); }, [&ed]() { ed.InvertSelection(); });
   add("Edit" + sep + "Adjust Last Operation...", "F9", [&ed]() { return ed.m_lastOp.type != EditorUI::LastOperation::Type::None; },
      [&ed]()
      {
         ed.m_adjustPopupOpen = true;
         ed.m_adjustPopupJustOpened = true;
         ed.m_adjustPopupPos = ImGui::GetMousePos();
      });
   add("Edit" + sep + "Snapping", "Shift+Tab", nullptr, [&ed]() { ed.ToggleSnapping(); });
   add("View" + sep + "Status Bar", "", nullptr, [&ed]() { ed.m_chrome.ToggleStatusBar(); });
   add("Object" + sep + "Duplicate", "Shift+D", [&ed]() { return ed.CanDuplicate(); }, [&ed]() { ed.DuplicateSelection(); });
   add("Object" + sep + "Delete", "Del", [&ed]() { return ed.CanDelete(); }, [&ed]() { ed.DeleteAction(); });
   add("Object" + sep + "Add Part...", "Shift+A", [&ed]() { return !ed.m_table->IsLocked() && !ed.IsInspectMode() && ed.m_pointEditPart == nullptr; }, [&ed]() { ed.m_chrome.RequestAddPartPopup(ImGui::GetMousePos()); });
   add("Object" + sep + "Move", "G", nullptr, [&ed]() { ed.SetGizmoOperation(ImGuizmo::TRANSLATE); });
   add("Object" + sep + "Rotate", "R", nullptr, [&ed]() { ed.SetGizmoOperation(ImGuizmo::ROTATE); });
   add("Object" + sep + "Scale", "S", nullptr, [&ed]() { ed.SetGizmoOperation(ImGuizmo::SCALE); });
   add("Object" + sep + "New Collection from Selection", "", editablePart, [&ed]() { ed.CreateCollection(true); });
   add("Object" + sep + "Edit Points", "Tab", [&ed]() { return ed.CanEnterPointEdit(); }, [&ed]() { ed.EnterPointEditMode(); });
   add("Object" + sep + "Set Origin" + sep + "Origin to Geometry", "", [&ed]() { return SetOrigin::CanApply(ed); }, [&ed]() { SetOrigin::Apply(ed, SetOrigin::Mode::Geometry); });
   add("Object" + sep + "Set Origin" + sep + "Origin to Geometry Bottom", "", [&ed]() { return SetOrigin::CanApply(ed); }, [&ed]() { SetOrigin::Apply(ed, SetOrigin::Mode::GeometryBottom); });
   add("Tools" + sep + "Convert to PBR...", "", editable, [&ed]() { ed.m_pbrConvertModal.Show(); });
   add("Debug" + sep + "Renderer Inspection", "", unlocked, [&ed]() { ed.m_inspectionModal.Show(); });
   for (int i = 0; i < PinnedPanel::WorkspaceCount; ++i)
   {
      const string &name = ed.m_pinned.GetWorkspace(i).name;
      add("Workspace" + sep + (name.empty() ? "Workspace " + std::to_string(i + 1) : name), i == 0 ? "Ctrl+1" : i == 1 ? "Ctrl+2" : "Ctrl+3", nullptr,
         [&ed, i]() { ed.SwitchWorkspace(i); });
   }
}

int SearchPalette::TypeIndex(const string &key, const string &name)
{
   const auto it = m_typeByKey.find(key);
   if (it != m_typeByKey.end())
      return it->second;
   TypeInfo info;
   info.name = name;
   info.lower = Lower(name);
   m_types.push_back(std::move(info));
   const int idx = static_cast<int>(m_types.size()) - 1;
   m_typeByKey[key] = idx;
   return idx;
}

// Rebuilds the object list (names and lowercase copies); the field catalogs are kept
void SearchPalette::BuildIndex()
{
   const PinTable *const table = m_editor.m_table;
   m_objects.clear();
   m_partType.clear();
   m_tableType = TypeIndex("table"s, "Table"s);
   m_matType = TypeIndex("material"s, "Material"s);

   m_objects.push_back({ PinKind::Table, string(), "Table"s, "table"s, m_tableType });
   for (const Material *const mat : table->m_materials)
      m_objects.push_back({ PinKind::Material, mat->m_name, mat->m_name, Lower(mat->m_name), m_matType });
   for (IEditable *const part : table->GetParts())
   {
      const ItemTypeEnum itemType = part->GetItemType();
      if (itemType == eItemTable)
         continue;
      int type;
      if (const auto cached = m_itemTypeIdx.find(static_cast<int>(itemType)); cached != m_itemTypeIdx.end())
         type = cached->second;
      else
      {
         type = TypeIndex("p"s + std::to_string(static_cast<int>(itemType)), string(LocalString(EditableRegistry::GetTypeNameStringID(itemType)).m_szbuffer));
         m_itemTypeIdx[static_cast<int>(itemType)] = type;
      }
      const string &name = part->GetName();
      m_partType[name] = type;
      m_objects.push_back({ PinKind::Part, name, name, Lower(name), type });
   }
   m_indexParts = table->GetParts().size();
   m_indexMats = table->m_materials.size();
   m_indexStale = false;
   m_dirty = true;
   RefreshCatalog();
   QueueMissingTypes();
}

// Learns the field list of each type from the registered definitions, in draw order
void SearchPalette::RefreshCatalog()
{
   struct Entry
   {
      int seq;
      string group;
      string shown;
   };
   vector<std::unordered_map<string, Entry>> found(m_types.size());
   for (const auto &[id, def] : m_editor.m_pinned.GetDefs())
   {
      int type = -1;
      switch (def.key.kind)
      {
      case PinKind::Table: type = m_tableType; break;
      case PinKind::Material: type = m_matType; break;
      case PinKind::Part:
      {
         const auto it = m_partType.find(def.key.name);
         if (it != m_partType.end())
            type = it->second;
         break;
      }
      }
      if (type < 0 || type >= static_cast<int>(found.size()))
         continue;
      const auto [it, added] = found[type].try_emplace(def.key.prop, Entry { def.seq, def.group, def.label.empty() ? def.key.prop : def.label });
      if (!added && def.seq < it->second.seq)
         it->second = Entry { def.seq, def.group, def.label.empty() ? def.key.prop : def.label };
   }
   for (size_t t = 0; t < found.size(); ++t)
   {
      if (found[t].empty())
         continue;
      vector<std::pair<int, const string *>> order;
      order.reserve(found[t].size());
      for (const auto &[label, entry] : found[t])
         order.emplace_back(entry.seq, &label);
      std::sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first != b.first ? a.first < b.first : *a.second < *b.second; });
      vector<Field> &fields = m_types[t].fields;
      fields.clear();
      fields.reserve(order.size());
      for (const auto &o : order)
      {
         const string &group = found[t][*o.second].group;
         const string &shown = found[t][*o.second].shown;
         fields.push_back({ *o.second, group, Lower(*o.second), Lower(group), shown, Lower(shown) });
      }
   }
}

// Up to 3 objects per type without catalog get harvested (first, middle, last: conditional fields differ per object)
void SearchPalette::QueueMissingTypes()
{
   vector<vector<size_t>> perType(m_types.size());
   for (size_t i = 0; i < m_objects.size(); ++i)
      if (m_types[m_objects[i].type].queued == 0 && m_types[m_objects[i].type].fields.empty())
         perType[m_objects[i].type].push_back(i);
   for (size_t t = 0; t < perType.size(); ++t)
   {
      const vector<size_t> &list = perType[t];
      if (list.empty())
         continue;
      m_types[t].queued = 3;
      size_t picks[3] = { 0, list.size() / 2, list.size() - 1 };
      for (size_t k = 0; k < 3; ++k)
         if (k == 0 || picks[k] != picks[k - 1])
            m_queue.push_back({ m_objects[list[picks[k]]].kind, m_objects[list[picks[k]]].name });
   }
}

void SearchPalette::Harvest()
{
   int budget = kHarvestsPerFrame;
   bool any = false;
   while (budget-- > 0 && !m_queue.empty())
   {
      const QueueItem item = m_queue.front();
      m_queue.erase(m_queue.begin());
      m_editor.m_pinned.EnsureHarvested(item.kind, item.name);
      any = true;
   }
   if (any)
   {
      RefreshCatalog();
      m_dirty = true;
   }
}

void SearchPalette::MakeResult(Result &r) const
{
   if (r.op >= 0)
   {
      const Operator &op = m_ops[r.op];
      r.label = op.label;
      // Parent path only: the label is the last component of the path
      r.right = op.path == op.label ? string() : op.path.substr(0, op.path.size() - op.label.size() - strlen(kSep) - 2);
      r.chip = op.shortcut;
      return;
   }
   const Object &o = m_objects[r.obj];
   const TypeInfo &type = m_types[o.type];
   if (r.isField)
   {
      string shown;
      if (r.field >= 0)
      {
         r.prop = type.fields[r.field].label;
         r.group = type.fields[r.field].group;
         shown = type.fields[r.field].shown;
      }
      else
      {
         const PinnedPanel::Def *def = m_editor.m_pinned.FindDef(PinKey { o.kind, o.name, r.prop });
         r.group = def ? def->group : string();
         if (def && !def->label.empty())
            shown = def->label;
      }
      r.label = o.display + " " + kSep + " " + (shown.empty() ? r.prop : shown);
      r.right = r.group.empty() ? type.name : r.group + " " + kDot + " " + type.name;
   }
   else
   {
      r.label = o.display + " " + kDot + " " + type.name;
   }
}

void SearchPalette::Recompute()
{
   m_results.clear();
   m_sel = 0;
   m_scrollToSel = true;

   vector<string> words;
   {
      const string lowered = Lower(m_lastQuery);
      size_t i = 0;
      while (i < lowered.size() && words.size() < kMaxWords)
      {
         while (i < lowered.size() && std::isspace(static_cast<unsigned char>(lowered[i])))
            ++i;
         const size_t start = i;
         while (i < lowered.size() && !std::isspace(static_cast<unsigned char>(lowered[i])))
            ++i;
         if (i > start)
            words.push_back(lowered.substr(start, i - start));
      }
   }

   if (words.empty())
   {
      // Last activated results, dropping the ones whose object is gone
      for (size_t k = 0; k < m_recent.size();)
      {
         const Recent &rec = m_recent[k];
         if (!rec.opId.empty())
         {
            int op = -1;
            for (size_t oi = 0; oi < m_ops.size(); ++oi)
               if (m_ops[oi].path == rec.opId)
                  op = static_cast<int>(oi);
            if (op < 0)
            {
               m_recent.erase(m_recent.begin() + k);
               continue;
            }
            Result r { -1, -1, 0, false, {}, {}, {}, {}, op };
            MakeResult(r);
            m_results.push_back(std::move(r));
            ++k;
            continue;
         }
         int found = -1;
         for (size_t oi = 0; oi < m_objects.size(); ++oi)
            if (m_objects[oi].kind == rec.kind && m_objects[oi].name == rec.name)
            {
               found = static_cast<int>(oi);
               break;
            }
         if (found < 0)
         {
            m_recent.erase(m_recent.begin() + k);
            continue;
         }
         Result r { found, -1, 0, rec.isField, rec.prop, {}, {}, {} };
         if (rec.isField)
         {
            const vector<Field> &fields = m_types[m_objects[found].type].fields;
            for (size_t f = 0; f < fields.size(); ++f)
               if (fields[f].label == rec.prop)
               {
                  r.field = static_cast<int>(f);
                  break;
               }
         }
         MakeResult(r);
         m_results.push_back(std::move(r));
         ++k;
      }
      return;
   }

   // Current selection ranks first
   bool selIsTable = false;
   const Material *selMat = nullptr;
   const IEditable *selPart = nullptr;
   {
      const Selection &sel = m_editor.m_selection;
      switch (sel.GetType())
      {
      case Selection::S_NONE: selIsTable = true; break;
      case Selection::S_MATERIAL: selMat = sel.GetMaterial(); break;
      case Selection::S_EDITABLE:
      {
         const auto part = sel.GetPart();
         selPart = part ? part->GetEditable() : nullptr;
         break;
      }
      default: break;
      }
   }
   const string selName = selMat ? selMat->m_name : (selPart ? selPart->GetName() : string());
   const string selNameLower = Lower(selName);
   // The material of the selected part ranks right after it: parts do not expose their material generically, so it is recognized by name
   // (a material named after the part, or the part named after it)
   const auto relatedToSelectedPart = [&](const string &materialLower)
   {
      return selPart != nullptr && materialLower.size() >= 3 && selNameLower.size() >= 3
         && (materialLower == selNameLower || selNameLower.find(materialLower) != string::npos || materialLower.find(selNameLower) != string::npos);
   };

   const unsigned full = (1u << words.size()) - 1u;
   vector<Candidate> cands;
   bool anyName = false; // A word names an object: fields then outrank operators
   for (size_t oi = 0; oi < m_objects.size(); ++oi)
   {
      const Object &o = m_objects[oi];
      const TypeInfo &type = m_types[o.type];
      // Words the object name or type already satisfy
      unsigned mask = 0;
      int base = 0;
      for (size_t w = 0; w < words.size(); ++w)
      {
         const int nameMatch = MatchWord(o.lower, words[w]);
         if (nameMatch != 0)
         {
            anyName = true;
            mask |= 1u << w;
            base += nameMatch == 2 ? 30 : 20;
            if (o.lower == words[w])
               base += 20; // Exact name
         }
         else if (MatchWord(type.lower, words[w]) != 0)
         {
            mask |= 1u << w;
            base += 10;
         }
      }
      const bool isSel = (o.kind == PinKind::Table && selIsTable) || (o.kind == PinKind::Material && selMat != nullptr && o.name == selName)
         || (o.kind == PinKind::Part && selPart != nullptr && o.name == selName);
      base += (isSel ? 100000 : 0) + (o.kind == PinKind::Material && relatedToSelectedPart(o.lower) ? 50000 : 0) - static_cast<int>(std::min<size_t>(o.name.size(), 100));

      if (mask == full)
      {
         cands.push_back({ static_cast<int>(oi), -1, base });
         continue;
      }
      // Remaining words must be found in a field label or group
      for (size_t f = 0; f < type.fields.size(); ++f)
      {
         const Field &field = type.fields[f];
         int total = base - static_cast<int>(std::min<size_t>(field.label.size(), 100) / 2);
         bool ok = true;
         for (size_t w = 0; w < words.size() && ok; ++w)
         {
            if (mask & (1u << w))
               continue;
            if (const int m = std::max(MatchWord(field.shownLower, words[w]), MatchWord(field.labelLower, words[w])); m != 0)
               total += (m == 2 ? 40 : 30) + (field.shownLower == words[w] ? 25 : 0); // Exact label first
            else if (const int g = MatchWord(field.groupLower, words[w]); g != 0)
               total += g == 2 ? 20 : 15;
            else
               ok = false;
         }
         if (ok)
            cands.push_back({ static_cast<int>(oi), static_cast<int>(f), total });
      }
   }

   for (size_t oi = 0; oi < m_ops.size(); ++oi)
   {
      const Operator &op = m_ops[oi];
      if (op.enabled && !op.enabled())
         continue;
      int total = (anyName ? 0 : 5) - static_cast<int>(std::min<size_t>(op.label.size(), 100) / 2);
      bool ok = true;
      for (size_t w = 0; w < words.size() && ok; ++w)
      {
         if (const int m = MatchWord(op.labelLower, words[w]); m != 0)
            total += (m == 2 ? 40 : 30) + (op.labelLower == words[w] ? 25 : 0); // Exact label first
         else if (const int p = MatchWord(op.pathLower, words[w]); p != 0)
            total += p == 2 ? 20 : 15;
         else
            ok = false;
      }
      if (ok)
         cands.push_back({ -1, -1, total, static_cast<int>(oi) });
   }

   const auto better = [](const Candidate &a, const Candidate &b)
   {
      if (a.score != b.score)
         return a.score > b.score;
      if (a.obj != b.obj)
         return a.obj < b.obj;
      return a.field != b.field ? a.field < b.field : a.op < b.op;
   };
   if (cands.size() > kMaxResults)
   {
      std::partial_sort(cands.begin(), cands.begin() + kMaxResults, cands.end(), better);
      cands.resize(kMaxResults);
   }
   else
      std::sort(cands.begin(), cands.end(), better);

   m_results.reserve(cands.size());
   for (const Candidate &c : cands)
   {
      Result r { c.obj, c.field, c.score, c.field >= 0, {}, {}, {}, {}, c.op };
      MakeResult(r);
      m_results.push_back(std::move(r));
   }
}

void SearchPalette::Remember(const Result &r)
{
   if (r.op >= 0)
   {
      const string &id = m_ops[r.op].path;
      for (size_t k = 0; k < m_recent.size(); ++k)
         if (m_recent[k].opId == id)
         {
            m_recent.erase(m_recent.begin() + k);
            break;
         }
      m_recent.insert(m_recent.begin(), Recent { PinKind::Table, string(), string(), false, id });
      if (m_recent.size() > kMaxRecent)
         m_recent.resize(kMaxRecent);
      return;
   }
   const Object &o = m_objects[r.obj];
   const string prop = r.isField ? r.prop : string();
   for (size_t k = 0; k < m_recent.size(); ++k)
      if (m_recent[k].kind == o.kind && m_recent[k].name == o.name && m_recent[k].isField == r.isField && m_recent[k].prop == prop && m_recent[k].opId.empty())
      {
         m_recent.erase(m_recent.begin() + k);
         break;
      }
   m_recent.insert(m_recent.begin(), Recent { o.kind, o.name, prop, r.isField, string() });
   if (m_recent.size() > kMaxRecent)
      m_recent.resize(kMaxRecent);
}

void SearchPalette::Activate(const Result &r, bool pin)
{
   if (r.op >= 0)
   {
      if (r.op >= static_cast<int>(m_ops.size()))
         return; // Ctrl+Enter (pin) has no meaning for a command: run it
      const Operator op = m_ops[r.op]; // Copy: the action may reopen or rebuild the palette
      if (op.enabled && !op.enabled())
         return;
      Close();
      Remember(r);
      m_pendingOp = r.op;
      return;
   }
   if (r.obj < 0 || r.obj >= static_cast<int>(m_objects.size()))
      return;
   const Object &o = m_objects[r.obj];
   const PinKey key { o.kind, o.name, r.isField ? r.prop : string() };

   if (pin)
   {
      if (!r.isField)
         return;
      m_editor.m_pinned.Pin(key);
      m_editor.m_liveUI.PushNotification("Added to Quick Favorites: " + r.label, 2000);
      Remember(r);
      m_focusInput = true;
      return;
   }

   Close();
   PinTable *const table = m_editor.m_table;
   bool found = false;
   switch (o.kind)
   {
   case PinKind::Table:
      m_editor.ClearSelection();
      found = true;
      break;
   case PinKind::Material:
      for (Material *const mat : table->m_materials)
         if (mat->m_name == o.name)
         {
            m_editor.SetSelection(Selection(mat));
            found = true;
            break;
         }
      break;
   case PinKind::Part:
      for (IEditable *const part : table->GetParts())
         if (part->GetName() == o.name)
         {
            const auto it = m_editor.m_editableMap.find(part);
            if (it != m_editor.m_editableMap.end())
            {
               m_editor.SetSelection(Selection(it->second));
               found = true;
            }
            break;
         }
      break;
   }
   if (!found)
   {
      m_editor.m_liveUI.PushNotification(o.display + " no longer exists", 2000);
      return;
   }
   if (r.isField)
   {
      m_editor.ShowAreaEditor(EditorUI::AreaEditor::Properties); // A workspace may hide the panel (or host another editor), which would never consume the request
      PropertyPane::RequestFocus(key);
   }
   Remember(r);
}

bool SearchPalette::ConsumedEscape()
{
   if (!m_escPending)
      return false;
   if (!ImGui::IsKeyDown(ImGuiKey_Escape))
      m_escPending = false; // Released: this is the frame of the release, still consumed
   return true;
}

void SearchPalette::Render()
{
   if (m_pendingOp >= 0)
   {
      // Chosen last frame: run it before anything else, outside any window, since it may swap the table
      const int idx = m_pendingOp;
      m_pendingOp = -1;
      if (idx < static_cast<int>(m_ops.size()))
      {
         const Operator op = m_ops[idx];
         if (!op.enabled || op.enabled())
            op.action();
      }
   }
   // The Esc that closed the palette is forgotten once released, even if the editor did not ask (e.g. its key handling was skipped)
   if (m_escPending && !ImGui::IsKeyDown(ImGuiKey_Escape) && !ImGui::IsKeyReleased(ImGuiKey_Escape))
      m_escPending = false;
   if (!m_open)
      return;

   // Index and harvests run before the palette window: harvesting opens its own hidden windows
   const PinTable *const table = m_editor.m_table;
   if (m_indexStale || table->GetParts().size() != m_indexParts || table->m_materials.size() != m_indexMats)
      BuildIndex();
   if (m_harvestSelection)
   {
      m_harvestSelection = false;
      const Selection &sel = m_editor.m_selection;
      if (sel.GetType() == Selection::S_MATERIAL && sel.GetMaterial() != nullptr)
         m_queue.insert(m_queue.begin(), QueueItem { PinKind::Material, sel.GetMaterial()->m_name });
      else if (sel.GetType() == Selection::S_EDITABLE && sel.GetPart() != nullptr)
         m_queue.insert(m_queue.begin(), QueueItem { PinKind::Part, sel.GetPart()->GetEditable()->GetName() });
      else if (sel.GetType() == Selection::S_NONE)
         m_queue.insert(m_queue.begin(), QueueItem { PinKind::Table, string() });
   }
   const bool harvested = !m_queue.empty();
   Harvest();

   if (m_dirty || m_query != m_lastQuery)
   {
      m_lastQuery = m_query;
      m_dirty = false;
      Recompute();
   }

   const ImGuiViewport *const vp = ImGui::GetMainViewport();
   const float dpi = m_editor.m_liveUI.GetDPI();
   const float width = 560.f * dpi;
   const float pad = 10.f * dpi; // Window padding
   const float spacingY = 2.f * dpi; // Item spacing between rows
   const float inputHeight = 28.f * dpi;
   const float inputGap = 6.f * dpi; // Between the input and the results
   const float footerGap = 4.f * dpi; // Between the footer rule and the footer text
   const float rowH = 24.f * dpi;
   const float innerWidth = width - 2.f * pad;
   const float rowHeight = rowH + spacingY;
   const float chrome = 2.f * pad + inputHeight + inputGap + 1.f + footerGap + spacingY + ImGui::GetTextLineHeight();
   const float maxList = std::max(3.f * rowHeight, vp->Size.y * 0.6f - chrome);
   size_t count = m_results.size();
   const float listHeight = count == 0 ? ImGui::GetTextLineHeight() : std::min(static_cast<float>(count) * rowHeight - spacingY, maxList);

   ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.2f), ImGuiCond_Always, ImVec2(0.5f, 0.f));
   ImGui::SetNextWindowSize(ImVec2(width, chrome + listHeight), ImGuiCond_Always);
   ImGui::SetNextWindowBgAlpha(0.96f);
   if (m_firstFrame || harvested)
      ImGui::SetNextWindowFocus();

   bool close = false;
   bool hasActivation = false;
   size_t activateIdx = 0;
   bool activatePin = false;

   constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse
      | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
   ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, spacingY));
   if (ImGui::Begin("##SearchPalette", nullptr, flags))
   {
      if (!m_firstFrame && !harvested && !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
         close = true;
      m_firstFrame = false;

      if (m_focusInput)
      {
         ImGui::SetKeyboardFocusHere();
         m_focusInput = false;
      }
      ImGui::SetNextItemWidth(innerWidth);
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(30.f * dpi, 0.5f * (inputHeight - ImGui::GetFontSize()))); // Room for the search icon
      ImGui::InputTextWithHint("##query", "Search menus and fields (F3)", &m_query);
      ImGui::PopStyleVar();
      ImGui::GetWindowDrawList()->AddText(ImVec2(ImGui::GetItemRectMin().x + 9.f * dpi, ImGui::GetItemRectMin().y + 0.5f * (inputHeight - ImGui::GetFontSize())), EditorTheme::TextDim, ICON_LC_SEARCH);
      ImGui::Dummy(ImVec2(0.f, std::max(0.f, inputGap - 2.f * spacingY)));
      if (m_query != m_lastQuery)
      {
         // Recompute now so that Enter acts on the list matching the text
         m_lastQuery = m_query;
         m_dirty = false;
         Recompute();
         count = m_results.size();
      }

      if (!ImGui::GetIO().KeyCtrl)
         m_ctrlHeldFromOpen = false;
      if (count > 0)
      {
         const int last = static_cast<int>(count) - 1;
         if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) && m_sel < last)
         {
            ++m_sel;
            m_scrollToSel = true;
         }
         if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) && m_sel > 0)
         {
            --m_sel;
            m_scrollToSel = true;
         }
         if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
         {
            hasActivation = true;
            activateIdx = static_cast<size_t>(m_sel);
            activatePin = ImGui::GetIO().KeyCtrl && !m_ctrlHeldFromOpen;
         }
      }
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
      {
         close = true;
         m_escPending = true;
      }

      if (count == 0)
         ImGui::TextDisabled("%s", m_lastQuery.empty() ? "Type to search" : "No matches");
      else if (ImGui::BeginChild("##results", ImVec2(innerWidth, listHeight), ImGuiChildFlags_None, ImGuiWindowFlags_None))
      {
         const ImGuiIO &io = ImGui::GetIO();
         const bool mouseMoved = io.MouseDelta.x != 0.f || io.MouseDelta.y != 0.f;
         ImDrawList *const draw = ImGui::GetWindowDrawList();
         ImGui::PushStyleColor(ImGuiCol_Header, EditorTheme::V(EditorTheme::Fill));
         ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorTheme::V(EditorTheme::Fill));
         ImGui::PushStyleColor(ImGuiCol_HeaderActive, EditorTheme::V(EditorTheme::FillActive));
         for (size_t i = 0; i < count; ++i)
         {
            const Result &r = m_results[i];
            const bool selected = static_cast<int>(i) == m_sel;
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_None, ImVec2(0.f, rowH)))
            {
               hasActivation = true;
               activateIdx = i;
               activatePin = false;
            }
            if (selected && m_scrollToSel)
               ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
            if (ImGui::IsItemHovered() && mouseMoved)
               m_sel = static_cast<int>(i);
            const ImVec2 itemMin = ImGui::GetItemRectMin();
            const ImVec2 itemMax = ImGui::GetItemRectMax();
            const float rightWidth = ImGui::CalcTextSize(r.right.c_str()).x;
            const float textY = itemMin.y + 0.5f * (rowH - ImGui::GetFontSize());
            const float margin = 10.f * dpi;
            const float labelEnd = itemMin.x + margin + ImGui::CalcTextSize(r.label.c_str()).x;
            draw->AddText(ImVec2(itemMin.x + margin, textY), EditorTheme::Text, r.label.c_str());
            // Operator shortcut as a small key chip (1 px border, faint fill, 4 px padding), right aligned with an 8 px margin
            float metaRight = itemMax.x - margin;
            if (!r.chip.empty())
            {
               const float chipPad = 4.f * dpi;
               const ImVec2 chipSize = ImGui::CalcTextSize(r.chip.c_str());
               const ImVec2 chipMax(itemMax.x - 8.f * dpi, itemMin.y + 0.5f * (rowH + chipSize.y) + chipPad);
               const ImVec2 chipMin(chipMax.x - chipSize.x - 2.f * chipPad, chipMax.y - chipSize.y - 2.f * chipPad);
               if (selected) // Dark chip on the blue row so that the keys stay readable
               {
                  draw->AddRectFilled(chipMin, chipMax, EditorTheme::Rgb(0, 0, 0, 90), 3.f * dpi);
                  draw->AddText(ImVec2(chipMin.x + chipPad, chipMin.y + chipPad), EditorTheme::Text, r.chip.c_str());
               }
               else
               {
                  draw->AddRectFilled(chipMin, chipMax, EditorTheme::Rgb(255, 255, 255, 20), 3.f * dpi);
                  draw->AddRect(chipMin, chipMax, EditorTheme::Hover, 3.f * dpi);
                  draw->AddText(ImVec2(chipMin.x + chipPad, chipMin.y + chipPad), EditorTheme::TextDim, r.chip.c_str());
               }
               metaRight = chipMin.x - margin;
            }
            // Skip the meta column when it would overlap the label
            if (!r.right.empty() && labelEnd + margin + rightWidth < metaRight)
               draw->AddText(ImVec2(metaRight - rightWidth, textY), selected ? EditorTheme::Text : EditorTheme::TextDim, r.right.c_str());
            ImGui::PopID();
         }
         ImGui::PopStyleColor(3);
         m_scrollToSel = false;
      }
      if (count != 0)
         ImGui::EndChild(); // Always paired with BeginChild, whatever it returned
      // Footer hint, separated by a 1 px rule
      {
         const ImVec2 rulePos = ImGui::GetCursorScreenPos();
         ImGui::GetWindowDrawList()->AddLine(ImVec2(rulePos.x, rulePos.y), ImVec2(rulePos.x + innerWidth, rulePos.y), EditorTheme::Border, 1.f);
         ImGui::Dummy(ImVec2(0.f, 1.f + footerGap - spacingY));
         ImGui::TextColored(EditorTheme::V(EditorTheme::TextFaint), "%s", kFooter);
      }
   }
   ImGui::End();
   ImGui::PopStyleVar(2);

   if (close)
      Close();
   else if (hasActivation && activateIdx < m_results.size())
   {
      const Result r = m_results[activateIdx]; // Copy: activation may change the palette state
      Activate(r, activatePin);
   }
}

}
