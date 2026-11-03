// license:GPLv3+

#pragma once

// Blender-style area layout tree of the live editor (see SPEC-area-layout.md, section 1).
//
// Pure C++20 plus nlohmann JSON: no ImGui and no EditorUI includes, so it can be unit tested.
//
// All public functions that take a node (`leaf`, `split`, `keep`, `consume`) take the stable AreaNode::id,
// never a vector index: indices change when the tree is compacted after a mutation, ids never do.
// Ids are unique within a session (drawn from a process wide counter, never reused), so an id kept by a
// drag or a menu can safely be re-resolved every frame: if Find(id) is null, the node is gone.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

enum class AreaType : uint8_t
{
   Outliner = 0,
   Properties = 1,
   Script = 2,
   PartLibrary = 3, // == legacy left/right_editor ints
   Viewport = 4,
   CameraPreview = 5,
   Count
};

constexpr int kPanelTypeCount = 4; // types offered in editor-type menus (CameraPreview added only by task 5)

// Names used in the workspace JSON ("Outliner", "Properties", "Script", "PartLibrary", "Viewport", "CameraPreview")
const char* AreaTypeJsonName(AreaType type);
bool AreaTypeFromJsonName(const std::string& name, AreaType& type);

// Blender naming: Vertical = divider line vertical, areas side by side
enum class SplitAxis : uint8_t
{
   Vertical,
   Horizontal
};

struct AreaRect
{
   float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
   float Width() const { return x1 - x0; }
   float Height() const { return y1 - y0; }
};

struct AreaNode
{
   uint32_t id = 0; // stable, never reused within a session (ImGui IDs, drag targets)
   int parent = -1;
   bool isSplit = false;
   // split
   SplitAxis axis = SplitAxis::Vertical;
   int child[2] = { -1, -1 };
   uint8_t fixed = 1; // child (0/1) whose size is stored
   float size = 0.f;  // logical px of child[fixed] along axis
   // leaf
   AreaType type = AreaType::Viewport;
   bool hidden = false;
};

// Result of AreaLayout::Compute for one leaf. Hidden leaves, leaves of an invisible subtree and leaves not
// shown because another leaf is maximized have visible == false and an empty rect.
struct LeafRect
{
   int id = -1;
   AreaType type = AreaType::Viewport;
   AreaRect rect;
   bool visible = false;
};

// Result of AreaLayout::Compute for one split whose two children are both visible.
struct SplitterRect
{
   int nodeId = -1;
   SplitAxis axis = SplitAxis::Vertical;
   uint8_t fixed = 1;
   AreaRect bounds; // rect of the whole split node (its cross extent is the splitter length)
   float pos = 0.f; // divider coordinate along the axis (x for Vertical, y for Horizontal), whole pixels
   AreaRect grab;   // grab strip: 6 * dpi wide, centred on the divider, spans bounds across the axis
   float minPos = 0.f, maxPos = 0.f; // allowed range of pos so that both children keep their minimum size

   // Logical size to store in AreaNode::size for a divider at the given position
   float SizeForPos(float p, float dpi) const;
};

class AreaLayout // value type, copyable (each Workspace holds one)
{
public:
   // Until task 5 (camera preview) lands, CameraPreview leaves are rejected and never offered.
   static constexpr bool cameraPreviewEnabled = false;

   static constexpr int kMaxDepth = 8;  // split levels from the root to a leaf
   static constexpr int kMaxLeaves = 6;

   // Minimum sizes, logical px (multiplied by dpi in Compute)
   static constexpr float kPanelMinW = 140.f, kPanelMinH = 100.f;
   static constexpr float kViewportMinW = 240.f, kViewportMinH = 180.f;
   static constexpr float kGrabWidth = 6.f; // splitter grab strip

   static constexpr float kDefaultOutlinerWidth = 200.f; // == OutlinerPanel::PaneWidth
   static constexpr float kDefaultPropertiesWidth = 360.f; // == PropertiesPanel::PaneWidth

   AreaLayout(); // default: the legacy default layout (Outliner 200 | Viewport | Properties 360)

   // --- Construction and (de)serialization ---

   // Legacy shape: V[fixed=first,size=outlinerW]( Leaf(left, hidden=!outliner), V[fixed=second,size=propertiesW]( Leaf(Viewport), Leaf(right, hidden=!properties) ) )
   // A width <= 0 or not finite means the default. Editors out of range 0..3, or equal, mean Outliner/Properties.
   static AreaLayout FromLegacy(bool outliner, bool properties, float outlinerW, float propertiesW, int leftEditor, int rightEditor);

   // Legacy keys for older builds: left/right = the non-Viewport leaf whose rect centre is nearest to the left/right
   // of the Viewport's centre, computed on a 1920x1080 reference rect at dpi 1 with hidden leaves laid out as
   // visible; widths = those leaves' widths; outliner/properties = their visibility. With no leaf on a side, that
   // side is reported hidden with the default width and a free panel type.
   void ToLegacy(bool& outliner, bool& properties, float& outlinerW, float& propertiesW, int& leftEditor, int& rightEditor) const;

   // Parses the "layout" workspace value. On failure returns false, logs PLOGW once, sets LastError() and leaves
   // *this unchanged.
   bool FromJson(const nlohmann::json& j);
   nlohmann::json ToJson() const;
   const std::string& LastError() const { return m_lastError; }

   // Exactly one Viewport; at most one leaf per other type; depth <= kMaxDepth; <= kMaxLeaves leaves; split
   // sizes finite and > 0; consistent links; Viewport not hidden; no CameraPreview unless cameraPreviewEnabled.
   bool Validate() const;
   bool Validate(std::string& why) const;

   // --- Queries ---

   const std::vector<AreaNode>& Nodes() const { return m_nodes; }
   int Root() const { return m_root; } // index into Nodes()
   const AreaNode* Find(int id) const; // null if the id is not in the tree
   int FindLeaf(AreaType type) const;  // id of the leaf of that type (hidden or not), -1 if none
   int ParentId(int id) const;         // id of the parent split, -1 for the root or an unknown id
   int LeafCount() const;
   bool IsHosted(AreaType type) const { return FindLeaf(type) >= 0; }
   // Structural equality (types, axes, sizes, hidden flags), ignoring node ids
   bool operator==(const AreaLayout& other) const;
   bool operator!=(const AreaLayout& other) const { return !(*this == other); }

   // Rects of every leaf (in order, first child first) and splitter strips of every split with two visible
   // children. bounds is the tree rect, dpi scales the logical sizes, sidebarPx (logical, 0 if the N-panel is
   // hidden) is added to the Viewport leaf's minimum width. If maximizedLeafId names a visible leaf, only that
   // leaf is shown, on the whole bounds, and there are no splitters.
   void Compute(AreaRect bounds, float dpi, float sidebarPx, std::vector<LeafRect>& leaves, std::vector<SplitterRect>& splitters,
      int maximizedLeafId = -1) const;

   // Id of the visible leaf under the point (half open rects), -1 if none. Works on the computed rects.
   static int LeafAt(const std::vector<LeafRect>& leaves, float x, float y);

   // --- Mutations (tree is compacted afterwards, ids survive) ---

   // At least one type not hosted (Outliner, Properties, Script, PartLibrary, then CameraPreview if enabled),
   // the leaf exists, and the tree stays within kMaxDepth / kMaxLeaves.
   bool CanSplit(int leaf) const;
   // Replaces the leaf by a split of the leaf (keeps its id) and a new leaf of the first free type. The new leaf is
   // the split's fixed child: newSizePx (logical) is its extent along the axis. Returns false (no change) if !CanSplit.
   // [deviation from the spec text, see report: fixed = the new leaf, whichever side it is on]
   bool Split(int leaf, SplitAxis axis, bool newOnSecondSide, float newSizePx);
   // Siblings, both leaves, both visible (not hidden), and the consumed leaf is not the Viewport
   bool CanJoin(int keepLeaf, int consumeLeaf) const;
   // Replaces the parent split by keep (keeps its id); consume is removed. Returns false (no change) if !CanJoin.
   bool Join(int keep, int consume);
   // Swaps the children (whole subtrees) of a split and sets fixed = 1 - fixed, so each side keeps its size
   bool Swap(int split);
   // Not for the Viewport leaf, nor to Viewport/Count/CameraPreview(disabled). If another leaf has the type, the two swap types.
   bool SetType(int leaf, AreaType type);
   // Not for the Viewport (refused)
   bool SetHidden(int leaf, bool hidden);
   // Stored size (logical px) of child[fixed] of a split, clamped to a positive finite value
   bool SetSplitSize(int split, float logicalPx);

private:
   explicit AreaLayout(std::nullptr_t) {} // empty tree (no root), to be filled by the factories

   std::vector<AreaNode> m_nodes;
   int m_root = -1;
   std::string m_lastError;

   int IndexOf(int id) const;
   void Compact();
   bool Vis(int idx, bool ignoreHidden) const;
   void CollectLeaves(int idx, std::vector<int>& out) const;
   int Depth(int idx) const;
   bool ValidateImpl(std::string& why) const;
   void Layout(int idx, AreaRect r, float dpi, float sidebarPx, bool ignoreHidden, std::vector<LeafRect>& leaves,
      std::vector<SplitterRect>* splitters) const;
   void MinSize(int idx, float dpi, float sidebarPx, bool ignoreHidden, float& w, float& h) const;
   bool Contains(int idx, AreaType type) const;
   int FirstFreeType() const; // -1 if none
};
