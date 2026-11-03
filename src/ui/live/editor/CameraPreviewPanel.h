// license:GPLv3+

#pragma once

#include "imgui/imgui.h"

#include <cstdint>

namespace VPX::EditorUI
{

class EditorUI;

// Camera preview, hosted by an editor area (single instance, not an editable viewport): the table as the player sees it (the table's
// view setup, as the editor's Preview Camera shows it), rendered by the Renderer into an offscreen target beside the edit viewport.
//
// Cost: nothing is rendered while no area shows it. When shown, it is rendered at the displayed image size (capped at 1280 px wide)
// and only when something changed (undo history, size, Refresh, end of an interactive edit), at half rate (every 2nd frame) while a
// gizmo, a G/R/S modal transform or a field drag runs, and at half rate all the time when Live is on.
class CameraPreviewPanel
{
public:
   explicit CameraPreviewPanel(EditorUI &editor);

   void RenderInArea(const ImVec2 &pos, const ImVec2 &size); // Window filling the given area rect, header included
   void NotHosted(); // Every frame the area is not shown: releases the preview render targets (once)

   // View setup shown: -1 = Auto (the one the player uses, the table's active view mode), else BG_DESKTOP / BG_FULLSCREEN / BG_FSS.
   // Rendering another one never changes the table's or the player's view mode. JSON names (per workspace layout leaf): "desktop",
   // "cabinet", "fss"; Auto is written as no key.
   int GetView() const { return m_view; }
   void SetView(int view);
   static const char *ViewJsonName(int view); // nullptr for Auto
   static int ViewFromJsonName(const char *name); // -1 (Auto) for null or unknown names

private:
   static constexpr int kMaxWidth = 1280;
   static constexpr int kStableFrames = 10; // A new size is adopted after being stable this many frames (no target churn while a splitter is dragged)

   EditorUI &m_editor;
   bool m_live = true;
   int m_view = -1; // See GetView
   int m_lastShownView = -1; // Effective view setup of the last request (Auto follows the player's mode changes)
   bool m_refresh = true;
   bool m_wasDragging = false;
   bool m_requested = false; // A preview was requested since the last release
   uint64_t m_lastUndoId = ~0ull;
   int m_width = 0, m_height = 0; // Requested render size (px)
   int m_pendingWidth = 0, m_pendingHeight = 0, m_pendingFrames = 0;
   unsigned int m_frame = 0;
};

}
