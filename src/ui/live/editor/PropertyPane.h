#pragma once

#include "fonts/IconsForkAwesome.h"
#include "IconsLucide.h"
#include "EditorTheme.h"
#include "utils/fileio.h"
#include "parts/Sound.h"
#include "parts/pintable.h"
#include "parts/Collection.h"
#include "parts/Material.h"
#include "renderer/Texture.h"
#include "imgui/imgui.h"
#include "imgui/imgui_stdlib.h"
#include "imgui/imgui_internal.h"
#include "ResURIPicker.h"
#include "PinnedPanel.h"

class PinUndo;

namespace VPX::EditorUI
{

// Undo of the edits made to a material through a property pane (materials are not IEditable parts, so PinUndo cannot snapshot them): the
// material is copied before the pane is drawn, and the copy is recorded as an undo step when the pane reports a modified field. A drag
// (or a typed value) on one field is a single step. Inactive when the material is null (e.g. in an inspection session)
class MaterialEditUndo
{
public:
   enum Source : int // Panes number their fields differently: edits from different panes never merge into one step
   {
      SourceProperties = 0x1000,
      SourcePartMaterial = 0x2000,
      SourceNPanel = 0x3000
   };
   MaterialEditUndo(PinUndo &undo, PinTable *table, Material *material, Source source);
   // Called once the pane is drawn: `modifiedField` is the id of the field edited this frame (0 if none)
   // Returns true when a new undo step was recorded (the caller then breaks the part edit coalescing)
   bool Commit(int modifiedField, const string &label);
   // The material of the table with this runtime identity (Material::m_uid), null if it was deleted since
   static Material *Find(const PinTable *table, uint64_t uid);
   // Material undo / redo steps skipped since the last call because their material is gone (the caller reports it)
   static int ConsumeSkipped() { return std::exchange(s_skipped, 0); }
   static void NoteSkipped() { s_skipped++; }

private:
   PinUndo &m_undo;
   PinTable *const m_table;
   Material *const m_material;
   const int m_source;
   std::unique_ptr<Material> m_before; // The material as it was before the pane was drawn
   static inline int s_skipped = 0;
};

// Resolves to a part's data class (the type of its m_d member); member pointers to fields declared on a data class base also convert
template <class T> using PartData = std::remove_cv_t<std::remove_reference_t<decltype(std::declval<T&>().m_d)>>;

class PropertyPane final
{
public:
   PropertyPane(PinTable* table);
   ~PropertyPane();

   void SetShowStartup(bool showStartup);

   bool BeginSection(const string& name);
   // Collapsing header in the section style (accent rule, TextDim chevron); label may carry a ###id
   static bool SectionHeader(const char* labelAndId, ImGuiTreeNodeFlags flags = 0);
   void EndSection();
   template <class T> T* GetEditedPart(T* obj) const;
   void TimerSection(IEditable* obj);

   enum class Unit
   {
      None,
      Percent, // 0..1 value
      PercentX100, // 0..100 value
      VPLength,
      VPMass,
      VPSpeed,
      Degree,
      Inches,
      Millimeters
   };
   static const char* GetUnitLabel(Unit unit);
   static void ConvertUnit(Unit from, Unit& to, float& value, int& nDecimalAdjust);
   static void ResolveUnit(Unit from, Unit& to, int& nDecimalAdjust); // Resolves display unit & decimals without converting a value
   void SetLengthUnit(Unit lengthUnit) { m_lengthUnit = lengthUnit; }

   // Multi-edit support: the other parts of a uniform-type selection, co-edited with the pane's one.
   // When set, fields show a mixed ("-") state for diverging values and edits apply to every part.
   void SetCoEditedParts(const vector<IEditable*>& parts) { m_coEdited = parts; }
   bool IsMultiEdit() const { return !m_coEdited.empty(); }

   // Field focus (search palette): the next panel row drawing this field scrolls to it, then flashes for a moment. While the
   // request is pending, the panel opens its sections and its Advanced header so that the field gets drawn
   static void RequestFocus(const PinKey &key);
   // Same scroll and flash for a row of the N-panel / Q popup (a favorite was just added): kept pending for a few seconds
   // until the row is drawn, so it also works when the N-panel is opened right after
   static void RequestOverlayFlash(const PinKey &key);
   // Properties search: lower case terms whose first occurrence in the labels of the fields drawn next is highlighted (null: none)
   void SetHighlight(const vector<string> *terms) { m_highlight = terms; }
   static bool IsFocusPending() { return s_focusPending; }
   static const PinKey &GetFocusKey() { return s_focusKey; }
   // "Apply to all traced inserts" button: queues the copy of this traced insert's bulb values to the other traced inserts of the table
   // (one undo step, run after the areas are drawn)
   void ApplyBulbToAllButton(IEditable* part);
   int GetEditCount() const { return static_cast<int>(m_coEdited.size()) + 1; }

   // Field tiers: Common fields make the curated sections (Look, Transform) the properties panel draws first; Advanced fields only
   // live in the full sheet, drawn below them under a collapsed "Advanced" header. Panes scope a run of fields with SetTier or a
   // TierScope; the panel also curates fields by group and label (see PropertiesPanel)
   enum class Tier
   {
      Advanced,
      Common
   };
   void SetTier(Tier tier) { m_tier = tier; }
   Tier GetTier() const { return m_tier; }
   class TierScope
   {
   public:
      TierScope(PropertyPane &pane, Tier tier) : m_pane(pane), m_prev(pane.m_tier) { pane.m_tier = tier; }
      ~TierScope() { m_pane.m_tier = m_prev; }
      TierScope(const TierScope &) = delete;
      TierScope &operator=(const TierScope &) = delete;
   private:
      PropertyPane &m_pane;
      const Tier m_prev;
   };
   // Hook run right after the pane header (name row): the panel draws its curated sections and the Advanced header there
   void SetAfterHeader(std::function<void(PropertyPane &)> hook) { m_afterHeader = std::move(hook); }
   // While set, sections and fields are skipped (the full sheet is collapsed under the Advanced header)
   void SkipRest(bool skip) { m_skipRest = skip; }
   // Dial surfaces (curated sections, pinned overlay): float fields with a known range draw as sliders (ctrl+click still types)
   void SetLookSliders(bool on) { m_lookSliders = on; }
   static string PinKeyLabel(const string &key); // Label shown for a stored favorite key (inverse of PinKeyProp)
   static string PinKeyProp(const string &label); // Stable favorite key of a field label (see the legacy map)
   static bool LookSliderRange(const string &label, Unit displayUnit, float &vMin, float &vMax);
   // Blender-style slider: the value is a filled bar behind the number (no grab knob over it); ctrl+click or double-click types
   static bool FillSlider(const char *id, float *value, float vMin, float vMax, const char *format);
   // Shared widget helpers (all fields of the editor use the same frame, chevron, checkbox and number look)
   // Text cut to maxWidth with a trailing "..." (the text itself when it fits)
   static string Ellipsize(const char *text, float maxWidth);
   // Line chevron (two segments) centred on `center`; `size` is its half width
   static void DrawChevron(ImDrawList *drawList, const ImVec2 &center, bool down, float size, ImU32 color);
   // Combo without ImGui's arrow block: a TextDim chevron-down at the right end, preview ellipsized to the field
   static bool BeginFlatCombo(const char *id, const char *preview);
   // Checkbox: unchecked = FrameBg, checked = Fill with a white tick
   static bool FlatCheckbox(const char *id, bool *value);
   // Number of decimals shown for a value in the given display unit (one precision per unit); `requested` is the part's own request
   static int FloatDecimals(Unit displayUnit, int requested, float shown = 0.f);
   // Blender-like number field: the fill slider frame with the value centred and no fill; drag to change, ctrl+click or double-click to type
   static bool NumberField(const char *id, float *value, int decimals);
   static void RoundDragged(float *values, const float *before, int count, int decimals);
   // Esc that cancelled a drag or a typed value: the editor must not treat it as its own Esc. True from the press until the key is released
   static bool ConsumeEscape();
   // Esc press that closes a modal: latched so that its release is not taken as an editor Esc (deselect, cancel)
   static bool ModalEscapePressed() { return ImGui::IsKeyPressed(ImGuiKey_Escape, false) && (s_escConsumed = true); }
   // A drag / typed edit was cancelled this frame and the value restored: the undo step it had started must be dropped. True once
   static bool ConsumeEditCancel()
   {
      const bool cancelled = s_editCancelled;
      s_editCancelled = false;
      return cancelled;
   }
   static float DragSpeed(int decimals, float magnitude) { return max(0.5f * powf(10.f, -static_cast<float>(decimals)), magnitude * 0.005f); }
   bool IsHarvest() const { return m_pinMode == PinMode::Harvest; } // Drawn in the hidden window that registers the pin definitions
   bool IsSkippingRest() const { return m_skipRest; }
   // Moves the cursor below a field whose label wrapped to two lines (called by the next row, sections and separators)
   void FlushRow();

   void Header(const string& typeName, const std::function<string()>& getName, const std::function<void(const string&)>& setName);
   void EditableHeader(const string& typeName, IEditable* editable);
   void Separator(const string& label);
   template <class T> void Checkbox(T* obj, const string& label, const std::function<bool(const T*)>& getter, const std::function<void(T*, bool)>& setter, const char* tooltip = nullptr);
   template <class T> void InputInt(T* obj, const string& label, const std::function<int(const T*)>& getter, const std::function<void(T*, int)>& setter, const char* tooltip = nullptr);
   template <class T>
   void SliderFloat(
      T* obj, const string& label, const std::function<float(const T*)>& getter, const std::function<void(T*, float)>& setter, float vMin, float vMax, int nDecimals, const char* tooltip = nullptr);
   template <class T>
   void InputFloat(
      T* obj, const string& label, const std::function<float(const T*)>& getter, const std::function<void(T*, float)>& setter, Unit unit, int nDecimals, const char* tooltip = nullptr);
   template <class T>
   void InputFloat2(T* obj, const string& label, const std::function<Vertex2D(const T*)>& getter, const std::function<void(T*, const Vertex2D&)>& setter, Unit unit, int nDecimals,
      const char* tooltip = nullptr);
   template <class T>
   void InputFloat3(
      T* obj, const string& label, const std::function<vec3(const T*)>& getter, const std::function<void(T*, const vec3&)>& setter, Unit unit, int nDecimals, const char* tooltip = nullptr);
   template <class T>
   void InputRGB(T* obj, const string& label, const std::function<vec3(const T*)>& getter, const std::function<void(T*, const vec3&)>& setter, const char* tooltip = nullptr);
   template <class T>
   void InputString(T* obj, const string& label, const std::function<string(const T*)>& getter, const std::function<void(T*, const string&)>& setter, const char* tooltip = nullptr);
   template <class T>
   void InputResURI(T* obj, const string& label, const std::function<string(const T*)>& getter, const std::function<void(T*, const string&)>& setter, ResURIKind kind,
      const char* tooltip = nullptr, DisplayFamily displayFamily = DisplayFamily::Any, int semanticType = -1, bool writableOnly = false, int dataFormat = CTLPI_STATE_FORMAT_FLOAT);
   template <class T>
   void Combo(
      T* obj, const string& label, const std::vector<string>& values, const std::function<int(const T*)>& getter, const std::function<void(T*, int)>& setter, const char* tooltip = nullptr);
   template <class T>
   void ImageCombo(T* obj, const string& label, const std::function<string(const T*)>& getter, const std::function<void(T*, const string&)>& setter, const char* tooltip = nullptr);
   template <class T>
   void MaterialCombo(T* obj, const string& label, const std::function<string(const T*)>& getter, const std::function<void(T*, const string&)>& setter, const char* tooltip = nullptr);
   // Emits the shared material block: material combo, optional image, optional "Show in Editor" (disabled when the part has
   // no image), optional normal map and "Object Space" toggle (disabled when the part has no normal map)
   template <class T>
   void MaterialVisuals(T* obj, const string& materialLabel, const std::function<string(const T*)>& materialGet, const std::function<void(T*, const string&)>& materialSet,
      const string& imageLabel, const std::function<string(const T*)>& imageGet, const std::function<void(T*, const string&)>& imageSet, bool PartData<T>::* showInEditor = nullptr,
      string PartData<T>::* normalMap = nullptr, bool PartData<T>::* objectSpace = nullptr)
   {
      if (materialGet && materialSet)
         MaterialCombo<T>(obj, materialLabel, materialGet, materialSet);
      if (imageGet && imageSet)
         ImageCombo<T>(obj, imageLabel, imageGet, imageSet);
      if (showInEditor != nullptr)
      {
         ImGui::BeginDisabled(imageGet && imageGet(obj).empty());
         Checkbox<T>(
            obj, "Show in Editor"s, [showInEditor](const T* o) { return o->m_d.*showInEditor; }, [showInEditor](T* o, bool v) { o->m_d.*showInEditor = v; },
            "Draw the image on this part in the editor (does not change how it renders in game)");
         ImGui::EndDisabled();
      }
      if (normalMap != nullptr)
         ImageCombo<T>(obj, "Normal Map"s, [normalMap](const T* o) { return o->m_d.*normalMap; }, [normalMap](T* o, const string& v) { o->m_d.*normalMap = v; });
      if (objectSpace != nullptr)
      {
         ImGui::BeginDisabled(normalMap == nullptr || (obj->m_d.*normalMap).empty());
         Checkbox<T>(
            obj, "Object Space"s, [objectSpace](const T* o) { return o->m_d.*objectSpace; }, [objectSpace](T* o, bool v) { o->m_d.*objectSpace = v; },
            "Interpret the normal map in object space instead of tangent space (for per-object baked normal maps)");
         ImGui::EndDisabled();
      }
   }
   template <class T>
   void MaterialVisuals(T* obj, const string& materialLabel, string PartData<T>::* material, const string& imageLabel = {}, string PartData<T>::* image = nullptr,
      bool PartData<T>::* showInEditor = nullptr, string PartData<T>::* normalMap = nullptr, bool PartData<T>::* objectSpace = nullptr)
   {
      MaterialVisuals<T>(obj, materialLabel, material != nullptr ? std::function<string(const T*)>([material](const T* o) { return o->m_d.*material; }) : nullptr,
         material != nullptr ? std::function<void(T*, const string&)>([material](T* o, const string& v) { o->m_d.*material = v; }) : nullptr, imageLabel,
         image != nullptr ? std::function<string(const T*)>([image](const T* o) { return o->m_d.*image; }) : nullptr,
         image != nullptr ? std::function<void(T*, const string&)>([image](T* o, const string& v) { o->m_d.*image = v; }) : nullptr, showInEditor, normalMap, objectSpace);
   }
   template <class T>
   void SurfaceCombo(T* obj, const string& label, const std::function<string(const T*)>& getter, const std::function<void(T*, const string&)>& setter, const char* tooltip = nullptr);
   template <class T>
   void LightmapCombo(T* obj, const string& label, const std::function<string(const T*)>& getter, const std::function<void(T*, const string&)>& setter, const char* tooltip = nullptr);
   template <class T>
   void RenderProbeCombo(T* obj, const string& label, const std::function<string(const T*)>& getter, const std::function<void(T*, const string&)>& setter, const char* tooltip = nullptr);
   template <class T>
   void CollectionCombo(T* obj, const string& label, const std::function<string(const T*)>& getter, const std::function<void(T*, const string&)>& setter, const char* tooltip = nullptr);
   template <class T>
   void SoundCombo(T* obj, const string& label, const std::function<string(const T*)>& getter, const std::function<void(T*, const string&)>& setter, const char* tooltip = nullptr);
   template <class T> void Font(T* obj, const std::function<FontDesc(const T*)>& getter, const std::function<void(T*, const FontDesc&)>& setter);

   // Layout helper for fields rendered manually instead of through a typed widget
   void PropertyLabel(const string& label, const char* tooltip = nullptr);

   int GetModifiedField() const { return m_modified; }
   // Label of the field modified last (for the undo history); set by the pinnable fields
   const string &GetModifiedLabel() const { return m_modifiedLabel; }

   // Pinned properties overlay support. Panel: fields register their definition and offer the pin context menu;
   // Harvest: fields only register their definition (pane drawn in a hidden window); Overlay: fields only offer the
   // context menu (the pane draws pinned fields). The owner identifies what provides the definitions: the part's
   // EditorUIPart for part panes (their getters/setters may capture it), nullptr for the edited object itself.
   enum class PinMode
   {
      Off,
      Panel,
      Harvest,
      Overlay
   };
   void SetPinning(PinnedPanel *pins, PinMode mode, const void *owner = nullptr)
   {
      m_pins = pins;
      m_pinMode = pins ? mode : PinMode::Off;
      m_pinOwner = owner;
   }
   void SetPinOwner(const void *owner) { m_pinOwner = owner; }

   // Pinnable field object types and the cast from the pointer resolved by the overlay (PinTable*, Material* or IEditable*)
   template <class T> static constexpr bool IsPinnable = std::is_base_of_v<PinTable, T> || std::is_same_v<T, Material> || std::is_base_of_v<IEditable, T>;
   template <class T> static T *PinCast(void *o)
   {
      if (o == nullptr)
         return nullptr;
      if constexpr (std::is_base_of_v<PinTable, T>)
         return static_cast<T *>(static_cast<PinTable *>(o));
      else if constexpr (std::is_same_v<T, Material>)
         return static_cast<Material *>(o);
      else if constexpr (std::is_base_of_v<IEditable, T>)
         return dynamic_cast<T *>(static_cast<IEditable *>(o));
      else
         return nullptr;
   }

private:
   template <class T, class MakeDraw, class CopyMaker> void PinField(T *obj, const string &label, const MakeDraw &makeDraw, const CopyMaker &makeCopy);
   template <class T, class MakeDraw> void PinField(T *obj, const string &label, const MakeDraw &makeDraw)
   {
      PinField<T>(obj, label, makeDraw, []() { return PinCopyFn(); });
   }
   // Value copy between two parts of a field: the destination must be of the type the field belongs to
   template <class T, class Getter, class Setter> static PinCopyFn MakeCopy(const Getter &getter, const Setter &setter)
   {
      return [getter, setter](void *src, void *dst, bool apply) -> bool
      {
         T *const from = PinCast<T>(src);
         T *const to = PinCast<T>(dst);
         if (from == nullptr || to == nullptr)
            return false;
         if (apply)
            setter(to, getter(from));
         return true;
      };
   }
   // Blender-style number widget shared by FillSlider (fill = true, range vMin..vMax) and NumberField: press and drag changes the value
   // relative to the grab (shift: fine, ctrl: snapped to steps), a click without drag (or ctrl+click, double click, nav) types the value
   static bool DragField(const char *id, float *value, float vMin, float vMax, bool fill, int decimals, const char *format);
   static bool DragFieldInt(const char *id, int *value); // Integer field: dragged and typed as an integer (exact above 2^24)
   static bool DragFieldImpl(const char *id, float *value, int *intValue, float vMin, float vMax, bool fill, int decimals, const char *format);
   // Row of `count` number fields sharing the item width (vector fields)
   static bool DragFieldN(const char *id, float *values, int count, int decimals, const char *format);
   // Name list combos (image, material, surface,...) register a definition that rebuilds their list when drawn
   template <class T>
   using NamedComboFn = void (PropertyPane::*)(T *, const string &, const std::function<string(const T *)> &, const std::function<void(T *, const string &)> &, const char *);
   template <class T>
   void PinNamedCombo(T *obj, const string &label, const std::function<string(const T *)> &getter, const std::function<void(T *, const string &)> &setter, const char *tooltip, NamedComboFn<T> combo);
   void PinContext(const PinKey &key, const IEditable *part, const Material *material);
   static string PinTip(const char *tooltip) { return tooltip ? string(tooltip) : string(); }
   static const char *PinTipPtr(const string &tip) { return tip.empty() ? nullptr : tip.c_str(); }

   PinnedPanel *m_pins = nullptr;
   PinMode m_pinMode = PinMode::Off;
   const void *m_pinOwner = nullptr;
   mutable string m_pinGroup; // Current section or separator, used to pin field groups
   string m_pinSection; // Current section (the Properties tabs sort fields by section)
   ImVec2 m_rowMin; // Top left of the current field row (label included), for the pin context menu
   float m_rowBottom = 0.f; // When the label of the last row wrapped: where the next row starts (screen space), else 0
   Tier m_tier = Tier::Advanced;
   std::function<void(PropertyPane &)> m_afterHeader;
   bool m_skipRest = false;
   bool m_lookSliders = true; // Fields with a known range are fill sliders on every tab (Look and Advanced alike)
   bool m_rowFocusable = false; // The row's last item is its only widget (search palette keyboard focus)
   bool m_pinCombo = true; // Combo registers its own definition (off while a name list combo registers its own)
   static inline PinKey s_focusKey;
   static inline bool s_focusPending = false;
   static inline int s_focusRequestFrame = 0;
   static inline double s_flashUntil = 0.0;
   static inline bool s_escConsumed = false;
   static inline bool s_editCancelled = false;
   static inline PinKey s_overlayKey;
   static inline bool s_overlayPending = false;
   static inline double s_overlayRequestTime = 0.0;
   static inline double s_overlayFlashUntil = 0.0;
   const vector<string> *m_highlight = nullptr;

   static constexpr const char* ICON_SAVE = ICON_LC_SAVE;

   bool IsStartup() const { return m_table->m_liveBaseTable && m_showStartup; }
   template <class T> T* GetStartupObj(T* obj) const;

   // Shared handling of fields that can be synchronized between a live table and its
   // startup version: BeginSyncField resolves the displayed instance and its counterpart
   // (null when there is none), EndSyncField draws the "copy to other version" button.
   template <class T> struct SyncField
   {
      T* display;
      T* other;
   };
   template <class T> SyncField<T> BeginSyncField(T* obj);

   // A co-edited part of a uniform-type multi selection, resolved on both edited/startup sides
   template <class T> struct CoTarget
   {
      T* display; // Counterpart on the same side as the pane's display object (never null)
      T* other; // Counterpart on the other side (nullptr when the part has none)
   };
   // Resolves the co-edited parts on the same side (edited or startup) as the pane's display object
   template <class T> vector<CoTarget<T>> GetCoTargets(const SyncField<T>& sync, T* obj) const { return ResolveTargets(m_coEdited, sync, obj); }
   template <class T> vector<CoTarget<T>> ResolveTargets(const vector<IEditable*>& parts, const SyncField<T>& sync, T* obj) const;
   // True when a field's value differs between the edited part and any co-edited part
   template <class T, class Getter> bool IsMixed(const Getter& getter, const T* display, const vector<CoTarget<T>>& targets) const;
   template <class T, class Getter, class Setter, class V>
   void EndSyncField(const SyncField<T>& sync, const vector<CoTarget<T>>& targets, const Getter& getter, const Setter& setter, const V& displayValue);

   // Shows the optional tooltip of a field when the last submitted widget is hovered
   static void WidgetTooltip(const char* tooltip)
   {
      if (tooltip != nullptr)
         ImGui::SetItemTooltip("%s", tooltip);
   }

   int m_modified = 0;
   string m_modifiedLabel;
   int m_modifyFieldId = 1;

   Unit m_lengthUnit = Unit::None;
   bool m_showStartup = false;
   PinTable* const m_table;
   vector<IEditable*> m_coEdited; // Other parts of a uniform-type selection, co-edited with the pane's object
   bool m_inSection = false;
   bool m_sectionHasSync = false;
   float m_syncWidth = 0.f;
   ImVec2 m_syncPos;
};


// Inline definition of template functions

template <class T> T* PropertyPane::GetStartupObj(T* obj) const
{
   T* startupObj = nullptr;
   if constexpr (std::is_base_of_v<PinTable, T>)
      startupObj = m_table->m_liveBaseTable;
   else if constexpr (std::is_base_of_v<IEditable, T>)
      startupObj = static_cast<T*>(m_table->GetStartupFromLive<IEditable>(obj));
   else
      startupObj = m_table->GetStartupFromLive<T>(obj);
   return startupObj;
}

template <class T> T* PropertyPane::GetEditedPart(T* obj) const
{
   if (m_showStartup)
   {
      T* startupObj = GetStartupObj<T>(obj);
      if (startupObj)
         return startupObj;
   }
   return obj;
}

template <class T, class MakeDraw, class CopyMaker> void PropertyPane::PinField(T *obj, const string &label, const MakeDraw &makeDraw, const CopyMaker &makeCopy)
{
   if constexpr (IsPinnable<T>)
   {
      if (m_pinMode == PinMode::Off || obj == nullptr)
         return;
      PinKey key;
      key.prop = PinKeyProp(label);
      const IEditable *part = nullptr;
      IEditable *editablePart = nullptr;
      const Material *material = nullptr;
      if constexpr (std::is_base_of_v<PinTable, T>)
         key.kind = PinKind::Table;
      else if constexpr (std::is_same_v<T, Material>)
      {
         key.kind = PinKind::Material;
         key.name = obj->m_name;
         material = obj;
      }
      else
      {
         key.kind = PinKind::Part;
         part = obj;
         editablePart = obj;
         key.name = part->GetName();
      }
      if (key.kind != PinKind::Table && key.name.empty())
         return;
      if (m_pinMode == PinMode::Panel || m_pinMode == PinMode::Harvest)
      {
         const void *const owner = (key.kind == PinKind::Part && m_pinOwner) ? m_pinOwner : static_cast<const void *>(obj);
         // A harvest always refreshes the definitions: it records which fields the object shows now, and in which order
         if (m_pinMode == PinMode::Harvest || m_pins->NeedsDef(key.Id(), owner))
            m_pins->SetDef(key, label, m_pinGroup, m_pinSection, owner, makeDraw(), makeCopy(), m_tier == Tier::Common);
      }
      if (m_modified == m_modifyFieldId)
         m_modifiedLabel = label;
      if (m_pinMode == PinMode::Panel || m_pinMode == PinMode::Overlay)
      {
         // Alt held while this field was edited: the new value also goes to the other selected parts (a uniform multi-edit already writes to all)
         if (editablePart != nullptr && m_coEdited.empty() && m_modified == m_modifyFieldId && ImGui::GetIO().KeyAlt)
            m_pins->ApplyToSelected(editablePart, makeCopy());
         PinContext(key, part, material);
      }
   }
}

template <class T> PropertyPane::SyncField<T> PropertyPane::BeginSyncField(T* obj)
{
   T* const startupObj = m_sectionHasSync ? GetStartupObj<T>(obj) : nullptr;
   if (startupObj == nullptr)
      return { obj, nullptr };
   return m_showStartup ? SyncField<T> { startupObj, obj } : SyncField<T> { obj, startupObj };
}

template <class T> vector<PropertyPane::CoTarget<T>> PropertyPane::ResolveTargets(const vector<IEditable*>& parts, const SyncField<T>& sync, T* obj) const
{
   vector<CoTarget<T>> targets;
   if (parts.empty())
      return targets;
   targets.reserve(parts.size());
   const bool editedSide = (sync.display == obj);
   for (IEditable* co : parts)
   {
      T* const edited = dynamic_cast<T*>(co);
      if (edited == nullptr)
         continue;
      T* const startup = GetStartupObj<T>(edited);
      T* const target = editedSide ? edited : startup;
      if (target != nullptr)
         targets.push_back({ target, editedSide ? startup : edited });
   }
   return targets;
}

template <class T, class Getter> bool PropertyPane::IsMixed(const Getter& getter, const T* display, const vector<CoTarget<T>>& targets) const
{
   return std::ranges::any_of(targets, [&getter, display](const CoTarget<T>& target) { return !(getter(target.display) == getter(display)); });
}

template <class T, class Getter, class Setter, class V>
void PropertyPane::EndSyncField(const SyncField<T>& sync, const vector<CoTarget<T>>& targets, const Getter& getter, const Setter& setter, const V& displayValue)
{
   // Esc while a field is being edited restores its initial value: the cancel is noted for the undo bookkeeping
   if (ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
      m_editCancelled = true;
   const bool hasOthers = (sync.other != nullptr) || std::ranges::any_of(targets, [](const CoTarget<T>& target) { return target.other != nullptr; });
   if (!hasOthers)
      return;
   const bool synced = (sync.other == nullptr || displayValue == getter(sync.other))
      && std::ranges::all_of(targets, [&getter](const CoTarget<T>& target) { return target.other == nullptr || getter(target.other) == getter(target.display); });
   if (synced)
      return; // Nothing drawn: the width stays reserved so that rows do not shift, the cursor stays under the field
   ImGui::SetCursorScreenPos(m_syncPos);
   ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Accent);
   if (ImGui::Button(ICON_SAVE))
   {
      if (sync.other != nullptr)
         setter(sync.other, getter(sync.display));
      for (const CoTarget<T>& target : targets)
         if (target.other != nullptr)
            setter(target.other, getter(target.display));
      m_modified = m_modifyFieldId;
   }
   ImGui::PopStyleColor();
   if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
      ImGui::SetTooltip("Copy this value to the %s version", m_showStartup ? "live" : "startup");
}

inline void PropertyPane::TimerSection(IEditable* obj)
{
   if (BeginSection("Timer"s))
   {
      Checkbox<IEditable>(
         obj, "Enable"s, //
         [](const IEditable* pObj) { return pObj->m_timerEnabled; }, //
         [](IEditable* pObj, bool v) { pObj->m_timerEnabled = v; });

      Combo<IEditable>(
         obj, "Timer Mode"s, vector { "Fixed Interval"s, "Per Frame"s, "Game Logic Sync"s }, //
         [](const IEditable* pObj)
         {
            int interval = pObj->m_timerInterval;
            return interval == -2 ? 2 : interval == -1 ? 1 : 0;
         }, //
         [](IEditable* pObj, int v)
         {
            if (v == 2)
               pObj->m_timerInterval = -2;
            else if (v == 1)
               pObj->m_timerInterval = -1;
            else
               pObj->m_timerInterval = 100;
         },
         "When the Timer event is fired: at a fixed interval, once per rendered frame, or on the game logic update tick");
      if (GetEditedPart<IEditable>(obj)->m_timerInterval >= 0)
      {
         InputInt<IEditable>(
            obj, "Interval (ms)"s, //
            [](const IEditable* pObj) { return pObj->m_timerInterval; }, //
            [](IEditable* pObj, int v) { pObj->m_timerInterval = v; });
      }
      EndSection();
   }
}

template <class T>
inline void PropertyPane::Checkbox(T* obj, const string& label, const std::function<bool(const T*)>& getter, const std::function<void(T*, bool)>& setter, const char* tooltip)
{
   if (m_skipRest)
      return;
   assert(m_inSection);
   m_modifyFieldId++;
   PropertyLabel(label, tooltip);
   const SyncField<T> sync = BeginSyncField(obj);
   const vector<CoTarget<T>> targets = GetCoTargets(sync, obj);
   ImGui::PushID(label.c_str());
   bool value = getter(sync.display);
   ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, IsMixed(getter, sync.display, targets));
   if (FlatCheckbox(("##" + label).c_str(), &value))
   {
      setter(sync.display, value);
      for (const CoTarget<T>& target : targets)
         setter(target.display, value);
      m_modified = m_modifyFieldId;
   }
   ImGui::PopItemFlag();
   WidgetTooltip(tooltip);
   EndSyncField(sync, targets, getter, setter, value);
   PinField<T>(obj, label,
      [&]() -> PinDrawFn
      {
         return [label, getter, setter, tip = PinTip(tooltip)](PropertyPane &p, void *o)
         {
            if (T *t = PinCast<T>(o))
               p.Checkbox<T>(t, label, getter, setter, PinTipPtr(tip));
         };
      },
      [&]() { return MakeCopy<T>(getter, setter); });
   ImGui::PopID();
}

template <class T>
inline void PropertyPane::InputInt(T* obj, const string& label, const std::function<int(const T*)>& getter, const std::function<void(T*, int)>& setter, const char* tooltip)
{
   if (m_skipRest)
      return;
   assert(m_inSection);
   m_modifyFieldId++;
   PropertyLabel(label, tooltip);
   m_rowFocusable = true;
   const SyncField<T> sync = BeginSyncField(obj);
   const vector<CoTarget<T>> targets = GetCoTargets(sync, obj);
   ImGui::PushID(label.c_str());
   int value = getter(sync.display);
   ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, IsMixed(getter, sync.display, targets));
   if (DragFieldInt(("##" + label).c_str(), &value)) // Same number field as the float fields
   {
      setter(sync.display, value);
      for (const CoTarget<T>& target : targets)
         setter(target.display, value);
      m_modified = m_modifyFieldId;
   }
   ImGui::PopItemFlag();
   WidgetTooltip(tooltip);
   EndSyncField(sync, targets, getter, setter, value);
   PinField<T>(obj, label,
      [&]() -> PinDrawFn
      {
         return [label, getter, setter, tip = PinTip(tooltip)](PropertyPane &p, void *o)
         {
            if (T *t = PinCast<T>(o))
               p.InputInt<T>(t, label, getter, setter, PinTipPtr(tip));
         };
      },
      [&]() { return MakeCopy<T>(getter, setter); });
   ImGui::PopID();
}

template <class T>
inline void PropertyPane::InputFloat(
   T* obj, const string& label, const std::function<float(const T*)>& getter, const std::function<void(T*, float)>& setter, Unit unit, int nDecimals, const char* tooltip)
{
   if (m_skipRest)
      return;
   assert(m_inSection);
   m_modifyFieldId++;
   int nDecimalAdjust;
   Unit displayUnit = m_lengthUnit; // ResolveUnit will set it to the supported converted display unit
   ResolveUnit(unit, displayUnit, nDecimalAdjust);
   const char* unitLabel = GetUnitLabel(displayUnit);
   PropertyLabel(unitLabel ? (label + " (" + unitLabel + ')') : label, tooltip);
   const SyncField<T> sync = BeginSyncField(obj);
   const vector<CoTarget<T>> targets = GetCoTargets(sync, obj);
   ImGui::PushID(label.c_str());
   float value = getter(sync.display);
   float displayValue = value;
   ConvertUnit(unit, displayUnit, displayValue, nDecimalAdjust);
   const int decimals = FloatDecimals(displayUnit, nDecimals, displayValue);
   const string format = "%." + std::to_string(decimals) + 'f';
   ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, IsMixed(getter, sync.display, targets));
   float vMin, vMax;
   // Ranges are soft (FillSlider extends them to include the value); fields without a known range are number fields
   const bool slider = m_lookSliders && LookSliderRange(label, displayUnit, vMin, vMax);
   m_rowFocusable = true;
   if (slider ? FillSlider(("##" + label).c_str(), &displayValue, vMin, vMax, format.c_str()) : NumberField(("##" + label).c_str(), &displayValue, decimals))
   {
      value = displayValue;
      ConvertUnit(displayUnit, unit, value, nDecimalAdjust);
      setter(sync.display, value);
      for (const CoTarget<T>& target : targets)
         setter(target.display, value);
      m_modified = m_modifyFieldId;
   }
   ImGui::PopItemFlag();
   WidgetTooltip(tooltip);
   EndSyncField(sync, targets, getter, setter, value);
   PinField<T>(obj, label,
      [&]() -> PinDrawFn
      {
         return [label, getter, setter, unit, nDecimals, tip = PinTip(tooltip)](PropertyPane &p, void *o)
         {
            if (T *t = PinCast<T>(o))
               p.InputFloat<T>(t, label, getter, setter, unit, nDecimals, PinTipPtr(tip));
         };
      },
      [&]() { return MakeCopy<T>(getter, setter); });
   ImGui::PopID();
}

template <class T>
inline void PropertyPane::SliderFloat(
   T* obj, const string& label, const std::function<float(const T*)>& getter, const std::function<void(T*, float)>& setter, float vMin, float vMax, int nDecimals, const char* tooltip)
{
   if (m_skipRest)
      return;
   assert(m_inSection);
   m_modifyFieldId++;
   PropertyLabel(label, tooltip);
   m_rowFocusable = true;
   const SyncField<T> sync = BeginSyncField(obj);
   const vector<CoTarget<T>> targets = GetCoTargets(sync, obj);
   ImGui::PushID(label.c_str());
   float value = getter(sync.display);
   const string format = "%." + std::to_string(FloatDecimals(Unit::None, nDecimals, value)) + 'f';
   ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, IsMixed(getter, sync.display, targets));
   // Slider range is a guide: ctrl+click types a value, which may go beyond it (the setters clamp where it matters)
   if (FillSlider(("##" + label).c_str(), &value, vMin, vMax, format.c_str()))
   {
      setter(sync.display, value);
      for (const CoTarget<T>& target : targets)
         setter(target.display, value);
      m_modified = m_modifyFieldId;
   }
   ImGui::PopItemFlag();
   WidgetTooltip(tooltip);
   EndSyncField(sync, targets, getter, setter, value);
   PinField<T>(obj, label,
      [&]() -> PinDrawFn
      {
         return [label, getter, setter, vMin, vMax, nDecimals, tip = PinTip(tooltip)](PropertyPane &p, void *o)
         {
            if (T *t = PinCast<T>(o))
            {
               p.SliderFloat<T>(t, label, getter, setter, vMin, vMax, nDecimals, PinTipPtr(tip));
            }
         };
      },
      [&]() { return MakeCopy<T>(getter, setter); });
   ImGui::PopID();
}

template <class T>
inline void PropertyPane::InputFloat2(
   T* obj, const string& label, const std::function<Vertex2D(const T*)>& getter, const std::function<void(T*, const Vertex2D&)>& setter, Unit unit, int nDecimals, const char* tooltip)
{
   if (m_skipRest)
      return;
   assert(m_inSection);
   m_modifyFieldId++;
   int nDecimalAdjust;
   Unit displayUnit = m_lengthUnit; // ResolveUnit will set it to the supported converted display unit
   ResolveUnit(unit, displayUnit, nDecimalAdjust);
   const int decimals = FloatDecimals(displayUnit, nDecimals);
   const string format = "%." + std::to_string(decimals) + 'f';
   const char* unitLabel = GetUnitLabel(displayUnit);
   PropertyLabel(unitLabel ? (label + " (" + unitLabel + ')') : label, tooltip);
   const SyncField<T> sync = BeginSyncField(obj);
   const vector<CoTarget<T>> targets = GetCoTargets(sync, obj);
   ImGui::PushID(label.c_str());
   Vertex2D value = getter(sync.display);
   Vertex2D displayValue = value;
   ConvertUnit(unit, displayUnit, displayValue.x, nDecimalAdjust);
   ConvertUnit(unit, displayUnit, displayValue.y, nDecimalAdjust);
   ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, IsMixed(getter, sync.display, targets));
   if (DragFieldN(("##" + label).c_str(), &displayValue.x, 2, decimals, format.c_str()))
   {
      value = displayValue;
      ConvertUnit(displayUnit, unit, value.x, nDecimalAdjust);
      ConvertUnit(displayUnit, unit, value.y, nDecimalAdjust);
      setter(sync.display, value);
      for (const CoTarget<T>& target : targets)
         setter(target.display, value);
      m_modified = m_modifyFieldId;
   }
   ImGui::PopItemFlag();
   WidgetTooltip(tooltip);
   EndSyncField(sync, targets, getter, setter, value);
   PinField<T>(obj, label,
      [&]() -> PinDrawFn
      {
         return [label, getter, setter, unit, nDecimals, tip = PinTip(tooltip)](PropertyPane &p, void *o)
         {
            if (T *t = PinCast<T>(o))
               p.InputFloat2<T>(t, label, getter, setter, unit, nDecimals, PinTipPtr(tip));
         };
      },
      [&]() { return MakeCopy<T>(getter, setter); });
   ImGui::PopID();
}

template <class T>
inline void PropertyPane::InputFloat3(
   T* obj, const string& label, const std::function<vec3(const T*)>& getter, const std::function<void(T*, const vec3&)>& setter, Unit unit, int nDecimals, const char* tooltip)
{
   if (m_skipRest)
      return;
   assert(m_inSection);
   m_modifyFieldId++;
   int nDecimalAdjust;
   Unit displayUnit = m_lengthUnit; // ResolveUnit will set it to the supported converted display unit
   ResolveUnit(unit, displayUnit, nDecimalAdjust);
   const int decimals = FloatDecimals(displayUnit, nDecimals);
   const string format = "%." + std::to_string(decimals) + 'f';
   const char* unitLabel = GetUnitLabel(displayUnit);
   PropertyLabel(unitLabel ? (label + " (" + unitLabel + ')') : label, tooltip);
   const SyncField<T> sync = BeginSyncField(obj);
   const vector<CoTarget<T>> targets = GetCoTargets(sync, obj);
   ImGui::PushID(label.c_str());
   vec3 value = getter(sync.display);
   vec3 displayValue = value;
   ConvertUnit(unit, displayUnit, displayValue.x, nDecimalAdjust);
   ConvertUnit(unit, displayUnit, displayValue.y, nDecimalAdjust);
   ConvertUnit(unit, displayUnit, displayValue.z, nDecimalAdjust);
   ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, IsMixed(getter, sync.display, targets));
   if (DragFieldN(("##" + label).c_str(), &displayValue.x, 3, decimals, format.c_str()))
   {
      value = displayValue;
      ConvertUnit(displayUnit, unit, value.x, nDecimalAdjust);
      ConvertUnit(displayUnit, unit, value.y, nDecimalAdjust);
      ConvertUnit(displayUnit, unit, value.z, nDecimalAdjust);
      setter(sync.display, value);
      for (const CoTarget<T>& target : targets)
         setter(target.display, value);
      m_modified = m_modifyFieldId;
   }
   ImGui::PopItemFlag();
   WidgetTooltip(tooltip);
   EndSyncField(sync, targets, getter, setter, value);
   PinField<T>(obj, label,
      [&]() -> PinDrawFn
      {
         return [label, getter, setter, unit, nDecimals, tip = PinTip(tooltip)](PropertyPane &p, void *o)
         {
            if (T *t = PinCast<T>(o))
               p.InputFloat3<T>(t, label, getter, setter, unit, nDecimals, PinTipPtr(tip));
         };
      },
      [&]() { return MakeCopy<T>(getter, setter); });
   ImGui::PopID();
}

template <class T>
inline void PropertyPane::InputRGB(T* obj, const string& label, const std::function<vec3(const T*)>& getter, const std::function<void(T*, const vec3&)>& setter, const char* tooltip)
{
   if (m_skipRest)
      return;
   assert(m_inSection);
   m_modifyFieldId++;
   PropertyLabel(label, tooltip);
   const SyncField<T> sync = BeginSyncField(obj);
   const vector<CoTarget<T>> targets = GetCoTargets(sync, obj);
   ImGui::PushID(label.c_str());
   vec3 value = getter(sync.display);
   ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, IsMixed(getter, sync.display, targets));
   // Bare 0..255 numbers (Blender style) and a bordered swatch, built by hand: ImGui::ColorEdit3 adds R:/G:/B: prefixes on wide fields
   bool colorChanged = false;
   ImGui::BeginGroup();
   {
      const ImGuiStyle& style = ImGui::GetStyle();
      const float swatch = ImGui::GetFrameHeight();
      const float fieldWidth = IM_TRUNC((ImGui::CalcItemWidth() - swatch - 3.f * style.ItemInnerSpacing.x) / 3.f);
      if (ImGui::BeginPopup("##ColorPicker"))
      {
         colorChanged |= ImGui::ColorPicker3("##Picker", &value.x);
         ImGui::EndPopup();
      }
      for (int c = 0; c < 3; c++)
      {
         if (c > 0)
            ImGui::SameLine(0.f, style.ItemInnerSpacing.x);
         int channel = static_cast<int>((&value.x)[c] * 255.f + 0.5f);
         ImGui::SetNextItemWidth(fieldWidth);
         if (ImGui::DragInt(c == 0 ? "##R" : c == 1 ? "##G" : "##B", &channel, 0.5f, 0, 255, "%d"))
         {
            (&value.x)[c] = static_cast<float>(channel) / 255.f;
            colorChanged = true;
         }
      }
      ImGui::SameLine(0.f, style.ItemInnerSpacing.x);
      if (ImGui::ColorButton("##Swatch", ImVec4(value.x, value.y, value.z, 1.f), ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoBorder, ImVec2(swatch, swatch)))
         ImGui::OpenPopup("##ColorPicker");
      ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(0, 0, 0, 100), style.FrameRounding, 0, 1.f);
   }
   ImGui::EndGroup();
   if (colorChanged)
   {
      setter(sync.display, value);
      for (const CoTarget<T>& target : targets)
         setter(target.display, value);
      m_modified = m_modifyFieldId;
   }
   ImGui::PopItemFlag();
   WidgetTooltip(tooltip);
   EndSyncField(sync, targets, getter, setter, value);
   PinField<T>(obj, label,
      [&]() -> PinDrawFn
      {
         return [label, getter, setter, tip = PinTip(tooltip)](PropertyPane &p, void *o)
         {
            if (T *t = PinCast<T>(o))
               p.InputRGB<T>(t, label, getter, setter, PinTipPtr(tip));
         };
      },
      [&]() { return MakeCopy<T>(getter, setter); });
   ImGui::PopID();
}

template <class T>
inline void PropertyPane::InputString(T* obj, const string& label, const std::function<string(const T*)>& getter, const std::function<void(T*, const string&)>& setter, const char* tooltip)
{
   if (m_skipRest)
      return;
   assert(m_inSection);
   m_modifyFieldId++;
   PropertyLabel(label, tooltip);
   const SyncField<T> sync = BeginSyncField(obj);
   const vector<CoTarget<T>> targets = GetCoTargets(sync, obj);
   ImGui::PushID(label.c_str());
   string value = getter(sync.display);
   ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, IsMixed(getter, sync.display, targets));
   if (ImGui::InputText(("##" + label).c_str(), &value))
   {
      setter(sync.display, value);
      for (const CoTarget<T>& target : targets)
         setter(target.display, value);
      m_modified = m_modifyFieldId;
   }
   ImGui::PopItemFlag();
   WidgetTooltip(tooltip);
   EndSyncField(sync, targets, getter, setter, value);
   ImGui::PopID();
}

template <class T>
inline void PropertyPane::InputResURI(T* obj, const string& label, const std::function<string(const T*)>& getter, const std::function<void(T*, const string&)>& setter, ResURIKind kind,
   const char* tooltip, DisplayFamily displayFamily, int semanticType, bool writableOnly, int dataFormat)
{
   if (m_skipRest)
      return;
   assert(m_inSection);
   m_modifyFieldId++;
   PropertyLabel(label, tooltip);
   const SyncField<T> sync = BeginSyncField(obj);
   const vector<CoTarget<T>> targets = GetCoTargets(sync, obj);
   ImGui::PushID(label.c_str());
   string value = getter(sync.display);
   const int fieldId = m_modifyFieldId;
   T* display = sync.display;
   ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
   ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, IsMixed(getter, sync.display, targets));
   if (ImGui::InputText(("##" + label).c_str(), &value))
   {
      setter(display, value);
      for (const CoTarget<T>& target : targets)
         setter(target.display, value);
      m_modified = fieldId;
   }
   ImGui::PopItemFlag();
   WidgetTooltip(tooltip);
   ImGui::SameLine();
   if (ImGui::Button("...", ImVec2(ImGui::GetFrameHeight(), 0.f)))
      ResURIPicker::Open(
         kind, value,
         [this, display, targets, setter, fieldId](const string& uri)
         {
            setter(display, uri);
            for (const CoTarget<T>& target : targets)
               setter(target.display, uri);
            m_modified = fieldId;
         },
         displayFamily, semanticType, writableOnly, dataFormat);
   WidgetTooltip(tooltip);
   if (m_pinMode != PinMode::Harvest) // The hidden harvest pane must not draw the picker a second time
      ResURIPicker::Render();
   EndSyncField(sync, targets, getter, setter, value);
   ImGui::PopID();
}

template <class T>
inline void PropertyPane::Combo(
   T* obj, const string& label, const std::vector<string>& values, const std::function<int(const T*)>& getter, const std::function<void(T*, int)>& setter, const char* tooltip)
{
   if (m_skipRest)
      return;
   assert(m_inSection);
   m_modifyFieldId++;
   PropertyLabel(label, tooltip);
   const SyncField<T> sync = BeginSyncField(obj);
   const vector<CoTarget<T>> targets = GetCoTargets(sync, obj);
   ImGui::PushID(label.c_str());
   int value = getter(sync.display);
   const int displayIndex = values.empty() ? -1 : clamp(value, 0, static_cast<int>(values.size()) - 1);
   const bool mixed = IsMixed(getter, sync.display, targets);
   if (BeginFlatCombo(("##" + label).c_str(), mixed ? "-" : (displayIndex < 0 ? "" : values[displayIndex].c_str())))
   {
      for (size_t i = 0; i < values.size(); i++)
      {
         if (ImGui::Selectable((values[i] + "##Item" + std::to_string(i)).c_str()))
         {
            setter(sync.display, static_cast<int>(i));
            for (const CoTarget<T>& target : targets)
               setter(target.display, static_cast<int>(i));
            value = static_cast<int>(i);
            m_modified = m_modifyFieldId;
         }
      }
      ImGui::EndCombo();
   }
   WidgetTooltip(tooltip);
   EndSyncField(sync, targets, getter, setter, value);
   if (m_pinCombo)
      PinField<T>(obj, label,
         [&]() -> PinDrawFn
         {
            return [label, values, getter, setter, tip = PinTip(tooltip)](PropertyPane &p, void *o)
            {
               if (T *t = PinCast<T>(o))
                  p.Combo<T>(t, label, values, getter, setter, PinTipPtr(tip));
            };
         },
         [&]() { return MakeCopy<T>(getter, setter); });
   ImGui::PopID();
}

template <class T>
inline void PropertyPane::PinNamedCombo(
   T *obj, const string &label, const std::function<string(const T *)> &getter, const std::function<void(T *, const string &)> &setter, const char *tooltip, NamedComboFn<T> combo)
{
   ImGui::PushID(label.c_str()); // Same ID scope as the combo's own row, for the pin context menu
   PinField<T>(obj, label,
      [&]() -> PinDrawFn
      {
         return [label, getter, setter, combo, tip = PinTip(tooltip)](PropertyPane &p, void *o)
         {
            if (T *t = PinCast<T>(o))
               (p.*combo)(t, label, getter, setter, PinTipPtr(tip));
         };
      },
      [&]() { return MakeCopy<T>(getter, setter); });
   ImGui::PopID();
}

template <class T>
inline void PropertyPane::ImageCombo(T* obj, const string& label, const std::function<string(const T*)>& getter, const std::function<void(T*, const string&)>& setter, const char* tooltip)
{
   if (m_skipRest)
      return;
   std::vector<string> images(m_table->m_vimage.size());
   const std::function<string(Texture*)> map = [](const Texture* image) -> string { return image->m_name; };
   std::ranges::transform(m_table->m_vimage.begin(), m_table->m_vimage.end(), images.begin(), map);
   std::sort(images.begin(), images.end(), StrLessNoCase);
   images.insert(images.begin(), ""s);
   m_pinCombo = false;
   Combo<T>(obj, label, images, [&](const T* obj) { return max(0, FindIndexOf(images, getter(obj))); }, [&](T* obj, int v) { setter(obj, images[v]); }, tooltip);
   m_pinCombo = true;
   PinNamedCombo<T>(obj, label, getter, setter, tooltip, &PropertyPane::ImageCombo<T>);
}

template <class T>
inline void PropertyPane::MaterialCombo(T* obj, const string& label, const std::function<string(const T*)>& getter, const std::function<void(T*, const string&)>& setter, const char* tooltip)
{
   if (m_skipRest)
      return;
   std::vector<string> materials(m_table->m_materials.size());
   const std::function<string(Material*)> map = [](const Material* material) { return material->m_name; };
   std::ranges::transform(m_table->m_materials.begin(), m_table->m_materials.end(), materials.begin(), map);
   std::sort(materials.begin(), materials.end(), StrLessNoCase);
   materials.insert(materials.begin(), ""s);
   m_pinCombo = false;
   Combo<T>(obj, label, materials, [&](const T* obj) { return max(0, FindIndexOf(materials, getter(obj))); }, [&](T* obj, int v) { setter(obj, materials[v]); }, tooltip);
   m_pinCombo = true;
   PinNamedCombo<T>(obj, label, getter, setter, tooltip, &PropertyPane::MaterialCombo<T>);
}

template <class T>
inline void PropertyPane::SurfaceCombo(T* obj, const string& label, const std::function<string(const T*)>& getter, const std::function<void(T*, const string&)>& setter, const char* tooltip)
{
   if (m_skipRest)
      return;
   std::vector<string> surfaces;
   for (const IEditable* pe : m_table->GetParts())
      if (pe->GetItemType() == ItemTypeEnum::eItemSurface || pe->GetItemType() == ItemTypeEnum::eItemRamp || pe->GetItemType() == ItemTypeEnum::eItemFlasher)
         surfaces.push_back(pe->GetName());
   std::sort(surfaces.begin(), surfaces.end(), StrLessNoCase);
   surfaces.insert(surfaces.begin(), ""s);
   m_pinCombo = false;
   Combo<T>(obj, label, surfaces, [&](const T* obj) { return max(0, FindIndexOf(surfaces, getter(obj))); }, [&](T* obj, int v) { setter(obj, surfaces[v]); }, tooltip);
   m_pinCombo = true;
   PinNamedCombo<T>(obj, label, getter, setter, tooltip, &PropertyPane::SurfaceCombo<T>);
}

template <class T>
inline void PropertyPane::LightmapCombo(T* obj, const string& label, const std::function<string(const T*)>& getter, const std::function<void(T*, const string&)>& setter, const char* tooltip)
{
   if (m_skipRest)
      return;
   std::vector<string> lightmaps;
   for (const IEditable* pe : m_table->GetParts())
      if (pe->GetItemType() == ItemTypeEnum::eItemLight)
         lightmaps.push_back(pe->GetName());
   std::sort(lightmaps.begin(), lightmaps.end(), StrLessNoCase);
   lightmaps.insert(lightmaps.begin(), ""s);
   m_pinCombo = false;
   Combo<T>(obj, label, lightmaps, [&](const T* obj) { return max(0, FindIndexOf(lightmaps, getter(obj))); }, [&](T* obj, int v) { setter(obj, lightmaps[v]); }, tooltip);
   m_pinCombo = true;
   PinNamedCombo<T>(obj, label, getter, setter, tooltip, &PropertyPane::LightmapCombo<T>);
}

template <class T>
inline void PropertyPane::RenderProbeCombo(
   T* obj, const string& label, const std::function<string(const T*)>& getter, const std::function<void(T*, const string&)>& setter, const char* tooltip)
{
   if (m_skipRest)
      return;
   std::vector<string> renderprobes;
   for (const RenderProbe* probe : m_table->m_vrenderprobe)
      renderprobes.push_back(probe->GetName());
   std::sort(renderprobes.begin(), renderprobes.end(), StrLessNoCase);
   renderprobes.insert(renderprobes.begin(), ""s);
   m_pinCombo = false;
   Combo<T>(obj, label, renderprobes, [&](const T* obj) { return max(0, FindIndexOf(renderprobes, getter(obj))); }, [&](T* obj, int v) { setter(obj, renderprobes[v]); }, tooltip);
   m_pinCombo = true;
   PinNamedCombo<T>(obj, label, getter, setter, tooltip, &PropertyPane::RenderProbeCombo<T>);
}

template <class T>
void PropertyPane::CollectionCombo(T* obj, const string& label, const std::function<string(const T*)>& getter, const std::function<void(T*, const string&)>& setter, const char* tooltip)
{
   if (m_skipRest)
      return;
   std::vector<string> collections;
   for (auto pcol : m_table->GetCollections())
      collections.push_back(pcol->m_name);
   std::sort(collections.begin(), collections.end(), StrLessNoCase);
   collections.insert(collections.begin(), ""s);
   m_pinCombo = false;
   Combo<T>(obj, label, collections, [&](const T* obj) { return max(0, FindIndexOf(collections, getter(obj))); }, [&](T* obj, int v) { setter(obj, collections[v]); }, tooltip);
   m_pinCombo = true;
   PinNamedCombo<T>(obj, label, getter, setter, tooltip, &PropertyPane::CollectionCombo<T>);
}

template <class T>
void PropertyPane::SoundCombo(T* obj, const string& label, const std::function<string(const T*)>& getter, const std::function<void(T*, const string&)>& setter, const char* tooltip)
{
   if (m_skipRest)
      return;
   std::vector<string> sounds;
   for (const VPX::Sound* sound : m_table->m_vsound)
      sounds.push_back(sound->GetName());
   std::sort(sounds.begin(), sounds.end(), StrLessNoCase);
   sounds.insert(sounds.begin(), ""s);
   m_pinCombo = false;
   Combo<T>(obj, label, sounds, [&](const T* obj) { return max(0, FindIndexOf(sounds, getter(obj))); }, [&](T* obj, int v) { setter(obj, sounds[v]); }, tooltip);
   m_pinCombo = true;
   PinNamedCombo<T>(obj, label, getter, setter, tooltip, &PropertyPane::SoundCombo<T>);
}

template <class T> void PropertyPane::Font(T* obj, const std::function<FontDesc(const T*)>& getter, const std::function<void(T*, const FontDesc&)>& setter)
{
   InputString<T>(
      obj, "Font"s, //
      [getter](const T* o) { return getter(o).name; }, //
      [getter, setter](T* o, const string& v)
      {
         FontDesc f = getter(o);
         f.name = v;
         setter(o, f);
      });
   InputFloat<T>(
      obj, "Font Size"s, //
      [getter](const T* o) { return static_cast<float>(getter(o).size) / 10000.f; }, //
      [getter, setter](T* o, float v)
      {
         FontDesc f = getter(o);
         f.size = static_cast<uint32_t>(max(0.f, v) * 10000.f);
         setter(o, f);
      },
      Unit::None, 1);
   Checkbox<T>(
      obj, "Bold"s, //
      [getter](const T* o) { return getter(o).IsBold(); }, //
      [getter, setter](T* o, bool v)
      {
         FontDesc f = getter(o);
         f.weight = v ? 700 : 400;
         setter(o, f);
      });
   Checkbox<T>(
      obj, "Italic"s, //
      [getter](const T* o) { return getter(o).IsItalic(); }, //
      [getter, setter](T* o, bool v)
      {
         FontDesc f = getter(o);
         f.attributes = v ? f.attributes | 0x02 : f.attributes & ~0x02;
         setter(o, f);
      });
}
}
