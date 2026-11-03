// license:GPLv3+

#include "core/stdafx.h"
#include "EditorUI.h"

#include "core/TableDB.h"
#include "core/VPXPluginAPIImpl.h"
#include "core/editablereg.h"
#include "core/extern.h"
#include "core/FileLocator.h"
#include "core/VPApp.h"
#include "core/vpversion.h"

#include "editor/EditorUIPart.h"
#include "editor/EditorUIPartRegistry.h"
#include "editor/LiveRenderContext.h"

#include "parts/Collection.h"
#include "parts/PartGroup.h"
#include "parts/ball.h"
#include "parts/light.h"
#include "parts/Material.h"
#include "parts/primitive.h"
#include "math/dragpoint.h"
#include "math/matrix.h"
#include "utils/UnityPrefabImporter.h"

#include "physics/PhysicsEngine.h"
#include "plugins/VPXPlugin.h"

#include "renderer/Anaglyph.h"
#include "renderer/Renderer.h"
#include "renderer/Sampler.h"
#include "renderer/Shader.h"
#include "renderer/VRDevice.h"

#include "ui/EditorClipboard.h"
#include "ui/VPXFileFeedback.h"
#include "ui/live/LiveUI.h"

#include "utils/BiffReader.h"
#include "utils/color.h"
#include "utils/JSONSerializer.h"

#include "imgui/imgui.h"

#if defined(ENABLE_DX9)
#include <shellapi.h>
#endif

#include "imgui/imgui_stdlib.h"
#include "imguizmo/ImGuizmo.h"
#include "imgui_markdown/imgui_markdown.h"

namespace VPX::EditorUI
{

// Vertical field of view (in degrees) of the perspective editor camera


EditorUI::EditorUI(LiveUI &liveUI)
   : m_liveUI(liveUI)
   , m_player(g_pplayer)
   , m_renderer(m_player->m_renderer)
   , m_chrome(*this)
   , m_outliner(*this)
   , m_properties(*this)
   , m_inspectionModal(*this)
   , m_pbrConvertModal(*this)
   , m_partLibrary(*this)
   , m_scriptPanel(*this)
   , m_undo(m_player->m_ptable)
{
   m_table = m_player->m_ptable;
   m_pininput = &(m_player->m_pininput);

   EditorUIPartRegistry::InitRegistry();

   // Store the current selection in each undo record, so that undoing also restores it
   m_undo.SetEditorStateCapture([this]() { return std::any(CaptureUndoSelection()); });

   ClearSelection();
   LoadNavPreferences();

   // Editor camera position. We use a right handed system for easy ImGuizmo integration while VPX renderer is left handed, so reverse X axis
   m_camDistance = m_table->m_bottom * 0.7f;
   const vec3 eye(m_table->m_right * 0.5f, m_table->m_bottom * 0.5f, -m_camDistance);
   const vec3 at(m_table->m_right * 0.5f, m_table->m_bottom * 0.5f, 0.f);
   constexpr vec3 up { 0.f, -1.f, 0.f };
   m_camView = Matrix3D::MatrixLookAtRH(eye, at, up);
   ImGuizmo::AllowAxisFlip(false);
}

void EditorUI::LoadNavPreferences()
{
   const Settings &settings = g_settingsService.GetAppSettings();
   m_navOrbitSelection = settings.GetEditor_NavOrbitSelection();
   m_navZoomToMouse = settings.GetEditor_NavZoomToMouse();
   m_navInvertZoom = settings.GetEditor_NavInvertZoom();
   m_navOrbitSensitivity = settings.GetEditor_NavOrbitSensitivity();
   m_navFovY = settings.GetEditor_NavFovY();
   m_navAutoDepth = settings.GetEditor_NavAutoDepth();
   m_navSelectionDisplay = settings.GetEditor_NavSelectionDisplay();
   m_navAltMiddleButton = settings.GetEditor_NavAltMiddleButton();
   m_navTrackpad = settings.GetEditor_NavTrackpad();
   m_navEmulateNumpad = settings.GetEditor_NavEmulateNumpad();
   m_navBlenderTransform = settings.GetEditor_NavBlenderTransform();
}

EditorUI::~EditorUI()
{
   // The table outlives the editor (Player deletes the UI first)
   m_fileOp.reset(); // Wait for a file operation in progress, its result being discarded with the session
   RestorePartsVisibility();
}

EditorUI::AsyncFileOp::~AsyncFileOp()
{
   if (worker.joinable())
      worker.join();
   if (table != nullptr)
      table->Release();
}

void EditorUI::Open()
{
   if (m_isOpened)
      return;
   m_isOpened = true;
   m_boxSelectActive = false;
   m_renderer->DisableStaticPrePass(true);
   m_player->SetPlayState(false); // Suspend play while editing

   // Drop input events queued while the editor was closed (e.g. the key press that ended a play test), so they don't trigger editor shortcuts
   ImGui::GetIO().ClearEventsQueue();
   ImGui::GetIO().ClearInputKeys();

   if (IsInspectMode())
   {
      // Inspecting a live copy (play testing): keep the game playing, use the normal player camera view with default shading
      m_camMode = ViewMode::PreviewCam;
      m_shadeMode = Renderer::ShadeMode::Default;
   }
   else
   {
      // Enter with the user camera setup as an orthographic top view fitted on the playfield bounds, rendered as masked wireframe
      m_camMode = ViewMode::EditorCam;
      m_perspectiveCam = false;
      m_predefinedView = PredefinedView::Top;
      m_shadeMode = Renderer::ShadeMode::Wireframe;
      // The playfield fit needs a valid display size and chrome height which may not be known yet (editor opened during startup): defer to the first rendered frame
      m_fitPlayfieldCamera = true;
   }
}

void EditorUI::Close()
{
   if (!m_isOpened)
      return;
   m_isOpened = false;
   m_flyMode = false;
   ExitPointEditMode(false);
   RestorePartsVisibility();
   m_inspectionModal.Close();
   m_pbrConvertModal.Close();
   m_partLibrary.Close();
   m_scriptPanel.Close();
   m_renderer->DisableStaticPrePass(false);
   m_renderer->SetShadeMode(Renderer::ShadeMode::Default);
}

void EditorUI::SetTable(PinTable *const table)
{
   assert(table != nullptr);
   // Only tables of a same base table / live copy pair are supported (guaranteed by the player's SetTable)
   PinTable *const baseTable = m_table->m_liveBaseTable ? m_table->m_liveBaseTable : m_table;
   assert(table == baseTable || table->m_liveBaseTable == baseTable);
   m_table = table;

   // Drop all the state bound to the previous table's editables (undo is bound to the base table which outlives the switch)
   if (m_pointEditPart)
      m_pointEditPart->SetPointEditContext(nullptr);
   m_pointEditPart.reset();
   m_pointEditCenter = false;
   m_centerSelected = false;
   m_pointSel.clear();
   m_pointDragPending = false;
   m_pointDragActive = false;
   m_savedSelection = Selection();
   m_savedMultiSel.clear();
   m_savedOutlinerAnchor = nullptr;
   ClearSelection();
   m_dynamicParts.clear();
   m_editables.clear();
   m_editableMap.clear();
   m_partGroupUseHistory.clear();
   m_lastUndoPart = nullptr;
   m_addPartType = eItemInvalid;
   m_partLibrary.CancelPlacement();
   m_boxSelectActive = false;
}

void EditorUI::PlayTest()
{
   if (IsInspectMode())
      return;
   // The editor visibility overrides must not leak into the duplicated table used for the play session
   RestorePartsVisibility();
   PinTable *const liveTable = m_table->CopyForPlay();
   m_player->SetTable(liveTable, Player::TableTransition::Stack);
}

void EditorUI::SaveTable()
{
   PLOGI << "SaveTable requested (inspect=" << IsInspectMode() << ", locked=" << m_table->IsLocked() << ", fileOpBusy=" << (m_fileOp != nullptr)
         << ", file=" << PathToUTF8(m_table->m_filename) << ')';
   {
      PLOGW << "SaveTable ignored: " << (m_table->IsLocked() ? "table is locked" : "a file operation is in progress");
      m_liveUI.PushNotification(m_table->IsLocked() ? "Nothing saved: the table is locked"s : "Nothing saved: a file operation is still in progress"s, 8000);
      if (m_table->IsLocked() || m_fileOp != nullptr)
         return;
      PinTable *saveTable = m_table;
      if (IsInspectMode())
      {
         // Inspection session: write the startup table, after copying back what was edited in the 'Live' tabs
         saveTable = m_table->m_liveBaseTable;
         if (saveTable->m_filename.empty())
         {
            m_liveUI.PushNotification("Nothing saved: the table has no file yet, use Save As from the editor"s, 8000);
            return;
         }
         int copied = 0, skipped = 0;
         for (Material *const live : m_liveEditedMaterials)
            if (Material *const startup = m_table->GetStartupFromLive<Material>(live))
            {
               const string name = startup->m_name;
               *startup = *live;
               startup->m_name = name;
               copied++;
            }
         for (IEditable *const live : m_liveEditedParts)
         {
            IEditable *const startup = m_table->GetStartupFromLive<IEditable>(live);
            if (startup == nullptr)
               continue;
            if (live->GetItemType() == eItemPrimitive && startup->GetItemType() == eItemPrimitive)
               static_cast<Primitive *>(startup)->m_d = static_cast<Primitive *>(live)->m_d;
            else if (live->GetItemType() == eItemLight && startup->GetItemType() == eItemLight)
               static_cast<Light *>(startup)->m_d = static_cast<Light *>(live)->m_d;
            else
            {
               skipped++;
               continue;
            }
            copied++;
         }
         m_liveEditedMaterials.clear();
         m_liveEditedParts.clear();
         string msg = std::format("Saved the startup table to '{}' ({} live edit(s) copied in", PathToUTF8(saveTable->m_filename), copied);
         if (skipped > 0)
            msg += std::format(", {} edited part(s) of other types NOT copied: use the save icon next to their fields", skipped);
         msg += ")";
         m_liveUI.PushNotification(msg, 10000);
      }
      else if (m_table->m_filename.empty())
      {
         SaveTableAs();
         return;
      }
   auto op = std::make_unique<AsyncFileOp>();
   op->type = AsyncFileOp::Type::Save;
   op->saveTable = saveTable;
   op->filename = saveTable->m_filename;
   op->displayPath = PathToUTF8(saveTable->m_filename);
   StartFileOp(std::move(op));
   }

void EditorUI::SaveTableAs()
{
   if (IsInspectMode() || m_table->IsLocked() || m_player->m_playfieldWnd == nullptr)
      return;
   m_pendingSaveAsPath = std::make_shared<string>();
   m_pendingSaveAsFolder = false;
   std::filesystem::path defaultLocation = m_table->m_filename;
   const bool isPack = !defaultLocation.empty() && JSONSerializer::IsPack(defaultLocation);
   const SDL_DialogFileFilter tableFilter = { "Visual Pinball Tables", "vpx" };
   const SDL_DialogFileFilter packFilter = { "Visual Pinball Packs", "vpz" };
   const SDL_DialogFileFilter filters[] = { isPack ? packFilter : tableFilter, isPack ? tableFilter : packFilter };
   if (defaultLocation.empty())
      defaultLocation = PathFromString(m_table->GetSettings().GetRecentDir_LoadDir()) / "new_table.vpx";
   else if (lowerCase(PathToUTF8(defaultLocation.extension())) != ".vpz"s)
   {
      std::error_code ec;
      if (!std::filesystem::is_directory(defaultLocation, ec))
         defaultLocation.replace_extension(".vpx");
   }
   const string location = PathToUTF8(defaultLocation); // SDL expects UTF-8
   SDL_ShowSaveFileDialog(
      [](void *userdata, const char *const *filelist, int filter)
      {
         auto *res = static_cast<std::shared_ptr<string> *>(userdata);
         if (filelist != nullptr && filelist[0] != nullptr)
            **res = filelist[0];
         delete res;
      },
      new std::shared_ptr<string>(m_pendingSaveAsPath), //
      m_player->m_playfieldWnd->GetCore(), filters, static_cast<int>(std::size(filters)), location.empty() ? nullptr : location.c_str());
}

void EditorUI::SaveTableAsPackFolder()
{
   if (IsInspectMode() || m_table->IsLocked() || m_player->m_playfieldWnd == nullptr)
      return;
   m_pendingSaveAsPath = std::make_shared<string>();
   m_pendingSaveAsFolder = true;
   const std::filesystem::path defaultLocation = m_table->m_filename.empty() ? PathFromString(m_table->GetSettings().GetRecentDir_LoadDir()) : m_table->m_filename.parent_path();
   const string location = PathToUTF8(defaultLocation); // SDL expects UTF-8
   SDL_ShowOpenFolderDialog(
      [](void *userdata, const char *const *filelist, int filter)
      {
         auto *res = static_cast<std::shared_ptr<string> *>(userdata);
         if (filelist != nullptr && filelist[0] != nullptr)
            **res = filelist[0];
         delete res;
      },
      new std::shared_ptr<string>(m_pendingSaveAsPath), //
      m_player->m_playfieldWnd->GetCore(), location.empty() ? nullptr : location.c_str(), false);
}

void EditorUI::LoadTable() { LoadTableDialog(false); }

void EditorUI::LoadTableFolder() { LoadTableDialog(true); }

void EditorUI::LoadTableDialog(const bool folder)
{
   if (IsInspectMode() || m_player->m_playfieldWnd == nullptr)
      return;
   // Loading another table discards the edited table's unsaved changes: ask for confirmation first
   m_pendingLoadFolder = folder;
   if (m_table->GetDirtyState() != SaveDirtyState::Clean)
   {
      m_pendingNewTable.reset();
      m_pendingClose.reset();
      m_confirmLoadTable = true;
      return;
   }
   ShowLoadTableDialog(folder);
}

void EditorUI::RequestClose(const int closeState)
{
   // Only closing a full editor session loses the edited table (a live edit started from the Win32 editor or a play test gets back to its editor)
   if (m_player->IsEditorMode() && !IsInspectMode() && m_table->GetDirtyState() != SaveDirtyState::Clean)
   {
      m_pendingNewTable.reset();
      m_pendingClose = closeState;
      m_confirmLoadTable = true;
      return;
   }
   m_player->SetCloseState(static_cast<Player::CloseState>(closeState));
}

void EditorUI::ShowLoadTableDialog(const bool folder)
{
   m_pendingLoadPath = std::make_shared<string>();
   // Start on the edited table's file (right folder, preselected file), falling back to the recent load dir
   std::filesystem::path defaultLocation = m_table->m_filename;
   if (defaultLocation.empty())
      defaultLocation = PathFromString(m_table->GetSettings().GetRecentDir_LoadDir()) / "";
   else if (folder)
      defaultLocation = defaultLocation.parent_path();
   const string location = PathToUTF8(defaultLocation); // SDL expects UTF-8
   if (folder)
   {
      SDL_ShowOpenFolderDialog(
         [](void *userdata, const char *const *filelist, int filter)
         {
            auto *res = static_cast<std::shared_ptr<string> *>(userdata);
            if (filelist != nullptr && filelist[0] != nullptr)
               **res = filelist[0];
            delete res;
         },
         new std::shared_ptr<string>(m_pendingLoadPath), //
         m_player->m_playfieldWnd->GetCore(), location.empty() ? nullptr : location.c_str(), false);
      return;
   }
   const SDL_DialogFileFilter filters[] = { { "Visual Pinball Tables", "vpx;vpt;vpz" } };
   SDL_ShowOpenFileDialog(
      [](void *userdata, const char *const *filelist, int filter)
      {
         auto *res = static_cast<std::shared_ptr<string> *>(userdata);
         if (filelist != nullptr && filelist[0] != nullptr)
            **res = filelist[0];
         delete res;
      },
      new std::shared_ptr<string>(m_pendingLoadPath), //
      m_player->m_playfieldWnd->GetCore(), filters, 1, location.empty() ? nullptr : location.c_str(), false);
}

void EditorUI::NewTable(const NewTableTemplate templateType)
{
   if (IsInspectMode())
      return;
   // Creating a new table discards the edited table's unsaved changes: ask for confirmation first
   if (m_table->GetDirtyState() != SaveDirtyState::Clean)
   {
      m_pendingNewTable = templateType;
      m_pendingClose.reset();
      m_confirmLoadTable = true;
      return;
   }
   LoadTableTemplate(templateType);
}

void EditorUI::LoadTableTemplate(const NewTableTemplate templateType)
{
   if (m_fileOp != nullptr)
      return;
   string path;
   switch (templateType)
   {
   case NewTableTemplate::Stripped: path = "strippedTable.vpz"s; break;
   case NewTableTemplate::Example: path = "exampleTable.vpz"s; break;
   case NewTableTemplate::LightSeq: path = "lightSeqTable.vpz"s; break;
   case NewTableTemplate::Blank:
   default: path = "blankTable.vpz"s;
   }
   auto op = std::make_unique<AsyncFileOp>();
   op->type = AsyncFileOp::Type::NewTable;
   op->filename = g_app->m_fileLocator.GetAppPath(FileLocator::AppSubFolder::Assets, path);
   op->displayPath = path;
   StartFileOp(std::move(op));
}

void EditorUI::StartFileOp(std::unique_ptr<AsyncFileOp> op)
{
   assert(m_fileOp == nullptr);
   switch (op->type)
   {
   case AsyncFileOp::Type::Save:
   {
      if (!IsInspectMode())
         RestorePartsVisibility(); // Save the authored visibility, not the editor display state
      AsyncFileOp *const fileOp = op.get();
      PinTable *const table = op->saveTable != nullptr ? op->saveTable : m_table;
      op->worker = std::thread(
         [fileOp, table]()
         {
            fileOp->result = table->Save(fileOp->feedback);
            fileOp->completed = true;
         });
      break;
   }
   case AsyncFileOp::Type::Load:
   case AsyncFileOp::Type::NewTable:
   {
      AsyncFileOp *const fileOp = op.get();
      op->worker = std::thread(
         [fileOp]()
         {
            CComObject<PinTable> *table;
            CComObject<PinTable>::CreateInstance(&table);
            table->AddRef();
            if (fileOp->type == AsyncFileOp::Type::NewTable)
            {
               // Same initial table setup as the Win32 editor (WinEditor::OpenNewTable)
               table->m_glassTopHeight = table->m_glassBottomHeight = 210;
               for (int i = 0; i < 16; i++)
                  table->m_rgcolorcustom[i] = RGB(0, 0, 0);
            }
            const HRESULT hr = table->LoadGameFromFilename(fileOp->filename, fileOp->feedback);
            // Same as the Win32 editor: hash/corrupt content errors still keep the loaded table
            if (SUCCEEDED(hr) || (hr == APPX_E_BLOCK_HASH_INVALID) || (hr == APPX_E_CORRUPT_CONTENT))
            {
               if (fileOp->type == AsyncFileOp::Type::NewTable)
               {
                  table->m_title = "New Table"s;
                  table->GetSettings().SetIniPath(std::filesystem::path());
                  table->m_filename.clear();
               }
               fileOp->table = table;
            }
            else
               table->Release();
            fileOp->result = hr;
            fileOp->completed = true;
         });
      break;
   }
   }
   m_fileOp = std::move(op);
}

void EditorUI::FinishFileOp()
{
   AsyncFileOp *const op = m_fileOp.get();
   const HRESULT result = op->result;
   switch (op->type)
   {
   case AsyncFileOp::Type::Load:
   case AsyncFileOp::Type::NewTable:
      if (op->table != nullptr)
      {
         if (op->type == AsyncFileOp::Type::Load)
            g_settingsService.GetAppSettings().SetRecentDir_LoadDir(PathToString(op->filename.parent_path()), false);
         PinTable *const table = op->table;
         op->table = nullptr;
         m_player->SetTable(table, Player::TableTransition::Replace); // The loaded table reference is adopted by the player
      }
      else
         m_liveUI.PushNotification(op->type == AsyncFileOp::Type::NewTable ? "Failed to create new table from '"s + op->displayPath : "Failed to load table '"s + op->displayPath, 10000);
      break;

   case AsyncFileOp::Type::Save:
      // Saves must always report their result: a failed save was previously silent (and looked like a successful one)
      if (FAILED(result))
      {
         PLOGE << "Save failed (hr=" << std::hex << static_cast<unsigned long>(result) << std::dec << ") for " << op->displayPath;
         m_liveUI.PushNotification("SAVE FAILED: '" + op->displayPath + "'", 15000);
      }
      else
      {
         PLOGI << "Save done: " << op->displayPath;
         if (!IsInspectMode()) // inspect mode already reported when starting the save
            m_liveUI.PushNotification("Saved " + op->displayPath, 5000);
      }
      if (SUCCEEDED(result))
      {
         m_undo.SetCleanPoint(SaveDirtyState::Clean);
         // Start a new undo record on next edit (instead of merging it with the previous one), so that the table gets marked dirty again
         m_lastUndoPart = nullptr;
         m_lastUndoId = 0;
         if (op->restoreOnFailure)
            g_settingsService.GetAppSettings().SetRecentDir_LoadDir(PathToString(m_table->m_filename.parent_path()), false);
      }
      else if (op->restoreOnFailure)
      {
         m_table->m_filename = op->previousFilename;
         m_table->m_title = op->previousTitle;
      }
      break;
   }
   m_fileOp.reset();
}

void EditorUI::RenderFileOpModal()
{
   if (m_fileOp == nullptr)
      return;
   const char *action;
   switch (m_fileOp->type)
   {
   case AsyncFileOp::Type::Load: action = "Loading table"; break;
   case AsyncFileOp::Type::NewTable: action = "Creating table"; break;
   default: action = "Saving table"; break;
   }
   const string title = action + "###FileOp"s;
   ImGui::OpenPopup(title.c_str());
   if (ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
   {
      if (!m_fileOp->displayPath.empty())
         ImGui::TextDisabled("%s", m_fileOp->displayPath.c_str());
      const unsigned int length = m_fileOp->feedback.length.load();
      const unsigned int progress = m_fileOp->feedback.progress.load();
      ImGui::ProgressBar(length != 0 ? static_cast<float>(progress) / length : 0.f, ImVec2(280.f * m_liveUI.GetDPI(), 0.f));
      if (length != 0)
         ImGui::Text("%u / %u", progress, length);
      ImGui::EndPopup();
   }
}

void EditorUI::ResetCameraFromPlayer()
{
   // Try to setup editor camera to match the used one, but only mostly since the EditorUI does not have some view setup features like off-center, ...
   m_camView = Matrix3D::MatrixScale(1.f, 1.f, -1.f) * m_renderer->GetMVP().GetView(0) * Matrix3D::MatrixScale(1.f, -1.f, 1.f);
}

void EditorUI::SetBackdropCamera()
{
   // Setup the editor camera to frame the desktop backdrop (a flat rectangle in EDITOR_BG_WIDTH x EDITOR_BG_HEIGHT coordinates, Y axis going down)
   const ImGuiIO &io = ImGui::GetIO();
   m_camDistance = 0.5f * max((float)EDITOR_BG_HEIGHT, (float)EDITOR_BG_WIDTH * io.DisplaySize.y / io.DisplaySize.x);
   const vec3 eye((float)EDITOR_BG_WIDTH * 0.5f, (float)EDITOR_BG_HEIGHT * 0.5f, -m_camDistance);
   const vec3 at((float)EDITOR_BG_WIDTH * 0.5f, (float)EDITOR_BG_HEIGHT * 0.5f, 0.f);
   constexpr vec3 up { 0.f, -1.f, 0.f };
   m_camView = Matrix3D::MatrixLookAtRH(eye, at, up);
}

Vertex2D EditorUI::GetUIVisibleFraction() const
{
   // When the editor UI is not displayed (fly mode, inspection modal), nothing is drawn over the render
   if (m_flyMode || m_inspectionModal.IsVisible())
      return Vertex2D(1.f, 1.f);
   // Otherwise, the top chrome (and the side panes, unless the table is locked) are drawn over the render:
   // evaluate the remaining visible fraction, applied as a symmetric margin so that fitted content stays centered
   const ImGuiIO &io = ImGui::GetIO();
   const float marginX = m_table->IsLocked() ? 0.f : max(OutlinerPanel::PaneWidth, PropertiesPanel::PaneWidth) * m_liveUI.GetDPI();
   const float marginY = m_chrome.GetTopBarHeight();
   return Vertex2D(max(0.1f, 1.f - 2.f * marginX / io.DisplaySize.x), max(0.1f, 1.f - 2.f * marginY / io.DisplaySize.y));
}

void EditorUI::SetPlayfieldCamera()
{
   // Setup the editor camera to frame the playfield bounds from a top view (table XY plane, Y axis going down)
   const ImGuiIO &io = ImGui::GetIO();
   const Vertex2D visible = GetUIVisibleFraction();
   const float aspect = io.DisplaySize.x / io.DisplaySize.y;
   m_camDistance = 0.5f * max(m_table->m_bottom / visible.y, m_table->m_right / (aspect * visible.x));
   const vec3 eye(m_table->m_right * 0.5f, m_table->m_bottom * 0.5f, -m_camDistance);
   const vec3 at(m_table->m_right * 0.5f, m_table->m_bottom * 0.5f, 0.f);
   constexpr vec3 up { 0.f, -1.f, 0.f };
   m_camView = Matrix3D::MatrixLookAtRH(eye, at, up);
}

void EditorUI::Render3D()
{
   // While a table save is running on the file operation worker thread, the table parts must not be
   // touched: the UI part renders write the editor visibility overrides into the parts' data (m_d),
   // racing with the serialization and saving the display state instead of the authored visibility
   // restored by RestorePartsVisibility when the save was started
   if (IsSaveInProgress())
      return;
   UpdateEditableList();
   LiveRenderContext ctx(m_player, nullptr, m_camMode, m_shadeMode, (m_table->m_liveBaseTable != nullptr) && m_player->IsPlaying());
   for (const auto &uiPart : m_editables)
   {
      if ((m_camMode == ViewMode::DesktopBackdrop && !uiPart->GetEditable()->m_desktopBackdrop) || (m_camMode != ViewMode::DesktopBackdrop && uiPart->GetEditable()->m_desktopBackdrop))
         continue;
      uiPart->Render(ctx);
   }
   m_partLibrary.Render3D();
}

void EditorUI::RenderUI()
{
   const ImGuiIO &io = ImGui::GetIO();
   ImGuizmo::SetOrthographic(m_camMode == ViewMode::DesktopBackdrop || (m_camMode == ViewMode::EditorCam && !m_perspectiveCam));
   ImGuizmo::BeginFrame();
   ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);

   // In desktop backdrop mode, only backdrop parts can be selected and edited, and conversely
   {
      const bool backdropMode = m_camMode == ViewMode::DesktopBackdrop;
      std::erase_if(m_multiSel, [backdropMode](const std::shared_ptr<EditorUIPart> &part) { return part->GetEditable()->m_desktopBackdrop != backdropMode; });
      if (m_selection.GetType() == Selection::S_EDITABLE && !IsPartSelected(m_selection.GetPart()))
         m_selection = m_multiSel.empty() ? Selection() : Selection(m_multiSel.back());
      if (m_outlinerAnchor && m_outlinerAnchor->GetEditable()->GetItemType() != eItemPartGroup && m_outlinerAnchor->GetEditable()->m_desktopBackdrop != backdropMode)
         m_outlinerAnchor.reset();
   }

   // Add part mode housekeeping: it is only available in edit mode, when the table is not locked, and
   // while not in drag point edit mode, so cancel it if one of these conditions is no longer met
   if (m_addPartType != eItemInvalid && (IsInspectMode() || m_table->IsLocked() || m_pointEditPart != nullptr))
      m_addPartType = eItemInvalid;

   // Drag point edit mode housekeeping: exit without restoring the selection if the edited part is no
   // longer the active selected part, and drop selected points that do not exist anymore (points are
   // recreated on undo, ...)
   if (m_pointEditPart)
   {
      DragPointCurve *const curve = m_pointEditPart->GetDragPointCurve();
      if (IsInspectMode() || (curve == nullptr && !m_pointEditPart->HasEditCenter()) || m_selection.GetType() != Selection::S_EDITABLE || m_selection.GetPart() != m_pointEditPart)
         ExitPointEditMode(false);
      else if (curve != nullptr)
      {
         std::erase_if(m_pointSel, [curve](const DragPoint *point) { return curve->GetPointIndex(point) < 0; });
      }
   }

   // Apply the file picked by the asynchronous 'Save As' file dialog (deferred to the main thread, in edit mode)
   if (!IsInspectMode() && m_pendingSaveAsPath && !m_pendingSaveAsPath->empty() && m_fileOp == nullptr)
   {
      // The table is saved through its filename: set it for the save, and restore it if the save fails (which reports the error)
      auto op = std::make_unique<AsyncFileOp>();
      op->type = AsyncFileOp::Type::Save;
      op->restoreOnFailure = true;
      op->previousFilename = m_table->m_filename;
      op->previousTitle = m_table->m_title;
      m_table->m_filename = PathFromUTF8(*m_pendingSaveAsPath);
      m_table->m_title = TitleFromFilename(m_table->m_filename);
      op->filename = m_table->m_filename;
      op->displayPath = PathToUTF8(m_table->m_filename);
      m_pendingSaveAsPath = nullptr;
      // A folder selection is a VPZ folder pack: the directory must exist for the table to be saved as a folder (not a .vpx file)
      if (m_pendingSaveAsFolder)
      {
         m_pendingSaveAsFolder = false;
         std::filesystem::create_directories(m_table->m_filename);
      }
      StartFileOp(std::move(op));
   }

   // Apply the file picked by the asynchronous 'Load' file dialog (deferred to the main thread, in edit mode):
   // the loaded table replaces the edited one through a player session switch (see Player::SetTable)
   if (!IsInspectMode() && m_pendingLoadPath && !m_pendingLoadPath->empty() && m_fileOp == nullptr)
   {
      auto op = std::make_unique<AsyncFileOp>();
      op->type = AsyncFileOp::Type::Load;
      op->filename = PathFromUTF8(*m_pendingLoadPath);
      op->displayPath = PathToUTF8(op->filename);
      m_pendingLoadPath = nullptr;
      StartFileOp(std::move(op));
   }

   // Apply the .prefab picked by the asynchronous 'Import Unity Prefab' dialog (deferred to the main thread)
   if (!IsInspectMode() && m_pendingUnityPrefabPath && !m_pendingUnityPrefabPath->empty())
   {
      const std::filesystem::path path = PathFromUTF8(*m_pendingUnityPrefabPath);
      m_pendingUnityPrefabPath = nullptr;
      ImportUnityPrefab(path);
   }

   // Apply the result of a completed asynchronous file operation (running a worker thread load/save)
   if (m_fileOp && m_fileOp->completed)
      FinishFileOp();

#if !((defined(__APPLE__) && ((defined(TARGET_OS_IOS) && TARGET_OS_IOS) || (defined(TARGET_OS_TV) && TARGET_OS_TV))) || defined(__ANDROID__))

   const bool showFullUI = !m_inspectionModal.IsVisible() && !m_flyMode;
   m_chrome.Render(showFullUI);
   if (showFullUI)
   {
      // Side panels
      m_outliner.Render(m_chrome.GetTopBarHeight());
      m_properties.Render(m_chrome.GetTopBarHeight());
   }
   m_inspectionModal.Render();
   m_pbrConvertModal.Render();
   m_partLibrary.Render();
   m_scriptPanel.Render();

#endif

   // Deferred playfield fit on editor opening (display size and chrome height are valid once the UI has been rendered)
   if (m_fitPlayfieldCamera && io.DisplaySize.x > 0.f && io.DisplaySize.y > 0.f)
   {
      m_fitPlayfieldCamera = false;
      SetPlayfieldCamera();
   }

   // Invisible full frame window for overlays
   ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
   ImGui::SetNextWindowPos(ImVec2(0, 0));
   ImGui::PushStyleColor(ImGuiCol_WindowBg, 0);
   ImGui::PushStyleColor(ImGuiCol_Border, 0);
   ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
   ImGui::Begin("overlays", nullptr,
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings
         | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
   ImDrawList *const overlayDrawList = ImGui::GetWindowDrawList();

   // Confirmation popup for discarding unsaved changes when loading another table or closing (OpenPopup must be
   // called in the same window scope as the matching BeginPopupModal)
   // All variants share one popup id, so that a request arriving while it is open retargets it
   const char *const confirmTitle = m_pendingClose ? "Quit###ConfirmDiscard" : m_pendingNewTable ? "New Table###ConfirmDiscard" : "Load Table###ConfirmDiscard";
   if (m_confirmLoadTable)
   {
      m_confirmLoadTable = false;
      ImGui::OpenPopup(confirmTitle);
   }
   if (ImGui::BeginPopupModal(confirmTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
   {
      ImGui::TextUnformatted("The current table has unsaved changes which will be lost.");
      ImGui::NewLine();
      if (ImGui::Button("Discard changes"))
      {
         ImGui::CloseCurrentPopup();
         if (m_pendingClose)
         {
            m_player->SetCloseState(static_cast<Player::CloseState>(*m_pendingClose));
            m_pendingClose.reset();
         }
         else if (m_pendingNewTable)
         {
            LoadTableTemplate(*m_pendingNewTable);
            m_pendingNewTable.reset();
         }
         else
            ShowLoadTableDialog(m_pendingLoadFolder);
      }
      ImGui::SameLine();
      ImGui::SetItemDefaultFocus();
      if (ImGui::Button("Cancel"))
      {
         m_pendingNewTable.reset();
         m_pendingClose.reset();
         ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
   }

   // Progress feedback of the asynchronous file operation (load/save run on a worker thread): a modal popup
   // that blocks all interaction while the operation is running
   RenderFileOpModal();

   ImGui::End();
   ImGui::PopStyleVar();
   ImGui::PopStyleColor(2);

   // Update editor camera
   const Matrix3D RH2LH = Matrix3D::MatrixScale(1.f, 1.f, -1.f);
   const Matrix3D YAxis = Matrix3D::MatrixScale(1.f, -1.f, 1.f);
   if (m_camMode == ViewMode::PreviewCam)
   {
      m_renderer->InitLayout();
      m_camView = RH2LH * m_renderer->GetMVP().GetView(0) * YAxis;
      m_camProj = YAxis * m_renderer->GetMVP().GetProj(0);
      // Keep the orbit distance consistent with the player camera, so that leaving the preview camera orbits around the table (not around a stale point)
      Matrix3D previewInv(m_camView);
      previewInv.Invert();
      const vec3 tableCenter(m_table->m_right * 0.5f, m_table->m_bottom * 0.5f, 0.f);
      m_camDistance = max((previewInv.GetOrthoNormalPos() - tableCenter).Dot(previewInv.GetOrthoNormalDir()), 100.f);
   }
   else
   {
      // Apply editor camera to renderer (move view/projection from right handed to left handed)
      // Convert from right handed (ImGuizmo view manipulate is right handed) to VPX's left handed coordinate system
      // Right Hand to Left Hand (note that RH2LH = inverse(RH2LH), so RH2LH.RH2LH is identity, which property is used below)
      const Matrix3D view = RH2LH * m_camView * YAxis;
      const Matrix3D proj = YAxis * m_camProj;
      m_renderer->SetViewProj(view, proj);

      if (m_perspectiveCam && m_camMode != ViewMode::DesktopBackdrop) // The desktop backdrop is a 2D view, always edited with an orthographic camera
      {
         // Convert from right handed (ImGuizmo view manipulate is right handed) to VPX's left handed coordinate system
         // Right Hand to Left Hand (note that RH2LH = inverse(RH2LH), so RH2LH.RH2LH is identity, which property is used below)
         //const Matrix3D RH2LH = Matrix3D::MatrixScale(1.f, 1.f, -1.f);
         //const Matrix3D YAxis = Matrix3D::MatrixScale(1.f, -1.f, -1.f);
         //float zNear, zFar;
         //m_table->ComputeNearFarPlane(RH2LH * m_camView * YAxis, 1.f, zNear, zFar);
         // Near plane follows the orbit distance (a fixed 5 left a 10000:1 range, so near-coplanar surfaces z-fought when zoomed out: rails, lockdown bar, VLM lightmaps)
         const float zNear = clamp(m_camDistance * 0.05f, 5.f, 250.f);
         constexpr float zFar = 50000.f;
         m_camProj = Matrix3D::MatrixPerspectiveFovRH(m_navFovY, io.DisplaySize.x / io.DisplaySize.y, zNear, zFar);
      }
      else
      {
         constexpr float zNear = 0.5f;
         constexpr float zFar = 50000.f;
         const float viewHeight = m_camDistance;
         const float viewWidth = viewHeight * io.DisplaySize.x / io.DisplaySize.y;
         m_camProj = Matrix3D::MatrixOrthoOffCenterRH(-viewWidth, viewWidth, -viewHeight, viewHeight, zNear, -zFar);
      }
   }

   // Selection manipulator
   Matrix3D transform;
   const bool isSelectionTransformValid = GetSelectionTransform(transform);
   if (isSelectionTransformValid && !m_hadSelectionTransform && m_gizmoOperation == ImGuizmo::OPERATION(0) && !m_modal.active)
      m_gizmoOperation = ImGuizmo::TRANSLATE; // A new selection shows the move gizmo (Esc hides it)
   m_hadSelectionTransform = isSelectionTransformValid;
   if (isSelectionTransformValid && !m_table->IsLocked() && !m_modal.active)
   {
      float camViewLH[16];
      memcpy(camViewLH, &m_camView.m[0][0], sizeof(float) * 4 * 4);
      for (int i = 8; i < 12; i++)
         camViewLH[i] = -camViewLH[i];
      const Matrix3D prevTransform(transform);
      ImGuizmo::OPERATION gizmoOperation = m_gizmoOperation;
      if (m_pointEditPart || m_camMode == ViewMode::DesktopBackdrop)
      {
         // Drag point curves are 2D in the table XY plane, and backdrop parts are 2D in the backdrop XY plane: restrict gizmo operations to this plane
         if (gizmoOperation == ImGuizmo::TRANSLATE)
            gizmoOperation = static_cast<ImGuizmo::OPERATION>(ImGuizmo::TRANSLATE_X | ImGuizmo::TRANSLATE_Y);
         else if (gizmoOperation == ImGuizmo::ROTATE)
            gizmoOperation = ImGuizmo::ROTATE_Z;
         else if (gizmoOperation == ImGuizmo::SCALE)
            gizmoOperation = static_cast<ImGuizmo::OPERATION>(ImGuizmo::SCALE_X | ImGuizmo::SCALE_Y);
      }
      else if (m_camMode == ViewMode::EditorCam)
      {
         // Predefined views are axis aligned 2D views: restrict gizmo operations to the view plane
         switch (m_predefinedView)
         {
         case EditorUI::PredefinedView::None: break;
         case EditorUI::PredefinedView::Left:
         case EditorUI::PredefinedView::Right:
            if (gizmoOperation == ImGuizmo::TRANSLATE)
               gizmoOperation = static_cast<ImGuizmo::OPERATION>(ImGuizmo::TRANSLATE_Y | ImGuizmo::TRANSLATE_Z);
            else if (gizmoOperation == ImGuizmo::ROTATE)
               gizmoOperation = ImGuizmo::ROTATE_X;
            else if (gizmoOperation == ImGuizmo::SCALE)
               gizmoOperation = static_cast<ImGuizmo::OPERATION>(ImGuizmo::SCALE_Y | ImGuizmo::SCALE_Z);
            break;
         case EditorUI::PredefinedView::Top:
         case EditorUI::PredefinedView::Bottom:
            if (gizmoOperation == ImGuizmo::TRANSLATE)
               gizmoOperation = static_cast<ImGuizmo::OPERATION>(ImGuizmo::TRANSLATE_X | ImGuizmo::TRANSLATE_Y);
            else if (gizmoOperation == ImGuizmo::ROTATE)
               gizmoOperation = ImGuizmo::ROTATE_Z;
            else if (gizmoOperation == ImGuizmo::SCALE)
               gizmoOperation = static_cast<ImGuizmo::OPERATION>(ImGuizmo::SCALE_X | ImGuizmo::SCALE_Y);
            break;
         case EditorUI::PredefinedView::Front:
         case EditorUI::PredefinedView::Back:
            if (gizmoOperation == ImGuizmo::TRANSLATE)
               gizmoOperation = static_cast<ImGuizmo::OPERATION>(ImGuizmo::TRANSLATE_X | ImGuizmo::TRANSLATE_Z);
            else if (gizmoOperation == ImGuizmo::ROTATE)
               gizmoOperation = ImGuizmo::ROTATE_Y;
            else if (gizmoOperation == ImGuizmo::SCALE)
               gizmoOperation = static_cast<ImGuizmo::OPERATION>(ImGuizmo::SCALE_X | ImGuizmo::SCALE_Z);
            break;
         }
      }
      // A gizmo drag whose selection changed under the mouse (outliner click, undo,...) is over: the new selection must not follow it without an undo record
      const bool gizmoLost = m_gizmoDragActive && m_multiSel != m_gizmoDragSelection;
      if (gizmoLost)
      {
         ImGuizmo::Enable(false); // Clears ImGuizmo's using state
         ImGuizmo::Enable(true);
      }
      else
      {
         ImGuizmo::Manipulate(camViewLH, (float *)(m_camProj.m), gizmoOperation, m_gizmoMode, (float *)(transform.m));
         if (memcmp(transform.m, prevTransform.m, 16 * sizeof(float)) != 0)
         {
            // Mark all selected parts for undo once per drag (m_lastUndoId is reset when the gizmo is released)
            if (!m_gizmoDragActive && m_table->m_liveBaseTable == nullptr)
            {
               m_gizmoDragActive = true;
               m_gizmoDragSelection = m_multiSel;
               m_undo.BeginUndo();
               for (const auto &part : m_multiSel)
                  m_undo.MarkForUndo(part->GetEditable());
               m_undo.EndUndo();
               m_lastUndoPart = nullptr;
               m_lastUndoId = 0x1000;
            }
            SetSelectionTransform(transform);
         }
      }
   }
   else if (ImGuizmo::IsUsingAny())
   {
      // A drag interrupted with the mouse still held (invalid selection, table locked, Esc,...) is not manipulated anymore,
      // so ImGuizmo would stay in its using state: reset it (the drag's undo record stands)
      ImGuizmo::Enable(false);
      ImGuizmo::Enable(true);
   }
   // Reset gizmo undo deduplication once the gizmo is released (each drag should create a single undo entry)
   if (m_lastUndoId == 0x1000 && !ImGuizmo::IsUsing())
   {
      m_gizmoDragActive = false;
      m_gizmoDragSelection.clear();
      m_lastUndoPart = nullptr;
      m_lastUndoId = 0;
   }

   m_renderer->SetShadeMode(m_shadeMode);

   // Selection and physic colliders overlay (suspended while a save is running: the part renders
   // would write into the parts' data being serialized on the worker thread, see Render3D)
   {
      LiveRenderContext ctx(m_player, overlayDrawList, m_camMode, m_shadeMode, (m_table->m_liveBaseTable != nullptr) && m_player->IsPlaying());
      ctx.m_selectionDisplay = m_navSelectionDisplay;
      if (!m_multiSel.empty() && m_selectionOverlay && !IsSaveInProgress())
      {
         if (m_renderer->m_renderDevice->GetCurrentRenderTarget()->HasDepth())
            m_renderer->m_renderDevice->Clear(clearType::ZBUFFER, 0);
         ctx.m_isSelected = true;
         for (const auto &part : m_multiSel)
         {
            ctx.m_isActive = part == m_selection.GetPart();
            part->Render(ctx);
         }
         ctx.m_isActive = false;
         ctx.m_isSelected = false;
      }

      if (isSelectionTransformValid)
      {
         const ImVec2 pos = ctx.Project(transform.GetOrthoNormalPos());
         overlayDrawList->AddCircleFilled(pos, 3.f * m_liveUI.GetDPI(), IM_COL32(255, 255, 255, 255), 16);
      }

      if (m_modal.active)
         overlayDrawList->AddText(
            ImVec2((OutlinerPanel::PaneWidth + 16.f) * m_liveUI.GetDPI(), io.DisplaySize.y - 32.f * m_liveUI.GetDPI()), IM_COL32(255, 255, 255, 255), m_modal.hud.c_str());

      // In desktop backdrop mode, draw the bounds of the backdrop area
      if (m_camMode == ViewMode::DesktopBackdrop)
      {
         const Vertex3Ds bounds[4] = { Vertex3Ds(0.f, 0.f, 0.f), Vertex3Ds((float)EDITOR_BG_WIDTH, 0.f, 0.f), Vertex3Ds((float)EDITOR_BG_WIDTH, (float)EDITOR_BG_HEIGHT, 0.f),
            Vertex3Ds(0.f, (float)EDITOR_BG_HEIGHT, 0.f) };
         for (int i = 0; i < 4; i++)
            ctx.DrawLine(bounds[i], bounds[(i + 1) % 4], IM_COL32(255, 255, 255, 64));
      }

      // In drag point edit mode, render the drag points of the edited part's curve (or its center point)
      if (m_pointEditPart)
      {
         if (m_pointEditCenter)
         {
            const ImVec2 pos = ctx.Project(m_pointEditPart->GetEditCenter());
            if (pos.x != FLT_MAX)
            {
               const ImU32 color = m_centerSelected ? IM_COL32(255, 128, 0, 255) : IM_COL32(80, 160, 255, 255);
               const float radius = 5.f * m_liveUI.GetDPI();
               overlayDrawList->AddCircleFilled(pos, radius, color, 12);
               overlayDrawList->AddCircle(pos, radius + m_liveUI.GetDPI(), IM_COL32(0, 0, 0, 255), 12, 1.5f);
            }
         }
         else
         {
            const auto &points = m_pointEditPart->GetDragPointCurve()->GetPoints();
            for (size_t i = 0; i < points.size(); i++)
            {
               const ImVec2 pos = ctx.Project(Vertex3Ds(points[i]->GetX(), points[i]->GetY(), m_pointEditPart->GetDragPointZ(points[i].get())));
               if (pos.x == FLT_MAX)
                  continue;
               const ImU32 color = IsPointSelected(points[i].get()) ? IM_COL32(255, 128, 0, 255) : (points[i]->IsSmooth() ? IM_COL32(80, 160, 255, 255) : IM_COL32(255, 96, 96, 255));
               const float radius = (i == 0 ? 5.f : 4.f) * m_liveUI.GetDPI(); // First point is drawn slightly larger to mark the curve start
               overlayDrawList->AddCircleFilled(pos, radius, color, 12);
               overlayDrawList->AddCircle(pos, radius + m_liveUI.GetDPI(), IM_COL32(0, 0, 0, 255), 12, 1.5f);
            }
         }
      }

      if (m_physOverlay == PhysicOverlay::All || (m_physOverlay == PhysicOverlay::Selected && !m_multiSel.empty()))
      {
         auto project = [ctx](Vertex3Ds v)
         {
            const ImVec2 pt = ctx.Project(v);
            return Vertex2D(pt.x, pt.y);
         };
         ImGui::PushStyleColor(ImGuiCol_PlotLines, IM_COL32(255, 0, 0, 255)); // We abuse ImGui colors to pass render colors
         ImGui::PushStyleColor(ImGuiCol_PlotHistogram, IM_COL32(255, 0, 0, 64));
         for (auto pho : m_player->m_physics->GetHitObjects())
            if (pho != nullptr && (m_physOverlay == PhysicOverlay::All || (m_physOverlay == PhysicOverlay::Selected && IsEditableSelected(pho->m_editable))))
               pho->DrawUI(project, overlayDrawList, true);
         ImGui::PopStyleColor(2);
      }
   }

   RenderNavCube();

   // Handle uncaught mouse & keyboard shortcuts (a modal transform in progress owns the mouse and keyboard)
   const bool modalWasActive = m_modal.active;
   if (modalWasActive && m_fileOp == nullptr)
      UpdateModalTransform();
   if (!io.WantCaptureMouse && m_fileOp == nullptr && !modalWasActive)
   {
      // Trackpad: two finger scroll pans the view (Ctrl / Cmd + scroll zooms)
      if (m_navTrackpad && (io.MouseWheel != 0 || io.MouseWheelH != 0) && !io.KeyCtrl && !io.KeySuper)
      {
         if (m_camMode == ViewMode::PreviewCam)
            m_camMode = ViewMode::EditorCam;
         Matrix3D view(m_camView);
         view.Invert();
         const vec3 up = view.GetOrthoNormalUp(), dir = view.GetOrthoNormalDir(), right = view.GetOrthoNormalRight(), pos = view.GetOrthoNormalPos();
         const vec3 camTarget = pos - dir * m_camDistance;
         const float halfH = m_perspectiveCam && m_camMode != ViewMode::DesktopBackdrop ? m_camDistance * tanf(0.5f * ANGTORAD(m_navFovY)) : m_camDistance;
         const float unitsPerPixel = 2.f * halfH / io.DisplaySize.y;
         const vec3 offset = (right * -io.MouseWheelH + up * io.MouseWheel) * (20.f * unitsPerPixel);
         m_camView = Matrix3D::MatrixLookAtRH(pos + offset, camTarget + offset, up);
      }
      // Zoom in/out with mouse wheel
      else if (io.MouseWheel != 0)
      {
         if (m_camMode == ViewMode::PreviewCam)
            m_camMode = ViewMode::EditorCam;
         Matrix3D view(m_camView);
         view.Invert();
         const vec3 up = view.GetOrthoNormalUp(), dir = view.GetOrthoNormalDir(), pos = view.GetOrthoNormalPos();
         vec3 camTarget = pos - dir * m_camDistance;
         const float zoomFactor = powf(1.1f, (m_navInvertZoom ? 1.f : -1.f) * ImGui::GetIO().MouseWheel);
         vec3 newEye;
         if (m_navZoomToMouse && m_camMode != ViewMode::PreviewCam)
         {
            // Zoom toward the point under the cursor: keep the point of the focal plane under the mouse at the same screen position
            const vec3 right = view.GetOrthoNormalRight();
            const float halfH = m_perspectiveCam && m_camMode != ViewMode::DesktopBackdrop ? m_camDistance * tanf(0.5f * ANGTORAD(m_navFovY)) : m_camDistance;
            const ImVec2 mouse = ImGui::GetMousePos();
            const float ndcX = 2.f * mouse.x / io.DisplaySize.x - 1.f, ndcY = 1.f - 2.f * mouse.y / io.DisplaySize.y;
            const vec3 offset = right * (ndcX * halfH * io.DisplaySize.x / io.DisplaySize.y) + up * (ndcY * halfH);
            camTarget += offset * (1.f - zoomFactor);
            newEye = camTarget + dir * m_camDistance * zoomFactor;
         }
         else
         {
            // Zoom about the selection center (the eye moves along the line to it, the view direction is kept so nothing recenters)
            vec3 pivot = camTarget;
            GetNavPivot(pivot);
            newEye = pivot + (pos - pivot) * zoomFactor;
         }
         m_camDistance *= zoomFactor;
         m_camView = Matrix3D::MatrixLookAtRH(newEye, newEye - dir * m_camDistance, up);
      }

      // Mouse pan / orbit. Optionally (preference), Alt/Option + left drag acts as the middle button (no middle button on a trackpad / Magic Mouse)
      const ImGuiMouseButton navButton = (m_navAltMiddleButton && io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left)) ? ImGuiMouseButton_Left : ImGuiMouseButton_Middle;
      if (!ImGui::IsMouseDown(navButton))
         m_orbitDragging = false;
      else
      {
         if (!m_orbitDragging)
         {
            // Orbit pivot, fixed for the whole drag: the selection center, else (auto depth) the 3D point under the cursor, else the view target
            m_orbitDragging = true;
            m_orbitPivotValid = m_camMode != ViewMode::PreviewCam && m_camMode != ViewMode::DesktopBackdrop
               && (GetNavPivot(m_orbitPivot) || (m_navAutoDepth && PickDepthPoint(ImGui::GetMousePos(), m_orbitPivot)));
         }
         ImVec2 drag = ImGui::GetMouseDragDelta(navButton);
         ImGui::ResetMouseDragDelta(navButton);
         if (m_camMode != ViewMode::PreviewCam && (drag.x != 0.f || drag.y != 0.f))
         {
            Matrix3D viewInverse(m_camView);
            viewInverse.Invert();
            vec3 dir = viewInverse.GetOrthoNormalDir();
            const vec3 up = viewInverse.GetOrthoNormalUp();
            const vec3 pos = viewInverse.GetOrthoNormalPos();
            const vec3 right = viewInverse.GetOrthoNormalRight();
            vec3 camTarget = pos - dir * m_camDistance;
            if (io.KeyShift || m_camMode == ViewMode::DesktopBackdrop || m_orbitLock)
            {
               // Pan so that the point under the cursor follows it (the view height at the orbit target depends on the projection)
               const float halfH = m_perspectiveCam && m_camMode != ViewMode::DesktopBackdrop ? m_camDistance * tanf(0.5f * ANGTORAD(m_navFovY)) : m_camDistance;
               drag.x *= 2.f * halfH / io.DisplaySize.y;
               drag.y *= 2.f * halfH / io.DisplaySize.y;
               camTarget = camTarget - right * drag.x + up * drag.y;
               m_camView = Matrix3D::MatrixLookAtRH(pos - right * drag.x + up * drag.y, camTarget, up);
            }
            else
            {
               m_predefinedView = PredefinedView::None;
               const Matrix3D rx = Matrix3D::MatrixRotate(drag.x * 0.01f * m_navOrbitSensitivity, up);
               const Matrix3D ry = Matrix3D::MatrixRotate(drag.y * 0.01f * m_navOrbitSensitivity, right);
               const Matrix3D roll = rx * ry;
               const vec3 pivot = m_orbitPivotValid ? m_orbitPivot : camTarget;
               const vec3 offset = roll.MultiplyVectorNoPerspective(pos - pivot); // Rigid rotation of the camera about the pivot (no recentering)
               dir = roll.MultiplyVectorNoPerspective(dir);
               dir.Normalize();

               // clamp
               vec3 planDir = CrossProduct(right, up);
               planDir.y = 0.f;
               planDir.Normalize();
               if (float dt = planDir.Dot(dir); dt < 0.0f)
               {
                  dir += planDir * dt;
                  dir.Normalize();
               }

               const vec3 newEye = pivot + offset;
               m_camView = Matrix3D::MatrixLookAtRH(newEye, newEye - dir * m_camDistance, up);
            }
         }
      }

      // Select: click selects the front-most part (click again to cycle through overlapping parts),
      // shift+click toggles a part in the multi selection, drag box selects parts (shift+drag adds to the selection)
      // In drag point edit mode, click selects a drag point of the edited curve (shift toggles), dragging a
      // selected point moves the selected points in the table XY plane, drag box selects points
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !(m_navAltMiddleButton && io.KeyAlt) && !ImGuizmo::IsOver() && !ImGuizmo::IsViewManipulateHovered()
         && !ImGuizmo::IsUsingViewManipulate()) // Alt+click is orbit when Alt acts as the middle button
      {
         m_boxSelectStart = ImGui::GetMousePos();
         if (m_partLibrary.IsPlacementArmed())
         {
            // Part library placement mode: drop the previewed part at its ghost position
            m_partLibrary.CommitPlacement();
         }
         else if (m_addPartType != eItemInvalid)
         {
            // Add part mode: create the pending part at the picked point of the playfield XY plane
            const ItemTypeEnum type = m_addPartType;
            m_addPartType = eItemInvalid;
            CreatePart(type, UnprojectToPlane(m_boxSelectStart, 0.f));
         }
         else if (m_pointEditPart != nullptr && m_pointEditCenter)
         {
            // Center point edit mode: clicking on the center selects it and starts a pending drag
            if (HitTestEditCenter(m_boxSelectStart))
            {
               if (io.KeyShift)
                  m_centerSelected = !m_centerSelected;
               else
                  m_centerSelected = true;
               m_pointDragPending = m_centerSelected;
               m_pointDragZ = m_pointEditPart->GetEditCenter().z;
               m_pointDragPos = UnprojectToPlane(m_boxSelectStart, m_pointDragZ);
            }
            else
               m_boxSelectActive = true;
         }
         else
         {
            DragPoint *const hitPoint = (m_pointEditPart != nullptr) ? HitTestDragPoint(m_boxSelectStart) : nullptr;
            if (hitPoint != nullptr)
            {
               if (io.KeyShift)
                  TogglePointSelection(hitPoint);
               else if (!IsPointSelected(hitPoint))
               {
                  m_pointSel.clear();
                  m_pointSel.push_back(hitPoint);
               }
               m_pointDragPending = IsPointSelected(hitPoint) && !hitPoint->m_uiLocked;
               m_pointDragZ = m_pointEditPart->GetDragPointZ(hitPoint);
               m_pointDragPos = UnprojectToPlane(m_boxSelectStart, m_pointDragZ);
            }
            else
               m_boxSelectActive = true;
         }
      }
      if (m_boxSelectActive)
      {
         if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
         {
            const ImVec2 end = ImGui::GetMousePos();
            if (fabsf(end.x - m_boxSelectStart.x) > 2.f || fabsf(end.y - m_boxSelectStart.y) > 2.f)
            {
               overlayDrawList->AddRectFilled(m_boxSelectStart, end, IM_COL32(255, 128, 0, 32));
               overlayDrawList->AddRect(m_boxSelectStart, end, IM_COL32(255, 128, 0, 255));
            }
         }
         else
         {
            m_boxSelectActive = false;
            const ImVec2 end = ImGui::GetMousePos();
            if (m_pointEditPart)
            {
               if (fabsf(end.x - m_boxSelectStart.x) > 4.f || fabsf(end.y - m_boxSelectStart.y) > 4.f)
                  BoxSelectPoints(m_boxSelectStart, end, io.KeyShift);
               else if (!io.KeyShift)
               {
                  m_pointSel.clear();
                  m_centerSelected = false;
               }
            }
            else if (fabsf(end.x - m_boxSelectStart.x) > 4.f || fabsf(end.y - m_boxSelectStart.y) > 4.f)
               BoxSelectParts(m_boxSelectStart, end, io.KeyShift);
            else
            {
               vector<HitTestResult> vhoHit;
               RayCastParts(end, vhoHit);
               if (io.KeyShift)
               {
                  // Shift+click toggles the front-most hit part in the multi selection
                  if (!vhoHit.empty())
                  {
                     const auto it = m_editableMap.find(vhoHit.front().m_obj->m_editable);
                     if (it != m_editableMap.end())
                     {
                        TogglePartSelection(it->second);
                        m_outlinerAnchor = it->second;
                     }
                  }
               }
               else if (vhoHit.empty())
                  ClearSelection();
               else
               {
                  // Click selects the nearest hit part, subsequent clicks cycle to the parts behind it
                  vector<std::shared_ptr<EditorUIPart>> hitParts;
                  for (const HitTestResult &hr : vhoHit)
                  {
                     const auto it = m_editableMap.find(hr.m_obj->m_editable);
                     if (it != m_editableMap.end() && std::ranges::find(hitParts, it->second) == hitParts.end())
                        hitParts.push_back(it->second);
                  }
                  if (hitParts.empty())
                     ClearSelection();
                  else if (io.KeyCtrl)
                  {
                     // Ctrl+click lists all the parts under the cursor to pick from
                     m_pickList = hitParts;
                     m_pickListPos = end;
                     m_pickListOpen = true;
                  }
                  else
                  {
                     size_t selectionIndex = 0;
                     const auto selIt = std::ranges::find(hitParts, m_selection.GetPart());
                     if (selIt != hitParts.end())
                        selectionIndex = static_cast<size_t>(selIt - hitParts.begin() + 1) % hitParts.size();
                     SetSelection(Selection(hitParts[selectionIndex]));
                     m_outlinerAnchor = hitParts[selectionIndex];
                  }
                  // TODO add debug action to make ball active: m_player->m_pactiveballDebug = m_pBall;
               }
            }
         }
      }

      // Drag the selected drag points in the table XY plane
      if (m_pointEditPart && (m_pointDragPending || m_pointDragActive))
      {
         if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
         {
            const Vertex2D pos = UnprojectToPlane(ImGui::GetMousePos(), m_pointDragZ);
            const Vertex2D delta(pos.x - m_pointDragPos.x, pos.y - m_pointDragPos.y);
            if (m_pointDragPending)
            {
               const ImVec2 dragDelta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
               if (fabsf(dragDelta.x) > 2.f || fabsf(dragDelta.y) > 2.f)
               {
                  // Drag actually starts: mark the edited part for undo once per drag
                  m_pointDragPending = false;
                  m_pointDragActive = m_pointEditCenter ? m_centerSelected : !m_pointSel.empty();
                  if (m_pointDragActive)
                  {
                     m_undo.BeginUndo();
                     m_undo.MarkForUndo(m_pointEditPart->GetEditable());
                     m_undo.EndUndo();
                  }
               }
            }
            if (m_pointDragActive && (delta.x != 0.f || delta.y != 0.f))
            {
               if (m_pointEditCenter)
               {
                  const Vertex3Ds center = m_pointEditPart->GetEditCenter();
                  m_pointEditPart->SetEditCenter(Vertex2D(center.x + delta.x, center.y + delta.y));
               }
               else
               {
                  DragPointCurve *const curve = m_pointEditPart->GetDragPointCurve();
                  for (DragPoint *point : m_pointSel)
                     point->Translate(delta);
                  curve->OnPointsModified();
               }
               m_renderer->ReinitRenderable(m_pointEditPart->GetEditable()->GetIRenderable());
               m_renderer->InvalidateShadowContent();
               m_player->m_physics->Update(m_pointEditPart->GetEditable());
            }
            m_pointDragPos = pos;
         }
         else
         {
            m_pointDragPending = false;
            m_pointDragActive = false;
         }
      }
   }
   if (m_boxSelectActive && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
      m_boxSelectActive = false; // Canceled: button released outside of the playfield
   if (!io.WantCaptureKeyboard && m_fileOp == nullptr && !modalWasActive)
      // Ctrl+S must work while a widget (e.g. a slider) has keyboard focus; only a text input being edited takes precedence
      if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_S, false) && io.KeyCtrl && !io.KeyAlt && !io.KeyShift)
         SaveTable();
   {
      if (ImGui::IsKeyReleased(ImGuiKey_Escape))
      {
         if (m_suppressNextEscape)
            m_suppressNextEscape = false; // Consumed by a field the Esc deactivated (e.g. the script editor)
         else if (m_gizmoOperation != ImGuizmo::OPERATION(0))
            m_gizmoOperation = ImGuizmo::OPERATION(0); // Cancel current operation
         else if (m_boxSelectActive)
            m_boxSelectActive = false; // Cancel current box selection
         else if (m_addPartType != eItemInvalid)
            m_addPartType = eItemInvalid; // Cancel add part mode
         else if (m_partLibrary.IsPlacementArmed())
            m_partLibrary.CancelPlacement(); // Cancel part library placement mode
         else if (m_pointEditPart)
            ExitPointEditMode(true); // Exit drag point edit mode
         else if (m_selection.GetType() != Selection::S_NONE)
            ClearSelection(); // Cancel current selection
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_F5) && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift)
      {
         // Play a shallow copy of the edited table
         if (!IsInspectMode() && !m_table->IsLocked())
            PlayTest();
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_N))
      {
         // New table from the default blank template
         if (io.KeyCtrl && !io.KeyAlt && !io.KeyShift)
            NewTable(NewTableTemplate::Blank);
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_O))
      {
         // Open a table file, or a table pack folder with Shift
         if (io.KeyCtrl && !io.KeyAlt && !io.KeyShift)
            LoadTable();
         else if (io.KeyCtrl && io.KeyShift && !io.KeyAlt)
            LoadTableFolder();
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_Tab, false) && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift)
      {
         // Toggle drag point edit mode on the active selected part. For parts exposing an editable
         // center (light bulb, light sequencer animation center), Tab first switches the mode from
         // the drag point curve to the center point before exiting.
         if (m_pointEditPart)
         {
            if (!m_pointEditCenter && m_pointEditPart->HasEditCenter())
            {
               m_pointEditCenter = true;
               m_centerSelected = true;
               m_pointSel.clear();
               m_pointDragPending = false;
               m_pointDragActive = false;
            }
            else
               ExitPointEditMode(true);
         }
         else
            EnterPointEditMode();
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_F) && io.KeyShift && !io.KeyCtrl && !io.KeyAlt) // Hide the UI (F frames the selection)
      {
         m_flyMode = !m_flyMode;
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_A))
      {
         if (io.KeyAlt && !io.KeyCtrl && !io.KeyShift)
            ClearSelection();
         else if (io.KeyShift && !io.KeyCtrl && !io.KeyAlt)
         {
            // Add a drag point on the curve segment nearest to the mouse position
            if (m_pointEditPart && !m_pointEditCenter && !io.WantCaptureMouse)
               AddPointOnNearestSegment();
            else if (!m_pointEditPart && !IsInspectMode() && !m_table->IsLocked())
            {
               // Request the part type picker popup at the mouse position (it is opened from the
               // toolbar window scope below, as OpenPopup must be called in the same window scope
               // as the matching BeginPopup)
               m_chrome.RequestAddPartPopup(ImGui::GetMousePos());
            }
         }
         else if (!io.KeyCtrl && !io.KeyAlt && !io.KeyShift)
         {
            // Select all: all curve points in drag point edit mode, the center point in center edit mode, all pickable parts otherwise
            if (m_pointEditPart)
            {
               if (m_pointEditCenter)
                  m_centerSelected = true;
               else
               {
                  m_pointSel.clear();
                  for (const auto &point : m_pointEditPart->GetDragPointCurve()->GetPoints())
                     m_pointSel.push_back(point.get());
               }
            }
            else
               SelectAllParts();
         }
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_C))
      {
         // Copy the selected parts (or the coordinates of the selected drag point in point edit mode)
         if (io.KeyCtrl && !io.KeyAlt && !io.KeyShift)
            CopySelection();
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_V))
      {
         // Paste at the mouse position (or to the selected drag point in point edit mode)
         if (io.KeyCtrl && !io.KeyAlt && !io.KeyShift)
            PasteSelection(ImGui::GetMousePos());
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_Delete))
      {
         if (!io.KeyAlt && !io.KeyCtrl && !io.KeyShift)
         {
            if (m_pointEditPart)
               DeleteSelectedPoints();
            else
               DeleteSelection();
         }
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_D) && io.KeyShift && !io.KeyCtrl && !io.KeyAlt)
      {
         // Duplicate the selected parts (through the editor clipboard) at the mouse position and grab them
         if (!m_pointEditPart && !m_multiSel.empty() && !IsInspectMode() && !m_table->IsLocked())
         {
            CopySelection();
            PasteSelection(ImGui::GetMousePos());
            if (m_navBlenderTransform)
               BeginModalTransform(ModalOp::Grab);
            else
               SetGizmoOperation(ImGuizmo::TRANSLATE);
         }
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_H))
      {
         if (m_table->m_liveBaseTable == nullptr && !io.KeyCtrl) // No UI visibility in inspection mode
         {
            bool changed = false;
            if (io.KeyAlt)
            { // Unhide all
               for (auto &part : m_editables)
                  if (part->GetEditable()->GetItemType() != eItemPartGroup && (part->GetEditable()->m_desktopBackdrop == (m_camMode == ViewMode::DesktopBackdrop))
                     && !part->GetEditable()->IsUIVisible(false))
                  {
                     part->GetEditable()->SetUIVisible(true);
                     changed = true;
                  }
            }
            else if (io.KeyShift)
            { // Hide unselected
               for (auto &part : m_editables)
                  if (part->GetEditable()->GetItemType() != eItemPartGroup && (part->GetEditable()->m_desktopBackdrop == (m_camMode == ViewMode::DesktopBackdrop)) && !IsPartSelected(part)
                     && part->GetEditable()->IsUIVisible(false))
                  {
                     part->GetEditable()->SetUIVisible(false);
                     changed = true;
                  }
            }
            else
            { // Hide selected
               for (const auto &part : m_multiSel)
                  if (part->GetEditable()->IsUIVisible(false))
                  {
                     part->GetEditable()->SetUIVisible(false);
                     changed = true;
                  }
               ClearSelection();
            }
            if (changed)
               m_table->SetNonUndoableDirty(SaveDirtyState::Dirty);
         }
      }
      else if ((ImGui::IsKeyPressed(ImGuiKey_W) || ImGui::IsKeyPressed(ImGuiKey_E) || ImGui::IsKeyPressed(ImGuiKey_T)) && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift)
      {
         // Gizmo tools, always available: W move, E rotate, T scale (repeat toggles world / local)
         SetGizmoOperation(ImGui::IsKeyPressed(ImGuiKey_W) ? ImGuizmo::TRANSLATE : ImGui::IsKeyPressed(ImGuiKey_E) ? ImGuizmo::ROTATE : ImGuizmo::SCALE);
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_G))
      {
         // Grab (translate)
         if (m_camMode == ViewMode::PreviewCam)
            m_camMode = ViewMode::EditorCam;
         if (io.KeyAlt && !m_pointEditPart)
         {
            Matrix3D tmp;
            if (GetSelectionTransform(tmp))
               SetSelectionTransform(tmp, true, false, false);
         }
         else if (!io.KeyCtrl && (m_navBlenderTransform != io.KeyShift)) // Shift picks the other scheme
            BeginModalTransform(ModalOp::Grab);
         else if (!io.KeyCtrl)
            SetGizmoOperation(ImGuizmo::TRANSLATE);
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_S))
      {
         // Ctrl+S (save) is handled earlier, before the WantCaptureKeyboard check
         if (!io.KeyCtrl)
         {
            // Scale
            if (m_camMode == ViewMode::PreviewCam)
               m_camMode = ViewMode::EditorCam;
            if (io.KeyAlt && !m_pointEditPart)
            {
               Matrix3D tmp;
               if (GetSelectionTransform(tmp))
                  SetSelectionTransform(tmp, false, true, false);
            }
            else if (m_navBlenderTransform != io.KeyShift)
               BeginModalTransform(ModalOp::Scale);
            else
               SetGizmoOperation(ImGuizmo::SCALE);
         }
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_R))
      {
         // Rotate
         if (m_camMode == ViewMode::PreviewCam)
            m_camMode = ViewMode::EditorCam;
         if (io.KeyAlt && !m_pointEditPart)
         {
            Matrix3D tmp;
            if (GetSelectionTransform(tmp))
               SetSelectionTransform(tmp, false, false, true);
         }
         else if (!io.KeyCtrl && (m_navBlenderTransform != io.KeyShift)) // Shift picks the other scheme
            BeginModalTransform(ModalOp::Rotate);
         else if (!io.KeyCtrl)
            SetGizmoOperation(ImGuizmo::ROTATE);
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_Z))
      {
         if (io.KeyCtrl)
         { // Undo
            m_lastUndoPart = nullptr;
            m_lastUndoId = 0;
            if (m_table->m_liveBaseTable == nullptr)
            {
               // TODO handle IsUndoPastCleanPoint
               const std::any state = m_undo.Undo();
               if (state.has_value())
                  RestoreUndoSelection(std::any_cast<const UndoSelectionState &>(state));
            }
         }
         else if (io.KeyAlt && !io.KeyShift)
            m_selectionOverlay = !m_selectionOverlay; // Alt+Z: show / hide the selection overlay
         else if (!io.KeyShift && !io.KeyAlt)
         { // Wireframe shade mode selection
            switch (m_shadeMode)
            {
            case Renderer::ShadeMode::Default: m_shadeMode = Renderer::ShadeMode::Wireframe; break;
            case Renderer::ShadeMode::Wireframe: m_shadeMode = Renderer::ShadeMode::NoDepthWireframe; break;
            case Renderer::ShadeMode::NoDepthWireframe: m_shadeMode = Renderer::ShadeMode::Default; break;
            }
         }
      }
      else if (IsNumpadPressed(0))
         CycleCamera();
      else if (IsNumpadPressed(5))
      {
         m_camMode = ViewMode::EditorCam;
         m_perspectiveCam = !m_perspectiveCam;
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_KeypadDecimal) || (ImGui::IsKeyPressed(ImGuiKey_F) && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift))
         FrameSelection();
      else if (ImGui::IsKeyPressed(ImGuiKey_Home))
         FrameAll();
      else if (ImGui::IsKeyPressed(ImGuiKey_KeypadDivide))
      {
         m_orbitLock = !m_orbitLock;
      }
      else if (IsNumpadPressed(2) || IsNumpadPressed(4) || IsNumpadPressed(6) || IsNumpadPressed(8))
         OrbitStep(IsNumpadPressed(4) || IsNumpadPressed(6), (IsNumpadPressed(4) || IsNumpadPressed(8)) ? 1.f : -1.f);
      else if (IsNumpadPressed(7))
         SetView(io.KeyCtrl ? PredefinedView::Bottom : PredefinedView::Top);
      else if (IsNumpadPressed(1))
         SetView(io.KeyCtrl ? PredefinedView::Back : PredefinedView::Front);
      else if (IsNumpadPressed(3))
         SetView(io.KeyCtrl ? PredefinedView::Right : PredefinedView::Left);
   }

   // Parts under the cursor list (Ctrl+click)
   if (m_pickListOpen)
   {
      ImGui::SetNextWindowPos(m_pickListPos, ImGuiCond_Appearing);
      ImGui::SetNextWindowSizeConstraints(ImVec2(220.f * m_liveUI.GetDPI(), 0.f), ImVec2(FLT_MAX, FLT_MAX));
      bool open = true;
      if (ImGui::Begin("Parts under cursor", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse))
      {
         for (const auto &part : m_pickList)
            if (ImGui::Selectable(part->GetEditable()->GetName().c_str(), IsPartSelected(part)))
            {
               SetSelection(Selection(part));
               m_outlinerAnchor = part;
               open = false;
            }
         if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows))
            open = false;
      }
      ImGui::End();
      if (!open || ImGui::IsKeyPressed(ImGuiKey_Escape))
         m_pickListOpen = false;
   }

   // Selected parts are kept dynamic in the UI quadtree so that they can be edited interactively. The selection also changes outside
   // of this pass (automation, undo, table switches) and parts get re-added as static (undelete), so the state is synced rather than
   // derived from selection changes
   std::erase_if(m_dynamicParts, [this](IEditable *edit) { return FindIndexOf(m_table->GetParts(), edit) == -1; });
   for (IEditable *const edit : m_dynamicParts)
      if (!IsEditableSelected(edit) && !m_player->m_physics->IsStatic(edit))
         m_player->m_physics->SetStatic(edit);
   std::erase_if(m_dynamicParts, [this](IEditable *edit) { return !IsEditableSelected(edit); });
   for (const auto &part : m_multiSel)
   {
      IEditable *const edit = part->GetEditable();
      if (edit->GetIHitable() == nullptr || edit->GetItemType() == eItemBall)
         continue;
      if (m_player->m_physics->IsStatic(edit))
         m_player->m_physics->SetDynamic(edit);
      if (FindIndexOf(m_dynamicParts, edit) == -1)
         m_dynamicParts.push_back(edit);
   }
}

void EditorUI::PushUndo(IEditable *part, unsigned int undoId)
{
   // No undo support in inspection mode (not implemented as it needs to handle both version of the table)
   if (m_table->m_liveBaseTable)
      return;

   // Filter out continuous modification after first one (dragging a part, editing values,...)
   if (part == m_lastUndoPart && undoId == m_lastUndoId)
      return;

   m_lastUndoPart = part;
   m_lastUndoId = undoId;
   m_undo.BeginUndo();
   m_undo.MarkForUndo(part);
   m_undo.EndUndo();
}

bool EditorUI::IsPartSelected(const std::shared_ptr<EditorUIPart> &part) const { return part != nullptr && std::ranges::find(m_multiSel, part) != m_multiSel.end(); }

bool EditorUI::IsEditableSelected(const IEditable *editable) const
{
   for (const auto &part : m_multiSel)
      if (part->GetEditable() == editable)
         return true;
   return false;
}

void EditorUI::ClearSelection()
{
   m_selection = Selection();
   m_multiSel.clear();
   m_multiSelImages.clear();
   m_multiSelSounds.clear();
   m_multiSelMaterials.clear();
   m_outlinerAnchor.reset();
   m_outlinerImageAnchor = nullptr;
   m_outlinerSoundAnchor = nullptr;
   m_outlinerMaterialAnchor = nullptr;
}

EditorUI::UndoSelectionState EditorUI::CaptureUndoSelection() const
{
   UndoSelectionState state { m_selection, m_multiSel, m_outlinerAnchor, m_pointEditPart, {}, m_pointEditCenter };
   // Drag points are deleted and recreated when their part is reloaded (undo, ...): store their index in the curve
   if (DragPointCurve *const curve = m_pointEditPart ? m_pointEditPart->GetDragPointCurve() : nullptr)
   {
      for (const DragPoint *point : m_pointSel)
         state.pointSel.push_back(curve->GetPointIndex(point));
   }
   return state;
}

void EditorUI::RestoreUndoSelection(const UndoSelectionState &state)
{
   // Ensure the editable map is up to date (undo may have deleted or undeleted parts)
   UpdateEditableList();
   // Parts are resolved through the editable map since undo may have removed and recreated them
   const auto resolve = [this](const std::shared_ptr<EditorUIPart> &part) -> std::shared_ptr<EditorUIPart>
   {
      if (part == nullptr)
         return nullptr;
      const auto it = m_editableMap.find(part->GetEditable());
      return it != m_editableMap.end() ? it->second : nullptr;
   };
   // Restore the drag point edit mode state (selected points are resolved by index since they are recreated on part reload)
   const std::shared_ptr<EditorUIPart> pointEditPart = resolve(state.pointEditPart);
   if (m_pointEditPart != pointEditPart)
   {
      if (m_pointEditPart)
         m_pointEditPart->SetPointEditContext(nullptr);
      m_pointEditPart = pointEditPart;
      if (m_pointEditPart)
         m_pointEditPart->SetPointEditContext(this);
      m_pointDragPending = false;
      m_pointDragActive = false;
   }
   m_pointEditCenter = state.pointEditCenter && m_pointEditPart && m_pointEditPart->HasEditCenter();
   m_centerSelected = m_pointEditCenter;
   m_pointSel.clear();
   if (DragPointCurve *const curve = m_pointEditPart ? m_pointEditPart->GetDragPointCurve() : nullptr)
   {
      const auto &points = curve->GetPoints();
      for (const int index : state.pointSel)
         if (index >= 0 && index < (int)points.size())
            m_pointSel.push_back(points[index].get());
   }
   m_multiSel.clear();
   for (const auto &part : state.multiSel)
      if (std::shared_ptr<EditorUIPart> resolved = resolve(part))
         m_multiSel.push_back(resolved);
   // Drop resources that no longer exist in the table
   std::erase_if(m_multiSelImages, [this](const Texture *image) { return std::ranges::find(m_table->m_vimage, image) == m_table->m_vimage.end(); });
   std::erase_if(m_multiSelSounds, [this](const VPX::Sound *sound) { return std::ranges::find(m_table->m_vsound, sound) == m_table->m_vsound.end(); });
   std::erase_if(m_multiSelMaterials, [this](const Material *material) { return std::ranges::find(m_table->m_materials, material) == m_table->m_materials.end(); });
   switch (state.selection.GetType())
   {
   case Selection::S_EDITABLE:
   {
      std::shared_ptr<EditorUIPart> part = resolve(state.selection.GetPart());
      if (part == nullptr || !IsPartSelected(part))
         part = m_multiSel.empty() ? nullptr : m_multiSel.back();
      m_selection = part ? Selection(part) : Selection();
      break;
   }
   case Selection::S_MATERIAL:
      m_selection = std::ranges::find(m_table->GetMaterialList(), state.selection.GetMaterial()) != m_table->GetMaterialList().end() ? state.selection : Selection();
      break;
   case Selection::S_IMAGE: m_selection = std::ranges::find(m_table->GetImageList(), state.selection.GetImage()) != m_table->GetImageList().end() ? state.selection : Selection(); break;
   case Selection::S_RENDERPROBE:
      m_selection = std::ranges::find(m_table->GetRenderProbeList(), state.selection.GetProbe()) != m_table->GetRenderProbeList().end() ? state.selection : Selection();
      break;
   case Selection::S_SOUND: m_selection = std::ranges::find(m_table->m_vsound, state.selection.GetSound()) != m_table->m_vsound.end() ? state.selection : Selection(); break;
   case Selection::S_COLLECTION:
      m_selection = std::ranges::find(m_table->GetCollections(), state.selection.GetCollection()) != m_table->GetCollections().end() ? state.selection : Selection();
      break;
   default: m_selection = state.selection; break; // S_NONE, S_CAMERA
   }
   m_outlinerAnchor = resolve(state.outlinerAnchor);
}

void EditorUI::SetSelection(const Selection &selection)
{
   m_selection = selection;
   m_multiSel.clear();
   m_multiSelImages.clear();
   m_multiSelSounds.clear();
   m_multiSelMaterials.clear();
   switch (selection.GetType())
   {
   case Selection::S_EDITABLE: m_multiSel.push_back(selection.GetPart()); break;
   case Selection::S_IMAGE: m_multiSelImages.push_back(selection.GetImage()); break;
   case Selection::S_SOUND: m_multiSelSounds.push_back(selection.GetSound()); break;
   case Selection::S_MATERIAL: m_multiSelMaterials.push_back(selection.GetMaterial()); break;
   default: break;
   }
}

void EditorUI::TogglePartSelection(const std::shared_ptr<EditorUIPart> &part)
{
   const auto it = std::ranges::find(m_multiSel, part);
   if (it == m_multiSel.end())
   {
      m_multiSel.push_back(part);
      m_selection = Selection(part);
   }
   else
   {
      const bool wasActive = m_selection.GetType() == Selection::S_EDITABLE && m_selection.GetPart() == part;
      m_multiSel.erase(it);
      if (m_multiSel.empty())
         m_selection = Selection();
      else if (wasActive)
         m_selection = Selection(m_multiSel.back());
   }
}

void EditorUI::SelectOutlinerRange(const std::shared_ptr<EditorUIPart> &part)
{
   // Range selection between the anchor part (last clicked one) and the given part
   if (m_outlinerAnchor == nullptr)
   {
      SetSelection(Selection(part));
      return;
   }
   int anchorPos = -1, partPos = -1, pos = 0;
   for (const auto &edit : m_editables)
   {
      if (edit->GetEditable()->GetItemType() == eItemPartGroup || (edit->GetEditable()->m_desktopBackdrop != (m_camMode == ViewMode::DesktopBackdrop)))
         continue;
      if (edit == m_outlinerAnchor)
         anchorPos = pos;
      if (edit == part)
         partPos = pos;
      pos++;
   }
   if (anchorPos < 0 || partPos < 0)
   {
      SetSelection(Selection(part));
      return;
   }
   if (partPos < anchorPos)
      std::swap(anchorPos, partPos);
   m_multiSel.clear();
   pos = 0;
   for (const auto &edit : m_editables)
   {
      if (edit->GetEditable()->GetItemType() == eItemPartGroup || (edit->GetEditable()->m_desktopBackdrop != (m_camMode == ViewMode::DesktopBackdrop)))
         continue;
      if (pos >= anchorPos && pos <= partPos)
         m_multiSel.push_back(edit);
      pos++;
   }
   m_selection = Selection(part);
}

void EditorUI::SelectPartsInGroup(const PartGroup *group)
{
   // Select all the parts contained in the given group (and in its sub groups, recursively)
   m_multiSel.clear();
   for (const auto &uiPart : m_editables)
   {
      IEditable *const editable = uiPart->GetEditable();
      if (editable->GetItemType() != eItemPartGroup && editable->IsChild(group) && (editable->m_desktopBackdrop == (m_camMode == ViewMode::DesktopBackdrop)))
         m_multiSel.push_back(uiPart);
   }
   m_selection = m_multiSel.empty() ? Selection() : Selection(m_multiSel.back());
   m_outlinerAnchor.reset();
}

void EditorUI::MoveSelectionToPartGroup(PartGroup *group)
{
   // Gather the parts that can be moved to the target group: a part can not be moved to itself, and a
   // group can not be moved to itself or one of its descendants (this would create a cycle). Only part
   // groups may be moved to the root of the hierarchy (group-less parts are live objects).
   vector<IEditable *> moved;
   for (const auto &uiPart : m_multiSel)
   {
      IEditable *const editable = uiPart->GetEditable();
      if (editable == nullptr || editable == group || editable->GetPartGroup() == group)
         continue;
      if (editable->GetItemType() == eItemPartGroup && group != nullptr && group->IsChild(static_cast<PartGroup *>(editable)))
         continue;
      if (group == nullptr && editable->GetItemType() != eItemPartGroup)
         continue;
      moved.push_back(editable);
   }
   if (moved.empty())
      return;
   m_undo.BeginUndo();
   for (IEditable *const editable : moved)
   {
      m_undo.MarkForUndo(editable);
      editable->SetPartGroup(group);
   }
   m_undo.EndUndo();
   UpdateEditableList(); // Refresh the outliner paths and ordering
}

void EditorUI::RayCastParts(const ImVec2 &mousePos, vector<HitTestResult> &vhoHit) const
{
   // Compute mouse position in clip space
   const float rClipWidth = (float)m_player->m_playfieldWnd->GetWidth() * 0.5f;
   const float rClipHeight = (float)m_player->m_playfieldWnd->GetHeight() * 0.5f;
   const float xcoord = (mousePos.x - rClipWidth) / rClipWidth;
   const float ycoord = (rClipHeight - mousePos.y) / rClipHeight;

   // Use the inverse of our 3D transform to determine where in 3D space the
   // screen pixel the user clicked on is at.  Get the point at the near
   // clipping plane (z=0) and the far clipping plane (z=1) to get the whole
   // range we need to hit test
   Matrix3D invMVP = m_renderer->GetMVP().GetModelViewProj(0);
   invMVP.Invert();
   const Vertex3Ds v3d = invMVP * Vertex3Ds { xcoord, ycoord, 0.f };
   const Vertex3Ds v3d2 = invMVP * Vertex3Ds { xcoord, ycoord, 1.f };

   // FIXME This is not really great as:
   // - picking depends on what was visible/enabled when quadtree was built (lazily at first pick), and also uses the physics quadtree for some parts
   // - primitives can have hit bug (Apron Top and Gottlieb arm of default table for example): degenerated geometry ?
   // We would need a dedicated quadtree for UI with all parts, and filter after picking by visibility
   vector<HitTestResult> vhoUnfilteredHit;
   m_player->m_physics->RayCast(v3d, v3d2, true, vhoUnfilteredHit);

   // Filter out the colliders that should not be picked
   for (const auto &hr : vhoUnfilteredHit)
   {
      const auto editable = hr.m_obj->m_editable;
      if (editable && !IsEditablePickable(editable))
         continue;
      vhoHit.push_back(hr);
   }
}

bool EditorUI::IsEditablePickable(const IEditable *editable) const
{
   // In desktop backdrop mode, only backdrop parts can be picked, and conversely in the other view modes
   if (editable->m_desktopBackdrop != (m_camMode == ViewMode::DesktopBackdrop))
      return false;
   const PartGroup *parent = editable->GetPartGroup();
   bool visible = editable->IsUIVisible(false);
   while (parent && visible)
   {
      if ((parent->GetPlayerModeVisibilityMask() & m_renderer->GetPlayerModeVisibilityMask()) == 0)
         visible = false;
      visible &= parent->IsUIVisible(false);
      parent = parent->GetPartGroup();
   }
   if (!visible)
      return false;
   const auto type = editable->GetItemType();
   if (!HasFlag(m_selectionFilter, SelectionFilter::Playfield) && type == ItemTypeEnum::eItemPrimitive && static_cast<const Primitive *>(editable)->IsPlayfield())
      return false;
   if (!HasFlag(m_selectionFilter, SelectionFilter::Primitives) && type == ItemTypeEnum::eItemPrimitive)
      return false;
   if (!HasFlag(m_selectionFilter, SelectionFilter::Lights) && type == ItemTypeEnum::eItemLight)
      return false;
   if (!HasFlag(m_selectionFilter, SelectionFilter::Flashers) && type == ItemTypeEnum::eItemFlasher)
      return false;
   return true;
}

void EditorUI::SelectAllParts()
{
   // Select all the pickable parts
   m_multiSel.clear();
   for (const auto &uiPart : m_editables)
   {
      IEditable *const editable = uiPart->GetEditable();
      if (editable != nullptr && editable->GetItemType() != eItemPartGroup && IsEditablePickable(editable))
         m_multiSel.push_back(uiPart);
   }
   m_selection = m_multiSel.empty() ? Selection() : Selection(m_multiSel.back());
}

void EditorUI::BoxSelectParts(const ImVec2 &cornerA, const ImVec2 &cornerB, bool add)
{
   // Select all the pickable parts whose transform position projects inside the given screen box
   if (!add)
   {
      m_multiSel.clear();
      m_selection = Selection();
   }
   const LiveRenderContext ctx(m_player, nullptr, m_camMode, m_shadeMode, false);
   const ImVec2 boxMin(std::min(cornerA.x, cornerB.x), std::min(cornerA.y, cornerB.y));
   const ImVec2 boxMax(std::max(cornerA.x, cornerB.x), std::max(cornerA.y, cornerB.y));
   for (const auto &uiPart : m_editables)
   {
      IEditable *const editable = uiPart->GetEditable();
      if (editable == nullptr || editable->GetItemType() == eItemPartGroup || !IsEditablePickable(editable))
         continue;
      Matrix3D transform;
      if (uiPart->GetTransform(transform) == EditorUIPart::TM_None)
         continue;
      const ImVec2 pos = ctx.Project(transform.GetOrthoNormalPos());
      if (pos.x >= boxMin.x && pos.x <= boxMax.x && pos.y >= boxMin.y && pos.y <= boxMax.y && !IsPartSelected(uiPart))
         m_multiSel.push_back(uiPart);
   }
   if (!m_multiSel.empty() && (m_selection.GetType() != Selection::S_EDITABLE || !IsPartSelected(m_selection.GetPart())))
      m_selection = Selection(m_multiSel.back());
}

void EditorUI::CreatePart(const ItemTypeEnum type, const Vertex2D &pos)
{
   IEditable *const pie = EditableRegistry::CreateAndInit(type, m_table, pos.x, pos.y);
   if (pie == nullptr)
      return;

   // Same initialization sequence as the WinUI editor
   if (auto *const scriptable = pie->GetIScriptable(); scriptable)
      m_table->GetUniqueName(type, scriptable->m_name);
   pie->m_desktopBackdrop = (m_camMode == ViewMode::DesktopBackdrop);
   m_table->AddPart(pie);
   pie->SetPartGroup(GetPartGroupForNewPart());

   // Same player side setup as player initialization
   if (type == eItemBall)
      m_player->m_vball.push_back(static_cast<Ball *>(pie));
   m_player->TimerSetup(pie);
   if (auto *const renderable = pie->GetIRenderable(); renderable)
      renderable->RenderSetup(m_renderer.get());
   if (pie->GetIHitable())
      m_player->m_physics->Add(pie);

   m_undo.BeginUndo();
   m_undo.MarkForCreate(pie);
   m_undo.EndUndo();

   // Create the UI part now (it would otherwise be lazily added on next frame) and select the new part
   UpdateEditableList();
   if (const auto it = m_editableMap.find(pie); it != m_editableMap.end())
   {
      SetSelection(Selection(it->second));
      m_outlinerAnchor = it->second;
   }
}

void EditorUI::CreateCollection(bool fromSelection)
{
   // Same initialization sequence as the WinUI editor (PinTableWnd::NewCollection)
   CComObject<Collection> *pcol;
   CComObject<Collection>::CreateInstance(&pcol);
   pcol->AddRef();
   pcol->m_name = m_table->GetUniqueName("Collection"s);
   if (fromSelection)
      for (const auto &uiPart : m_multiSel)
      {
         IEditable *const editable = uiPart->GetEditable();
         if (editable && editable->GetIScriptable()) // Collections only reference scriptable parts (persisted by name)
            m_table->AddPartToCollection(pcol, editable);
      }
   m_table->AddCollection(pcol);
   pcol->Release();
   m_table->SetNonUndoableDirty(SaveDirtyState::Dirty);
   SetSelection(Selection(pcol));
}

PartGroup *EditorUI::GetPartGroupForNewPart()
{
   // Assign new parts to the group of the current selection. When the selection does not define a
   // group, favor the most recently used ones (still respecting UI visibility), then fall back to
   // the visibility mask (first UI visible root group, else first root group). The target is only
   // empty when the table does not have any part group at all.
   PartGroup *partGroup = nullptr;
   if (const std::shared_ptr<EditorUIPart> &part = m_selection.GetPart())
      partGroup = part->GetEditable()->GetItemType() == eItemPartGroup ? static_cast<PartGroup *>(part->GetEditable()) : part->GetEditable()->GetPartGroup();
   if (partGroup == nullptr)
   {
      const vector<IEditable *> &parts = m_table->GetParts();
      std::erase_if(m_partGroupUseHistory, [&parts](const PartGroup *group) { return std::ranges::find(parts, group) == parts.end(); });
      for (PartGroup *const group : m_partGroupUseHistory)
         if (group->IsUIVisible(true))
         {
            partGroup = group;
            break;
         }
   }
   if (partGroup == nullptr)
   {
      for (IEditable *const part : m_table->GetParts())
      {
         if (part->GetItemType() == eItemPartGroup && part->GetPartGroup() == nullptr)
         {
            if (part->IsUIVisible(false))
            {
               partGroup = static_cast<PartGroup *>(part);
               break;
            }
            if (partGroup == nullptr)
               partGroup = static_cast<PartGroup *>(part);
         }
      }
   }
   if (partGroup != nullptr)
   {
      std::erase(m_partGroupUseHistory, partGroup);
      m_partGroupUseHistory.insert(m_partGroupUseHistory.begin(), partGroup);
   }
   return partGroup;
}

void EditorUI::CopySelection()
{
   if (m_pointEditPart)
   {
      // Copy the coordinates of the selected drag point or center point
      if (m_pointEditCenter)
      {
         if (m_centerSelected)
            VPX::EditorClipboard::CopyPoint(m_pointEditPart->GetEditCenter());
      }
      else if (m_pointSel.size() == 1)
         VPX::EditorClipboard::CopyPoint(m_pointSel.front()->GetVertex());
      return;
   }
   vector<IEditable *> parts;
   for (const auto &part : m_multiSel)
      if (IEditable *const editable = part->GetEditable(); editable != nullptr)
         parts.push_back(editable);
   RestorePartsVisibility(); // Copy the authored visibility, not the editor display state
   VPX::EditorClipboard::CopyParts(parts);
}

void EditorUI::PasteSelection(const ImVec2 &pos)
{
   if (m_table->IsLocked() || IsInspectMode())
      return;
   if (m_pointEditPart)
   {
      // Paste the copied coordinates to the selected drag point or center point
      Vertex3Ds pointPos;
      if (m_pointEditCenter)
      {
         if (m_centerSelected && VPX::EditorClipboard::GetPoint(pointPos))
         {
            BeginPointEdit();
            m_pointEditPart->SetEditCenter(Vertex2D(pointPos.x, pointPos.y));
            EndPointEdit();
         }
      }
      else if (m_pointSel.size() == 1 && VPX::EditorClipboard::GetPoint(pointPos))
      {
         BeginPointEdit();
         m_pointSel.front()->SetX(pointPos.x);
         m_pointSel.front()->SetY(pointPos.y);
         m_pointSel.front()->SetZ(pointPos.z);
         EndPointEdit();
      }
      return;
   }
   const vector<vector<uint8_t>> parts = VPX::EditorClipboard::GetParts();
   if (parts.empty())
      return;
   const bool backdrop = (m_camMode == ViewMode::DesktopBackdrop);
   PartGroup *const partGroup = GetPartGroupForNewPart();
   vector<IEditable *> pasted;
   Vertex2D pasteMin(FLT_MAX, FLT_MAX), pasteMax(-FLT_MAX, -FLT_MAX);
   m_undo.BeginUndo();
   for (const vector<uint8_t> &partData : parts)
   {
      if (partData.size() <= sizeof(int))
         continue;
      const ItemTypeEnum type = *reinterpret_cast<const ItemTypeEnum *>(partData.data());
      IEditable *const editable = EditableRegistry::Create(type);
      if (editable == nullptr)
         continue;
      BiffReader reader(partData.data() + sizeof(int), static_cast<uint32_t>(partData.size() - sizeof(int)), CURRENT_FILE_FORMAT_VERSION, nullptr, 0);
      editable->Load(reader);
      editable->m_desktopBackdrop = backdrop;
      // If the original name is not yet used, use that one, otherwise add/increase the suffix until we find a name that's not used yet
      if (!m_table->IsNameUnique(editable->GetName()))
      {
         // First remove the existing suffix
         const string input = editable->GetName();
         size_t lastNonDigit = input.length();
         while (lastNonDigit > 0 && input[lastNonDigit - 1] >= '0' && input[lastNonDigit - 1] <= '9')
            --lastNonDigit;
         editable->SetName(m_table->GetUniqueName(input.substr(0, lastNonDigit)));
      }
      editable->SetPartGroup(partGroup);
      m_table->AddPart(editable);
      m_undo.MarkForCreate(editable);
      pasted.push_back(editable);
      const Vertex2D center = editable->GetCenter();
      pasteMin.x = min(pasteMin.x, center.x);
      pasteMin.y = min(pasteMin.y, center.y);
      pasteMax.x = max(pasteMax.x, center.x);
      pasteMax.y = max(pasteMax.y, center.y);
   }
   m_undo.EndUndo();
   if (pasted.empty())
   {
      m_undo.Discard();
      return;
   }
   // Move the pasted parts so that their combined center lands at the given position (in the table XY plane, keeping their Z)
   const Vertex2D target = UnprojectToPlane(pos, 0.f);
   const Vertex2D offset(target.x - 0.5f * (pasteMin.x + pasteMax.x), target.y - 0.5f * (pasteMin.y + pasteMax.y));
   for (IEditable *const editable : pasted)
   {
      editable->Translate(offset);
      // Player side setup, same as part creation
      if (editable->GetItemType() == eItemBall)
         m_player->m_vball.push_back(static_cast<Ball *>(editable));
      m_player->TimerSetup(editable);
      if (auto *const renderable = editable->GetIRenderable(); renderable)
         renderable->RenderSetup(m_renderer.get());
      if (editable->GetIHitable())
         m_player->m_physics->Add(editable);
   }
   // Select the pasted parts
   UpdateEditableList();
   m_multiSel.clear();
   for (IEditable *const editable : pasted)
      if (const auto it = m_editableMap.find(editable); it != m_editableMap.end())
         m_multiSel.push_back(it->second);
   m_selection = m_multiSel.empty() ? Selection() : Selection(m_multiSel.back());
   m_outlinerAnchor = m_multiSel.empty() ? nullptr : m_multiSel.back();
}

void EditorUI::DeleteSelection()
{
   if (m_table->IsLocked() || IsInspectMode())
      return;
   if (m_pointEditPart) // Toolbar delete while editing drag points: delete the points, the edited part must stay alive
   {
      DeleteSelectedPoints();
      return;
   }
   // Balls are runtime objects, parts without a part group are live objects or root groups: they can not be deleted (and stay selected)
   vector<IEditable *> partsToDelete;
   for (const auto &part : m_multiSel)
      if (IEditable *const edit = part->GetEditable(); edit->GetItemType() != eItemBall && edit->GetPartGroup() != nullptr && std::ranges::find(partsToDelete, edit) == partsToDelete.end())
         partsToDelete.push_back(edit);
   // Delete group content, nested groups included (partsToDelete grows while iterating)
   for (size_t i = 0; i < partsToDelete.size(); i++)
      if (partsToDelete[i]->GetItemType() == eItemPartGroup)
         for (IEditable *const edit : m_table->GetParts())
            if (edit->GetPartGroup() == partsToDelete[i] && edit->GetItemType() != eItemBall && std::ranges::find(partsToDelete, edit) == partsToDelete.end())
               partsToDelete.push_back(edit);
   if (partsToDelete.empty())
      return;

   // Begun before the selection changes, so that undoing restores the deleted parts as selected
   m_undo.BeginUndo();
   for (IEditable *const edit : partsToDelete)
   {
      if (const auto it = m_editableMap.find(edit); it != m_editableMap.end())
      {
         const std::shared_ptr<EditorUIPart> uiPart = it->second;
         uiPart->RestorePartVisibility(); // While the part is alive, so that it is authored when undeleted
         m_editableMap.erase(it);
         RemoveFromVectorSingle(m_editables, uiPart);
      }
      // Release the player side resources (restored by PinUndo::Undo), the render ones at the end of the frame as the render thread may still use them.
      // The release runs on the render thread: an undelete that happens before it must not touch the render state (see PinUndo::Undo)
      if (edit->GetIHitable())
         m_player->m_physics->Remove(edit);
      if (edit->m_phittimer)
         m_player->TimerRelease(edit);
      m_renderer->DropRenderable(edit->GetIRenderable());
      edit->AddRef(); // Keep the part alive until its deferred render release has been processed
      edit->m_renderReleasePending = true;
      m_renderer->m_renderDevice->AddEndOfFrameCmd([edit]()
         {
            if (edit->GetIRenderable())
               edit->GetIRenderable()->RenderRelease();
            edit->m_renderReleasePending = false;
            edit->Release();
         });
      m_undo.MarkForDelete(edit); // The undo record keeps the part alive (until undone or dropped)
      for (Collection *const collection : edit->m_vCollection) // m_vCollection is kept for Undelete
         collection->RemovePart(edit);
      m_table->RemovePart(edit);
   }
   m_undo.EndUndo();
   m_lastUndoPart = nullptr; // Start a new record on next edit (a deleted part's address may be reused)
   m_lastUndoId = 0;

   // Remove the deleted parts from the selection (non deletable parts stay selected)
   std::erase_if(m_multiSel, [this](const auto &part) { return std::ranges::find(m_editables, part) == m_editables.end(); });
   if (m_selection.GetType() == Selection::S_EDITABLE && !IsPartSelected(m_selection.GetPart()))
      m_selection = m_multiSel.empty() ? Selection() : Selection(m_multiSel.back());
   if (m_outlinerAnchor && std::ranges::find(m_editables, m_outlinerAnchor) == m_editables.end())
      m_outlinerAnchor.reset();
}

void EditorUI::RestorePartsVisibility() const
{
   // The editor keeps overriding the parts' m_d visibility fields with the outliner state (see EditableUIPart::Render).
   // Only parts still in the table are known to be alive: UI parts of removed parts are only dropped by UpdateEditableList
   const ankerl::unordered_dense::set<const IEditable *> liveParts(m_table->GetParts().begin(), m_table->GetParts().end());
   for (const auto &uiPart : m_editables)
      if (liveParts.contains(uiPart->GetEditable()))
         uiPart->RestorePartVisibility();
}

void EditorUI::UpdateEditableList()
{
   // In full edit mode, parts without a part group are live objects generated at runtime (implicit
   // playfield mesh, script created parts, ...): they are not part of the table file, so they are
   // not listed in the outliner and can not be selected
   const auto isLiveObject = [this](const IEditable *edit) { return (m_table->m_liveBaseTable == nullptr) && (edit->GetPartGroup() == nullptr) && (edit->GetItemType() != eItemPartGroup); };
   // Remove UI parts of removed editables (or of live objects in full edit mode)
   const ankerl::unordered_dense::set<const IEditable *> liveParts(m_table->GetParts().begin(), m_table->GetParts().end());
   std::erase_if(m_editables,
      [this, &liveParts, &isLiveObject](const auto &uiPart)
      {
         if (!liveParts.contains(uiPart->GetEditable()) || isLiveObject(uiPart->GetEditable()))
         {
            m_editableMap.erase(uiPart->GetEditable());
            return true;
         }
         return false;
      });
   // Drop removed parts from the multi selection, keeping a valid active part
   std::erase_if(m_multiSel, [&liveParts, &isLiveObject](const auto &part) { return !liveParts.contains(part->GetEditable()) || isLiveObject(part->GetEditable()); });
   if (m_selection.GetType() == Selection::S_EDITABLE && !IsPartSelected(m_selection.GetPart()))
      m_selection = m_multiSel.empty() ? Selection() : Selection(m_multiSel.back());
   if (m_outlinerAnchor && (!liveParts.contains(m_outlinerAnchor->GetEditable()) || isLiveObject(m_outlinerAnchor->GetEditable())))
      m_outlinerAnchor.reset();
   // Add UI parts for new editables
   bool needSort = false;
   ankerl::unordered_dense::set<PartGroup *> newGroups;
   for (const auto &edit : m_table->GetParts())
   {
      if (isLiveObject(edit))
         continue;
      const auto it = m_editableMap.find(edit);
      if (it == m_editableMap.end()) // New part
      {
         std::shared_ptr<EditorUIPart> uiPart = EditorUIPartRegistry::Create(edit);
         if (uiPart == nullptr) // eItemTable, eItemLightCenter, eItemDragPoint, eItemCollection
            uiPart = std::make_shared<BaseUIPart>(edit);
         if (m_table->m_liveBaseTable)
            edit->SetUIVisible(true);
         else if (edit->GetItemType() == eItemPartGroup)
            newGroups.insert(static_cast<PartGroup *>(edit));
         uiPart->SetOutlinerPath(edit->GetPathString(false));
         m_editables.push_back(uiPart);
         m_editableMap[edit] = std::move(uiPart);
         needSort = true;
      }
      else if (it->second->GetOutlinerPath() != edit->GetPathString(false)) // Name or parent group has changed
      {
         needSort = true;
         it->second->SetOutlinerPath(edit->GetPathString(false));
      }
   }
   // Win32 UI does not manage PartGroup UI hidden/shown state, so we lazily initialize new groups in a
   // single pass: hidden by default in edit mode, shown if they contain at least one visible part
   if (!newGroups.empty())
   {
      for (PartGroup *group : newGroups)
         group->SetUIVisible(false);
      for (const auto &edit : m_table->GetParts())
         if (edit->GetItemType() != eItemPartGroup && edit->IsUIVisible(false))
            for (PartGroup *group = edit->GetPartGroup(); group != nullptr; group = group->GetPartGroup())
               if (newGroups.contains(group))
                  group->SetUIVisible(true);
   }
   // Sort in tree order for the outliner (root live objects first, then each group directly followed by its content, siblings by name).
   // Paths are compared name by name: comparing the joined path strings would put "Lights GI/..." between "Lights" and "Lights/..."
   // (' ' sorts before '/'), and the outliner would then lose the parts of "Lights"
   if (needSort)
   {
      const auto pathOf = [](const IEditable *edit)
      {
         vector<const string *> path;
         for (const IEditable *part = edit; part != nullptr; part = part->GetPartGroup())
            path.push_back(&part->GetName());
         std::ranges::reverse(path);
         return path;
      };
      vector<std::pair<vector<const string *>, std::shared_ptr<EditorUIPart>>> sorted;
      sorted.reserve(m_editables.size());
      for (auto &uiPart : m_editables)
         sorted.emplace_back(pathOf(uiPart->GetEditable()), std::move(uiPart));
      std::ranges::sort(sorted,
         [](const auto &a, const auto &b)
         {
            const bool isRootA = a.second->GetEditable()->GetPartGroup() == nullptr && a.second->GetEditable()->GetItemType() != eItemPartGroup;
            const bool isRootB = b.second->GetEditable()->GetPartGroup() == nullptr && b.second->GetEditable()->GetItemType() != eItemPartGroup;
            if (isRootA != isRootB)
               return isRootA;
            return std::ranges::lexicographical_compare(a.first, b.first, [](const string *x, const string *y) { return StrLessNoCase(*x, *y); });
         });
      for (size_t i = 0; i < sorted.size(); i++)
         m_editables[i] = std::move(sorted[i].second);
   }
}

void EditorUI::RenderNavCube()
{
   // Edit mode only: not in the 2D desktop backdrop view, nor in inspect (play) mode, nor with the side panels hidden
   if (!m_showNavCube || IsInspectMode() || m_flyMode || m_table->IsLocked() || m_inspectionModal.IsVisible() || m_camMode == ViewMode::DesktopBackdrop)
      return;

   ImGuiIO &io = ImGui::GetIO();
   const float dpi = m_liveUI.GetDPI();
   const float size = 128.f * dpi;
   const ImVec2 pos(io.DisplaySize.x - PropertiesPanel::PaneWidth * dpi - size - 16.f * dpi, m_chrome.GetTopBarHeight() + 8.f * dpi);

   // The cube is computed in a right handed, Y up world, while the editor world is X right, Y down the table, Z down (height is -Z):
   // R (row vector convention) maps editor Z (down) to -Y and editor Y to Z, a pure rotation about X. viewG = R^-1 * viewV, viewV = R * viewG
   static constexpr float R[16] = { 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, -1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f };
   auto mul = [](const float *a, const float *b, float *o)
   {
      for (int i = 0; i < 4; i++)
         for (int j = 0; j < 4; j++)
            o[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
   };
   float Rinv[16];
   for (int i = 0; i < 4; i++)
      for (int j = 0; j < 4; j++)
         Rinv[i * 4 + j] = R[j * 4 + i];

   Matrix3D inv(m_camView);
   inv.Invert();
   const vec3 oldDir = inv.GetOrthoNormalDir();
   const vec3 camTarget = inv.GetOrthoNormalPos() - oldDir * m_camDistance; // Same orbit pivot as the mouse orbit

   float viewG[16], viewGIn[16];
   mul(Rinv, &m_camView.m[0][0], viewG);
   memcpy(viewGIn, viewG, sizeof(viewG));
   // Own right handed projection so that the cube handedness does not depend on the editor (possibly orthographic) projection
   const Matrix3D proj = Matrix3D::MatrixPerspectiveFovRH(m_navFovY, io.DisplaySize.x / io.DisplaySize.y, 1.f, 1000.f);
   const float identity[16] = { 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f };

   // Do not fight the selection gizmo: while it is dragged, the cube gets no mouse input (it is still drawn)
   const ImVec2 savedMousePos = io.MousePos;
   const bool savedMouseDown = io.MouseDown[0];
   const bool gizmoBusy = ImGuizmo::IsUsing();
   if (gizmoBusy)
   {
      io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
      io.MouseDown[0] = false;
   }
   ImGuizmo::ViewManipulate(viewG, proj.m[0], ImGuizmo::TRANSLATE, ImGuizmo::WORLD, const_cast<float *>(identity), m_camDistance, pos, ImVec2(size, size), 0x40101010);
   if (gizmoBusy)
   {
      io.MousePos = savedMousePos;
      io.MouseDown[0] = savedMouseDown;
   }

   if (memcmp(viewG, viewGIn, sizeof(viewG)) == 0)
      return;
   // ImGuizmo's drag orbit builds a look at with a fixed Y up: from an axis view (default Top) the first drag frame has a zero mouse delta, the view direction
   // is parallel to up and the look at degenerates (zero or NaN basis), which would poison the editor camera for good. Skip such frames, the next ones recover.
   for (int axis = 0; axis < 3; axis++)
   {
      const float l2 = viewG[axis] * viewG[axis] + viewG[4 + axis] * viewG[4 + axis] + viewG[8 + axis] * viewG[8 + axis];
      if (!(l2 > 0.5f && l2 < 2.f)) // also rejects NaN
         return;
   }

   // Back to the editor camera: keep the pivot and distance, take the new direction and up
   float viewV[16];
   mul(R, viewG, viewV);
   Matrix3D newInv;
   memcpy(&newInv.m[0][0], viewV, sizeof(viewV));
   newInv.Invert();
   vec3 newDir = newInv.GetOrthoNormalDir();
   vec3 newUp = newInv.GetOrthoNormalUp();
   m_predefinedView = PredefinedView::None;

   if (!io.MouseDown[0]) // Face click: the cube animates the view toward an axis; use the editor predefined views up vectors and an orthographic camera
   {
      struct Axis
      {
         vec3 dir, up;
         PredefinedView view;
      };
      const Axis axes[6] = { { vec3(0.f, 0.f, -1.f), vec3(0.f, -1.f, 0.f), PredefinedView::Top }, { vec3(0.f, 0.f, 1.f), vec3(0.f, -1.f, 0.f), PredefinedView::Bottom },
         { vec3(0.f, 1.f, 0.f), vec3(0.f, 0.f, -1.f), PredefinedView::Front }, { vec3(0.f, -1.f, 0.f), vec3(0.f, 0.f, -1.f), PredefinedView::Back },
         { vec3(-1.f, 0.f, 0.f), vec3(0.f, 0.f, -1.f), PredefinedView::Left }, { vec3(1.f, 0.f, 0.f), vec3(0.f, 0.f, -1.f), PredefinedView::Right } };
      m_perspectiveCam = false;
      for (const Axis &a : axes)
         if (newDir.Dot(a.dir) > 0.9999f)
         {
            newDir = a.dir;
            newUp = a.up;
            m_predefinedView = a.view;
            break;
         }
   }
   m_camMode = ViewMode::EditorCam;
   m_camView = Matrix3D::MatrixLookAtRH(camTarget + newDir * m_camDistance, camTarget, newUp);
}

void EditorUI::FrameBounds(const FRect3D &bounds)
{
   const ImGuiIO &io = ImGui::GetIO();
   m_camMode = ViewMode::EditorCam;
   Matrix3D view(m_camView);
   view.Invert();
   const vec3 up = view.GetOrthoNormalUp();
   const vec3 dir = view.GetOrthoNormalDir();
   const vec3 right = view.GetOrthoNormalRight();
   const vec3 newTarget(0.5f * (bounds.left + bounds.right), 0.5f * (bounds.top + bounds.bottom), -0.5f * (bounds.zlow + bounds.zhigh));

   // Evaluate the distance needed for all the bounds corners to be inside the view, keeping the camera orientation
   // and fitting inside the area left visible by the UI (a symmetric margin accounts for the UI drawn over the render)
   const float aspect = io.DisplaySize.x / io.DisplaySize.y;
   const Vertex2D visible = GetUIVisibleFraction();
   const float tanHalfFovY = tanf(0.5f * ANGTORAD(m_navFovY));
   float distance = 0.f;
   for (int i = 0; i < 8; i++)
   {
      const vec3 toCorner = vec3((i & 1) ? bounds.right : bounds.left, (i & 2) ? bounds.bottom : bounds.top, (i & 4) ? -bounds.zlow : -bounds.zhigh) - newTarget;
      const float dx = fabsf(right.Dot(toCorner));
      const float dy = fabsf(up.Dot(toCorner));
      distance = max(
         distance, m_perspectiveCam ? dir.Dot(toCorner) + max(dy / (tanHalfFovY * visible.y), dx / (tanHalfFovY * aspect * visible.x)) : max(dy / visible.y, dx / (aspect * visible.x)));
   }
   if (distance > 0.f) // Keep the current distance for degenerate (point) selections
      m_camDistance = distance * 1.1f; // Small margin so that the selection does not exactly touch the view borders
   const vec3 newEye = newTarget + dir * m_camDistance;
   m_camView = Matrix3D::MatrixLookAtRH(newEye, newTarget, up);
}

// Numpad key, or the matching top row digit when numpad emulation is enabled (laptops without a numpad)
bool EditorUI::IsNumpadPressed(int digit) const
{
   if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_Keypad0 + digit)))
      return true;
   return IsDigitNavKey(digit) && ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_0 + digit));
}

// The digits are game keys (start, coin...) while the game runs and text for widgets: emulate only with the 3D view focused and the game not running (edit mode, or inspect mode while paused)
bool EditorUI::IsDigitNavKey(int digit) const
{
   const ImGuiIO &io = ImGui::GetIO();
   if (digit < 0 || digit > 9 || !m_navEmulateNumpad || (IsInspectMode() && m_player->IsGameRunning()) || io.WantTextInput || io.WantCaptureKeyboard)
      return false;
   return !io.KeyAlt;
}

namespace
{
   // The editor camera works in a right handed space where Z is the opposite of the table's one
   inline vec3 FlipZ(const vec3 &v) { return vec3(v.x, v.y, -v.z); }
   inline vec3 RotateAround(const vec3 &v, const vec3 &k, const float angle) { return v * cosf(angle) + CrossProduct(k, v) * sinf(angle) + k * (k.Dot(v) * (1.f - cosf(angle))); }
}

void EditorUI::BeginModalTransform(ModalOp op)
{
   Matrix3D initial;
   if (m_table->IsLocked() || m_modal.active || !GetSelectionTransform(initial))
      return;
   if (m_camMode == ViewMode::PreviewCam)
      m_camMode = ViewMode::EditorCam;
   const ImGuizmo::OPERATION savedGizmo = m_gizmoOperation;
   m_gizmoOperation = ImGuizmo::OPERATION(0);
   m_modal = ModalTransform();
   m_modal.savedGizmo = savedGizmo;
   m_modal.active = true;
   m_modal.op = op;
   m_modal.initial = initial;
   m_modal.startMouse = ImGui::GetMousePos();
   // Same undo record as a gizmo drag: one entry for the whole operation
   if (m_table->m_liveBaseTable == nullptr)
   {
      m_undo.BeginUndo();
      for (const auto &part : m_multiSel)
         m_undo.MarkForUndo(part->GetEditable());
      m_undo.EndUndo();
      m_modal.hasUndo = true;
   }
   m_lastUndoPart = nullptr;
   m_lastUndoId = 0;
   UpdateModalTransform(); // Initialize the start angle/distance and the status text
}

void EditorUI::EndModalTransform(const bool confirm)
{
   if (!m_modal.active)
      return;
   if (!confirm)
   {
      if (m_modal.hasUndo)
      {
         const std::any state = m_undo.Undo();
         if (state.has_value())
            RestoreUndoSelection(std::any_cast<const UndoSelectionState &>(state));
      }
      else
         SetSelectionTransform(m_modal.initial);
   }
   m_modal.active = false;
   m_gizmoOperation = m_modal.savedGizmo; // The gizmo comes back after the transform
   m_lastUndoPart = nullptr;
   m_lastUndoId = 0;
}

void EditorUI::UpdateModalTransform()
{
   const ImGuiIO &io = ImGui::GetIO();
   const bool first = m_modal.hud.empty();
   Matrix3D current;
   if (!GetSelectionTransform(current) || m_table->IsLocked())
   {
      EndModalTransform(false);
      return;
   }
   if (!first)
   {
      if (ImGui::IsKeyReleased(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))
      {
         EndModalTransform(false);
         return;
      }
      if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) || ImGui::IsMouseClicked(ImGuiMouseButton_Left))
      {
         EndModalTransform(true);
         return;
      }
      // Axis constraint (pressing the same axis again removes it), Shift constrains to the plane instead
      const std::pair<ImGuiKey, int> axisKeys[] = { { ImGuiKey_X, 0 }, { ImGuiKey_Y, 1 }, { ImGuiKey_Z, 2 } };
      for (const auto &[key, axis] : axisKeys)
         if (ImGui::IsKeyPressed(key, false))
         {
            const bool same = m_modal.axis == axis && m_modal.plane == io.KeyShift;
            m_modal.axis = same ? -1 : axis;
            m_modal.plane = !same && io.KeyShift;
         }
      // Numeric input
      for (int i = 0; i < io.InputQueueCharacters.Size; i++)
      {
         const ImWchar c = io.InputQueueCharacters[i];
         if ((c >= '0' && c <= '9') || c == '.')
            m_modal.typed += static_cast<char>(c);
         else if (c == '-')
         {
            if (!m_modal.typed.empty() && m_modal.typed[0] == '-')
               m_modal.typed.erase(0, 1);
            else
               m_modal.typed.insert(0, "-");
         }
      }
      if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !m_modal.typed.empty())
         m_modal.typed.pop_back();
   }
   const bool hasValue = !m_modal.typed.empty() && m_modal.typed != "-" && m_modal.typed != ".";
   const float value = hasValue ? static_cast<float>(atof(m_modal.typed.c_str())) : 0.f;

   // Camera basis & projection (in the camera's space)
   Matrix3D view(m_camView);
   view.Invert();
   const vec3 right = view.GetOrthoNormalRight(), up = view.GetOrthoNormalUp(), dir = view.GetOrthoNormalDir(), eye = view.GetOrthoNormalPos();
   const Matrix3D &m0 = m_modal.initial;
   const vec3 pivot = FlipZ(vec3(m0._41, m0._42, m0._43));
   const bool perspective = m_perspectiveCam && m_camMode != ViewMode::DesktopBackdrop;
   const float depth = max((eye - pivot).Dot(dir), 1e-3f);
   const float halfH = perspective ? depth * tanf(0.5f * ANGTORAD(m_navFovY)) : m_camDistance;
   const float aspect = io.DisplaySize.x / io.DisplaySize.y;
   const ImVec2 screenPivot(0.5f * io.DisplaySize.x * (1.f + (pivot - eye).Dot(right) / (halfH * aspect)), 0.5f * io.DisplaySize.y * (1.f - (pivot - eye).Dot(up) / halfH));
   const ImVec2 mouse = ImGui::GetMousePos();
   const float curAngle = atan2f(mouse.y - screenPivot.y, mouse.x - screenPivot.x);
   const float curDist = hypotf(mouse.x - screenPivot.x, mouse.y - screenPivot.y);
   if (first)
   {
      m_modal.startAngle = curAngle;
      m_modal.startDist = max(curDist, 8.f);
   }

   Matrix3D result(m0);
   const char *axisNames[] = { "X", "Y", "Z" };
   const vec3 axisWorld[] = { vec3(1.f, 0.f, 0.f), vec3(0.f, 1.f, 0.f), vec3(0.f, 0.f, 1.f) };
   const string axisText = m_modal.axis < 0 ? "" : string(m_modal.plane ? " -" : " ") + axisNames[m_modal.axis];
   char text[128];
   switch (m_modal.op)
   {
   case ModalOp::Grab:
   {
      const float unitsPerPixel = 2.f * halfH / io.DisplaySize.y;
      vec3 move = right * ((mouse.x - m_modal.startMouse.x) * unitsPerPixel) - up * ((mouse.y - m_modal.startMouse.y) * unitsPerPixel);
      if (hasValue)
         move = FlipZ(axisWorld[m_modal.axis < 0 ? 0 : m_modal.axis]) * value;
      else if (m_modal.axis >= 0)
      {
         const vec3 a = FlipZ(axisWorld[m_modal.axis]);
         move = m_modal.plane ? move - a * move.Dot(a) : a * move.Dot(a);
      }
      const vec3 delta = FlipZ(move);
      result._41 += delta.x;
      result._42 += delta.y;
      result._43 += delta.z;
      snprintf(text, sizeof(text), "Grab%s  %.2f  %.2f  %.2f%s%s", axisText.c_str(), delta.x, delta.y, delta.z, hasValue ? "   [" : "", hasValue ? (m_modal.typed + "]").c_str() : "");
      break;
   }
   case ModalOp::Rotate:
   {
      const vec3 k = m_modal.axis < 0 ? dir : FlipZ(axisWorld[m_modal.axis]);
      const float facing = k.Dot(dir) >= 0.f ? 1.f : -1.f;
      const float angle = hasValue ? ANGTORAD(value) : -(curAngle - m_modal.startAngle) * facing;
      const vec3 rows[3] = { vec3(m0._11, m0._12, m0._13), vec3(m0._21, m0._22, m0._23), vec3(m0._31, m0._32, m0._33) };
      float *const out[3] = { &result._11, &result._21, &result._31 };
      for (int i = 0; i < 3; i++)
      {
         const vec3 r = FlipZ(RotateAround(FlipZ(rows[i]), k, angle));
         out[i][0] = r.x;
         out[i][1] = r.y;
         out[i][2] = r.z;
      }
      snprintf(text, sizeof(text), "Rotate%s  %.1f deg%s", axisText.c_str(), RADTOANG(angle), hasValue ? "   [typed]" : "");
      break;
   }
   case ModalOp::Scale:
   {
      const float factor = hasValue ? value : curDist / m_modal.startDist;
      float *const out[3] = { &result._11, &result._21, &result._31 };
      for (int i = 0; i < 3; i++)
         if (m_modal.axis < 0 || (m_modal.plane ? i != m_modal.axis : i == m_modal.axis)) // Axes are the part's local ones
            for (int j = 0; j < 3; j++)
               out[i][j] *= factor;
      snprintf(text, sizeof(text), "Scale%s  %.3f%s", axisText.c_str(), factor, hasValue ? "   [typed]" : "");
      break;
   }
   }
   m_modal.hud = text;
   SetSelectionTransform(result);
}

// Cycle the camera: preview camera, editor camera, desktop backdrop
void EditorUI::CycleCamera()
{
   m_predefinedView = PredefinedView::None;
   switch (m_camMode)
   {
   case ViewMode::PreviewCam:
      m_camMode = ViewMode::EditorCam;
      ResetCameraFromPlayer();
      break;
   case ViewMode::EditorCam:
      m_camMode = ViewMode::DesktopBackdrop;
      SetBackdropCamera();
      break;
   case ViewMode::DesktopBackdrop: m_camMode = ViewMode::PreviewCam; break;
   }
}

// Editor Camera center on the whole selection, adjusting the zoom so that it fills the view
void EditorUI::FrameSelection()
{
   FRect3D bounds;
   if (GetSelectionBounds(bounds))
      FrameBounds(bounds);
}

void EditorUI::FrameAll()
{
   FRect3D bounds(0.f, m_table->m_right, 0.f, m_table->m_bottom, 0.f, m_table->m_glassTopHeight);
   FrameBounds(bounds);
}

// Editor Camera to an axis aligned view, keeping the orbit target and distance
// Change the vertical field of view keeping the framing of the orbit target (the distance is adjusted so the view does not jump)
// 3D point (camera space) under the cursor: the front-most pickable part hit by the mouse ray, else the playfield plane
bool EditorUI::PickDepthPoint(const ImVec2 &mousePos, vec3 &point) const
{
   const float rClipWidth = (float)m_player->m_playfieldWnd->GetWidth() * 0.5f;
   const float rClipHeight = (float)m_player->m_playfieldWnd->GetHeight() * 0.5f;
   Matrix3D invMVP = m_renderer->GetMVP().GetModelViewProj(0);
   invMVP.Invert();
   const Vertex3Ds a = invMVP * Vertex3Ds { (mousePos.x - rClipWidth) / rClipWidth, (rClipHeight - mousePos.y) / rClipHeight, 0.f };
   const Vertex3Ds b = invMVP * Vertex3Ds { (mousePos.x - rClipWidth) / rClipWidth, (rClipHeight - mousePos.y) / rClipHeight, 1.f };
   float t = -1.f;
   vector<HitTestResult> hits;
   RayCastParts(mousePos, hits);
   if (!hits.empty())
      t = hits.front().m_time; // Sorted by distance from the viewer
   else if (fabsf(b.z - a.z) > 1e-10f)
      t = (0.f - a.z) / (b.z - a.z);
   if (!(t >= 0.f && t <= 1.f))
      return false;
   point = FlipZ(vec3(a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z)));
   return true;
}

void EditorUI::SetFov(const float fovY)
{
   const float newFov = clamp(fovY, 15.f, 100.f);
   if (m_perspectiveCam && m_camMode == ViewMode::EditorCam)
   {
      Matrix3D inv(m_camView);
      inv.Invert();
      const vec3 dir = inv.GetOrthoNormalDir(), up = inv.GetOrthoNormalUp();
      const vec3 camTarget = inv.GetOrthoNormalPos() - dir * m_camDistance;
      m_camDistance *= tanf(0.5f * ANGTORAD(m_navFovY)) / tanf(0.5f * ANGTORAD(newFov));
      m_camView = Matrix3D::MatrixLookAtRH(camTarget + dir * m_camDistance, camTarget, up);
   }
   m_navFovY = newFov;
}

void EditorUI::SetView(const PredefinedView view)
{
   m_predefinedView = view;
   m_camMode = ViewMode::EditorCam;
   Matrix3D inv(m_camView);
   inv.Invert();
   const vec3 camTarget = inv.GetOrthoNormalPos() - inv.GetOrthoNormalDir() * m_camDistance;
   vec3 newUp { 0.f, 0.f, -1.f }, newDir;
   switch (view)
   {
   case PredefinedView::Top:
      newUp = vec3(0.f, -1.f, 0.f);
      newDir = vec3(0.f, 0.f, -1.f);
      break;
   case PredefinedView::Bottom:
      newUp = vec3(0.f, -1.f, 0.f);
      newDir = vec3(0.f, 0.f, 1.f);
      break;
   case PredefinedView::Front: newDir = vec3(0.f, 1.f, 0.f); break;
   case PredefinedView::Back: newDir = vec3(0.f, -1.f, 0.f); break;
   case PredefinedView::Left: newDir = vec3(-1.f, 0.f, 0.f); break;
   case PredefinedView::Right: newDir = vec3(1.f, 0.f, 0.f); break;
   default: return;
   }
   m_camView = Matrix3D::MatrixLookAtRH(camTarget + newDir * m_camDistance, camTarget, newUp);
}

// Step the orbit by 15 degrees: around the vertical axis (yaw) or the view's horizontal axis
void EditorUI::OrbitStep(const bool yaw, const float sign)
{
   if (m_camMode == ViewMode::PreviewCam)
      m_camMode = ViewMode::EditorCam;
   m_predefinedView = PredefinedView::None;
   Matrix3D view(m_camView);
   view.Invert();
   const vec3 up = view.GetOrthoNormalUp(), dir = view.GetOrthoNormalDir(), right = view.GetOrthoNormalRight(), pos = view.GetOrthoNormalPos();
   vec3 camTarget = pos - dir * m_camDistance;
   GetNavPivot(camTarget);
   const Matrix3D rot = Matrix3D::MatrixRotate(sign * ANGTORAD(15.f), yaw ? vec3(0.f, 0.f, -1.f) : right);
   m_camMode = ViewMode::EditorCam;
   const vec3 newEye = camTarget + rot.MultiplyVectorNoPerspective(pos - camTarget); // Rigid rotation about the pivot
   m_camView = Matrix3D::MatrixLookAtRH(newEye, newEye - rot.MultiplyVectorNoPerspective(dir) * m_camDistance, rot.MultiplyVectorNoPerspective(up));
}

void EditorUI::SetGizmoOperation(ImGuizmo::OPERATION operation)
{
   if (operation == ImGuizmo::OPERATION(0))
   {
      m_gizmoOperation = operation;
      return;
   }
   if (m_camMode == ViewMode::PreviewCam)
      m_camMode = ViewMode::EditorCam;
   // Repeating the same operation toggles between world and local coordinates, switching to another one resets to world
   m_gizmoMode = m_gizmoOperation == operation ? (m_gizmoMode == ImGuizmo::LOCAL ? ImGuizmo::WORLD : ImGuizmo::LOCAL) : ImGuizmo::WORLD;
   m_gizmoOperation = operation;
}

bool EditorUI::GetSelectionTransform(Matrix3D &transform) const
{
   if (m_pointEditPart)
   {
      // In center edit mode, the gizmo operates on the part's center point
      if (m_pointEditCenter)
      {
         if (!m_centerSelected)
            return false;
         const Vertex3Ds center = m_pointEditPart->GetEditCenter();
         transform = Matrix3D::MatrixTranslate(center.x, center.y, center.z);
         return true;
      }
      // In drag point edit mode, the gizmo operates on the selected points (positioned at their centroid,
      // which stays fixed when rotating or scaling the points around it, unlike the bounding box center)
      if (m_pointSel.empty())
         return false;
      Vertex2D center(0.f, 0.f);
      float z = 0.f;
      for (const DragPoint *point : m_pointSel)
      {
         center += Vertex2D(point->GetX(), point->GetY());
         z += m_pointEditPart->GetDragPointZ(point);
      }
      const float invCount = 1.f / (float)m_pointSel.size();
      transform = Matrix3D::MatrixTranslate(center.x * invCount, center.y * invCount, z * invCount);
      return true;
   }
   if (m_selection.GetType() == Selection::S_EDITABLE)
   {
      const EditorUIPart::TransformMask mask = m_selection.GetPart()->GetTransform(transform);
      return mask != EditorUIPart::TransformMask::TM_None;
   }
   return false;
}

// Navigation pivot: the center of the selection bounds when 'orbit around selection' is enabled and something is selected (target left unchanged otherwise)
bool EditorUI::GetNavPivot(vec3 & target) const
{
   FRect3D bounds;
   if (!m_navOrbitSelection || m_camMode == ViewMode::DesktopBackdrop || !GetSelectionBounds(bounds))
      return false;
   target = vec3(0.5f * (bounds.left + bounds.right), 0.5f * (bounds.top + bounds.bottom), -0.5f * (bounds.zlow + bounds.zhigh));
   return true;
}

bool EditorUI::GetSelectionBounds(FRect3D &bounds) const
{
   bounds.Clear();
   if (m_pointEditPart)
   {
      // In center edit mode, the bounds are the part's center point
      if (m_pointEditCenter)
      {
         if (m_centerSelected)
         {
            const Vertex3Ds center = m_pointEditPart->GetEditCenter();
            bounds.Extend(FRect3D(center.x, center.x, center.y, center.y, center.z, center.z));
         }
      }
      else
         // In drag point edit mode, the bounds are the ones of the selected points
         for (const DragPoint *point : m_pointSel)
         {
            const float z = m_pointEditPart->GetDragPointZ(point);
            bounds.Extend(FRect3D(point->GetX(), point->GetX(), point->GetY(), point->GetY(), z, z));
         }
   }
   else
   {
      const auto extendWithPart = [this, &bounds](const std::shared_ptr<EditorUIPart> &part)
      {
         IEditable *const editable = part->GetEditable();
         if (editable->GetIHitable() != nullptr)
            for (const HitObject *const hitObject : m_player->m_physics->GetUIHitObjects(editable))
               bounds.Extend(hitObject->m_hitBBox);
         // Also include the part's position to cover parts without hit objects
         Matrix3D transform;
         if (part->GetTransform(transform) != EditorUIPart::TM_None)
         {
            const Vertex3Ds pos = transform.GetOrthoNormalPos();
            bounds.Extend(FRect3D(pos.x, pos.x, pos.y, pos.y, pos.z, pos.z));
         }
      };
      for (const auto &part : m_multiSel)
         extendWithPart(part);
      if (m_multiSel.empty() && m_selection.GetPart())
         extendWithPart(m_selection.GetPart());
   }
   return bounds.left <= bounds.right;
}

void EditorUI::SetSelectionTransform(const Matrix3D &newTransform, bool clearPosition, bool clearScale, bool clearRotation) const
{
   if (m_pointEditPart)
   {
      // In point edit mode, apply the gizmo transform delta to the selected points in the table XY plane
      // (to the part's center point in center edit mode)
      if (m_pointEditCenter)
      {
         if (!m_centerSelected)
            return;
         Matrix3D oldTransform;
         GetSelectionTransform(oldTransform);
         Matrix3D invOldTransform(oldTransform);
         invOldTransform.Invert();
         const Matrix3D delta = invOldTransform * newTransform;
         const Vertex3Ds v = delta * m_pointEditPart->GetEditCenter();
         m_pointEditPart->SetEditCenter(Vertex2D(v.x, v.y));
      }
      else
      {
         if (m_pointSel.empty())
            return;
         Matrix3D oldTransform;
         GetSelectionTransform(oldTransform);
         Matrix3D invOldTransform(oldTransform);
         invOldTransform.Invert();
         const Matrix3D delta = invOldTransform * newTransform;
         for (DragPoint *point : m_pointSel)
         {
            const Vertex3Ds v = delta * point->GetVertex();
            point->SetX(v.x);
            point->SetY(v.y);
         }
         m_pointEditPart->GetDragPointCurve()->OnPointsModified();
      }
      m_renderer->ReinitRenderable(m_pointEditPart->GetEditable()->GetIRenderable());
      m_renderer->InvalidateShadowContent();
      m_player->m_physics->Update(m_pointEditPart->GetEditable());
      return;
   }

   if (m_selection.GetType() != Selection::S_EDITABLE)
      return;

   // Decompose a transform into the position/scale/rotation components used by the parts' SetTransform
   const auto extractTRS = [](Matrix3D transform, vec3 &pos, vec3 &scale, vec3 &rot)
   {
      const Vertex3Ds right(transform._11, transform._12, transform._13);
      const Vertex3Ds up(transform._21, transform._22, transform._23);
      const Vertex3Ds dir(transform._31, transform._32, transform._33);
      scale.Set(max(right.Length(), 1e-8f), max(up.Length(), 1e-8f), max(dir.Length(), 1e-8f)); // Clamp to avoid division by zero

      transform._11 /= scale.x; // Normalize transform to evaluate rotation
      transform._12 /= scale.x;
      transform._13 /= scale.x;
      transform._21 /= scale.y;
      transform._22 /= scale.y;
      transform._23 /= scale.y;
      transform._31 /= scale.z;
      transform._32 /= scale.z;
      transform._33 /= scale.z;

      pos.Set(transform._41, transform._42, transform._43);

      // Derived from https://learnopencv.com/rotation-matrix-to-euler-angles/
      const float sy = sqrtf(transform._11 * transform._11 + transform._21 * transform._21);
      if (sy > 1e-6f)
      {
         rot.x = -RADTOANG(atan2f(transform._32, transform._33));
         rot.y = -RADTOANG(atan2f(-transform._31, sy));
         rot.z = -RADTOANG(atan2f(transform._21, transform._11));
      }
      else
      {
         rot.x = -RADTOANG(atan2f(transform._23, transform._22));
         rot.y = -RADTOANG(atan2f(-transform._31, sy));
         rot.z = 0.f;
      }
   };

   const auto applyTransform = [this, &extractTRS](const std::shared_ptr<EditorUIPart> &part, const Matrix3D &partTransform, bool clearPos, bool clearScale, bool clearRot)
   {
      vec3 pos, scale, rot;
      extractTRS(partTransform, pos, scale, rot);
      if (clearPos)
         pos.Set(0.f, 0.f, 0.f);
      if (clearScale)
         scale.Set(1.f, 1.f, 1.f);
      if (clearRot)
         rot.Set(0.f, 0.f, 0.f);
      part->SetTransform(pos, scale, rot);
      m_renderer->ReinitRenderable(part->GetEditable()->GetIRenderable());
      m_renderer->InvalidateShadowContent(); // Cached (static) shadow pages and light maps follow the move
      m_player->m_physics->Update(part->GetEditable());
   };

   if (clearPosition || clearScale || clearRotation)
   {
      // Reset the requested transform components of each selected part
      for (const auto &part : m_multiSel)
      {
         Matrix3D partTransform;
         if (part->GetTransform(partTransform) == EditorUIPart::TM_None)
            continue;
         applyTransform(part, partTransform, clearPosition, clearScale, clearRotation);
      }
      return;
   }

   // Apply the active part's transform delta to every selected part
   Matrix3D delta;
   m_selection.GetPart()->GetTransform(delta);
   delta.Invert();
   delta = delta * newTransform;
   for (const auto &part : m_multiSel)
   {
      Matrix3D partTransform;
      if (part->GetTransform(partTransform) == EditorUIPart::TM_None)
         continue;
      applyTransform(part, partTransform * delta, false, false, false);
   }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
//
// Drag point edit mode: edit the points of the DragPointCurve of the active selected part.
// Entered/exited with Tab (or Escape). The selection is saved on entry and restored on exit.
//

void EditorUI::EnterPointEditMode()
{
   if (m_pointEditPart || IsInspectMode() || m_table->IsLocked())
      return;
   if (m_selection.GetType() != Selection::S_EDITABLE || m_selection.GetPart() == nullptr
      || (m_selection.GetPart()->GetDragPointCurve() == nullptr && !m_selection.GetPart()->HasEditCenter()))
      return;
   m_savedSelection = m_selection;
   m_savedMultiSel = m_multiSel;
   m_savedOutlinerAnchor = m_outlinerAnchor;
   m_pointEditPart = m_selection.GetPart();
   m_pointEditPart->SetPointEditContext(this);
   // Parts without outline points but with an editable center (light sequencer, lights of circle shape) edit it directly
   const DragPointCurve *const curve = m_pointEditPart->GetDragPointCurve();
   m_pointEditCenter = m_pointEditPart->HasEditCenter() && (curve == nullptr || curve->GetPoints().empty());
   m_centerSelected = m_pointEditCenter;
   m_pointSel.clear();
}

void EditorUI::ExitPointEditMode(bool restoreSelection)
{
   if (m_pointEditPart)
      m_pointEditPart->SetPointEditContext(nullptr);
   m_pointEditPart.reset();
   m_pointEditCenter = false;
   m_centerSelected = false;
   m_pointSel.clear();
   m_pointDragPending = false;
   m_pointDragActive = false;
   if (!restoreSelection)
      return;
   // Restore the selection as it was when entering this mode, filtering out parts deleted since then
   const ankerl::unordered_dense::set<const IEditable *> liveParts(m_table->GetParts().begin(), m_table->GetParts().end());
   std::erase_if(m_savedMultiSel, [&liveParts](const std::shared_ptr<EditorUIPart> &part) { return !liveParts.contains(part->GetEditable()); });
   m_multiSel = m_savedMultiSel;
   m_selection = (m_savedSelection.GetType() == Selection::S_EDITABLE && (m_savedSelection.GetPart() == nullptr || !liveParts.contains(m_savedSelection.GetPart()->GetEditable())))
      ? (m_multiSel.empty() ? Selection() : Selection(m_multiSel.back()))
      : m_savedSelection;
   m_outlinerAnchor = (m_savedOutlinerAnchor == nullptr || liveParts.contains(m_savedOutlinerAnchor->GetEditable())) ? m_savedOutlinerAnchor : nullptr;
}

bool EditorUI::IsPointSelected(const DragPoint *point) const { return std::ranges::find(m_pointSel, point) != m_pointSel.end(); }

void EditorUI::TogglePointSelection(DragPoint *point)
{
   const auto it = std::ranges::find(m_pointSel, point);
   if (it == m_pointSel.end())
      m_pointSel.push_back(point);
   else
      m_pointSel.erase(it);
}

void EditorUI::BeginPointEdit()
{
   if (m_pointEditPart == nullptr)
      return;
   m_undo.BeginUndo();
   m_undo.MarkForUndo(m_pointEditPart->GetEditable());
   m_undo.EndUndo();
}

void EditorUI::EndPointEdit()
{
   if (m_pointEditPart == nullptr)
      return;
   if (DragPointCurve *const curve = m_pointEditPart->GetDragPointCurve())
      curve->OnPointsModified();
   m_renderer->ReinitRenderable(m_pointEditPart->GetEditable()->GetIRenderable());
   m_renderer->InvalidateShadowContent();
   m_player->m_physics->Update(m_pointEditPart->GetEditable());
}

void EditorUI::AddPointOnNearestSegment()
{
   DragPointCurve *const curve = (m_pointEditPart != nullptr && !m_table->IsLocked()) ? m_pointEditPart->GetDragPointCurve() : nullptr;
   if (curve == nullptr)
      return;
   const auto &points = curve->GetPoints();
   if (points.empty())
      return;
   // Unproject the mouse position on the drag plane of the edited curve
   const float z = m_pointEditPart->GetDragPointZ(m_pointSel.empty() ? points.front().get() : m_pointSel.front());
   const Vertex2D pos = UnprojectToPlane(ImGui::GetMousePos(), z);
   m_undo.BeginUndo();
   m_undo.MarkForUndo(m_pointEditPart->GetEditable());
   m_undo.EndUndo();
   DragPoint *const point = m_pointEditPart->AddPointOnCurve(pos);
   if (point == nullptr)
   {
      m_undo.Discard();
      return;
   }
   // Select the newly created point
   m_pointSel.clear();
   m_pointSel.push_back(point);
   m_renderer->ReinitRenderable(m_pointEditPart->GetEditable()->GetIRenderable());
   m_renderer->InvalidateShadowContent();
   m_player->m_physics->Update(m_pointEditPart->GetEditable());
}

Vertex2D EditorUI::UnprojectToPlane(const ImVec2 &mousePos, float z) const
{
   // Compute the mouse ray and intersect it with the table plane at the given Z coordinate
   const float rClipWidth = (float)m_player->m_playfieldWnd->GetWidth() * 0.5f;
   const float rClipHeight = (float)m_player->m_playfieldWnd->GetHeight() * 0.5f;
   const float xcoord = (mousePos.x - rClipWidth) / rClipWidth;
   const float ycoord = (rClipHeight - mousePos.y) / rClipHeight;
   Matrix3D invMVP = m_renderer->GetMVP().GetModelViewProj(0);
   invMVP.Invert();
   const Vertex3Ds v3d = invMVP * Vertex3Ds { xcoord, ycoord, 0.f };
   const Vertex3Ds v3d2 = invMVP * Vertex3Ds { xcoord, ycoord, 1.f };
   const float dz = v3d2.z - v3d.z;
   const float t = (fabsf(dz) > 1e-10f) ? (z - v3d.z) / dz : 0.f;
   return Vertex2D(v3d.x + t * (v3d2.x - v3d.x), v3d.y + t * (v3d2.y - v3d.y));
}

DragPoint *EditorUI::HitTestDragPoint(const ImVec2 &mousePos) const
{
   const LiveRenderContext ctx(m_player, nullptr, m_camMode, m_shadeMode, false);
   const float maxDist = 10.f * m_liveUI.GetDPI();
   DragPoint *best = nullptr;
   float bestDist = maxDist;
   for (const auto &point : m_pointEditPart->GetDragPointCurve()->GetPoints())
   {
      const ImVec2 pos = ctx.Project(Vertex3Ds(point->GetX(), point->GetY(), m_pointEditPart->GetDragPointZ(point.get())));
      if (pos.x == FLT_MAX)
         continue;
      const float dx = pos.x - mousePos.x;
      const float dy = pos.y - mousePos.y;
      const float dist = sqrtf(dx * dx + dy * dy);
      if (dist < bestDist)
      {
         bestDist = dist;
         best = point.get();
      }
   }
   return best;
}

bool EditorUI::HitTestEditCenter(const ImVec2 &mousePos) const
{
   const LiveRenderContext ctx(m_player, nullptr, m_camMode, m_shadeMode, false);
   const ImVec2 pos = ctx.Project(m_pointEditPart->GetEditCenter());
   if (pos.x == FLT_MAX)
      return false;
   const float maxDist = 10.f * m_liveUI.GetDPI();
   const float dx = pos.x - mousePos.x;
   const float dy = pos.y - mousePos.y;
   return dx * dx + dy * dy < maxDist * maxDist;
}

void EditorUI::BoxSelectPoints(const ImVec2 &cornerA, const ImVec2 &cornerB, bool add)
{
   // Select all the drag points of the edited curve projecting inside the given screen box
   if (!add)
      m_pointSel.clear();
   const LiveRenderContext ctx(m_player, nullptr, m_camMode, m_shadeMode, false);
   if (m_pointEditCenter)
   {
      // In center edit mode, select the part's center point if it projects inside the box
      const ImVec2 boxMin(std::min(cornerA.x, cornerB.x), std::min(cornerA.y, cornerB.y));
      const ImVec2 boxMax(std::max(cornerA.x, cornerB.x), std::max(cornerA.y, cornerB.y));
      const ImVec2 pos = ctx.Project(m_pointEditPart->GetEditCenter());
      if (!add)
         m_centerSelected = false;
      if (pos.x >= boxMin.x && pos.x <= boxMax.x && pos.y >= boxMin.y && pos.y <= boxMax.y)
         m_centerSelected = true;
      return;
   }
   const ImVec2 boxMin(std::min(cornerA.x, cornerB.x), std::min(cornerA.y, cornerB.y));
   const ImVec2 boxMax(std::max(cornerA.x, cornerB.x), std::max(cornerA.y, cornerB.y));
   for (const auto &point : m_pointEditPart->GetDragPointCurve()->GetPoints())
   {
      const ImVec2 pos = ctx.Project(Vertex3Ds(point->GetX(), point->GetY(), m_pointEditPart->GetDragPointZ(point.get())));
      if (pos.x >= boxMin.x && pos.x <= boxMax.x && pos.y >= boxMin.y && pos.y <= boxMax.y && !IsPointSelected(point.get()))
         m_pointSel.push_back(point.get());
   }
}

void EditorUI::DeleteSelectedPoints()
{
   if (m_pointEditPart == nullptr || m_pointSel.empty() || m_table->IsLocked() || IsInspectMode())
      return;
   DragPointCurve *const curve = m_pointEditPart->GetDragPointCurve();
   vector<DragPoint *> deletable;
   for (DragPoint *point : m_pointSel)
      if (point->CanDelete())
         deletable.push_back(point);
   if (deletable.empty())
      return;
   BeginPointEdit();
   for (DragPoint *point : deletable)
   {
      m_pointSel.erase(std::ranges::find(m_pointSel, point));
      curve->DeletePoint(point);
   }
   EndPointEdit();
}


void EditorUI::PickUnityPrefab()
{
   if (m_table->IsLocked() || IsInspectMode() || m_player->m_playfieldWnd == nullptr)
      return;
   m_pendingUnityPrefabPath = std::make_shared<string>();
   std::filesystem::path defaultLocation = PathFromString(m_table->GetSettings().GetRecentDir_ImportDir());
   if (defaultLocation.empty())
      defaultLocation = m_table->m_filename.empty() ? PathFromString(m_table->GetSettings().GetRecentDir_LoadDir()) : m_table->m_filename.parent_path();
   const string location = PathToUTF8(defaultLocation); // SDL expects UTF-8
   const SDL_DialogFileFilter filters[] = { { "Unity Prefab", "prefab" } };
   SDL_ShowOpenFileDialog(
      [](void *userdata, const char *const *filelist, int filter)
      {
         auto *res = static_cast<std::shared_ptr<string> *>(userdata);
         if (filelist != nullptr && filelist[0] != nullptr)
            **res = filelist[0];
         delete res;
      },
      new std::shared_ptr<string>(m_pendingUnityPrefabPath), //
      m_player->m_playfieldWnd->GetCore(), filters, 1, location.empty() ? nullptr : location.c_str(), false);
}

void EditorUI::ImportUnityPrefab(const std::filesystem::path &path)
{
   if (m_table->IsLocked() || IsInspectMode())
      return;
   g_settingsService.GetAppSettings().SetRecentDir_ImportDir(PathToString(path.parent_path()), false);
   vector<IEditable *> created;
   const UnityImportResult res = UnityImportPrefab(m_table, path, GetPartGroupForNewPart(), created);
   if (!res.success)
   {
      m_liveUI.PushNotification("Unity prefab import failed: "s + res.error, 10000);
      for (const string &w : res.warnings)
         PLOGW << "UnityPrefabImport: " << w;
      return;
   }
   for (const string &w : res.warnings)
      PLOGW << "UnityPrefabImport: " << w;

   // A running session looks assets up through hash maps built at its start: refresh them so the
   // imported materials and images resolve (same fixup as PinTable::ImportParts).
   if (m_table == m_player->m_ptable)
      m_table->SetupLookUpTables(true);

   m_undo.BeginUndo();
   for (IEditable *const part : created)
   {
      part->AddRef(); // Created parts keep a creation reference, released by the undo record (see PinTable::Uncreate)
      m_undo.MarkForCreate(part);
   }
   m_undo.EndUndo();

   // Player side setup, same as part creation
   for (IEditable *const part : created)
   {
      if (part->GetItemType() == eItemBall)
         m_player->m_vball.push_back(static_cast<Ball *>(part));
      m_player->TimerSetup(part);
      if (auto *const renderable = part->GetIRenderable(); renderable)
         renderable->RenderSetup(m_renderer.get());
      if (part->GetIHitable())
         m_player->m_physics->Add(part);
   }

   // Select the imported parts
   UpdateEditableList();
   m_multiSel.clear();
   for (IEditable *const part : created)
      if (const auto it = m_editableMap.find(part); it != m_editableMap.end())
         m_multiSel.push_back(it->second);
   if (!m_multiSel.empty())
      m_selection = Selection(m_multiSel.back());

   m_liveUI.PushNotification("Imported "s + std::to_string(res.parts) + " part(s), "s + std::to_string(res.materials) + " material(s), "s + std::to_string(res.images) + " image(s) from '"s
         + PathToUTF8(path.filename()) + "'"s + (res.warnings.empty() ? ""s : " ("s + std::to_string(res.warnings.size()) + " warnings)"s),
      10000);
}


}