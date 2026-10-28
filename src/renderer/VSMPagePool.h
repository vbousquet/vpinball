// license:GPLv3+

#pragma once

#include "math/vector.h"
#include "math/matrix.h"

// PBR Virtual Shadow Map bookkeeping (UE5-style paged shadow maps).
//
// Each shadowed PBR light owns a large *virtual* map tiled into fixed-size pages; only pages
// containing casters are allocated a physical layer of the pool depth texture array. Pages are
// persistent spatial caches: a page renders once when allocated, then re-renders only when marked
// dirty (dynamic caster coverage every frame, light move, content edit). No temporal accumulation.
//
// This unit is deliberately free of GPU/renderer types so the coverage marking, allocation and
// eviction logic can be unit-tested headless. The renderer mirrors the same conventions on the
// GPU side (one RenderTarget per physical layer, tex_vsm_pool / tex_vsm_pagetables samplers).
namespace VSM
{
constexpr int PAGE_SIZE = 128; // physical page edge in texels (one array layer per page)
constexpr int MAX_LIGHTS = 32; // max shadowed lights (index+1 packed in 8 bits of the light record)
constexpr int CUBE_FACE_GRID = 2; // virtual pages per cube-face edge -> 4 pages per face, 24 per light
constexpr int MAX_ORTHO_GRID = 64; // max pages per edge of an ortho virtual map (64 pages = 8192 texels)
constexpr int PAGETABLE_STRIDE = MAX_ORTHO_GRID * MAX_ORTHO_GRID; // page-table texels reserved per light
constexpr int PAGETABLE_W = 512; // tex_vsm_pagetables texture width in texels (height 256 holds 32 lights)
constexpr int LAYOUT_ORTHO = 0; // ortho virtual map: gridW x gridH pages over the fitted light-space box
constexpr int LAYOUT_CUBE = 2; // cube virtual map: 6 faces of CUBE_FACE_GRID^2 pages (layout ids kept for fs_pbr)

// Forward/up axes of the 6 cube faces, matching the rasterization lookAt calls and the
// pbr_shadow_cube_lookup face order in fs_pbr (+X, -X, +Y, -Y, +Z, -Z)
extern const Vertex3Ds CUBE_FACE_FWD[6];
extern const Vertex3Ds CUBE_FACE_UP[6];

// Virtual page state of one shadowed light. For LAYOUT_ORTHO the virtual map is a gridW x gridH
// page grid over the light's fitted ortho box; for LAYOUT_CUBE it is 6 faces of gridW x gridH
// pages each (gridW == gridH == CUBE_FACE_GRID), face f's entries starting at f * gridW * gridH.
struct LightMap
{
   int layout = LAYOUT_ORTHO;
   int gridW = 0; // virtual map width in pages (per face for cube)
   int gridH = 0; // virtual map height in pages (per face for cube)
   int ptBase = 0; // first page-table texel of this light in tex_vsm_pagetables (shadowIndex * PAGETABLE_STRIDE)

   std::vector<int> phys; // per virtual page: physical pool layer + 1 (0 = not resident)
   std::vector<uint8_t> needed; // caster coverage marked this frame
   std::vector<uint8_t> dirty; // stale content: page must be (re)rendered

   int EntryCount() const { return gridW * gridH * (layout == LAYOUT_CUBE ? 6 : 1); }
   void Reset(int layout_, int w, int h);
};

// Physical page pool: a fixed number of layers of the shared depth texture array. Allocation pops
// the free list; when empty, the coldest page not marked needed this frame is evicted (LRU).
class PagePool
{
public:
   explicit PagePool(int layerCount) { Reset(layerCount); }

   void Reset(int layerCount);

   // Clears the per-frame eviction protection; stamp identifies this marking frame
   void BeginFrame(uint64_t stamp)
   {
      m_stamp = stamp;
      m_protected.assign(m_ownerLight.size(), uint8_t(0));
   }

   // Allocates a physical layer for (light, vpage): free list first, else LRU-evicts the coldest
   // unprotected page (the evicted owner's entry is reported through the out params). Returns the
   // layer index, or -1 when every page is protected this frame.
   int Alloc(int light, int vpage, int* evictLight = nullptr, int* evictVPage = nullptr);

   void Free(int layer);

   // Marks a resident layer as needed for the current frame (LRU stamp + eviction protection)
   void Touch(int layer)
   {
      m_stampOf[layer] = m_stamp;
      m_protected[layer] = 1;
   }

   int LayerCount() const { return (int)m_ownerLight.size(); }
   int FreeCount() const { return (int)m_free.size(); }

private:
   std::vector<int> m_free; // free physical layers
   std::vector<int> m_ownerLight; // layer -> owner light index (-1 = free)
   std::vector<int> m_ownerPage; // layer -> owner virtual page
   std::vector<uint64_t> m_stampOf; // layer -> last-needed frame stamp
   std::vector<uint8_t> m_protected; // layer -> marked needed this frame (no eviction)
   uint64_t m_stamp = 0;
};

// Marks the virtual pages overlapped by a caster AABB for an ortho light. boundsMin/boundsMax are
// world space; the box (l, r, b, t) is the fitted ortho extent in light view space as returned by
// the ortho fit (light view matrix + box fit over the table bounds). Coverage matches the shader's
// uv convention: px = u * gridW with u = (lx - l) / (r - l), py = v * gridH with v = (t - ly) / (t - b).
// 'dilate' inflates the marked page rect on each side (PCF penumbra bleed). Sets needed[py*gridW+px].
void MarkOrthoPages(const Matrix3D& lightView, float l, float r, float b, float t, const Vertex3Ds& boundsMin, const Vertex3Ds& boundsMax, int gridW, int gridH, int dilate, uint8_t* needed);

// Marks the virtual pages of the 6 cube faces for a near light. A caster crossing a face plane
// conservatively marks that whole face; otherwise each corner projects with the same formulas as
// pbr_shadow_cube_lookup and the covered page range is marked. Sets needed[f*g*g + py*g + px].
void MarkCubePages(const Vertex3Ds& lightPos, float zNear, const Vertex3Ds& boundsMin, const Vertex3Ds& boundsMax, int faceGrid, int dilate, uint8_t* needed);
}
