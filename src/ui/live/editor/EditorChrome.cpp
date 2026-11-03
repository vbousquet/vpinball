// license:GPLv3+

#include "core/stdafx.h"
#include "EditorChrome.h"
#include "IconsLucide.h"

#include "core/FileLocator.h"
#include "core/VPApp.h"
#include "renderer/Sampler.h"
#include "renderer/Texture.h"
#include "ui/EditorClipboard.h"
#include "ui/live/EditorUI.h"
#include "ui/live/LiveUI.h"
#include "parts/light.h"
#include "imgui/imgui_internal.h" // FindWindowByName (placement of the tool shelf relative to the Adjust Last Operation panel)

namespace VPX::EditorUI
{

namespace
{
constexpr float ToolButtonSize = 28.f; // Toolbar button (square), in pixels at 100% DPI
constexpr float ToolGroupGap = 8.f; // Gap between button groups (holds the 1 px separator)
constexpr float ToolSeparatorHeight = 16.f;
constexpr float StatusBarHeight = 22.f; // Bottom status bar, in pixels at 100% DPI
#ifdef __APPLE__
constexpr const char *ModKey = "Cmd"; // ImGui maps Cmd to Ctrl on macOS
#else
constexpr const char *ModKey = "Ctrl";
#endif
float s_toolDpi = 1.f;

// Toolbar icon button: transparent, hovered = Hover, active (mode on) = Raised background with Accent icon
bool ToolButton(const char *icon, bool active = false)
{
   ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::V(active ? EditorTheme::Raised : 0u));
   ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::V(EditorTheme::Hover));
   ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::V(EditorTheme::Hover));
   ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::V(active ? EditorTheme::Accent : EditorTheme::Text));
   const bool pressed = ImGui::Button(icon, ImVec2(ToolButtonSize * s_toolDpi, ToolButtonSize * s_toolDpi));
   ImGui::PopStyleColor(4);
   return pressed;
}

// Magnet icon (the Lucide subset has none): a U shaped bar with light tips, centered at c, radius r
void DrawMagnet(ImDrawList *drawList, const ImVec2 &c, const float r, const ImU32 color)
{
   const float w = r * 0.5f; // Half width of the U (arc radius)
   const float thickness = max(2.f, r * 0.28f);
   const float top = c.y - r * 0.2f; // Where the arc starts
   const float bottom = c.y + r * 0.2f; // Where the legs end
   drawList->PathArcTo(ImVec2(c.x, top), w, IM_PI, IM_PI * 2.f, 16);
   drawList->PathStroke(color, 0, thickness);
   drawList->AddLine(ImVec2(c.x - w, top), ImVec2(c.x - w, bottom), color, thickness);
   drawList->AddLine(ImVec2(c.x + w, top), ImVec2(c.x + w, bottom), color, thickness);
   drawList->AddLine(ImVec2(c.x - w, bottom), ImVec2(c.x - w, bottom + r * 0.25f), EditorTheme::Rgb(230, 230, 230), thickness); // Tips
   drawList->AddLine(ImVec2(c.x + w, bottom), ImVec2(c.x + w, bottom + r * 0.25f), EditorTheme::Rgb(230, 230, 230), thickness);
}

// Text drawn over the render (view name, tool name): 1 px drop shadow so that it stays readable on light renders
void ShadowedText(const char *text)
{
   const ImVec2 pos = ImGui::GetCursorScreenPos();
   ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + 1.f, pos.y + 1.f), EditorTheme::TextShadow, text);
   ImGui::TextUnformatted(text);
}

// Group separator: 1 px vertical line centered in the group gap
void ToolSeparator()
{
   const float half = 0.5f * (ToolGroupGap - 1.f) * s_toolDpi;
   ImGui::SameLine(0.f, half);
   const ImVec2 pos = ImGui::GetCursorScreenPos();
   const float inset = 0.5f * (ToolButtonSize - ToolSeparatorHeight) * s_toolDpi;
   ImGui::GetWindowDrawList()->AddLine(ImVec2(pos.x, pos.y + inset), ImVec2(pos.x, pos.y + inset + ToolSeparatorHeight * s_toolDpi), EditorTheme::Border, 1.f);
   ImGui::Dummy(ImVec2(1.f, ToolButtonSize * s_toolDpi));
   ImGui::SameLine(0.f, half);
}

// Edit mode selection (same actions as the Esc, G, R and S keyboard shortcuts), shown in the tool shelf or in the top toolbar when the shelf is hidden
const struct
{
   ImGuizmo::OPERATION operation;
   const char *icon;
   const char *tooltip;
} editModes[] = {
   { (ImGuizmo::OPERATION)0, ICON_LC_MOUSE_POINTER_2, "Select\n[Esc]" }, //
   { ImGuizmo::TRANSLATE, ICON_LC_MOVE, "Grab\n[G]" }, //
   { ImGuizmo::ROTATE, ICON_LC_ROTATE_3D, "Rotate\n[R]" }, //
   { ImGuizmo::SCALE, ICON_LC_SCALE_3D, "Scale\n[S]" }, //
};
}

void EditorChrome::Render(bool showFullUI)
{
   // Gives some transparency when positioning camera to better view camera view bounds
   // TODO for some reasons, this breaks the modal background behavior
   //SetupImGuiStyle(m_editor.m_selection.GetType() == Selection::S_CAMERA ? 0.3f : 1.0f);

   m_menuBarHeight = 0.0f;
   m_toolBarHeight = showFullUI ? (ToolButtonSize + 4.f) * m_editor.m_liveUI.GetDPI() + 1.f : 0.f; // + 1 px separator at the bottom, panels start below it
   m_statusBarHeight = (showFullUI && m_showStatusBar) ? StatusBarHeight * m_editor.m_liveUI.GetDPI() : 0.f;

   if (!showFullUI)
      return;

   RenderMenuBar();
   RenderToolbar();
   RenderStatusOverlay();
   RenderUndoHistory();
}

void EditorChrome::RenderMenuBar()
{
   EditorUI &editor = m_editor;
   if (ImGui::BeginMainMenuBar())
   {
      if (!editor.IsInspectMode() && ImGui::BeginMenu("File"))
      {
         if (ImGui::BeginMenu("New"))
         {
            if (ImGui::MenuItem("New Table", "Ctrl+N"))
               editor.NewTable(EditorUI::NewTableTemplate::Blank);
            if (ImGui::MenuItem("Completely Blank Table"))
               editor.NewTable(EditorUI::NewTableTemplate::Stripped);
            if (ImGui::MenuItem("Full Example Table"))
               editor.NewTable(EditorUI::NewTableTemplate::Example);
            if (ImGui::MenuItem("Light Sequence Demo Table"))
               editor.NewTable(EditorUI::NewTableTemplate::LightSeq);
            ImGui::EndMenu();
         }
         if (ImGui::MenuItem("Load..."))
            editor.LoadTable();
         if (ImGui::MenuItem("Load Pack Folder..."))
            editor.LoadTableFolder();
         if (ImGui::MenuItem("Save", "Ctrl+S"))
            editor.SaveTable();
         if (ImGui::MenuItem("Save As..."))
            editor.SaveTableAs();
         if (ImGui::MenuItem("Save As Pack Folder..."))
            editor.SaveTableAsPackFolder();
         ImGui::Separator();
         if (ImGui::MenuItem("Play", "F5", false, !editor.m_table->IsLocked()))
            editor.PlayTest();
         ImGui::Separator();
         if (ImGui::MenuItem("Quit"))
            editor.RequestClose(Player::CS_CLOSE_APP);
         ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Edit"))
      {
         if (ImGui::MenuItem("Undo", "Ctrl+Z", false, editor.CanUndo()))
            editor.UndoSteps(1);
         if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, editor.CanRedo()))
            editor.RedoSteps(1);
         if (ImGui::MenuItem("Undo History...", nullptr, false, editor.m_table->m_liveBaseTable == nullptr && (editor.m_undo.HasUndo() || editor.m_undo.HasRedo())))
            OpenUndoHistory();
         if (ImGui::MenuItem("Adjust Last Operation...", "F9", false, editor.m_lastOp.type != EditorUI::LastOperation::Type::None))
         {
            editor.m_adjustPopupOpen = true;
            editor.m_adjustPopupJustOpened = true;
            editor.m_adjustPopupPos = ImGui::GetMousePos();
         }
         if (ImGui::MenuItem("Snapping", "Shift+Tab", editor.m_snapEnabled))
            editor.ToggleSnapping();
         ImGui::Separator();
         if (ImGui::MenuItem("Copy", "Ctrl+C", false, editor.CanCopy()))
            editor.CopySelection();
         if (ImGui::MenuItem("Paste", "Ctrl+V", false, editor.CanPaste()))
         {
            const ImGuiViewport *const viewport = ImGui::GetMainViewport();
            editor.PasteSelection(ImVec2(viewport->GetCenter().x, viewport->GetCenter().y));
         }
         if (ImGui::MenuItem("Duplicate", "Shift+D", false, editor.CanDuplicate()))
            editor.DuplicateSelection();
         if (ImGui::MenuItem("Delete", "Del", false, editor.CanDelete()))
            editor.DeleteAction();
         ImGui::Separator();
         if (ImGui::MenuItem("Select All", "A", false, editor.CanSelect()))
            editor.SelectAllAction();
         if (ImGui::MenuItem("Deselect", "Alt+A", false, editor.CanSelect() && editor.m_selection.GetType() != Selection::S_NONE))
            editor.ClearSelection();
         if (ImGui::MenuItem("Invert Selection", "Ctrl+I", false, editor.CanSelect()))
            editor.InvertSelection();
         ImGui::EndMenu();
      }
      if (!editor.IsInspectMode() && ImGui::BeginMenu("Tools"))
      {
         if (ImGui::MenuItem("Convert to PBR...", nullptr, false, !editor.m_table->IsLocked()))
            editor.m_pbrConvertModal.Show();
         if (ImGui::MenuItem("Import Unity Prefab...", nullptr, false, !editor.m_table->IsLocked()))
            editor.PickUnityPrefab();
         if (ImGui::BeginMenu("Light States"))
         {
            // The editor does not run the table script (which usually sets the lamp states): set the light state of the table or of a collection here
            const auto setStates = [&](const vector<IEditable *> &parts, const float state)
            {
               for (IEditable *const part : parts)
                  if (part->GetItemType() == eItemLight)
                     static_cast<Light *>(part)->put_State(state);
            };
            const float states[] = { 0.f, 0.706f, 1.f };
            const char *stateNames[] = { "Off", "0.706 (lamp top)", "On" };
            if (ImGui::BeginMenu("All Lights"))
            {
               for (int i = 0; i < 3; i++)
                  if (ImGui::MenuItem(stateNames[i]))
                     setStates(editor.m_table->GetParts(), states[i]);
               ImGui::EndMenu();
            }
            for (auto *const collection : editor.m_table->GetCollections())
            {
               bool hasLight = false;
               for (const IEditable *const part : collection->GetParts())
                  hasLight |= part->GetItemType() == eItemLight;
               if (hasLight && ImGui::BeginMenu(collection->m_name.c_str()))
               {
                  for (int i = 0; i < 3; i++)
                     if (ImGui::MenuItem(stateNames[i]))
                        setStates(collection->GetParts(), states[i]);
                  ImGui::EndMenu();
               }
            }
            ImGui::EndMenu();
         }
         if (ImGui::BeginMenu("Navigation"))
         {
            Settings &settings = g_settingsService.GetAppSettings();
            if (ImGui::MenuItem("Orbit Around Selection", nullptr, &editor.m_navOrbitSelection))
               settings.SetEditor_NavOrbitSelection(editor.m_navOrbitSelection, false);
            if (ImGui::MenuItem("Zoom To Mouse Position", nullptr, &editor.m_navZoomToMouse))
               settings.SetEditor_NavZoomToMouse(editor.m_navZoomToMouse, false);
            if (ImGui::MenuItem("Auto Depth (Orbit Around Point Under Cursor)", nullptr, &editor.m_navAutoDepth))
               settings.SetEditor_NavAutoDepth(editor.m_navAutoDepth, false);
            if (ImGui::MenuItem("Invert Zoom", nullptr, &editor.m_navInvertZoom))
               settings.SetEditor_NavInvertZoom(editor.m_navInvertZoom, false);
            if (ImGui::MenuItem("Alt + Drag Acts As Middle Button", nullptr, &editor.m_navAltMiddleButton))
               settings.SetEditor_NavAltMiddleButton(editor.m_navAltMiddleButton, false);
            if (ImGui::MenuItem("Trackpad (Scroll Pans, Ctrl/Cmd+Scroll Zooms)", nullptr, &editor.m_navTrackpad))
               settings.SetEditor_NavTrackpad(editor.m_navTrackpad, false);
            if (ImGui::MenuItem("Emulate Numpad (Top Row Digits)", nullptr, &editor.m_navEmulateNumpad))
               settings.SetEditor_NavEmulateNumpad(editor.m_navEmulateNumpad, false);
            if (ImGui::MenuItem("Blender Transform Keys (G / R / S)", nullptr, &editor.m_navBlenderTransform))
               settings.SetEditor_NavBlenderTransform(editor.m_navBlenderTransform, false);
            float fov = editor.m_navFovY;
            if (ImGui::SliderFloat("Field Of View", &fov, 15.f, 100.f, "%.1f deg"))
            {
               editor.SetFov(fov);
               settings.SetEditor_NavFovY(editor.m_navFovY, false);
            }
            const char *selectionModes[] = { "Selection: Edges", "Selection: Edges + Tint", "Selection: Hidden" };
            if (ImGui::BeginMenu(selectionModes[editor.m_navSelectionDisplay]))
            {
               for (int i = 0; i < 3; i++)
                  if (ImGui::MenuItem(selectionModes[i] + 11, nullptr, editor.m_navSelectionDisplay == i))
                  {
                     editor.m_navSelectionDisplay = i;
                     settings.SetEditor_NavSelectionDisplay(i, false);
                  }
               ImGui::EndMenu();
            }
            if (ImGui::SliderFloat("Orbit Sensitivity", &editor.m_navOrbitSensitivity, 0.1f, 5.f, "%.1f"))
               settings.SetEditor_NavOrbitSensitivity(editor.m_navOrbitSensitivity, false);
            ImGui::EndMenu();
         }
         ImGui::EndMenu();
      }
         using PV = EditorUI::PredefinedView;
         const auto viewItem = [&](const char *label, const char *keys, PV view)
            if (ImGui::MenuItem(label, keys, editor.m_camMode == ViewMode::EditorCam && editor.m_predefinedView == view))
               editor.SetView(view);
         };
         viewItem("Top", "7", PV::Top);
         viewItem("Bottom", "Ctrl+7", PV::Bottom);
         viewItem("Front", "1", PV::Front);
         viewItem("Back", "Ctrl+1", PV::Back);
         viewItem("Left", "3", PV::Left);
         viewItem("Right", "Ctrl+3", PV::Right);
         ImGui::Separator();
         if (ImGui::MenuItem("Perspective / Orthographic", "5"))
            editor.m_camMode = ViewMode::EditorCam;
            editor.m_perspectiveCam = !editor.m_perspectiveCam;
         }
         if (ImGui::MenuItem("Cycle Camera (Preview / Editor / Backdrop)", "0"))
            editor.CycleCamera();
         if (ImGui::MenuItem("Lock Orbit", "Numpad /", editor.m_orbitLock))
            editor.m_orbitLock = !editor.m_orbitLock;
         ImGui::Separator();
         if (ImGui::MenuItem("Orbit Left", "4"))
            editor.OrbitStep(true, 1.f);
         if (ImGui::MenuItem("Orbit Right", "6"))
            editor.OrbitStep(true, -1.f);
         if (ImGui::MenuItem("Orbit Up", "8"))
            editor.OrbitStep(false, 1.f);
         if (ImGui::MenuItem("Orbit Down", "2"))
            editor.OrbitStep(false, -1.f);
         ImGui::Separator();
         if (ImGui::MenuItem("Frame Selected", "F"))
            editor.FrameSelection();
         if (ImGui::MenuItem("Frame All", "Home"))
            editor.FrameAll();
         ImGui::Separator();
         static constexpr const char *workspaceKeys[PinnedPanel::WorkspaceCount] = { "Ctrl+1", "Ctrl+2", "Ctrl+3" };
         for (int i = 0; i < PinnedPanel::WorkspaceCount; i++)
            if (ImGui::MenuItem(("Workspace: " + editor.m_pinned.GetWorkspace(i).name).c_str(), workspaceKeys[i], editor.m_pinned.GetActiveWorkspace() == i))
               editor.SwitchWorkspace(i);
         ImGui::Separator();
         if (ImGui::MenuItem("Outliner", nullptr, editor.IsAreaEditorShown(EditorUI::AreaEditor::Outliner)))
            editor.ToggleAreaEditor(EditorUI::AreaEditor::Outliner);
         if (ImGui::MenuItem("Properties", nullptr, editor.IsAreaEditorShown(EditorUI::AreaEditor::Properties)))
            editor.ToggleAreaEditor(EditorUI::AreaEditor::Properties);
         // Editor type of the left and right areas (Blender's editor type switcher, also the button at the left of each area header)
         for (int side = 0; side < 2; ++side)
         {
            const bool right = side == 1;
            if (ImGui::BeginMenu(right ? "Right Area" : "Left Area"))
            {
               bool &shown = right ? editor.m_showProperties : editor.m_showOutliner;
               if (ImGui::MenuItem("Visible", nullptr, shown))
                  shown = !shown;
               ImGui::Separator();
               const EditorUI::AreaEditor current = right ? editor.m_rightEditor : editor.m_leftEditor;
               for (int i = 0; i < static_cast<int>(EditorUI::AreaEditor::Count); ++i)
               {
                  const EditorUI::AreaEditor type = static_cast<EditorUI::AreaEditor>(i);
                  if (ImGui::MenuItem(EditorUI::AreaEditorName(type), nullptr, current == type))
                  {
                     editor.SetAreaEditor(right, type);
                     shown = true;
                  }
               }
               ImGui::EndMenu();
            }
         }
         if (ImGui::MenuItem("Sidebar", "N", editor.m_showSidebar))
            editor.ToggleSidebar();
         if (ImGui::MenuItem("Quick Favorites", "Q"))
            editor.OpenQuickFavorites();
         if (ImGui::MenuItem("Menu Search...", "F3"))
         if (ImGui::MenuItem("Navigation Cube", nullptr, editor.m_showNavCube))
            editor.m_showNavCube = !editor.m_showNavCube;
      if (ImGui::BeginMenu("View"))
      {
         if (ImGui::MenuItem("Pinned Properties", "P", editor.m_pinned.IsVisible()))
            editor.m_pinned.ToggleVisible();
         if (ImGui::MenuItem("Status Bar", nullptr, m_showStatusBar))
            m_showStatusBar = !m_showStatusBar;
         ImGui::EndMenu();
      if (editor.IsInspectMode() && !editor.m_table->IsLocked() && ImGui::BeginMenu("Debug"))
      {
         if (ImGui::MenuItem("Open debugger"))
            editor.m_player->m_showDebugger = true;
         if (ImGui::MenuItem("Renderer Inspection"))
            editor.m_inspectionModal.Show();
         if (ImGui::MenuItem(editor.m_player->IsPlaying() ? "Pause" : "Play"))
            editor.m_player->SetPlayState(!editor.m_player->IsPlaying());
         ImGui::EndMenu();
      }
      else if (!editor.IsInspectMode() && !editor.m_table->IsLocked() && ImGui::BeginMenu("Debug"))
      {
         if (ImGui::MenuItem("Renderer Inspection"))
            editor.m_inspectionModal.Show();
         ImGui::EndMenu();
      }
      // Workspace tabs (Blender's top bar), centered in the menu bar
      if (!editor.m_table->IsLocked())
      {
         float tabsWidth = 0.f;
         for (int i = 0; i < PinnedPanel::WorkspaceCount; i++)
            tabsWidth += ImGui::CalcTextSize(editor.m_pinned.GetWorkspace(i).name.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.f + ImGui::GetStyle().ItemSpacing.x;
         const float startX = max(ImGui::GetCursorPosX() + ImGui::GetStyle().ItemSpacing.x, 0.5f * (ImGui::GetWindowWidth() - tabsWidth));
         ImGui::SetCursorPosX(startX);
         for (int i = 0; i < PinnedPanel::WorkspaceCount; i++)
         {
            const string &name = editor.m_pinned.GetWorkspace(i).name;
            const bool active = editor.m_pinned.GetActiveWorkspace() == i;
            const ImVec2 textSize = ImGui::CalcTextSize(name.c_str());
            const ImVec2 tabSize(textSize.x + tabPadding * 2.f, tabHeight * dpi);
            ImGui::SetCursorScreenPos(ImVec2(x, barBottom - tabSize.y));
            if (ImGui::InvisibleButton((name + "##Workspace").c_str(), tabSize))
               editor.SwitchWorkspace(i);
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 tabMin = ImGui::GetItemRectMin();
            const ImVec2 tabMax = ImGui::GetItemRectMax();
            drawList->AddRectFilled(tabMin, tabMax, active ? EditorTheme::TabActive : (hovered ? EditorTheme::TabHover : 0u), 5.f * dpi, ImDrawFlags_RoundCornersTop);
            drawList->AddText(ImVec2(tabMin.x + tabPadding, tabMin.y + 0.5f * (tabSize.y - textSize.y)), active ? IM_COL32_WHITE : EditorTheme::TextDim, name.c_str());
            ImGui::SetItemTooltip("Workspace (Ctrl+%d)", i + 1);
         }
      }
      float buttonWidth = 0.f;
      if (editor.IsInspectMode())
         buttonWidth += ImGui::CalcTextSize(ICON_LC_UNDO_2, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
      buttonWidth += ImGui::CalcTextSize(ICON_LC_X, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
      ImVec2 padding = ImGui::GetCursorScreenPos();
      padding.x += ImGui::GetContentRegionAvail().x - buttonWidth;
      ImGui::SetCursorScreenPos(padding);
      if (editor.IsInspectMode())
      {
         if (ImGui::Button(ICON_LC_UNDO_2)) // ICON_LC_SQUARE
            editor.Close();
         if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Get back to player");
      }
      if (ImGui::Button(ICON_LC_X))
         editor.RequestClose(editor.IsInspectMode() ? Player::CS_STOP_PLAY : Player::CS_CLOSE_APP);
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Close editor");
      m_menuBarHeight = ImGui::GetWindowSize().y;
      ImGui::EndMainMenuBar();
   }
}

void EditorChrome::RenderToolbar()
{
   EditorUI &editor = m_editor;
   const ImGuiViewport *const viewport = ImGui::GetMainViewport();
   ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + m_menuBarHeight));
   ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, m_toolBarHeight));
   constexpr ImGuiWindowFlags window_flags
      = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
   ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
   ImGui::Begin("TOOLBAR", nullptr, window_flags);
   ImGui::PopStyleVar();
   if (editor.IsInspectMode())
   {
      if (ImGui::Button(editor.m_player->IsPlaying() ? ICON_LC_PAUSE : ICON_LC_PLAY))
         editor.m_player->SetPlayState(!editor.m_player->IsPlaying());
      ImGui::SameLine();
      ImGui::BeginDisabled(editor.m_player->IsPlaying());
      if (ImGui::Button(ICON_LC_SKIP_FORWARD))
         editor.m_player->m_step = true;
      ImGui::EndDisabled();
   }
   else
   {
      const float iconSize = ImGui::GetContentRegionAvail().y;

      // File operations: same actions as the File menu, each button opening a sub menu
      if (ImGui::Button(ICON_LC_FILE_PLUS))
         ImGui::OpenPopup("New table Popup");
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("New table\n[Ctrl+N]");
      if (ImGui::BeginPopup("New table Popup"))
      {
         if (ImGui::MenuItem("New Table", "Ctrl+N"))
            editor.NewTable(EditorUI::NewTableTemplate::Blank);
         if (ImGui::MenuItem("Completely Blank Table"))
            editor.NewTable(EditorUI::NewTableTemplate::Stripped);
         if (ImGui::MenuItem("Full Example Table"))
            editor.NewTable(EditorUI::NewTableTemplate::Example);
         if (ImGui::MenuItem("Light Sequence Demo Table"))
            editor.NewTable(EditorUI::NewTableTemplate::LightSeq);
         ImGui::EndPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button(ICON_LC_FOLDER_OPEN))
         ImGui::OpenPopup("Open table Popup");
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Open table");
      if (ImGui::BeginPopup("Open table Popup"))
      {
         if (ImGui::MenuItem("Table file..."))
            editor.LoadTable();
         if (ImGui::MenuItem("Table pack folder..."))
            editor.LoadTableFolder();
         ImGui::EndPopup();
      }
      ImGui::SameLine();
      ImGui::BeginDisabled(editor.m_table->IsLocked());
      if (ImGui::Button(ICON_LC_SAVE))
         ImGui::OpenPopup("Save table Popup");
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Save table\n[Ctrl+S]");
      if (ImGui::BeginPopup("Save table Popup"))
      {
         if (ImGui::MenuItem("Save", "Ctrl+S"))
            editor.SaveTable();
         if (ImGui::MenuItem("Save As File..."))
            editor.SaveTableAs();
         if (ImGui::MenuItem("Save As Pack Folder..."))
            editor.SaveTableAsPackFolder();
         ImGui::EndPopup();
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::Separator();
      ImGui::SameLine();

      // Play a shallow copy of the edited table
      ImGui::BeginDisabled(editor.m_table->IsLocked());
      if (ImGui::Button(ICON_LC_PLAY))
         editor.PlayTest();
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Play table\n[F5]");
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::Separator();
      ImGui::SameLine();

      // Edit mode selection: moved to the tool shelf when it is shown
      if (!ToolShelfVisible())
      {
         for (const auto &mode : editModes)
         {
            const bool active = (editor.m_gizmoOperation == mode.operation);
            if (ToolButton(mode.icon, active))
               editor.SetGizmoOperation(mode.operation);
            if (ImGui::IsItemHovered())
               ImGui::SetTooltip("%s", mode.tooltip);
            if (&mode != std::end(editModes) - 1)
               ImGui::SameLine(0.f, toolGap);
         }
      }
      ImGui::Separator();
      ImGui::SameLine();

      // Snapping: magnet toggle (Shift+Tab) and a small chevron button opening the snapping popover
      ImGui::SameLine(0.f, toolGap);
      {
         const bool snapOn = editor.m_snapEnabled;
         if (ToolButton("##Snap", snapOn))
            editor.ToggleSnapping();
         const ImVec2 bmin = ImGui::GetItemRectMin(), bmax = ImGui::GetItemRectMax();
         DrawMagnet(ImGui::GetWindowDrawList(), ImVec2(0.5f * (bmin.x + bmax.x), 0.5f * (bmin.y + bmax.y)), (bmax.x - bmin.x) * 0.5f, snapOn ? EditorTheme::Accent : EditorTheme::Text);
         ImGui::SetItemTooltip("Snapping: %s\n[Shift+Tab] Hold Ctrl to invert while transforming", snapOn ? "on" : "off");
         ImGui::SameLine(0.f, 0.f);
         ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::V(0u));
         ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::V(EditorTheme::Hover));
         ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::V(EditorTheme::Hover));
         const bool openPopover = ImGui::Button("##SnapOptions", ImVec2(0.5f * ToolButtonSize * s_toolDpi, ToolButtonSize * s_toolDpi));
         ImGui::PopStyleColor(3);
         {
            const ImVec2 cmin = ImGui::GetItemRectMin(), cmax = ImGui::GetItemRectMax();
            const float cx = 0.5f * (cmin.x + cmax.x), cy = 0.5f * (cmin.y + cmax.y), half = 3.f * s_toolDpi;
            ImGui::GetWindowDrawList()->AddTriangleFilled(ImVec2(cx - half, cy - half * 0.5f), ImVec2(cx + half, cy - half * 0.5f), ImVec2(cx, cy + half * 0.5f), EditorTheme::TextDim);
         }
         ImGui::SetItemTooltip("Snapping options");
         if (openPopover)
            ImGui::OpenPopup("Snap Popover");
         ImGui::SetNextWindowPos(ImVec2(bmin.x, bmax.y), ImGuiCond_Appearing);
         if (ImGui::BeginPopup("Snap Popover"))
         {
            bool changed = false;
            ImGui::TextUnformatted("Snap To");
            if (ImGui::RadioButton("Increment", editor.m_snapTarget == 0))
            {
               editor.m_snapTarget = 0;
               changed = true;
            }
            ImGui::SetItemTooltip("Snap to multiples of the increments");
            if (ImGui::RadioButton("Points", editor.m_snapTarget == 1))
            {
               editor.m_snapTarget = 1;
               changed = true;
            }
            ImGui::SetItemTooltip("Snap to the centers and drag points of the other parts (G move, without axis constraint; the gizmo uses the increment)");
            ImGui::Spacing();
            ImGui::TextUnformatted("Increments");
            ImGui::SetNextItemWidth(120.f * s_toolDpi);
            changed |= ImGui::DragFloat("Move (vpu)", &editor.m_snapMove, 0.5f, 0.01f, 1000.f, "%.2f");
            ImGui::SetNextItemWidth(120.f * s_toolDpi);
            changed |= ImGui::DragFloat("Rotate (deg)", &editor.m_snapRotate, 0.5f, 0.1f, 180.f, "%.1f");
            ImGui::SetNextItemWidth(120.f * s_toolDpi);
            changed |= ImGui::DragFloat("Scale", &editor.m_snapScale, 0.005f, 0.001f, 10.f, "%.3f");
            ImGui::Spacing();
            ImGui::TextUnformatted("Affect");
            changed |= ImGui::Checkbox("Move", &editor.m_snapAffectMove);
            changed |= ImGui::Checkbox("Rotate", &editor.m_snapAffectRotate);
            changed |= ImGui::Checkbox("Scale", &editor.m_snapAffectScale);
            if (changed)
               editor.SaveSnapPreferences();
            ImGui::EndPopup();
         }
      }

      // Add part buttons (same parts and icons as the WinUI toolbar)
      if (m_addPartButtons.empty())
      {
         static const struct
         {
            ItemTypeEnum type;
            const char *name;
            const char *file;
         } addParts[] = {
            { eItemSurface, "Surface", "editor/wall.png" }, //
            { eItemGate, "Gate", "editor/gate.png" }, //
            { eItemRamp, "Ramp", "editor/ramp.png" }, //
            { eItemFlipper, "Flipper", "editor/flipper.png" }, //
            { eItemPlunger, "Plunger", "editor/plunger.png" }, //
            { eItemBall, "Ball", "editor/ball.png" }, //
            { eItemBumper, "Bumper", "editor/bumper.png" }, //
            { eItemSpinner, "Spinner", "editor/spinner.png" }, //
            { eItemTimer, "Timer", "editor/timer.png" }, //
            { eItemTrigger, "Trigger", "editor/trigger.png" }, //
            { eItemLight, "Light", "editor/light.png" }, //
            { eItemKicker, "Kicker", "editor/kicker.png" }, //
            { eItemHitTarget, "Target", "editor/target.png" }, //
            { eItemDecal, "Decal", "editor/decal.png" }, //
            { eItemTextbox, "Textbox", "editor/textbox.png" }, //
            { eItemDispReel, "Display Reel", "editor/reel.png" }, //
            { eItemLightSeq, "Light Seq", "editor/lightseq.png" }, //
            { eItemPrimitive, "Primitive", "editor/primitive.png" }, //
            { eItemFlasher, "Flasher", "editor/flasher.png" }, //
            { eItemRubber, "Rubber", "editor/rubber.png" }, //
         };
         for (const auto &def : addParts)
         {
            std::shared_ptr<const BaseTexture> tex = BaseTexture::CreateFromFile(g_app->m_fileLocator.GetAppPath(FileLocator::AppSubFolder::Assets, def.file));
            m_addPartButtons.push_back({ def.type, def.name, tex ? std::make_shared<Sampler>(editor.m_renderer->m_renderDevice, def.name, tex, false) : nullptr });
         }
      }
      // Add part button, opening the part type picker popup (same as the Shift+A shortcut)
      if (!ToolShelfVisible()) // Moved to the tool shelf when it is shown (the popup below is shared)
      {
         ImGui::BeginDisabled(editor.m_pointEditPart != nullptr || editor.m_table->IsLocked());
         const bool addPartActive = (editor.m_addPartType != eItemInvalid);
         if (ToolButton(ICON_LC_PLUS, addPartActive))
            RequestAddPartPopup(ImGui::GetMousePos());
         if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Add part\n[Shift+A]");
         ImGui::EndDisabled();
      }

      // Copy/Paste buttons (same actions as the Ctrl+C and Ctrl+V keyboard shortcuts)
      ImGui::SameLine(0.f, toolGap);
      ImGui::BeginDisabled(!editor.CanCopy());
      if (ToolButton(ICON_LC_COPY))
         editor.CopySelection();
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Copy selection\n[Ctrl+C]");
      ImGui::EndDisabled();
      ImGui::SameLine(0.f, toolGap);
      ImGui::BeginDisabled(!editor.CanPaste());
      if (ToolButton(ICON_LC_CLIPBOARD_PASTE))
         editor.PasteSelection(ImVec2(viewport->GetCenter().x, viewport->GetCenter().y));
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Paste\n[Ctrl+V]");
      ImGui::EndDisabled();

      // Delete selection button, always visible but only enabled when there are selected parts
      ImGui::SameLine();
      ImGui::BeginDisabled(editor.m_selection.GetType() != Selection::S_EDITABLE || editor.m_table->IsLocked());
      if (ImGui::Button(ICON_LC_TRASH_2))
         editor.DeleteSelection();
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Delete selection\n[Delete]");
      ImGui::EndDisabled();

      // Part library import/export buttons
      ImGui::SameLine();
      ImGui::BeginDisabled(editor.m_table->IsLocked());
      if (ImGui::Button(ICON_LC_DOWNLOAD))
         editor.OpenPartLibrary();
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Import parts (part library)");
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(!editor.CanExportPartSelection());
      if (ImGui::Button(ICON_LC_UPLOAD))
         editor.ExportPartSelection();
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Export selection to a VPZ pack");
      ImGui::EndDisabled();

      // Script editor button
      ImGui::SameLine();
      ImGui::Separator();
      ImGui::SameLine();
      const bool scriptActive = editor.m_scriptPanel.IsVisible();
      if (scriptActive)
         ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
      if (ImGui::Button(ICON_LC_CODE_XML))
         editor.m_scriptPanel.Show();
      if (scriptActive)
         ImGui::PopStyleColor();
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Edit table script");

      // Part type picker popup (Shift+A in standard mode): picking a type arms add part mode,
      // clicking outside of the popup dismisses it without arming add part mode
      if (m_openAddPartPopup)
      {
         m_openAddPartPopup = false;
         ImGui::OpenPopup("Add Part");
      }
      ImGui::SetNextWindowPos(m_addPartPopupPos, ImGuiCond_Appearing);
      if (ImGui::BeginPopup("Add Part"))
      {
         int column = 0;
         for (const auto &button : m_addPartButtons)
         {
            if (button.icon == nullptr)
               continue;
            if (column > 0)
               ImGui::SameLine();
            if (ImGui::ImageButton(button.name, button.icon, ImVec2(iconSize, iconSize)))
            {
               editor.m_addPartType = button.type;
               ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered())
               ImGui::SetTooltip("Add %s", button.name);
            column = (column + 1) % 5;
         }
         ImGui::EndPopup();
      }
   }
   const float buttonWidth = //
      ImGui::CalcTextSize(ICON_LC_RULER, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f //
      + ImGui::CalcTextSize(ICON_LC_STICKY_NOTE, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f //
      + ImGui::CalcTextSize(ICON_LC_LIST_FILTER, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
   ImGui::SameLine(ImGui::GetContentRegionAvail().x - buttonWidth);
   if (ImGui::Button(ICON_LC_RULER)) // Unit selection menu
      ImGui::OpenPopup("Unit Popup");
   if (ImGui::BeginPopup("Unit Popup"))
   {
      ImGui::TextUnformatted("Units:");
      ImGui::Separator();
      if (ImGui::RadioButton("VP Units", editor.m_units == EditorUI::Units::VPX))
         editor.m_units = EditorUI::Units::VPX;
      if (ImGui::RadioButton("Metric", editor.m_units == EditorUI::Units::Metric))
         editor.m_units = EditorUI::Units::Metric;
      if (ImGui::RadioButton("Imperial", editor.m_units == EditorUI::Units::Imperial))
         editor.m_units = EditorUI::Units::Imperial;
      ImGui::EndPopup();
   }
   ImGui::SameLine();
   if (ImGui::Button(ICON_LC_STICKY_NOTE)) // Overlay option menu
      ImGui::OpenPopup("Overlay Popup");
   if (ImGui::BeginPopup("Overlay Popup"))
   {
      ImGui::TextUnformatted("Overlays:");
      ImGui::Separator();
      ImGui::Checkbox("Overlay selection", &editor.m_selectionOverlay);
      ImGui::Separator();
      ImGui::TextUnformatted("Physic Overlay:");
      if (ImGui::RadioButton("None", editor.m_physOverlay == EditorUI::PhysicOverlay::None))
         editor.m_physOverlay = EditorUI::PhysicOverlay::None;
      if (ImGui::RadioButton("Selected", editor.m_physOverlay == EditorUI::PhysicOverlay::Selected))
         editor.m_physOverlay = EditorUI::PhysicOverlay::Selected;
      if (ImGui::RadioButton("All", editor.m_physOverlay == EditorUI::PhysicOverlay::All))
         editor.m_physOverlay = EditorUI::PhysicOverlay::All;
      ImGui::EndPopup();
   }
   ImGui::SameLine();
   if (ImGui::Button(ICON_LC_LIST_FILTER)) // Selection filter
      ImGui::OpenPopup("Selection filter Popup");
   if (ImGui::BeginPopup("Selection filter Popup"))
   {
      bool pf = EditorUI::HasFlag(editor.m_selectionFilter, EditorUI::SelectionFilter::Playfield);
      bool prims = EditorUI::HasFlag(editor.m_selectionFilter, EditorUI::SelectionFilter::Primitives);
      bool lights = EditorUI::HasFlag(editor.m_selectionFilter, EditorUI::SelectionFilter::Lights);
      bool flashers = EditorUI::HasFlag(editor.m_selectionFilter, EditorUI::SelectionFilter::Flashers);
      ImGui::TextUnformatted("Selection filters:");
      ImGui::Separator();
      if (ImGui::Checkbox("Playfield", &pf))
         editor.m_selectionFilter = pf ? (editor.m_selectionFilter | EditorUI::SelectionFilter::Playfield) : (editor.m_selectionFilter & ~EditorUI::SelectionFilter::Playfield);
      if (ImGui::Checkbox("Primitives", &prims))
         editor.m_selectionFilter = prims ? (editor.m_selectionFilter | EditorUI::SelectionFilter::Primitives) : (editor.m_selectionFilter & ~EditorUI::SelectionFilter::Primitives);
      if (ImGui::Checkbox("Lights", &lights))
         editor.m_selectionFilter = lights ? (editor.m_selectionFilter | EditorUI::SelectionFilter::Lights) : (editor.m_selectionFilter & ~EditorUI::SelectionFilter::Lights);
      if (ImGui::Checkbox("Flashers", &flashers))
         editor.m_selectionFilter = flashers ? (editor.m_selectionFilter | EditorUI::SelectionFilter::Flashers) : (editor.m_selectionFilter & ~EditorUI::SelectionFilter::Flashers);
      ImGui::EndPopup();
   }
   ImGui::End();
}

bool EditorChrome::ToolShelfVisible() const
   // Same states as the nav cube (the fly mode and the renderer inspection are also handled by the caller, which only draws the chrome in the full UI)
   const EditorUI &editor = m_editor;
   return m_showToolShelf && !editor.IsInspectMode() && !editor.m_table->IsLocked() && !editor.m_flyMode && !editor.m_inspectionModal.IsVisible()
      && editor.m_camMode != ViewMode::DesktopBackdrop;
   if (!ToolShelfVisible())
      return;
   EditorUI &editor = m_editor;
   const float dpi = editor.m_liveUI.GetDPI();
   s_toolDpi = dpi;
   const ImGuiStyle &style = ImGui::GetStyle();
   // Below the status overlay texts (orbit lock + view name, tool name, optional point edit line): same metrics as RenderStatusOverlay
   const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
   float textStackHeight = style.WindowPadding.y + 2.f * style.FramePadding.y + 2.f * lineHeight;
   if (editor.m_pointEditPart)
      textStackHeight += lineHeight;
   const ImVec2 pos(viewportContent.x0 + 6.f * dpi, viewportContent.y0 + 5.f * dpi + textStackHeight + 4.f * dpi);
   // Estimated size (the window auto resizes): 4 edit modes, a separator, add, orientation toggle
   const float pad = 3.f * dpi;
   const float gap = 2.f * dpi;
   const float button = ToolButtonSize * dpi;
   const float sepHeight = ToolGroupGap * dpi;
   const float estWidth = button + 2.f * pad;
   const float estHeight = 6.f * button + 4.f * gap + 2.f * sepHeight + 2.f * pad;
   // Do not overlap the nav cube / N-panel side (too narrow viewport) nor the F9 Adjust Last panel at the bottom left (last frame rect, if shown)
   if (viewportContent.Width() < estWidth + 220.f * dpi || pos.y + estHeight > viewportContent.y1 - 8.f * dpi)
      return;
   if (const ImGuiWindow *adjust = ImGui::FindWindowByName("Adjust Last Operation"); adjust != nullptr && adjust->Active && pos.y + estHeight + 4.f * dpi > adjust->Pos.y)
      return;
   ImGui::SetNextWindowPos(pos);
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
   ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.f * dpi);
   ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
   ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::V((EditorTheme::Strip & 0x00FFFFFFu) | (217u << 24))); // ~85% alpha
   ImGui::PushStyleColor(ImGuiCol_Border, EditorTheme::V(EditorTheme::Border));
   const bool open = ImGui::Begin("##ToolShelf", nullptr,
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
         | ImGuiWindowFlags_NoNav);
   ImGui::PopStyleColor(2);
   ImGui::PopStyleVar(4);
   if (open)
      // Edit modes
      for (const auto &mode : editModes)
         const bool active = (editor.m_gizmoOperation == mode.operation);
         if (ToolButton(mode.icon, active))
            editor.SetGizmoOperation(mode.operation);
         if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", mode.tooltip);
      // Separator
         const ImVec2 sp = ImGui::GetCursorScreenPos();
         ImGui::GetWindowDrawList()->AddLine(ImVec2(sp.x + 3.f * dpi, sp.y + 0.5f * sepHeight), ImVec2(sp.x + button - 3.f * dpi, sp.y + 0.5f * sepHeight), EditorTheme::TabActive, 1.f);
         ImGui::Dummy(ImVec2(button, sepHeight));
      // Add part: opens the part type picker popup of the top toolbar (consumed there next frame), anchored at the bottom right of the button
      ImGui::BeginDisabled(editor.m_pointEditPart != nullptr || editor.m_table->IsLocked());
      if (ToolButton(ICON_LC_PLUS, editor.m_addPartType != eItemInvalid))
         RequestAddPartPopup(ImVec2(ImGui::GetItemRectMax().x + 2.f * dpi, ImGui::GetItemRectMax().y));
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Add part\n[Shift+A]");
      ImGui::EndDisabled();
      // Orientation of the gizmo (Global / Local), only meaningful for Grab, Rotate and Scale
         const bool local = (editor.m_gizmoMode == ImGuizmo::LOCAL);
         ImGui::BeginDisabled(editor.m_gizmoOperation == ImGuizmo::OPERATION(0));
         if (ToolButton(ICON_LC_BOX, local))
            editor.m_gizmoMode = local ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
         if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Transform orientation: %s\nClick to switch to %s\n[repeat G / R / S]", local ? "Local" : "Global", local ? "Global" : "Local");
         ImGui::EndDisabled();
   ImGui::End();
void EditorChrome::RenderStatusOverlay()
{
   EditorUI &editor = m_editor;
   const ImGuiIO &io = ImGui::GetIO();

   // Overlay Info Text
   const ImVec2 overlayPos((editor.m_showOutliner ? editor.m_outlinerWidth : 0.f) * editor.m_liveUI.GetDPI(), m_toolBarHeight + m_menuBarHeight + 5.f * editor.m_liveUI.GetDPI());

   // Camera orbit lock toggle (drawn in its own window as the status overlay does not capture inputs)
   ImGui::SetNextWindowPos(overlayPos);
   ImGui::Begin("orbit lock toggle", nullptr,
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
         | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav);
   const bool stylePushed = editor.m_orbitLock;
   if (stylePushed)
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
   if (ImGui::Button(editor.m_orbitLock ? ICON_LC_LOCK : ICON_LC_LOCK_OPEN))
      editor.m_orbitLock = !editor.m_orbitLock;
   if (stylePushed)
      ImGui::PopStyleColor();
   if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Lock camera orbit\n[Keypad /]");
   const float lockToggleWidth = ImGui::GetItemRectSize().x;
   ImGui::End();

   ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x - (editor.m_showOutliner ? editor.m_outlinerWidth : 0.f) * editor.m_liveUI.GetDPI(),
      io.DisplaySize.y - m_toolBarHeight - m_menuBarHeight - 5.f * editor.m_liveUI.GetDPI())); // Fixed outliner width (to be adjusted when moving ImGui to the docking branch)
   ImGui::SetNextWindowPos(overlayPos);
   ImGui::Begin("text overlay", nullptr, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav);
   const float labelAlign = ImGui::GetCursorPosX() + lockToggleWidth + ImGui::GetStyle().ItemSpacing.x;
   ImGui::SetCursorPos(ImVec2(labelAlign, ImGui::GetCursorPosY() + ImGui::GetStyle().FramePadding.y));
   switch (editor.m_camMode)
   {
   case ViewMode::PreviewCam: ImGui::TextUnformatted("Preview Camera"); break;
   case ViewMode::EditorCam:
   {
      string text;
      switch (editor.m_predefinedView)
      {
      case EditorUI::PredefinedView::None: text = "User"sv; break;
      case EditorUI::PredefinedView::Left: text = "Left"sv; break;
      case EditorUI::PredefinedView::Right: text = "Right"sv; break;
      case EditorUI::PredefinedView::Top: text = "Top"sv; break;
      case EditorUI::PredefinedView::Bottom: text = "Bottom"sv; break;
      case EditorUI::PredefinedView::Front: text = "Front"sv; break;
      case EditorUI::PredefinedView::Back: text = "Back"sv; break;
      }
      ImGui::TextUnformatted((text + (editor.m_perspectiveCam ? " Perspective" : " Orthographic")).c_str());
      break;
   }
   case ViewMode::DesktopBackdrop: ImGui::TextUnformatted("Desktop Backdrop"); break;
   }
   ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + ImGui::GetStyle().FramePadding.x, ImGui::GetCursorPosY() + ImGui::GetStyle().FramePadding.y));
   const char *gizmoIcon = nullptr;
   const char *gizmoLabel = nullptr;
   if (editor.m_gizmoOperation == ImGuizmo::OPERATION(0))
   {
      gizmoIcon = ICON_LC_MOUSE_POINTER_2;
      gizmoLabel = "Select";
   }
   else if (editor.m_gizmoOperation == ImGuizmo::TRANSLATE)
   {
      gizmoIcon = ICON_LC_MOVE;
      gizmoLabel = "Grab";
   }
   else if (editor.m_gizmoOperation == ImGuizmo::SCALE)
   {
      gizmoIcon = ICON_LC_SCALE_3D;
      gizmoLabel = "Scale";
   }
   else if (editor.m_gizmoOperation == ImGuizmo::ROTATE)
   {
      gizmoIcon = ICON_LC_ROTATE_3D;
      gizmoLabel = "Rotate";
   }
   if (gizmoIcon)
   {
      ImGui::TextUnformatted(gizmoIcon);
      ImGui::SameLine(labelAlign);
      ImGui::TextUnformatted(gizmoLabel);
   }
   if (editor.m_pointEditPart)
      ImGui::TextUnformatted(editor.m_pointEditCenter ? "Center Edit (Tab to exit)" : "Drag Point Edit (Tab to exit)");
   ImGui::End();
}

void EditorChrome::RenderUndoHistory()
{
   if (!m_undoHistoryOpen)
      return;
   EditorUI &editor = m_editor;
   const float dpi = editor.m_liveUI.GetDPI();
   // The history can not change under the window: it is closed with the editor state it lists
   if (editor.m_table->m_liveBaseTable != nullptr || editor.m_modal.active)
   {
      m_undoHistoryOpen = false;
      return;
   }
   ImGui::SetNextWindowPos(m_undoHistoryPos, ImGuiCond_Appearing);
   ImGui::SetNextWindowSizeConstraints(ImVec2(260.f * dpi, 0.f), ImVec2(420.f * dpi, 420.f * dpi));
   bool open = true;
   int undoSteps = 0, redoSteps = 0; // Requested move in the history, applied once the window is done
   if (ImGui::Begin("Undo History", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
   {
      const size_t undoCount = editor.m_undo.UndoCount();
      const size_t redoCount = editor.m_undo.RedoCount();
      if (undoCount == 0 && redoCount == 0)
         ImGui::TextDisabled("No history");
      // Steps that can be redone, dimmed, the furthest first (so that the list reads newest first)
      for (size_t i = redoCount; i-- > 0;)
      {
         ImGui::PushID(static_cast<int>(i) + 1000);
         ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::V(EditorTheme::TextFaint));
         if (ImGui::Selectable(editor.m_undo.RedoLabel(i).c_str()))
            redoSteps = static_cast<int>(i) + 1;
         ImGui::PopStyleColor();
         ImGui::PopID();
      }
      // Done steps, the newest first: the first one is the current state, picking an older one undoes down to the state it left
      for (size_t i = 0; i < undoCount; i++)
      {
         ImGui::PushID(static_cast<int>(i));
         if (ImGui::Selectable(editor.m_undo.UndoLabel(i).c_str(), i == 0))
            undoSteps = static_cast<int>(i);
         ImGui::PopID();
      }
      if (undoCount > 0 || redoCount > 0)
      {
         ImGui::PushID("original");
         ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::V(EditorTheme::TextDim));
         if (ImGui::Selectable("Original", undoCount == 0))
            undoSteps = static_cast<int>(undoCount);
         ImGui::PopStyleColor();
         ImGui::PopID();
      }
      // Clicking outside of the window dismisses it (not on the frame it opened, as the click that opened it may be still pending)
      // (a press on a row makes it the active item, which would read as not hovered and close the window before the row is released)
      if (!m_undoHistoryJustOpened && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
         && !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByPopup | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
         open = false;
   }
   ImGui::End();
   m_undoHistoryJustOpened = false;
   if (redoSteps > 0)
   {
      editor.RedoSteps(static_cast<size_t>(redoSteps));
      open = false;
   }
   else if (undoSteps > 0)
   {
      editor.UndoSteps(static_cast<size_t>(undoSteps));
      open = false;
   }
   if (!open)
      m_undoHistoryOpen = false;
}

void EditorChrome::RenderStatusBar(const bool showFullUI)
{
   if (!showFullUI || m_statusBarHeight <= 0.f)
      return;
   EditorUI &editor = m_editor;
   const ImGuiIO &io = ImGui::GetIO();
   const float dpi = editor.m_liveUI.GetDPI();
   const ImGuiViewport *const viewport = ImGui::GetMainViewport();
   const float height = m_statusBarHeight;

   // Key hints of the current context: [key chip] label
   struct Hint
   {
      string key, label;
   };
   vector<Hint> hints;
   const string mod = ModKey;
   EditorUI::AreaEditor areaUnderMouse = EditorUI::AreaEditor::Count;
   bool sidebarUnderMouse = false;
   const bool overArea = editor.GetAreaUnderMouse(areaUnderMouse, sidebarUnderMouse);
   if (editor.m_palette.IsOpen())
      hints = { { "Type", "Search" }, { "Enter", "Run" }, { mod + "+Enter", "Add to Quick Favorites" }, { "Esc", "Close" } };
   else if (io.WantTextInput)
      hints = { { "Enter", "Confirm" }, { "Esc", "Cancel" }, { mod + "+A", "Select All Text" } };
   else if (editor.m_modal.active)
      hints = { { "LMB / Enter", "Confirm" }, { "RMB / Esc", "Cancel" }, { "X Y Z", "Axis" }, { "Shift+X Y Z", "Plane" }, { "0-9", "Exact Value" },
         { mod, editor.m_snapEnabled ? "Hold: Snap Off" : "Hold: Snap On" } };
   else if (editor.m_boxSelectActive && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !overArea)
      hints = { { "Release LMB", "Select" }, { "Shift", "Add to Selection" }, { "Esc", "Cancel" } };
   else if (overArea && sidebarUnderMouse)
      hints = { { "Drag", "Adjust" }, { "Double-click", "Type Value" }, { "N", "Hide Sidebar" } };
   else if (overArea)
   {
      switch (areaUnderMouse)
      {
      case EditorUI::AreaEditor::Outliner:
         hints = { { "LMB", "Select" }, { mod + "+LMB", "Toggle" }, { "Shift+LMB", "Range" }, { "Double-click", "Frame" }, { mod + "+F", "Filter" } };
         break;
      case EditorUI::AreaEditor::Properties:
         hints = { { "Wheel", "Scroll" }, { "Double-click", "Type Value" }, { mod + "+F", "Search Fields" }, { mod + "+K", "Search to Field" } };
         break;
      case EditorUI::AreaEditor::Script: hints = { { mod + "+S", "Save Table" }, { "F3", "Menu Search" } }; break;
      default: hints = { { "LMB", "Select" }, { "Wheel", "Scroll" } }; break;
      }
   }
   else if (editor.m_addPartType != eItemInvalid)
      hints = { { "LMB", "Place Part" }, { "Esc", "Cancel" } };
   else if (editor.m_pointEditPart)
      hints = { { "LMB", "Select Point" }, { "Shift+LMB", "Toggle" }, { "G", "Move" }, { "Shift+A", "Add Point" }, { "Del", "Delete" }, { "Tab", "Exit" } };
   else
   {
      // Navigation hints follow the navigation preferences
      hints.push_back({ "LMB", "Select" });
      hints.push_back({ editor.m_navAltMiddleButton ? "MMB / Alt+LMB" : "MMB", "Orbit" });
      hints.push_back({ "Shift+MMB", "Pan" });
      if (editor.m_navTrackpad)
      {
         hints.push_back({ "Scroll", "Pan" });
         hints.push_back({ mod + "+Scroll", "Zoom" });
      }
      else
         hints.push_back({ "Wheel", "Zoom" });
      hints.push_back({ "G", "Move" });
      hints.push_back({ "R", "Rotate" });
      hints.push_back({ "S", "Scale" });
      hints.push_back({ "Shift+Tab", editor.m_snapEnabled ? "Snap: On" : "Snap: Off" });
      if (editor.m_lastOp.type != EditorUI::LastOperation::Type::None)
         hints.push_back({ "F9", "Adjust Last Operation" });
   }

   // Last operation report on the right, fading out
   const double age = ImGui::GetTime() - editor.m_reportTime;
   const float reportAlpha = age < 3.5 ? 1.f : age < 4.5 ? static_cast<float>(4.5 - age) : 0.f;
   const bool hasReport = reportAlpha > 0.f && !editor.m_report.empty();

   const float pad = 8.f * dpi;
   ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + viewport->Size.y - height));
   ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, height));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
   ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::V(EditorTheme::Bar));
   ImGui::Begin("STATUSBAR", nullptr,
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav
         | ImGuiWindowFlags_NoFocusOnAppearing);
   ImGui::PopStyleColor();
   ImGui::PopStyleVar(3);
   ImDrawList *const draw = ImGui::GetWindowDrawList();
   const ImVec2 barMin = ImGui::GetWindowPos();
   const ImVec2 barMax(barMin.x + viewport->Size.x, barMin.y + height);
   draw->AddLine(barMin, ImVec2(barMax.x, barMin.y), EditorTheme::Border, 1.f); // 1 px top border

   const float textH = ImGui::GetFontSize();
   const float textY = barMin.y + 0.5f * (height - textH);
   float rightLimit = barMax.x - pad;
   if (hasReport)
   {
      const ImVec2 size = ImGui::CalcTextSize(editor.m_report.c_str());
      const float x = barMax.x - pad - size.x;
      const auto fade = [reportAlpha](ImU32 color) { return (color & 0x00FFFFFFu) | (static_cast<ImU32>(static_cast<float>((color >> 24) & 0xFFu) * reportAlpha) << 24); };
      draw->AddText(ImVec2(x, textY), fade(EditorTheme::Text), editor.m_report.c_str());
      rightLimit = x - 2.f * pad;
   }

   // Hints from the left, as many as fit before the report
   const float chipPadX = 4.f * dpi, chipPadY = 1.f * dpi, gap = 4.f * dpi, hintGap = 12.f * dpi;
   float x = barMin.x + pad;
   for (const Hint &hint : hints)
   {
      const ImVec2 keySize = ImGui::CalcTextSize(hint.key.c_str());
      const ImVec2 labelSize = ImGui::CalcTextSize(hint.label.c_str());
      const float width = keySize.x + 2.f * chipPadX + gap + labelSize.x;
      if (x + width > rightLimit)
         break;
      const ImVec2 chipMin(x, barMin.y + 0.5f * (height - (keySize.y + 2.f * chipPadY)));
      const ImVec2 chipMax(x + keySize.x + 2.f * chipPadX, chipMin.y + keySize.y + 2.f * chipPadY);
      draw->AddRectFilled(chipMin, chipMax, EditorTheme::Rgb(255, 255, 255, 20), 3.f * dpi);
      draw->AddRect(chipMin, chipMax, EditorTheme::Hover, 3.f * dpi);
      draw->AddText(ImVec2(chipMin.x + chipPadX, chipMin.y + chipPadY), EditorTheme::TextDim, hint.key.c_str());
      draw->AddText(ImVec2(chipMax.x + gap, textY), EditorTheme::TextDim, hint.label.c_str());
      x += width + hintGap;
   }
   ImGui::End();
}

}
