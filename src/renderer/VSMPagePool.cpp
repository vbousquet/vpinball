// license:GPLv3+

#include "core/stdafx.h"
#include "VSMPagePool.h"

#include <algorithm>
#include <float.h>
#include <math.h>

namespace VSM
{
const Vertex3Ds CUBE_FACE_FWD[6] = { { 1.f, 0.f, 0.f }, { -1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }, { 0.f, -1.f, 0.f }, { 0.f, 0.f, 1.f }, { 0.f, 0.f, -1.f } };
const Vertex3Ds CUBE_FACE_UP[6] = { { 0.f, 0.f, 1.f }, { 0.f, 0.f, 1.f }, { 0.f, 0.f, 1.f }, { 0.f, 0.f, 1.f }, { 0.f, -1.f, 0.f }, { 0.f, -1.f, 0.f } };

void LightMap::Reset(int layout_, int w, int h)
{
   layout = layout_;
   gridW = w;
   gridH = h;
   const int count = EntryCount();
   phys.assign(count, 0);
   needed.assign(count, 0);
   dirty.assign(count, 1); // a fresh map renders every marked page once
}

void PagePool::Reset(int layerCount)
{
   m_free.resize(layerCount);
   for (int i = 0; i < layerCount; i++)
      m_free[i] = layerCount - 1 - i; // pop order: layer 0 first
   m_ownerLight.assign(layerCount, -1);
   m_ownerPage.assign(layerCount, -1);
   m_stampOf.assign(layerCount, 0);
   m_protected.assign(layerCount, 0);
   m_stamp = 0;
}

int PagePool::Alloc(int light, int vpage, int* evictLight, int* evictVPage)
{
   int layer;
   if (!m_free.empty())
   {
      layer = m_free.back();
      m_free.pop_back();
   }
   else
   {
      // LRU eviction among pages not marked needed this frame
      int best = -1;
      for (int i = 0; i < (int)m_ownerLight.size(); i++)
         if (!m_protected[i] && (best < 0 || m_stampOf[i] < m_stampOf[best]))
            best = i;
      if (best < 0)
         return -1;
      layer = best;
      if (evictLight != nullptr)
         *evictLight = m_ownerLight[layer];
      if (evictVPage != nullptr)
         *evictVPage = m_ownerPage[layer];
   }
   m_ownerLight[layer] = light;
   m_ownerPage[layer] = vpage;
   Touch(layer);
   return layer;
}

void PagePool::Free(int layer)
{
   if (layer < 0 || layer >= (int)m_ownerLight.size() || m_ownerLight[layer] < 0)
      return;
   m_ownerLight[layer] = -1;
   m_ownerPage[layer] = -1;
   m_stampOf[layer] = 0;
   m_protected[layer] = 0;
   m_free.push_back(layer);
}

void MarkOrthoPages(const Matrix3D& lightView, float l, float r, float b, float t, const Vertex3Ds& boundsMin, const Vertex3Ds& boundsMax, int gridW, int gridH, int dilate, uint8_t* needed)
{
   float px0 = FLT_MAX, px1 = -FLT_MAX, py0 = FLT_MAX, py1 = -FLT_MAX;
   for (int c = 0; c < 8; c++)
   {
      const Vertex3Ds corner(c & 1 ? boundsMax.x : boundsMin.x, c & 2 ? boundsMax.y : boundsMin.y, c & 4 ? boundsMax.z : boundsMin.z);
      const Vertex3Ds v = lightView.MultiplyVectorNoPerspective(corner);
      px0 = std::min(px0, (v.x - l) / (r - l) * gridW);
      px1 = std::max(px1, (v.x - l) / (r - l) * gridW);
      py0 = std::min(py0, (t - v.y) / (t - b) * gridH);
      py1 = std::max(py1, (t - v.y) / (t - b) * gridH);
   }
   const int x0 = std::max(0, (int)floorf(px0) - dilate), x1 = std::min(gridW - 1, (int)ceilf(px1) - 1 + dilate);
   const int y0 = std::max(0, (int)floorf(py0) - dilate), y1 = std::min(gridH - 1, (int)ceilf(py1) - 1 + dilate);
   for (int y = y0; y <= y1; y++)
      for (int x = x0; x <= x1; x++)
         needed[y * gridW + x] = 1;
}

void MarkCubePages(const Vertex3Ds& lightPos, float zNear, const Vertex3Ds& boundsMin, const Vertex3Ds& boundsMax, int faceGrid, int dilate, uint8_t* needed)
{
   for (int f = 0; f < 6; f++)
   {
      // Face selection and uv formulas matching pbr_shadow_cube_lookup in fs_pbr
      int inFront = 0;
      float u0 = FLT_MAX, u1 = -FLT_MAX, v0 = FLT_MAX, v1 = -FLT_MAX;
      for (int c = 0; c < 8; c++)
      {
         const Vertex3Ds corner(c & 1 ? boundsMax.x : boundsMin.x, c & 2 ? boundsMax.y : boundsMin.y, c & 4 ? boundsMax.z : boundsMin.z);
         const float dx = corner.x - lightPos.x, dy = corner.y - lightPos.y, dz = corner.z - lightPos.z;
         float fw, fu, fv;
         switch (f)
         {
         case 0:
            fw = dx;
            fu = -dy / fw;
            fv = dz / fw;
            break;
         case 1:
            fw = -dx;
            fu = dy / fw;
            fv = dz / fw;
            break;
         case 2:
            fw = dy;
            fu = dx / fw;
            fv = dz / fw;
            break;
         case 3:
            fw = -dy;
            fu = -dx / fw;
            fv = dz / fw;
            break;
         case 4:
            fw = dz;
            fu = dx / fw;
            fv = -dy / fw;
            break;
         default:
            fw = -dz;
            fu = -dx / fw;
            fv = -dy / fw;
            break;
         }
         if (fw <= zNear)
            continue;
         inFront++;
         const float su = fu * 0.5f + 0.5f, sv = fv * -0.5f + 0.5f;
         u0 = std::min(u0, su);
         u1 = std::max(u1, su);
         v0 = std::min(v0, sv);
         v1 = std::max(v1, sv);
      }
      if (inFront == 0)
         continue; // caster entirely behind this face
      int x0, x1, y0, y1;
      if (inFront < 8)
      {
         // AABB straddles the face plane: conservatively mark the whole face
         x0 = 0;
         x1 = faceGrid - 1;
         y0 = 0;
         y1 = faceGrid - 1;
      }
      else
      {
         x0 = std::max(0, (int)floorf(u0 * faceGrid) - dilate);
         x1 = std::min(faceGrid - 1, (int)ceilf(u1 * faceGrid) - 1 + dilate);
         y0 = std::max(0, (int)floorf(v0 * faceGrid) - dilate);
         y1 = std::min(faceGrid - 1, (int)ceilf(v1 * faceGrid) - 1 + dilate);
      }
      uint8_t* const face = needed + f * faceGrid * faceGrid;
      for (int y = y0; y <= y1; y++)
         for (int x = x0; x <= x1; x++)
            face[y * faceGrid + x] = 1;
   }
}
}
