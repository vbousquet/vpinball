// license:GPLv3+

#include "core/stdafx.h"

#include "AreaLayout.h"

#include "utils/Logger.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iterator>

namespace
{
std::atomic<uint32_t> g_nextNodeId { 1 };

uint32_t NewNodeId() { return g_nextNodeId.fetch_add(1, std::memory_order_relaxed); }

constexpr const char* kTypeNames[] = { "Outliner", "Properties", "Script", "PartLibrary", "Viewport", "CameraPreview" };
static_assert(std::size(kTypeNames) == static_cast<size_t>(AreaType::Count));

// Types a split may give to a new leaf, in order
constexpr AreaType kNewLeafOrder[] = { AreaType::Outliner, AreaType::Properties, AreaType::Script, AreaType::PartLibrary, AreaType::CameraPreview };

AreaRect Empty(const AreaRect& r) { return { r.x0, r.y0, r.x0, r.y0 }; }
}

const char* AreaTypeJsonName(const AreaType type)
{
   return static_cast<size_t>(type) < std::size(kTypeNames) ? kTypeNames[static_cast<size_t>(type)] : "";
}

bool AreaTypeFromJsonName(const std::string& name, AreaType& type)
{
   for (size_t i = 0; i < std::size(kTypeNames); ++i)
      if (name == kTypeNames[i])
      {
         type = static_cast<AreaType>(i);
         return true;
      }
   return false;
}

float SplitterRect::SizeForPos(const float p, const float dpi) const
{
   const float start = axis == SplitAxis::Vertical ? bounds.x0 : bounds.y0;
   const float end = axis == SplitAxis::Vertical ? bounds.x1 : bounds.y1;
   return (fixed == 1 ? end - p : p - start) / (dpi > 0.f ? dpi : 1.f);
}

// ---------------------------------------------------------------------------------------------------------------------
// Construction

AreaLayout::AreaLayout() : AreaLayout(nullptr) { *this = FromLegacy(true, true, 0.f, 0.f, 0, 1); }

AreaLayout AreaLayout::FromLegacy(const bool outliner, const bool properties, float outlinerW, float propertiesW, const int leftEditor, const int rightEditor)
{
   if (!std::isfinite(outlinerW) || outlinerW <= 0.f)
      outlinerW = kDefaultOutlinerWidth;
   if (!std::isfinite(propertiesW) || propertiesW <= 0.f)
      propertiesW = kDefaultPropertiesWidth;
   const bool valid = leftEditor >= 0 && leftEditor < kPanelTypeCount && rightEditor >= 0 && rightEditor < kPanelTypeCount && leftEditor != rightEditor;

   AreaLayout l(nullptr);
   l.m_nodes.resize(5);
   AreaNode& root = l.m_nodes[0]; // V[first, outlinerW]
   root.isSplit = true;
   root.axis = SplitAxis::Vertical;
   root.child[0] = 1;
   root.child[1] = 2;
   root.fixed = 0;
   root.size = outlinerW;
   AreaNode& left = l.m_nodes[1];
   left.parent = 0;
   left.type = valid ? static_cast<AreaType>(leftEditor) : AreaType::Outliner;
   left.hidden = !outliner;
   AreaNode& inner = l.m_nodes[2]; // V[second, propertiesW]
   inner.parent = 0;
   inner.isSplit = true;
   inner.axis = SplitAxis::Vertical;
   inner.child[0] = 3;
   inner.child[1] = 4;
   inner.fixed = 1;
   inner.size = propertiesW;
   AreaNode& vp = l.m_nodes[3];
   vp.parent = 2;
   vp.type = AreaType::Viewport;
   AreaNode& right = l.m_nodes[4];
   right.parent = 2;
   right.type = valid ? static_cast<AreaType>(rightEditor) : AreaType::Properties;
   right.hidden = !properties;
   for (AreaNode& n : l.m_nodes)
      n.id = NewNodeId();
   l.m_root = 0;
   return l;
}

void AreaLayout::ToLegacy(bool& outliner, bool& properties, float& outlinerW, float& propertiesW, int& leftEditor, int& rightEditor) const
{
   AreaLayout ref = *this; // hidden leaves laid out as visible
   std::vector<LeafRect> leaves;
   std::vector<SplitterRect> splitters;
   ref.Layout(ref.m_root, { 0.f, 0.f, 1920.f, 1080.f }, 1.f, 0.f, true, leaves, &splitters);

   float vcx = 960.f;
   for (const LeafRect& lr : leaves)
      if (lr.type == AreaType::Viewport)
         vcx = (lr.rect.x0 + lr.rect.x1) * 0.5f;

   const LeafRect* left = nullptr;
   const LeafRect* right = nullptr;
   float leftCx = 0.f, rightCx = 0.f;
   for (const LeafRect& lr : leaves)
   {
      if (lr.type == AreaType::Viewport)
         continue;
      const float cx = (lr.rect.x0 + lr.rect.x1) * 0.5f;
      if (cx < vcx && (!left || cx > leftCx))
      {
         left = &lr;
         leftCx = cx;
      }
      else if (cx > vcx && (!right || cx < rightCx))
      {
         right = &lr;
         rightCx = cx;
      }
   }

   auto hiddenOf = [this](const LeafRect* lr) {
      const AreaNode* n = Find(lr->id);
      return n && n->hidden;
   };
   outliner = left != nullptr && !hiddenOf(left);
   properties = right != nullptr && !hiddenOf(right);
   outlinerW = left ? left->rect.Width() : kDefaultOutlinerWidth;
   propertiesW = right ? right->rect.Width() : kDefaultPropertiesWidth;
   leftEditor = left ? static_cast<int>(left->type) : -1;
   rightEditor = right ? static_cast<int>(right->type) : -1;
   if (leftEditor < 0)
   {
      for (const AreaType t : { AreaType::Outliner, AreaType::Properties, AreaType::Script, AreaType::PartLibrary })
         if (static_cast<int>(t) != rightEditor)
         {
            leftEditor = static_cast<int>(t);
            break;
         }
   }
   if (rightEditor < 0)
   {
      for (const AreaType t : { AreaType::Properties, AreaType::Outliner, AreaType::Script, AreaType::PartLibrary })
         if (static_cast<int>(t) != leftEditor)
         {
            rightEditor = static_cast<int>(t);
            break;
         }
   }
}

// ---------------------------------------------------------------------------------------------------------------------
// JSON

namespace
{
nlohmann::json NodeToJson(const std::vector<AreaNode>& nodes, const int idx)
{
   const AreaNode& n = nodes[idx];
   nlohmann::json j = nlohmann::json::object();
   if (n.isSplit)
   {
      j["split"] = n.axis == SplitAxis::Vertical ? "vertical" : "horizontal";
      j["fixed"] = n.fixed == 0 ? "first" : "second";
      j["size"] = n.size;
      j["children"] = nlohmann::json::array({ NodeToJson(nodes, n.child[0]), NodeToJson(nodes, n.child[1]) });
   }
   else
   {
      j["editor"] = AreaTypeJsonName(n.type);
      if (n.hidden)
         j["hidden"] = true;
   }
   return j;
}
}

nlohmann::json AreaLayout::ToJson() const { return m_root >= 0 ? NodeToJson(m_nodes, m_root) : nlohmann::json::object(); }

bool AreaLayout::FromJson(const nlohmann::json& j)
{
   AreaLayout tmp(nullptr);
   std::string why;
   int leafCount = 0;

   // Returns the node index or -1 (why set)
   auto parse = [&](auto&& self, const nlohmann::json& n, const int depth, const int parent) -> int {
      if (depth > kMaxDepth)
      {
         why = "layout deeper than " + std::to_string(kMaxDepth) + " levels";
         return -1;
      }
      if (!n.is_object())
      {
         why = "node is not an object";
         return -1;
      }
      const int idx = static_cast<int>(tmp.m_nodes.size());
      tmp.m_nodes.emplace_back();
      tmp.m_nodes[idx].id = NewNodeId();
      tmp.m_nodes[idx].parent = parent;
      if (n.contains("split"))
      {
         if (!n["split"].is_string() || !n.contains("fixed") || !n["fixed"].is_string() || !n.contains("size") || !n["size"].is_number()
            || !n.contains("children") || !n["children"].is_array() || n["children"].size() != 2)
         {
            why = "malformed split node";
            return -1;
         }
         const std::string axis = n["split"].get<std::string>();
         const std::string fixed = n["fixed"].get<std::string>();
         if ((axis != "vertical" && axis != "horizontal") || (fixed != "first" && fixed != "second"))
         {
            why = "unknown split axis or fixed child";
            return -1;
         }
         tmp.m_nodes[idx].isSplit = true;
         tmp.m_nodes[idx].axis = axis == "vertical" ? SplitAxis::Vertical : SplitAxis::Horizontal;
         tmp.m_nodes[idx].fixed = fixed == "first" ? 0 : 1;
         tmp.m_nodes[idx].size = static_cast<float>(n["size"].get<double>());
         for (int c = 0; c < 2; ++c)
         {
            const int ci = self(self, n["children"][c], depth + 1, idx);
            if (ci < 0)
               return -1;
            tmp.m_nodes[idx].child[c] = ci; // (not a reference: the vector grows while parsing)
         }
      }
      else
      {
         AreaType type;
         if (!n.contains("editor") || !n["editor"].is_string() || !AreaTypeFromJsonName(n["editor"].get<std::string>(), type))
         {
            why = "unknown or missing editor name";
            return -1;
         }
         if (n.contains("hidden") && !n["hidden"].is_boolean())
         {
            why = "hidden is not a boolean";
            return -1;
         }
         if (++leafCount > kMaxLeaves)
         {
            why = "more than " + std::to_string(kMaxLeaves) + " leaves";
            return -1;
         }
         tmp.m_nodes[idx].type = type;
         tmp.m_nodes[idx].hidden = n.value("hidden", false);
      }
      return idx;
   };

   const int root = parse(parse, j, 0, -1);
   if (root >= 0)
   {
      tmp.m_root = root;
      tmp.ValidateImpl(why);
   }
   else
      tmp.m_root = -1;

   if (root < 0 || !why.empty())
   {
      m_lastError = why.empty() ? "invalid layout" : why;
      PLOGW << "Editor layout rejected: " << m_lastError;
      return false;
   }
   *this = std::move(tmp);
   m_lastError.clear();
   return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Validation

bool AreaLayout::Validate() const
{
   std::string why;
   return ValidateImpl(why);
}

bool AreaLayout::Validate(std::string& why) const { return ValidateImpl(why); }

// Returns true if valid; otherwise false with `why` set (non empty). When valid `why` is left untouched (empty).
bool AreaLayout::ValidateImpl(std::string& why) const
{
   const int count = static_cast<int>(m_nodes.size());
   if (m_root < 0 || m_root >= count || m_nodes[m_root].parent != -1)
   {
      why = "no root";
      return false;
   }
   std::vector<uint8_t> seen(count, 0);
   int visited = 0, leaves = 0, viewports = 0;
   bool typeSeen[static_cast<size_t>(AreaType::Count)] = {};

   auto walk = [&](auto&& self, const int idx, const int depth) -> bool {
      if (depth > kMaxDepth)
      {
         why = "layout deeper than " + std::to_string(kMaxDepth) + " levels";
         return false;
      }
      if (idx < 0 || idx >= count || seen[idx])
      {
         why = "broken node links";
         return false;
      }
      seen[idx] = 1;
      ++visited;
      const AreaNode& n = m_nodes[idx];
      if (n.isSplit)
      {
         if (n.fixed > 1 || !std::isfinite(n.size) || n.size <= 0.f)
         {
            why = "split size is not a finite positive number";
            return false;
         }
         for (int c = 0; c < 2; ++c)
         {
            if (n.child[c] < 0 || n.child[c] >= count || m_nodes[n.child[c]].parent != idx)
            {
               why = "broken node links";
               return false;
            }
            if (!self(self, n.child[c], depth + 1))
               return false;
         }
         return true;
      }
      if (static_cast<size_t>(n.type) >= static_cast<size_t>(AreaType::Count) || (n.type == AreaType::CameraPreview && !cameraPreviewEnabled))
      {
         why = "unknown or disabled editor type";
         return false;
      }
      if (++leaves > kMaxLeaves)
      {
         why = "more than " + std::to_string(kMaxLeaves) + " leaves";
         return false;
      }
      if (n.type == AreaType::Viewport)
      {
         ++viewports;
         if (n.hidden)
         {
            why = "the Viewport cannot be hidden";
            return false;
         }
      }
      if (typeSeen[static_cast<size_t>(n.type)])
      {
         why = std::string("duplicate editor ") + AreaTypeJsonName(n.type);
         return false;
      }
      typeSeen[static_cast<size_t>(n.type)] = true;
      return true;
   };

   if (!walk(walk, m_root, 0))
      return false;
   if (viewports != 1)
   {
      why = "layout needs exactly one Viewport";
      return false;
   }
   if (visited != count)
   {
      why = "unreachable nodes";
      return false;
   }
   return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Queries

int AreaLayout::IndexOf(const int id) const
{
   if (id < 0)
      return -1;
   for (size_t i = 0; i < m_nodes.size(); ++i)
      if (m_nodes[i].id == static_cast<uint32_t>(id))
         return static_cast<int>(i);
   return -1;
}

const AreaNode* AreaLayout::Find(const int id) const
{
   const int i = IndexOf(id);
   return i >= 0 ? &m_nodes[i] : nullptr;
}

int AreaLayout::FindLeaf(const AreaType type) const
{
   for (const AreaNode& n : m_nodes)
      if (!n.isSplit && n.type == type)
         return static_cast<int>(n.id);
   return -1;
}

int AreaLayout::ParentId(const int id) const
{
   const int i = IndexOf(id);
   return i >= 0 && m_nodes[i].parent >= 0 ? static_cast<int>(m_nodes[m_nodes[i].parent].id) : -1;
}

int AreaLayout::LeafCount() const
{
   int c = 0;
   for (const AreaNode& n : m_nodes)
      c += n.isSplit ? 0 : 1;
   return c;
}

bool AreaLayout::operator==(const AreaLayout& other) const
{
   auto eq = [&](auto&& self, const int a, const int b) -> bool {
      if (a < 0 || b < 0)
         return a == b;
      const AreaNode& x = m_nodes[a];
      const AreaNode& y = other.m_nodes[b];
      if (x.isSplit != y.isSplit)
         return false;
      if (!x.isSplit)
         return x.type == y.type && x.hidden == y.hidden;
      return x.axis == y.axis && x.fixed == y.fixed && x.size == y.size && self(self, x.child[0], y.child[0]) && self(self, x.child[1], y.child[1]);
   };
   return eq(eq, m_root, other.m_root);
}

int AreaLayout::Depth(const int idx) const
{
   int d = 0;
   for (int p = m_nodes[idx].parent; p >= 0; p = m_nodes[p].parent)
      ++d;
   return d;
}

bool AreaLayout::Contains(const int idx, const AreaType type) const
{
   const AreaNode& n = m_nodes[idx];
   return n.isSplit ? (Contains(n.child[0], type) || Contains(n.child[1], type)) : n.type == type;
}

bool AreaLayout::Vis(const int idx, const bool ignoreHidden) const
{
   const AreaNode& n = m_nodes[idx];
   return n.isSplit ? (Vis(n.child[0], ignoreHidden) || Vis(n.child[1], ignoreHidden)) : (ignoreHidden || !n.hidden);
}

void AreaLayout::CollectLeaves(const int idx, std::vector<int>& out) const
{
   const AreaNode& n = m_nodes[idx];
   if (n.isSplit)
   {
      CollectLeaves(n.child[0], out);
      CollectLeaves(n.child[1], out);
   }
   else
      out.push_back(idx);
}

// ---------------------------------------------------------------------------------------------------------------------
// Layout

void AreaLayout::MinSize(const int idx, const float dpi, const float sidebarPx, const bool ignoreHidden, float& w, float& h) const
{
   const AreaNode& n = m_nodes[idx];
   w = h = 0.f;
   if (!Vis(idx, ignoreHidden))
      return;
   if (!n.isSplit)
   {
      if (n.type == AreaType::Viewport)
      {
         w = (kViewportMinW + sidebarPx) * dpi;
         h = kViewportMinH * dpi;
      }
      else
      {
         w = kPanelMinW * dpi;
         h = kPanelMinH * dpi;
      }
      return;
   }
   float w0, h0, w1, h1;
   MinSize(n.child[0], dpi, sidebarPx, ignoreHidden, w0, h0);
   MinSize(n.child[1], dpi, sidebarPx, ignoreHidden, w1, h1);
   const bool v0 = Vis(n.child[0], ignoreHidden), v1 = Vis(n.child[1], ignoreHidden);
   if (v0 && v1)
   {
      if (n.axis == SplitAxis::Vertical)
      {
         w = w0 + w1;
         h = std::max(h0, h1);
      }
      else
      {
         w = std::max(w0, w1);
         h = h0 + h1;
      }
   }
   else
   {
      w = v0 ? w0 : w1;
      h = v0 ? h0 : h1;
   }
}

void AreaLayout::Layout(const int idx, const AreaRect r, const float dpi, const float sidebarPx, const bool ignoreHidden,
   std::vector<LeafRect>& leaves, std::vector<SplitterRect>* splitters) const
{
   const AreaNode& n = m_nodes[idx];
   if (!n.isSplit)
   {
      const bool vis = Vis(idx, ignoreHidden);
      leaves.push_back({ static_cast<int>(n.id), n.type, vis ? r : Empty(r), vis });
      return;
   }
   const bool v0 = Vis(n.child[0], ignoreHidden), v1 = Vis(n.child[1], ignoreHidden);
   if (!v0 || !v1)
   {
      // One child (or none) takes everything; the other takes no space and draws no splitter
      Layout(n.child[0], v0 ? r : Empty(r), dpi, sidebarPx, ignoreHidden, leaves, splitters);
      Layout(n.child[1], v1 ? r : Empty(r), dpi, sidebarPx, ignoreHidden, leaves, splitters);
      return;
   }

   const bool vertical = n.axis == SplitAxis::Vertical;
   const float a0 = vertical ? r.x0 : r.y0;
   const float a1 = vertical ? r.x1 : r.y1;
   const float total = std::max(0.f, a1 - a0);
   const int f = n.fixed, o = 1 - n.fixed;
   float w, h;
   MinSize(n.child[f], dpi, sidebarPx, ignoreHidden, w, h);
   const float minF = vertical ? w : h;
   MinSize(n.child[o], dpi, sidebarPx, ignoreHidden, w, h);
   const float minO = vertical ? w : h;

   float fixedPx = n.size * dpi;
   const bool squeezed = total <= minF + minO;
   if (squeezed) // smaller than the minimums: shrink proportionally
      fixedPx = (minF + minO) > 0.f ? total * minF / (minF + minO) : total * 0.5f;
   else
      fixedPx = std::clamp(fixedPx, minF, total - minO);

   float pos = f == 1 ? a1 - fixedPx : a0 + fixedPx;
   pos = std::max(a0, std::min(std::round(pos), a1));

   AreaRect c0 = r, c1 = r;
   if (vertical)
   {
      c0.x1 = pos;
      c1.x0 = pos;
   }
   else
   {
      c0.y1 = pos;
      c1.y0 = pos;
   }

   if (splitters)
   {
      SplitterRect s;
      s.nodeId = static_cast<int>(n.id);
      s.axis = n.axis;
      s.fixed = n.fixed;
      s.bounds = r;
      s.pos = pos;
      const float half = kGrabWidth * dpi * 0.5f;
      s.grab = vertical ? AreaRect { pos - half, r.y0, pos + half, r.y1 } : AreaRect { r.x0, pos - half, r.x1, pos + half };
      if (squeezed)
         s.minPos = s.maxPos = pos;
      else
      {
         s.minPos = f == 1 ? a0 + minO : a0 + minF;
         s.maxPos = f == 1 ? a1 - minF : a1 - minO;
         if (s.minPos > s.maxPos)
            s.minPos = s.maxPos = pos;
      }
      splitters->push_back(s);
   }

   Layout(n.child[0], c0, dpi, sidebarPx, ignoreHidden, leaves, splitters);
   Layout(n.child[1], c1, dpi, sidebarPx, ignoreHidden, leaves, splitters);
}

void AreaLayout::Compute(const AreaRect bounds, const float dpi, const float sidebarPx, std::vector<LeafRect>& leaves,
   std::vector<SplitterRect>& splitters, const int maximizedLeafId) const
{
   leaves.clear();
   splitters.clear();
   if (m_root < 0)
      return;

   const int maxIdx = IndexOf(maximizedLeafId);
   if (maxIdx >= 0 && !m_nodes[maxIdx].isSplit && !m_nodes[maxIdx].hidden)
   {
      std::vector<int> all;
      CollectLeaves(m_root, all);
      for (const int i : all)
      {
         const AreaNode& n = m_nodes[i];
         const bool vis = i == maxIdx;
         leaves.push_back({ static_cast<int>(n.id), n.type, vis ? bounds : Empty(bounds), vis });
      }
      return;
   }
   Layout(m_root, bounds, dpi > 0.f ? dpi : 1.f, sidebarPx, false, leaves, &splitters);
}

int AreaLayout::LeafAt(const std::vector<LeafRect>& leaves, const float x, const float y)
{
   for (const LeafRect& l : leaves)
      if (l.visible && x >= l.rect.x0 && x < l.rect.x1 && y >= l.rect.y0 && y < l.rect.y1)
         return l.id;
   return -1;
}

// ---------------------------------------------------------------------------------------------------------------------
// Mutations

void AreaLayout::Compact()
{
   if (m_root < 0)
      return;
   std::vector<int> remap(m_nodes.size(), -1);
   std::vector<int> order; // preorder: parents before children
   auto walk = [&](auto&& self, const int idx) -> void {
      remap[idx] = static_cast<int>(order.size());
      order.push_back(idx);
      if (m_nodes[idx].isSplit)
      {
         self(self, m_nodes[idx].child[0]);
         self(self, m_nodes[idx].child[1]);
      }
   };
   walk(walk, m_root);
   std::vector<AreaNode> out;
   out.reserve(order.size());
   for (const int old : order)
   {
      AreaNode n = m_nodes[old];
      n.parent = n.parent >= 0 ? remap[n.parent] : -1;
      if (n.isSplit)
      {
         n.child[0] = remap[n.child[0]];
         n.child[1] = remap[n.child[1]];
      }
      out.push_back(n);
   }
   m_nodes = std::move(out);
   m_root = 0;
}

int AreaLayout::FirstFreeType() const
{
   for (const AreaType t : kNewLeafOrder)
   {
      if (t == AreaType::CameraPreview && !cameraPreviewEnabled)
         continue;
      if (FindLeaf(t) < 0)
         return static_cast<int>(t);
   }
   return -1;
}

bool AreaLayout::CanSplit(const int leaf) const
{
   const int i = IndexOf(leaf);
   if (i < 0 || m_nodes[i].isSplit)
      return false;
   return FirstFreeType() >= 0 && LeafCount() < kMaxLeaves && Depth(i) + 1 <= kMaxDepth;
}

bool AreaLayout::Split(const int leaf, const SplitAxis axis, const bool newOnSecondSide, float newSizePx)
{
   if (!CanSplit(leaf))
      return false;
   if (!std::isfinite(newSizePx) || newSizePx <= 0.f)
      newSizePx = 1.f;
   const int li = IndexOf(leaf);
   const int parent = m_nodes[li].parent;
   const AreaType newType = static_cast<AreaType>(FirstFreeType());

   const int si = static_cast<int>(m_nodes.size());
   const int ni = si + 1;
   m_nodes.emplace_back();
   m_nodes.emplace_back();
   AreaNode& s = m_nodes[si];
   AreaNode& n = m_nodes[ni];
   s.id = NewNodeId();
   s.isSplit = true;
   s.axis = axis;
   s.parent = parent;
   s.child[newOnSecondSide ? 0 : 1] = li;
   s.child[newOnSecondSide ? 1 : 0] = ni;
   s.fixed = newOnSecondSide ? 1 : 0; // the new leaf keeps newSizePx
   s.size = newSizePx;
   n.id = NewNodeId();
   n.parent = si;
   n.type = newType;
   m_nodes[li].parent = si;
   if (parent >= 0)
   {
      AreaNode& p = m_nodes[parent];
      p.child[p.child[0] == li ? 0 : 1] = si;
   }
   else
      m_root = si;
   Compact();
   return true;
}

bool AreaLayout::CanJoin(const int keepLeaf, const int consumeLeaf) const
{
   const int k = IndexOf(keepLeaf), c = IndexOf(consumeLeaf);
   if (k < 0 || c < 0 || k == c)
      return false;
   const AreaNode& a = m_nodes[k];
   const AreaNode& b = m_nodes[c];
   if (a.isSplit || b.isSplit || a.parent < 0 || a.parent != b.parent)
      return false;
   return !a.hidden && !b.hidden && b.type != AreaType::Viewport;
}

bool AreaLayout::Join(const int keep, const int consume)
{
   if (!CanJoin(keep, consume))
      return false;
   const int k = IndexOf(keep);
   const int p = m_nodes[k].parent;
   const int g = m_nodes[p].parent;
   m_nodes[k].parent = g;
   if (g >= 0)
   {
      AreaNode& gn = m_nodes[g];
      gn.child[gn.child[0] == p ? 0 : 1] = k;
   }
   else
      m_root = k;
   Compact(); // drops the old parent split and the consumed leaf
   return true;
}

bool AreaLayout::Swap(const int split)
{
   const int i = IndexOf(split);
   if (i < 0 || !m_nodes[i].isSplit)
      return false;
   AreaNode& n = m_nodes[i];
   std::swap(n.child[0], n.child[1]);
   n.fixed = static_cast<uint8_t>(1 - n.fixed);
   return true;
}

bool AreaLayout::SetType(const int leaf, const AreaType type)
{
   const int i = IndexOf(leaf);
   if (i < 0 || m_nodes[i].isSplit || m_nodes[i].type == AreaType::Viewport)
      return false;
   if (type == AreaType::Viewport || type >= AreaType::Count || (type == AreaType::CameraPreview && !cameraPreviewEnabled))
      return false;
   AreaNode& n = m_nodes[i];
   if (n.type == type)
      return true;
   const int o = IndexOf(FindLeaf(type));
   if (o >= 0)
      m_nodes[o].type = n.type; // each panel is a single instance: the other leaf takes the type this leaf leaves
   n.type = type;
   return true;
}

bool AreaLayout::SetHidden(const int leaf, const bool hidden)
{
   const int i = IndexOf(leaf);
   if (i < 0 || m_nodes[i].isSplit || m_nodes[i].type == AreaType::Viewport)
      return false;
   m_nodes[i].hidden = hidden;
   return true;
}

bool AreaLayout::SetSplitSize(const int split, const float logicalPx)
{
   const int i = IndexOf(split);
   if (i < 0 || !m_nodes[i].isSplit || !std::isfinite(logicalPx))
      return false;
   m_nodes[i].size = std::max(1.f, logicalPx);
   return true;
}
