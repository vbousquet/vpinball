// license:GPLv3+

#include "core/stdafx.h"
#include "CameraPreviewPanel.h"

#include "IconsLucide.h"
#include "EditorTheme.h"
#include "renderer/Renderer.h"
#include "renderer/RenderTarget.h"
#include "ui/live/EditorUI.h"
#include "ui/live/LiveUI.h"

namespace VPX::EditorUI
{

CameraPreviewPanel::CameraPreviewPanel(EditorUI &editor)
   : m_editor(editor)
{
}

void CameraPreviewPanel::NotHosted()
{
   if (!m_requested)
      return;
   m_requested = false;
   m_refresh = true;
   m_width = m_height = 0;
   m_pendingWidth = m_pendingHeight = m_pendingFrames = 0;
   if (m_editor.m_renderer)
      m_editor.m_renderer->ReleaseEditorPreview();
}

namespace
{
   constexpr const char *kViewNames[] = { "Desktop", "Cabinet", "FSS" }; // Indexed by ViewSetupID (BG_DESKTOP, BG_FULLSCREEN, BG_FSS)
   constexpr const char *kViewJsonNames[] = { "desktop", "cabinet", "fss" };
}

void CameraPreviewPanel::SetView(const int view)
{
   const int v = (view >= BG_DESKTOP && view < BG_INVALID) ? view : -1;
   if (v != m_view)
   {
      m_view = v;
      m_refresh = true;
   }
}

const char *CameraPreviewPanel::ViewJsonName(const int view)
{
   return (view >= BG_DESKTOP && view < BG_INVALID) ? kViewJsonNames[view] : nullptr;
}

int CameraPreviewPanel::ViewFromJsonName(const char *const name)
{
   if (name != nullptr)
      for (int i = 0; i < BG_INVALID; i++)
         if (strcmp(name, kViewJsonNames[i]) == 0)
            return i;
   return -1;
}

void CameraPreviewPanel::RenderInArea(const ImVec2 &pos, const ImVec2 &size)
{
   m_frame++;
   if (m_editor.BeginAreaWindow("CAMERAPREVIEW", pos, size))
   {
      // Header: editor type button, title, Live toggle and Refresh
      m_editor.RenderAreaTypeButton(EditorUI::AreaEditor::CameraPreview);
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      ImGui::TextDisabled("Camera Preview");
      // Header laid out against the area's right edge, keeping the corner zone (area split / join) free
      const float dpi = m_editor.m_liveUI.GetDPI();
      const float cornerPad = 12.f * dpi;
      const float refreshW = ImGui::CalcTextSize(ICON_LC_REFRESH_CW).x + 2.f * ImGui::GetStyle().FramePadding.x;
      const float rightEdge = ImGui::GetWindowContentRegionMax().x - cornerPad;
      ImGui::SameLine();
      ImGui::Checkbox("Live", &m_live);
      ImGui::SetItemTooltip("Render the player view continuously (every 2nd frame). Off: only after a change (click Refresh).");

      // View setup: Auto (the player's) or a fixed one of the table's three view setups
      const int activeView = static_cast<int>(m_editor.m_table->GetViewMode());
      const int shownView = m_view >= 0 ? m_view : activeView;
      ImGui::SameLine();
      const string autoLabel = "Auto ("s + kViewNames[clamp(activeView, 0, 2)] + ')';
      const char *const comboLabel = m_view >= 0 ? kViewNames[m_view] : autoLabel.c_str();
      ImGui::SetNextItemWidth(min(ImGui::CalcTextSize(autoLabel.c_str()).x + ImGui::GetFrameHeight() + 2.f * ImGui::GetStyle().FramePadding.x,
         max(40.f * dpi, rightEdge - refreshW - ImGui::GetStyle().ItemSpacing.x - ImGui::GetCursorPosX())));
      if (ImGui::BeginCombo("##CameraPreviewView", comboLabel))
      {
         if (ImGui::Selectable(autoLabel.c_str(), m_view < 0))
            SetView(-1);
         ImGui::SetItemTooltip("The view setup the player uses (the table's active view mode)");
         for (int i = 0; i < BG_INVALID; i++)
            if (ImGui::Selectable(kViewNames[i], m_view == i))
               SetView(i);
         ImGui::EndCombo();
      }
      ImGui::SetItemTooltip("View setup shown by the preview: Desktop, Cabinet (fullscreen) or FSS. Does not change the table's or the player's view mode.");
      if (shownView != m_lastShownView)
      {
         m_lastShownView = shownView;
         m_refresh = true;
      }
      ImGui::SameLine(max(ImGui::GetCursorPosX(), rightEdge - refreshW));
      if (ImGui::Button(ICON_LC_REFRESH_CW "##CameraPreviewRefresh"))
         m_refresh = true;
      ImGui::SetItemTooltip("Refresh");
      ImGui::Separator();

      // Always the player window's aspect, letterboxed in the area (top-anchored): the view setup frames (and, in window mode, rotates)
      // the table after the aspect, so any other aspect (e.g. a portrait one for a tall area) is not what the player shows and crops the table
      const ImGuiIO &io = ImGui::GetIO();
      ImVec2 avail = ImGui::GetContentRegionAvail();
      avail.y -= 4.f * dpi; // The image starts 4 px down: keep it inside the area
      const float aspect = m_editor.m_renderer->GetDisplayAspectRatio();
      float imgW = avail.x, imgH = avail.x / aspect;
      if (imgH > avail.y)
      {
         imgH = avail.y;
         imgW = imgH * aspect;
      }
      if (imgW >= 8.f && imgH >= 8.f)
      {
         // Render size: the image size in framebuffer pixels, capped
         int w = static_cast<int>(roundf(imgW * io.DisplayFramebufferScale.x));
         int h = static_cast<int>(roundf(imgH * io.DisplayFramebufferScale.y));
         if (w > kMaxWidth)
         {
            h = static_cast<int>(roundf(static_cast<float>(h) * static_cast<float>(kMaxWidth) / static_cast<float>(w)));
            w = kMaxWidth;
         }
         if (w != m_pendingWidth || h != m_pendingHeight)
         {
            m_pendingWidth = w;
            m_pendingHeight = h;
            m_pendingFrames = 0;
         }
         else
            m_pendingFrames++;
         if ((m_pendingWidth != m_width || m_pendingHeight != m_height) && (m_width == 0 || m_pendingFrames >= kStableFrames))
         {
            m_width = m_pendingWidth;
            m_height = m_pendingHeight;
            m_refresh = true;
         }

         // Re-render on changes: any new undo step (part edits, view setup edits), and at half rate while something is being dragged
         const uint64_t undoId = m_editor.m_undo.TopRecordId();
         if (undoId != m_lastUndoId)
         {
            m_lastUndoId = undoId;
            m_refresh = true;
         }
         // An interactive edit (gizmo drag, G/R/S modal transform, field drag; not a focused text field) keeps one undo record from its start: render while it runs, and once when it ends
         const bool dragging = ImGuizmo::IsUsingAny() || m_editor.m_modal.active || (ImGui::IsAnyItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left));
         if (m_wasDragging && !dragging)
            m_refresh = true;
         m_wasDragging = dragging;
         if (m_refresh || ((m_live || dragging) && (m_frame & 1u) == 0))
         {
            m_editor.m_renderer->RequestEditorPreview(m_width, m_height, m_view >= 0 ? static_cast<ViewSetupID>(m_view) : BG_INVALID);
            m_requested = true;
            m_refresh = false;
         }

         ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + (avail.x - imgW) * 0.5f, ImGui::GetCursorPosY() + 4.f * dpi)); // Top-anchored under the header
         if (RenderTarget *const rt = m_editor.m_renderer->GetEditorPreview(); rt != nullptr)
            ImGui::Image(rt->GetColorSampler(), ImVec2(imgW, imgH));
         else
            ImGui::Dummy(ImVec2(imgW, imgH));
         ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), EditorTheme::WorkspaceOff, 0.f, 0, 1.f);
      }
   }
   m_editor.EndAreaWindow();
}

}
