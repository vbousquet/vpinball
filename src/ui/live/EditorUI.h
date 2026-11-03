// license:GPLv3+

#pragma once

#include "input/InputManager.h"
#include "core/pinundo.h"
#include "imgui/imgui.h"
#include "imguizmo/ImGuizmo.h"
#include "imgui_markdown/imgui_markdown.h"
#include "editor/EditorUIPart.h"
#include "editor/Selection.h"
#include "editor/EditorChrome.h"
#include "editor/OutlinerPanel.h"
#include "editor/PartLibraryPanel.h"
#include "editor/PinnedPanel.h"
#include "editor/PropertiesPanel.h"
#include "editor/RendererInspectionModal.h"
#include "editor/ConvertToPBRModal.h"
#include "editor/ScriptPanel.h"
#include "editor/SearchPalette.h"
#include "math/matrix.h"
#include "renderer/Renderer.h"
#include "unordered_dense.h"
#include "ui/VPXFileFeedback.h"

#include <any>
#include <atomic>
#include <optional>
#include <thread>

class LiveUI;
class PinTable;
class Player;
class InputManager;
class Renderer;
class DragPoint;
class PartGroup;
class FRect3D;
struct HitTestResult;
class IEditable;

namespace VPX
{
class Sound;
}

namespace VPX::EditorUI
{

class EditorUI final
   : private DragPointEditContext
{
   // The editor's UI is split between sub components which, for the time being, access
   // the shared editor state directly (to be refined as domain objects are extracted)
   friend class EditorChrome;
   friend class OutlinerPanel;
   friend class PropertiesPanel;
   friend class RendererInspectionModal;
   friend class ConvertToPBRModal;
   friend class PartLibraryPanel;
   friend class ScriptPanel;
   friend class PinnedPanel;
   friend class SearchPalette;

public:
   EditorUI(LiveUI &liveUI);
   ~EditorUI();

   void Open();
   bool IsOpened() const { return m_isOpened; }
   void Render3D();
   void RenderUI();
   void Close();

   // Switch the editor to a new table (either the base edited table or one of its live copies)
   void SetTable(PinTable *table);

   // Start playing a shallow copy of the edited table (edition is suspended, restored when the play session ends)
   void PlayTest();

   bool IsPreview() const { return m_camMode == ViewMode::PreviewCam; }
   bool IsBackdropEditMode() const { return m_camMode == ViewMode::DesktopBackdrop; }

   bool IsInspectMode() const { return m_table->m_liveBaseTable != nullptr; }

   // Posts a message in the status bar (last operation report, fades after a few seconds)
   void Report(const string &message);

   // True if the top row digit (0..9) is a view navigation key (numpad emulation) rather than a game key: editor UI up and game not running (edit mode, or inspect mode while paused)
   bool IsDigitNavKey(int digit) const;

private:
   // UI Context
   LiveUI &m_liveUI;
   Player *m_player;
   PinTable *m_table; // The table displayed by the player
   InputManager *m_pininput;
   std::unique_ptr<Renderer>& m_renderer;

   // Sub components (UI panels & modals)
   EditorChrome m_chrome;
   OutlinerPanel m_outliner;
   PropertiesPanel m_properties;
   RendererInspectionModal m_inspectionModal;
   ConvertToPBRModal m_pbrConvertModal;
   PartLibraryPanel m_partLibrary;
   ScriptPanel m_scriptPanel;
   PinnedPanel m_pinned; // Floating overlay of pinned properties
   SearchPalette m_palette; // Cmd/Ctrl+K search to field

   Selection m_selection;

   // Multi selection of editable parts: all currently selected parts, with m_selection.GetPart() being the
   // active one (property pane target and gizmo pivot). Empty unless m_selection.GetType() == S_EDITABLE.
   vector<std::shared_ptr<EditorUIPart>> m_multiSel;
   vector<IEditable *> m_dynamicParts; // Parts made dynamic in the UI quadtree because they are selected
   std::shared_ptr<EditorUIPart> m_outlinerAnchor; // Anchor part for shift+click range selection in the outliner
   // Multi selection of shared resources (images, sounds, materials): all selected items, with
   // m_selection's payload being the active one. Empty unless m_selection has the matching type.
   vector<Texture *> m_multiSelImages;
   vector<VPX::Sound *> m_multiSelSounds;
   vector<Material *> m_multiSelMaterials;
   Texture *m_outlinerImageAnchor = nullptr; // Anchors for shift+click range selection in the outliner
   VPX::Sound *m_outlinerSoundAnchor = nullptr;
   Material *m_outlinerMaterialAnchor = nullptr;
   bool m_pickListOpen = false; // Ctrl+click list of the parts under the cursor
   ImVec2 m_pickListPos;
   vector<std::shared_ptr<EditorUIPart>> m_pickList;
   bool m_boxSelectActive = false;
   ImVec2 m_boxSelectStart;
   bool IsPartSelected(const std::shared_ptr<EditorUIPart> &part) const;
   bool IsEditableSelected(const IEditable *editable) const;
   void ClearSelection();
   void SetSelection(const Selection &selection);
   void TogglePartSelection(const std::shared_ptr<EditorUIPart> &part);
   void SelectOutlinerRange(const std::shared_ptr<EditorUIPart> &part);
   void SelectPartsInGroup(const PartGroup *group);
   void MoveSelectionToPartGroup(PartGroup *group); // Move all selected parts/groups to the given group (nullptr = root, part groups only)
   void RayCastParts(const ImVec2 &mousePos, vector<HitTestResult> &vhoHit) const;
   bool IsEditablePickable(const IEditable *editable) const;
   void BoxSelectParts(const ImVec2 &cornerA, const ImVec2 &cornerB, bool add);
   void SelectAllParts();

   // Selection snapshot stored in undo records and restored on undo
   struct UndoSelectionState
   {
      Selection selection;
      vector<std::shared_ptr<EditorUIPart>> multiSel;
      std::shared_ptr<EditorUIPart> outlinerAnchor;
      std::shared_ptr<EditorUIPart> pointEditPart; // Part in drag point edit mode, nullptr when not in that mode
      vector<int> pointSel; // Indices of the selected points in the edited part's curve
      bool pointEditCenter = false; // Whether the part's center point is being edited instead of its drag point curve
   };
   UndoSelectionState CaptureUndoSelection() const;
   void RestoreUndoSelection(const UndoSelectionState &state);

   // Drag point edit mode (entered/exited with Tab when the active selected part has a DragPointCurve):
   // while active, the part's curve points are rendered and can be selected & transformed in the table XY
   // plane. For parts exposing an editable center (light bulb, light sequencer animation center), Tab
   // switches the mode between the drag point curve and the center point before exiting.
   std::shared_ptr<EditorUIPart> m_pointEditPart; // Part whose DragPointCurve is being edited (nullptr when not in point edit mode)
   bool m_pointEditCenter = false; // Edit the part's center point instead of its drag point curve
   bool m_centerSelected = false; // Whether the part's center point is selected in center edit mode
   vector<DragPoint *> m_pointSel; // Selected drag points of the edited part's curve
   Selection m_savedSelection; // Selection state saved on mode entry, restored on exit
   vector<std::shared_ptr<EditorUIPart>> m_savedMultiSel;
   std::shared_ptr<EditorUIPart> m_savedOutlinerAnchor;
   bool m_pointDragPending = false; // Left button is down on a selected point (drag not started yet)
   bool m_pointDragActive = false; // Left button drag is moving the selected points
   float m_pointDragZ = 0.f; // Table Z coordinate of the plane in which points are dragged
   Vertex2D m_pointDragPos; // Last drag position in table coordinates
   void EnterPointEditMode();
   void ExitPointEditMode(bool restoreSelection);
   bool IsPointSelected(const DragPoint *point) const;
   void TogglePointSelection(DragPoint *point);
   DragPoint *HitTestDragPoint(const ImVec2 &mousePos) const;
   bool HitTestEditCenter(const ImVec2 &mousePos) const;
   void BoxSelectPoints(const ImVec2 &cornerA, const ImVec2 &cornerB, bool add);
   Vertex2D UnprojectToPlane(const ImVec2 &mousePos, float z) const;
   void AddPointOnNearestSegment();
   void DeleteSelectedPoints();

   // DragPointEditContext implementation
   const vector<DragPoint *> &GetSelectedPoints() const override { return m_pointSel; }
   bool IsCenterEditMode() const override { return m_pointEditCenter; }
   void BeginPointEdit() override;
   void EndPointEdit() override;

   // Decorated editable parts (kept sorted for the outliner, and indexed by editable)
   vector<std::shared_ptr<EditorUIPart>> m_editables;
   ankerl::unordered_dense::map<const IEditable *, std::shared_ptr<EditorUIPart>> m_editableMap;
   void UpdateEditableList();
   void RestorePartsVisibility() const; // Revert the editor visibility overrides of the table parts (reapplied on next render)

   // Enter/Exit edit mode (manage table backup, dynamic mode,...)
   void ResetCameraFromPlayer();
   void SetBackdropCamera();
   void SetPlayfieldCamera();
   Vertex2D GetUIVisibleFraction() const;

   // Undo support
   void PushUndo(IEditable *part, unsigned int editId, const string &label = string());
   PinUndo m_undo;
   static string CountLabel(const char *verb, size_t count, const string &name); // "Move PartName" / "Move 3 parts"
   bool CanUndo() const;
   bool CanRedo() const;
   void UndoSteps(size_t count); // Edit > Undo (Ctrl+Z), count > 1 for the undo history
   void RedoSteps(size_t count); // Edit > Redo (Ctrl+Shift+Z / Ctrl+Y)
   void RestoreUndoState(const std::any &state);
   void SwitchWorkspace(int index); // Applies a workspace and reports it in the status bar

   // Edit menu actions (also the keyboard shortcuts and the F3 operators)
   bool CanCopy() const;
   bool CanPaste() const;
   bool CanDuplicate() const;
   bool CanDelete() const;
   bool CanSelect() const; // Select all / invert
   void DuplicateSelection();
   void DeleteAction(); // Delete key: the selected drag points in point edit mode, the selected parts otherwise
   void SelectAllAction(); // A key: all curve points in drag point edit mode, the center point in center edit mode, all pickable parts otherwise
   void InvertSelection();

   // Snapping (magnet button of the toolbar), editor preferences: the increments apply to the gizmo and to the G / R / S modal transforms,
   // Ctrl inverts the snapping while transforming
   bool m_snapEnabled = false;
   int m_snapTarget = 0; // 0 = increment (grid), 1 = points (G modal transform, unconstrained: centers and drag points of the unselected parts)
   float m_snapMove = 10.f; // VP units
   float m_snapRotate = 15.f; // Degrees
   float m_snapScale = 0.1f;
   bool m_snapAffectMove = true;
   bool m_snapAffectRotate = true;
   bool m_snapAffectScale = true;
   enum class SnapKind { Move, Rotate, Scale };
   void LoadSnapPreferences();
   void SaveSnapPreferences() const;
   bool IsSnapActive(SnapKind kind) const; // Snapping enabled (inverted while Ctrl is held) and applying to the kind of transform
   void ToggleSnapping();

   // Adjust Last Operation (Blender's redo panel, F9): the parameters of the last move / rotate / scale / duplicate / paste / add part,
   // editable as long as nothing else happened (no new undo step, same selection). Adjusting re-evaluates the operation from the state it
   // started from, the undo record (which holds the state before the operation) is unchanged: one Ctrl+Z still reverts all.
   struct LastOperation
   {
      enum class Type { None, Add, Paste, Duplicate, Move, Rotate, Scale };
      Type type = Type::None;
      uint64_t undoId = 0; // PinUndo::TopRecordId() after the operation
      vector<std::shared_ptr<EditorUIPart>> selection;
      Matrix3D initial; // Transform of the selection before (move, rotate, scale) or after (placements) the operation
      vec3 base { 0.f, 0.f, 0.f }; // Placements: position the offset is relative to (table coordinates)
      vec3 offset { 0.f, 0.f, 0.f }; // Move: delta, placements: offset from the base (table coordinates)
      vec3 axisView { 0.f, 0.f, 1.f }; // Rotation about the view axis: the axis when the operation was done (camera space)
      int axis = -1; // Rotate / scale: -1 view axis (rotate) or all axes (scale), 0..2 for X / Y / Z
      bool plane = false; // Scale: the axes perpendicular to the axis
      float angle = 0.f; // Degrees
      float factor = 1.f;
      bool expanded = true;
   } m_lastOp;
   bool m_adjustPopupOpen = false; // F9
   bool m_adjustPopupJustOpened = false;
   ImVec2 m_adjustPopupPos {};
   void SetLastOperation(LastOperation::Type type, const Matrix3D &initial);
   void ApplyLastOperation();
   bool RenderAdjustParameters(); // Widgets of the parameters, true if one changed
   void RenderAdjustPanel(); // Panel at the bottom left of the viewport, and the F9 popup
   static const char *LastOperationName(LastOperation::Type type);

   // Status bar report (see Report)
   string m_report;
   double m_reportTime = -1000.0;
   IEditable *m_lastUndoPart = nullptr;
   unsigned int m_lastUndoId = 0;
   bool m_gizmoDragActive = false; // A gizmo drag recorded an undo step and is still in progress
   vector<std::shared_ptr<EditorUIPart>> m_gizmoDragSelection; // Selection the drag was started on (a changed selection must not follow it)
   bool m_suppressNextEscape = false; // The Esc release of an Esc that deactivated an edited field (e.g. the script editor) is not an editor Esc

   // Add/Remove parts
   void DeleteSelection();
   ItemTypeEnum m_addPartType = eItemInvalid; // Part type pending placement (eItemInvalid when not in add part mode)
   void CreatePart(ItemTypeEnum type, const Vertex2D &pos);
   void CreateCollection(bool fromSelection); // Create a collection, optionally populated with the selected parts
   PartGroup *GetPartGroupForNewPart();
   vector<PartGroup *> m_partGroupUseHistory; // Recently used insertion target part groups, most recent first

   enum class NewTableTemplate
   {
      Blank,
      Stripped,
      Example,
      LightSeq
   };

   // File operations (the 'Save As' and 'Load' file dialogs are asynchronous: their result is applied in RenderUI)
   void SaveTable();
   void SaveTableAs();
   void SaveTableAsPackFolder(); // Save as a VPZ folder pack
   void LoadTable();
   void LoadTableFolder(); // Load a VPZ folder pack
   void NewTable(NewTableTemplate templateType);
   void ShowLoadTableDialog(bool folder = false);
   void LoadTableDialog(bool folder);
   void LoadTableTemplate(NewTableTemplate templateType);
   std::shared_ptr<string> m_pendingSaveAsPath;
   std::shared_ptr<string> m_pendingLoadPath;
   std::shared_ptr<string> m_pendingUnityPrefabPath;
   bool m_pendingSaveAsFolder = false; // Pending 'Save As' result is a folder pack, not a file
   bool m_pendingLoadFolder = false; // Pending 'Load' result is a folder pack, not a file
   std::optional<NewTableTemplate> m_pendingNewTable; // New table template awaiting the 'discard unsaved changes' confirmation
   bool m_confirmLoadTable = false; // Request the 'discard unsaved changes' confirmation popup in RenderUI

   // Asynchronous file operations: table loading and saving run on a worker thread while a modal progress
   // feedback blocks all user interaction. The result is applied on the main thread in RenderUI.
   struct FileOpFeedback final : public VPXFileFeedback
   {
      std::atomic<unsigned int> progress { 0 };
      std::atomic<unsigned int> length { 0 };
      void SetProgress(const unsigned int value) override { progress = value; }
      void SetLength(const unsigned int value) override { length = value; }
   };
   struct AsyncFileOp
   {
      enum class Type
      {
         Load,
         NewTable,
         Save
      };
      ~AsyncFileOp();
      Type type = Type::Load;
      std::filesystem::path filename; // File being loaded or saved
      string displayPath; // Path displayed in the progress modal
      FileOpFeedback feedback;
      HRESULT result = E_FAIL; // Written by the worker thread before flagging the operation as completed
      std::atomic<bool> completed { false };
      std::thread worker;
      PinTable *table = nullptr; // Loaded table (load ops only): reference adopted by the player on success, released on failure
      PinTable *saveTable = nullptr; // Table written by a save op (the startup table when saving from an inspection session)
      // 'Save As' restore data: filename and title to restore when the save fails
      bool restoreOnFailure = false;
      std::filesystem::path previousFilename;
      string previousTitle;
   };
   // Inspection session: live copies edited from the 'Live' tabs, copied back to the startup table by 'Save'
   // (the startup table is the one written to disk; live values are otherwise discarded when the session ends)
   ankerl::unordered_dense::set<Material *> m_liveEditedMaterials;
   ankerl::unordered_dense::set<IEditable *> m_liveEditedParts;
   std::unique_ptr<AsyncFileOp> m_fileOp; // Running asynchronous file operation (all editor interaction is blocked while set)
   void StartFileOp(std::unique_ptr<AsyncFileOp> op);
   void FinishFileOp();
   void RenderFileOpModal();
   // Whether a save of the edited table is running on the file operation worker thread: while it is, the
   // UI parts must not write into the parts' data which is being serialized (see EditorUI::Render3D)
   bool IsSaveInProgress() const { return m_fileOp != nullptr && m_fileOp->type == AsyncFileOp::Type::Save; }

   // Part library (import/export of parts through partial VPZ packs, handled by the PartLibraryPanel dialog)
   void OpenPartLibrary() { m_partLibrary.Show(); }
   bool CanExportPartSelection() const { return m_partLibrary.CanExportSelection(); }
   void ExportPartSelection() { m_partLibrary.ExportSelection(); }

   // Unity prefab import (Tools > Import Unity Prefab...)
   void PickUnityPrefab();
   void ImportUnityPrefab(const std::filesystem::path &path);

public:
   // Closes the session with the given Player::CloseState, first asking to discard unsaved changes when they would be lost
   void RequestClose(int closeState);
private:
   std::optional<int> m_pendingClose; // Close state awaiting the 'discard unsaved changes' confirmation

   // Clipboard (copy/paste of parts through the OS clipboard, and of drag point coordinates in point edit mode)
   void CopySelection();
   void PasteSelection(const ImVec2 &pos);

   // Rendering
   enum class PhysicOverlay
   {
      None,
      Selected,
      All
   };
   PhysicOverlay m_physOverlay = PhysicOverlay::None;
   bool m_selectionOverlay = true;
   enum class SelectionFilter : uint32_t
   {
      None = 0,
      Playfield = 0x0002,
      Primitives = 0x0004,
      Lights = 0x0008,
      Flashers = 0x0010,
      All = 0x0002 | 0x0004 | 0x0008 | 0x0010
   };
   friend constexpr SelectionFilter operator|(const SelectionFilter a, const SelectionFilter b) { return static_cast<SelectionFilter>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); }
   friend constexpr SelectionFilter operator&(const SelectionFilter a, const SelectionFilter b) { return static_cast<SelectionFilter>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b)); }
   friend constexpr SelectionFilter operator~(const SelectionFilter a) { return static_cast<SelectionFilter>(~static_cast<uint32_t>(a)); }
   static constexpr bool HasFlag(const SelectionFilter flags, const SelectionFilter flag) { return (flags & flag) != SelectionFilter::None; }
   SelectionFilter m_selectionFilter = SelectionFilter::All;

   // UI state
   bool m_isOpened = false;
   bool m_flyMode = false;
   enum class Units
   {
      VPX, Metric, Imperial
   } m_units = Units::VPX;

   // 3D editor
   ImGuizmo::OPERATION m_gizmoOperation = (ImGuizmo::OPERATION)0;
   bool m_hadSelectionTransform = false; // Whether the previous frame had a selection to transform
   ImGuizmo::MODE m_gizmoMode = ImGuizmo::WORLD;
   void SetGizmoOperation(ImGuizmo::OPERATION operation); // Edit mode actions of the Esc (select), G (grab), S (scale) and R (rotate) shortcuts
   bool GetSelectionTransform(Matrix3D &transform) const;
   bool GetSelectionBounds(FRect3D &bounds) const;

   // Blender style modal transform (G / R / S then move the mouse, X / Y / Z to constrain to an axis (Shift for the plane
   // perpendicular to it), type a number for an exact value, Enter / left click to confirm, Esc / right click to cancel)
   enum class ModalOp { Grab, Rotate, Scale };
   struct ModalTransform
   {
      bool active = false;
      ModalOp op = ModalOp::Grab;
      int axis = -1; // Constraint axis (0..2 for X/Y/Z), -1 for none (view plane / view axis / uniform)
      bool plane = false; // Constrain to the plane perpendicular to the axis
      bool hasUndo = false; // An undo record was started when the transform began (canceling reverts it)
      string typed; // Typed numeric value
      string hud; // Status text drawn over the render
      ImGuizmo::OPERATION savedGizmo = ImGuizmo::OPERATION(0); // Gizmo tool to restore when the transform ends
      ImVec2 startMouse;
      float startAngle = 0.f, startDist = 1.f; // Angle and distance of the start mouse position around the pivot on screen
      Matrix3D initial; // Selection transform when the operation began
      vec3 duplicateOrigin { 0.f, 0.f, 0.f }; // Position of the duplicated selection
      bool duplicate = false; // Grab following a duplicate: its undo record is folded into the one of the paste on confirmation
      vec3 lastDelta { 0.f, 0.f, 0.f }; // Parameters of the last evaluation (kept as the adjustable parameters of the operation when confirmed)
      vec3 lastAxis { 0.f, 0.f, 1.f }; // Rotation axis (in the editor camera space, see FlipZ)
      float lastAngle = 0.f; // Degrees
      float lastFactor = 1.f;
      vector<Vertex2D> snapPoints; // Snap targets of the 'points' snapping (centers and drag points of the unselected parts), built on first use
      bool snapPointsBuilt = false;
   } m_modal;
   Matrix3D m_gizmoStartTransform; // Selection transform when the current gizmo drag began
   bool m_gizmoDragTranslate = false; // The current gizmo drag is a translation
   void BeginModalTransform(ModalOp op);
   void UpdateModalTransform();
   void EndModalTransform(bool confirm);
   void FrameBounds(const FRect3D &bounds); // Center the editor camera on the bounds and adjust the zoom to fit them
   bool IsNumpadPressed(int digit) const;
   bool GetNavPivot(vec3 &target) const; // Orbit / zoom pivot (selection center), false if none
   void SetSelectionTransform(const Matrix3D &transform, bool clearPosition = false, bool clearScale = false, bool clearRotation = false) const;

   // Editor camera
   ViewMode m_camMode = ViewMode::PreviewCam;
   bool m_perspectiveCam = true;
   Renderer::ShadeMode m_shadeMode = Renderer::ShadeMode::Default;
   enum class PredefinedView
   {
      None, Front, Back, Right, Left, Top, Bottom
   } m_predefinedView = PredefinedView::None;
   bool m_navZoomToMouse = true; // Navigation preference: wheel zoom goes toward the cursor
   int m_navSelectionDisplay = 0; // 0 = edges only, 1 = edges and tint, 2 = hidden
   bool m_navAutoDepth = true; // Navigation preference: with no selection, orbit around the 3D point under the cursor
   bool m_orbitDragging = false; // Orbit / pan drag in progress
   bool m_orbitPivotValid = false; // m_orbitPivot (camera space) is the orbit pivot of the current drag
   vec3 m_orbitPivot;
   bool PickDepthPoint(const ImVec2 &mousePos, vec3 &point) const;
   float m_navFovY = 39.6f; // Editor perspective camera vertical field of view in degrees (50 mm lens equivalent)
   void SetFov(float fovY);
   bool m_navInvertZoom = false; // Navigation preferences (stored in the Editor section of the settings)
   bool m_navAltMiddleButton = true;
   bool m_navTrackpad = true;
   float m_navOrbitSensitivity = 1.f;
   void LoadNavPreferences();
   bool m_navBlenderTransform = true; // Navigation preference: G / R / S start a modal transform instead of the gizmo
   bool m_navEmulateNumpad = true; // Navigation preference: top row digits act as the numpad view keys
   bool m_navOrbitSelection = true; // Navigation preference: orbit and zoom around the selection
   void SetView(PredefinedView view); // Axis aligned view (also reachable from the View menu)
   void CycleCamera();
   void FrameSelection();
   void FrameAll();
   void OrbitStep(bool yaw, float sign); // 15 degree orbit step
   bool m_orbitLock = false; // When locked, camera drags pan instead of orbiting
   bool m_fitPlayfieldCamera = false;
   Matrix3D m_camView, m_camProj;
   float m_camDistance;
   bool m_showNavCube = true; // View > Navigation Cube
   bool m_showOutliner = true; // View > Outliner (workspaces)
   bool m_showProperties = true; // View > Properties (workspaces)
   // Areas (Blender-style fixed layout: outliner | viewport + N-panel | properties): widths in logical pixels, resized by the
   // splitters between areas, saved per workspace
   float m_outlinerWidth = OutlinerPanel::PaneWidth;
   float m_propertiesWidth = PropertiesPanel::PaneWidth;
   float m_sidebarWidth = 300.f;
   // N-panel (Blender's sidebar at the viewport's right edge): tabs Item (selection transform), Favorites (the Quick Favorites set), View
   enum class SidebarTab
   {
      Item,
      Favorites,
      View
   };
   bool m_showSidebar = false;
   SidebarTab m_sidebarTab = SidebarTab::Item;
   void ToggleSidebar(); // N
   void RenderSplitters();
   bool GetAreaUnderMouse(AreaEditor &type, bool &sidebar) const; // Area editor under the mouse (or the N-panel), false over the viewport
   void ToggleAreaEditor(AreaEditor type); // View menu: hide / show the area hosting the type, or host it
   void RenderAreas(float topBarHeight); // Hosts the panels of the left and right areas
   void RenderAreaTypeButton(AreaEditor current); // Header button of the area hosting the given type, and its editor type popup
   bool ConsumedAreaEscape(); // The Esc that closed the editor type popup is not an editor Esc
   bool m_areaEscPending = false;
   bool ConsumedPaneEscape(); // The Esc that cancelled a property pane field edit is not an editor Esc
   bool m_paneEscPending = false;
   bool BeginAreaWindow(const char *name, const ImVec2 &pos, const ImVec2 &size); // Window filling an area rect (Script and Part Library), always pair with EndAreaWindow
   void EndAreaWindow();
   void ShowSidebar(SidebarTab tab);
   void OpenQuickFavorites() { m_openQuickFavorites = true; } // Q: popup of the Quick Favorites at the mouse
   bool m_openQuickFavorites = false;
   void RenderNavCube(); // ImGuizmo view cube: drag orbits around the orbit pivot, click a face snaps to the axis view
};

}
