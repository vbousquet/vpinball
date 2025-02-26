// license:GPLv3+

#pragma once

#include "collide.h"

//#define DISABLE_ZTEST // z values of the BBox of (objects within) a node can be constant over some traversal levels (as its a quadtree and not an octree!), so we could also just ignore z tests overall. This can lead to performance benefits on some tables ("flat" ones) and performance penalties on others (e.g. when a ball moves under detailed meshes)

//#define USE_EMBREE //!! experimental, but working, collision detection replacement for our quad and kd-tree //!! picking in debug mode so far not implemented though
#ifdef USE_EMBREE
   #include "embree3/rtcore.h"
#endif

class HitQuadtree final
{
public:
   HitQuadtree();
   ~HitQuadtree();

   void Reset(const vector<HitObject*>& vho);
   void Insert(HitObject* ho);
   void Remove(HitObject* ho);
   void Update(); // Update bounds of hit objects and adjust tree accordingly
   const vector<HitObject*>& GetHitObjects() const { return m_vAllHO; }

   unsigned int GetObjectCount() const { return static_cast<unsigned int>(m_vho.size()); }
   unsigned int GetNLevels() const { return 0; } // Not implemented yet
   void DumpTree(const int indentLevel);

#ifndef USE_EMBREE
   void HitTestBall(const HitBall* const pball, CollisionEvent& coll) const;
#else
   void HitTestBall(vector<HitBall*> ball) const;
#endif
   void HitTestXRay(const HitBall* const pball, vector<HitTestResult>& pvhoHit, CollisionEvent& coll) const;

private:
   void Initialize();

   vector<HitObject*> m_vAllHO; // all items
   vector<HitObject*> m_vho; // items at this level

#ifndef USE_EMBREE
   void CreateNextLevel(const FRect& bounds, const unsigned int level, unsigned int level_empty); // FRect3D for an octree

   IFireEvents* __restrict m_unique; // everything below/including this node shares the same original primitive/hittarget object (just for early outs if not collidable),
                                     // so this is actually cast then to a Primitive* or HitTarget*
   eObjType m_ObjType; // only used if m_unique != nullptr, to identify which object type this is

   bool m_leaf;
   HitQuadtree * __restrict m_children; // always 4 entries

   Vertex2D m_vcenter;

   // helper arrays for SSE boundary checks
   void InitSseArrays();
   float* __restrict lefts_rights_tops_bottoms_zlows_zhighs = nullptr; // 4xSIMD rearranged BBox data, layout: 4xleft,4xright,4xtop,4xbottom,4xzlow,4xzhigh, 4xleft... ... ... the last entries are potentially filled with 'invalid' boxes for alignment/padding
   
#else
   RTCDevice m_embree_device; //!! have only one instead of the two (dynamic and non-dynamic)?
   RTCScene m_scene;
#endif
};
