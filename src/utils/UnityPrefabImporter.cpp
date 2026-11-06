// license:GPLv3+

#include "core/stdafx.h"
#include "UnityPrefabImporter.h"
#include "UnityYaml.h"

#include "parts/pintable.h"
#include "parts/primitive.h"
#include "parts/PartGroup.h"
#include "parts/Material.h"
#include "renderer/Texture.h"
#include "core/editablereg.h"

#ifdef __STANDALONE__
#define _WINDOWS_
#endif
#include <FreeImage.h>

#if defined(__wasm_simd128__) && defined(__has_include)
#if __has_include(<arm_neon.h>)
#include <arm_neon.h>
#endif
#endif
#define XXH_VECTOR XXH_SCALAR
#define XXH_INLINE_ALL
#include "xxhash.h"
#include "ufbx/ufbx.h"

#include <unordered_map>
#include <unordered_set>
#include <functional>

namespace
{

// -----------------------------------------------------------------------------------------------
// Small math helpers (self contained: affine 4x4 + quaternion, Unity conventions)

struct Mat4
{
   float m[4][4];
   static Mat4 Identity()
   {
      Mat4 r {};
      r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.f;
      return r;
   }
   static Mat4 TRS(const float px, const float py, const float pz, const float qx, const float qy, const float qz, const float qw, const float sx, const float sy, const float sz)
   {
      const float x2 = qx + qx, y2 = qy + qy, z2 = qz + qz;
      const float xx = qx * x2, xy = qx * y2, xz = qx * z2;
      const float yy = qy * y2, yz = qy * z2, zz = qz * z2;
      const float wx = qw * x2, wy = qw * y2, wz = qw * z2;
      Mat4 r = Identity();
      r.m[0][0] = (1.f - (yy + zz)) * sx;
      r.m[0][1] = (xy - wz) * sy;
      r.m[0][2] = (xz + wy) * sz;
      r.m[1][0] = (xy + wz) * sx;
      r.m[1][1] = (1.f - (xx + zz)) * sy;
      r.m[1][2] = (yz - wx) * sz;
      r.m[2][0] = (xz - wy) * sx;
      r.m[2][1] = (yz + wx) * sy;
      r.m[2][2] = (1.f - (xx + yy)) * sz;
      r.m[0][3] = px;
      r.m[1][3] = py;
      r.m[2][3] = pz;
      return r;
   }
   Mat4 operator*(const Mat4& o) const
   {
      Mat4 r {};
      for (int i = 0; i < 4; i++)
         for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++)
               r.m[i][j] += m[i][k] * o.m[k][j];
      return r;
   }
   vec3 TransformPoint(const float x, const float y, const float z) const
   {
      return { m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3], m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3], m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3] };
   }
   vec3 TransformDir(const float x, const float y, const float z) const
   {
      return { m[0][0] * x + m[0][1] * y + m[0][2] * z, m[1][0] * x + m[1][1] * y + m[1][2] * z, m[2][0] * x + m[2][1] * y + m[2][2] * z };
   }
   float Det3() const { return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]); }
};

// Unity (LH, Y-up, m) -> VPX (LH, X right / Y toward player / Z up, VPU).
// Candidate mapping kept in one place: verified visually on an asymmetric asset (see spec).
// Handedness itself (RH FBX -> LH) is converted at load time via ufbx's mirror axis, so the
// mapping below is handedness-preserving and no winding reversal is needed here.
constexpr float kUtoVx = 1.f, kUtoVy = 0.f, kUtoVz = 0.f;
constexpr float kVtoVx = 0.f, kVtoVy = 0.f, kVtoVz = 1.f;
constexpr float kWtoVx = 0.f, kWtoVy = -1.f, kWtoVz = 0.f;
constexpr bool kFlipWinding = false;
constexpr bool kFlipV = true;

inline vec3 UnityToVpxDir(const float x, const float y, const float z)
{
   return { x * kUtoVx + y * kVtoVx + z * kWtoVx, x * kUtoVy + y * kVtoVy + z * kWtoVy, x * kUtoVz + y * kVtoVz + z * kWtoVz };
}
inline vec3 UnityToVpxPos(const float x, const float y, const float z)
{
   const vec3 d = UnityToVpxDir(x, y, z);
   return { MTOVPU(d.x), MTOVPU(d.y), MTOVPU(d.z) };
}

// -----------------------------------------------------------------------------------------------
// Unity source model

struct URenderNode
{
   string name;
   Mat4 world = Mat4::Identity();
   string fbxGuid;
   int64_t meshFileId = 0;
   vector<string> materialGuids;
};

struct Importer
{
   explicit Importer(PinTable* table)
      : m_table(table)
   {
   }

   PinTable* const m_table;
   UnityImportResult m_result;
   std::unordered_map<string, std::filesystem::path> m_guidToPath; // guid -> asset file
   std::unordered_map<string, ufbx_scene*> m_fbxCache; // fbx guid -> loaded scene
   std::unordered_set<string> m_loadingPrefabs; // recursion guard
   vector<IEditable*>* m_created = nullptr;

   void Warn(const string& w) { m_result.warnings.push_back(w); }

   // ----- GUID index --------------------------------------------------------------------------

   bool BuildGuidIndex(const std::filesystem::path& prefabPath)
   {
      // Root = nearest ancestor named "Assets" (Unity project layout), else the prefab folder
      std::filesystem::path root = prefabPath.parent_path();
      for (std::filesystem::path p = prefabPath.parent_path(); !p.empty(); p = p.parent_path())
         if (p.filename() == "Assets")
         {
            root = p;
            break;
         }
      std::error_code ec;
      for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec))
      {
         if (!it->is_regular_file(ec) || it->path().extension() != ".meta")
            continue;
         std::ifstream f(it->path(), std::ios::binary);
         std::ostringstream ss;
         ss << f.rdbuf();
         const string guid = UnityYaml::MetaGuid(ss.str());
         if (!guid.empty())
            m_guidToPath.emplace(guid, it->path().parent_path() / it->path().stem()); // strip ".meta"
      }
      return !m_guidToPath.empty();
   }

   // ----- prefab parsing ----------------------------------------------------------------------

   // A parsed prefab file, kept per recursion level
   struct PrefabData
   {
      // GameObject nodes: transform fileId -> node index
      struct Node
      {
         int64_t goId = 0;
         int64_t trId = 0;
         int64_t fatherTrId = 0;
         string name;
         bool active = true;
         float px = 0, py = 0, pz = 0;
         float qx = 0, qy = 0, qz = 0, qw = 1;
         float sx = 1, sy = 1, sz = 1;
         bool hasMesh = false;
         string fbxGuid;
         int64_t meshFileId = 0;
         vector<string> materialGuids;
         bool rendererEnabled = true;
      };
      vector<Node> nodes;
      std::unordered_map<int64_t, size_t> trToNode; // transform fileId -> node index
      std::unordered_map<int64_t, size_t> compToNode; // component fileId -> node index
      struct Instance
      {
         int64_t fileId = 0;
         int64_t parentTrId = 0;
         string sourceGuid;
         const UnityYaml::Value* mods = nullptr; // m_Modifications list
      };
      vector<Instance> instances;
      // stripped Transform docs: outer fileId -> (instance fileId, inner source fileId)
      std::unordered_map<int64_t, std::pair<int64_t, int64_t>> strippedTransforms;
      // stripped GameObject docs: outer fileId -> (instance fileId, inner source fileId)
      std::unordered_map<int64_t, std::pair<int64_t, int64_t>> strippedGameObjects;
   };

   bool LoadPrefabFile(const std::filesystem::path& path, PrefabData& out)
   {
      std::ifstream f(path, std::ios::binary);
      if (!f)
      {
         Warn("cannot open prefab '" + PathToUTF8(path) + "'");
         return false;
      }
      std::ostringstream ss;
      ss << f.rdbuf();
      vector<UnityYaml::Document> docs;
      try
      {
         docs = UnityYaml::Parse(ss.str());
      }
      catch (const std::exception& e)
      {
         Warn("YAML parse failed in '" + PathToUTF8(path) + "': " + e.what());
         return false;
      }

      std::unordered_map<int64_t, const UnityYaml::Document*> goDocs, trDocs, mfDocs, mrDocs;
      for (const UnityYaml::Document& d : docs)
      {
         if (d.stripped && d.classId == 4)
         {
            UnityYaml::Ref src, inst;
            UnityYaml::AsRef(d.root["m_CorrespondingSourceObject"], src);
            UnityYaml::AsRef(d.root["m_PrefabInstance"], inst);
            out.strippedTransforms[d.fileId] = { inst.fileID, src.fileID };
            continue;
         }
         if (d.stripped && d.classId == 1)
         {
            UnityYaml::Ref src, inst;
            UnityYaml::AsRef(d.root["m_CorrespondingSourceObject"], src);
            UnityYaml::AsRef(d.root["m_PrefabInstance"], inst);
            out.strippedGameObjects[d.fileId] = { inst.fileID, src.fileID };
            continue;
         }
         if (d.stripped)
            continue;
         switch (d.classId)
         {
         case 1: goDocs[d.fileId] = &d; break;
         case 4: trDocs[d.fileId] = &d; break;
         case 33: mfDocs[d.fileId] = &d; break;
         case 23: mrDocs[d.fileId] = &d; break;
         case 1001:
         {
            PrefabData::Instance inst;
            inst.fileId = d.fileId;
            UnityYaml::Ref parentRef, srcRef;
            const UnityYaml::Value& mod = d.root["m_Modification"];
            if (UnityYaml::AsRef(mod["m_TransformParent"], parentRef))
               inst.parentTrId = parentRef.fileID;
            if (UnityYaml::AsRef(d.root["m_SourcePrefab"], srcRef))
               inst.sourceGuid = srcRef.guid;
            inst.mods = &mod["m_Modifications"];
            out.instances.push_back(inst);
            break;
         }
         default: break;
         }
      }

      // Compose GameObject + components into nodes (keyed by Transform fileId)
      for (const auto& [goId, doc] : goDocs)
      {
         PrefabData::Node node;
         node.goId = goId;
         const UnityYaml::Value& go = doc->root;
         node.name = go["m_Name"].AsString();
         node.active = go["m_IsActive"].AsBool(true);
         for (size_t i = 0; i < go["m_Component"].Size(); i++)
         {
            UnityYaml::Ref comp;
            if (!UnityYaml::AsRef(go["m_Component"].At(i)["component"], comp))
               continue;
            const int64_t cid = comp.fileID;
            out.compToNode[cid] = out.nodes.size();
            if (trDocs.contains(cid))
            {
               node.trId = cid;
               const UnityYaml::Value& tr = trDocs[cid]->root;
               UnityYaml::AsVec3(tr["m_LocalPosition"], node.px, node.py, node.pz);
               UnityYaml::AsVec4(tr["m_LocalRotation"], node.qx, node.qy, node.qz, node.qw);
               UnityYaml::AsVec3(tr["m_LocalScale"], node.sx, node.sy, node.sz);
               UnityYaml::Ref father;
               if (UnityYaml::AsRef(tr["m_Father"], father))
                  node.fatherTrId = father.fileID;
            }
            else if (mfDocs.contains(cid))
            {
               UnityYaml::Ref mesh;
               if (UnityYaml::AsRef(mfDocs[cid]->root["m_Mesh"], mesh) && mesh.fileID != 0)
               {
                  node.hasMesh = true;
                  node.meshFileId = mesh.fileID;
                  node.fbxGuid = mesh.guid;
               }
            }
            else if (mrDocs.contains(cid))
            {
               const UnityYaml::Value& mr = mrDocs[cid]->root;
               node.rendererEnabled = mr["m_Enabled"].AsBool(true);
               for (size_t m = 0; m < mr["m_Materials"].Size(); m++)
               {
                  UnityYaml::Ref mat;
                  if (UnityYaml::AsRef(mr["m_Materials"].At(m), mat) && !mat.guid.empty())
                     node.materialGuids.push_back(mat.guid);
               }
            }
         }
         out.trToNode[node.trId] = out.nodes.size();
         out.nodes.push_back(std::move(node));
      }
      return true;
   }

   // Builds the flat render-node list of a prefab file (expanding nested PrefabInstances).
   // 'world' outputs are Unity-space (LH, Y-up, meters).
   void CollectRenderNodes(const std::filesystem::path& path, const Mat4& parentWorld, vector<URenderNode>& out, int depth)
   {
      if (depth > 8)
      {
         Warn("prefab nesting deeper than 8 levels");
         return;
      }
      const string key = PathToUTF8(path.lexically_normal());
      if (m_loadingPrefabs.contains(key))
      {
         Warn("prefab reference cycle at '" + key + "'");
         return;
      }
      m_loadingPrefabs.insert(key);
      PrefabData data;
      if (!LoadPrefabFile(path, data))
      {
         m_loadingPrefabs.erase(key);
         return;
      }

      // world transform per node (Transform fileId order), computed recursively over father links
      vector<Mat4> worlds(data.nodes.size(), Mat4::Identity());
      vector<bool> computed(data.nodes.size(), false);
      const std::function<void(size_t)> computeWorld = [&](size_t i)
      {
         if (computed[i])
            return;
         computed[i] = true;
         PrefabData::Node& n = data.nodes[i];
         Mat4 parent = parentWorld;
         const auto it = data.trToNode.find(n.fatherTrId);
         if (it != data.trToNode.end())
         {
            computeWorld(it->second);
            parent = worlds[it->second];
         }
         else if (data.strippedTransforms.contains(n.fatherTrId))
         { // parented under a nested prefab transform: treat as instance root (approximation, warning below)
         }
         worlds[i] = parent * Mat4::TRS(n.px, n.py, n.pz, n.qx, n.qy, n.qz, n.qw, n.sx, n.sy, n.sz);
      };
      for (size_t i = 0; i < data.nodes.size(); i++)
         computeWorld(i);
      for (const auto& [fid, src] : data.strippedTransforms)
         if (!data.trToNode.contains(fid))
            Warn("child parented to nested prefab transform — approximated to instance root");

      // Emit render nodes
      for (size_t i = 0; i < data.nodes.size(); i++)
      {
         const PrefabData::Node& n = data.nodes[i];
         if (!n.active || !n.hasMesh)
            continue;
         if (!n.rendererEnabled)
         {
            Warn("skipping disabled renderer '" + n.name + "'");
            continue;
         }
         URenderNode r;
         r.name = n.name;
         r.world = worlds[i];
         r.fbxGuid = n.fbxGuid;
         r.meshFileId = n.meshFileId;
         r.materialGuids = n.materialGuids;
         out.push_back(std::move(r));
      }

      // Expand PrefabInstances
      for (const PrefabData::Instance& inst : data.instances)
      {
         const auto pathIt = m_guidToPath.find(inst.sourceGuid);
         if (pathIt == m_guidToPath.end())
         {
            Warn("unresolved nested prefab guid " + inst.sourceGuid);
            continue;
         }
         const size_t baseIndex = out.size();
         CollectRenderNodes(pathIt->second, Mat4::Identity(), out, depth + 1);

         // Apply instance modifications to the expanded source nodes:
         //  - m_LocalPosition/Rotation/Scale on the instance root transform -> instance transform
         //  - m_Name -> rename of the instance root
         // The target fileIDs of the modifications refer to objects *inside* the source prefab;
         // our expanded nodes no longer carry source fileIDs, so we apply the transform overrides
         // to a synthesized instance transform applied to the whole expanded subtree (standard
         // Unity prefab-instance semantics for root transforms).
         float px = 0, py = 0, pz = 0, qx = 0, qy = 0, qz = 0, qw = 1, sx = 1, sy = 1, sz = 1;
         string name;
         if (inst.mods != nullptr)
            for (size_t m = 0; m < inst.mods->Size(); m++)
            {
               const UnityYaml::Value& mod = inst.mods->At(m);
               const string pp = mod["propertyPath"].AsString();
               const UnityYaml::Value& val = mod["value"];
               const float f = static_cast<float>(val.AsFloat());
               if (pp == "m_LocalPosition.x")
                  px = f;
               else if (pp == "m_LocalPosition.y")
                  py = f;
               else if (pp == "m_LocalPosition.z")
                  pz = f;
               else if (pp == "m_LocalRotation.x")
                  qx = f;
               else if (pp == "m_LocalRotation.y")
                  qy = f;
               else if (pp == "m_LocalRotation.z")
                  qz = f;
               else if (pp == "m_LocalRotation.w")
                  qw = f;
               else if (pp == "m_LocalScale.x")
                  sx = f;
               else if (pp == "m_LocalScale.y")
                  sy = f;
               else if (pp == "m_LocalScale.z")
                  sz = f;
               else if (pp == "m_Name")
                  name = val.AsString();
               else if (pp.rfind("m_LocalEulerAnglesHint", 0) == 0 || pp == "m_ConstrainProportionsScale" || pp == "m_RootOrder")
               {
               } // hints/order: ignorable
               else
                  Warn("unhandled prefab override '" + pp + "' in '" + key + "'");
            }
         // Compose the instance transform with the transform of the expanded source root, and
         // attach under the instance's parent transform world.
         Mat4 instLocal = Mat4::TRS(px, py, pz, qx, qy, qz, qw, sx, sy, sz);
         Mat4 instWorld = parentWorld;
         const auto pit = data.trToNode.find(inst.parentTrId);
         if (pit != data.trToNode.end())
            instWorld = worlds[pit->second];
         const Mat4 total = instWorld * instLocal;
         for (size_t i = baseIndex; i < out.size(); i++)
         {
            out[i].world = total * out[i].world;
            if (!name.empty() && i == baseIndex)
               out[i].name = name;
         }
      }
      m_loadingPrefabs.erase(key);
   }

   // ----- FBX -----------------------------------------------------------------------------------

   ufbx_scene* GetFbx(const string& guid)
   {
      const auto cached = m_fbxCache.find(guid);
      if (cached != m_fbxCache.end())
         return cached->second;
      const auto pathIt = m_guidToPath.find(guid);
      if (pathIt == m_guidToPath.end())
      {
         Warn("unresolved mesh asset guid " + guid);
         m_fbxCache[guid] = nullptr;
         return nullptr;
      }
      ufbx_load_opts opts = {};
      opts.target_axes = ufbx_axes_left_handed_y_up;
      opts.target_unit_meters = 1.0;
      opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
      opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
      // Convert right handed sources to Unity's left handed axes the way Unity's own FBX importer
      // does: mirror X, with a winding reversal to keep faces front (handled inside ufbx).
      // Left at its NONE default, ufbx bakes the handedness change into geometry as a negative
      // scale, inverting positions while leaving normals and winding untouched (inside-out mesh).
      opts.handedness_conversion_axis = UFBX_MIRROR_AXIS_X;
      opts.target_axes = ufbx_axes_left_handed_y_up;
      ufbx_error err;
      const string fbxPath = PathToUTF8(pathIt->second);
      ufbx_scene* scene = ufbx_load_file(ufbx_string_view { fbxPath.c_str(), fbxPath.size() }, &opts, &err);
      if (scene == nullptr)
      {
         char errBuf[512];
         ufbx_format_error(errBuf, sizeof(errBuf), &err);
         Warn("FBX load failed for '" + fbxPath + "': " + errBuf);
      }
      m_fbxCache[guid] = scene;
      return scene;
   }

   static int64_t UnityFileId(const string& typeAndName)
   {
      const XXH64_hash_t h = XXH64(typeAndName.data(), typeAndName.size(), 0);
      return static_cast<int64_t>(h);
   }

   // Finds the ufbx_mesh matching a Unity mesh fileID inside the scene (hash of the mesh name).
   ufbx_mesh* FindMesh(ufbx_scene* scene, int64_t fileId, const string& goName)
   {
      // Candidate names: Unity names mesh sub-assets after FBX nodes ("<name>", "<name>_MeshPart<i>")
      for (size_t i = 0; i < scene->meshes.count; i++)
      {
         ufbx_mesh* mesh = scene->meshes.data[i];
         for (size_t n = 0; n < scene->nodes.count; n++)
         {
            const ufbx_node* node = scene->nodes.data[n];
            if (node->mesh != mesh)
               continue;
            const string name(node->name.data, node->name.length);
            if (UnityFileId("Type:Mesh->" + name + "0") == fileId)
               return mesh;
            for (int p = 0; p < 8; p++)
               if (UnityFileId("Type:Mesh->" + name + "_MeshPart" + std::to_string(p) + "0") == fileId)
                  return mesh;
         }
      }
      // Fallback: single-mesh files or name match with the GameObject
      if (scene->meshes.count == 1)
         return scene->meshes.data[0];
      for (size_t n = 0; n < scene->nodes.count; n++)
      {
         const ufbx_node* node = scene->nodes.data[n];
         const string name(node->name.data, node->name.length);
         if (node->mesh != nullptr && name == goName)
            return node->mesh;
      }
      return nullptr;
   }

   // ----- textures ------------------------------------------------------------------------------

   // Adds a Texture to the table, deduplicating by name+content (identical assets are shared,
   // name collisions with different content get renamed). Returns the final image name.
   string AddImage(Texture* tex, const string& wantedName, const std::filesystem::path& srcPath)
   {
      if (tex == nullptr)
      {
         Warn("failed to create image '" + wantedName + "' from '" + PathToUTF8(srcPath) + "'");
         return string();
      }
      if (Texture* const existing = m_table->GetImage(wantedName))
      {
         if (existing->HasSameContent(*tex))
         {
            delete tex;
            return wantedName;
         }
         string name = wantedName;
         for (int i = 1; m_table->GetImage(name); i++)
            name = wantedName + " (" + std::to_string(i) + ")";
         tex->m_name = name;
         m_table->m_vimage.push_back(tex);
         m_result.images++;
         return name;
      }
      tex->m_name = wantedName;
      m_table->m_vimage.push_back(tex);
      m_result.images++;
      return wantedName;
   }

   // Decodes an image file to a 32bpp FIBITMAP (caller unloads). nullptr + warning on failure.
   FIBITMAP* DecodeBitmap(const std::filesystem::path& path)
   {
      const string u8 = PathToUTF8(path);
      const FREE_IMAGE_FORMAT fif = FreeImage_GetFIFFromFilename(u8.c_str());
      FIBITMAP* src = (fif != FIF_UNKNOWN) ? FreeImage_Load(fif, u8.c_str()) : nullptr;
      FIBITMAP* dib = nullptr;
      if (src != nullptr)
      {
         dib = (FreeImage_GetBPP(src) == 32) ? src : FreeImage_ConvertTo32Bits(src);
         if (dib != src)
            FreeImage_Unload(src);
      }
      if (dib == nullptr)
         Warn("cannot decode image '" + u8 + "'");
      return dib;
   }

   // Encodes a 32bpp FIBITMAP to lossless WEBP and wraps it in a Texture (takes ownership of dib).
   Texture* SaveWebpTexture(FIBITMAP* dib, const string& name, const std::filesystem::path& path)
   {
      vector<uint8_t> data;
      FIMEMORY* mem = FreeImage_OpenMemory();
      if (mem != nullptr && FreeImage_SaveToMemory(FIF_WEBP, dib, mem, WEBP_LOSSLESS))
      {
         data.resize(FreeImage_TellMemory(mem));
         FreeImage_SeekMemory(mem, 0, SEEK_SET);
         FreeImage_ReadMemory(data.data(), 1, static_cast<unsigned>(data.size()), mem);
      }
      if (mem != nullptr)
         FreeImage_CloseMemory(mem);
      FreeImage_Unload(dib);
      if (data.empty())
      {
         Warn("failed to encode image '" + name + "'");
         return nullptr;
      }
      return Texture::CreateFromMemory(name, path, std::move(data));
   }

   // Repacks an HDRP MaskMap (R=metallic, G=AO, A=smoothness) into a VPX ORM image (R=AO, G=roughness, B=metallic).
   Texture* ConvertMaskMap(const std::filesystem::path& path, const string& name, float aoMin, float aoMax, float smoothMin, float smoothMax, float metalMin, float metalMax)
   {
      FIBITMAP* dib = DecodeBitmap(path);
      if (dib == nullptr)
         return nullptr;
      const unsigned w = FreeImage_GetWidth(dib), h = FreeImage_GetHeight(dib);
      FIBITMAP* out = FreeImage_AllocateT(FIT_BITMAP, w, h, 32, FI_RGBA_RED_MASK, FI_RGBA_GREEN_MASK, FI_RGBA_BLUE_MASK);
      if (out == nullptr)
      {
         FreeImage_Unload(dib);
         return nullptr;
      }
      for (unsigned y = 0; y < h; y++)
      {
         const BYTE* srow = FreeImage_GetScanLine(dib, y);
         BYTE* drow = FreeImage_GetScanLine(out, y);
         for (unsigned x = 0; x < w; x++, srow += 4, drow += 4)
         {
            const float metal = srow[FI_RGBA_RED] / 255.f;
            const float ao = srow[FI_RGBA_GREEN] / 255.f;
            const float smooth = srow[FI_RGBA_ALPHA] / 255.f;
            const float r = aoMin + (aoMax - aoMin) * ao;
            const float g = 1.f - (smoothMin + (smoothMax - smoothMin) * smooth);
            const float b = metalMin + (metalMax - metalMin) * metal;
            drow[FI_RGBA_RED] = static_cast<BYTE>(saturate(r) * 255.f + 0.5f);
            drow[FI_RGBA_GREEN] = static_cast<BYTE>(saturate(g) * 255.f + 0.5f);
            drow[FI_RGBA_BLUE] = static_cast<BYTE>(saturate(b) * 255.f + 0.5f);
            drow[FI_RGBA_ALPHA] = 255;
         }
      }
      FreeImage_Unload(dib);
      return SaveWebpTexture(out, name, path);
   }

   // Unity tangent-space normal maps are bound as-is: VPX's derivative-reconstructed tangent frame
   // convention differs from Unity's, but this is a renderer-side concern (see TBN_trafo) that the
   // importer must not paper over with channel swizzling.


   // ----- materials -----------------------------------------------------------------------------

   struct UnityMat
   {
      string name;
      bool hasBaseMap = false, hasNormalMap = false, hasMaskMap = false, hasEmissiveMap = false;
      std::filesystem::path baseMap, normalMap, maskMap, emissiveMap;
      float metallic = 0, smoothness = 0.5f, aoMin = 0, aoMax = 1, smoothMin = 0, smoothMax = 1, metalMin = 0, metalMax = 1;
      float baseR = 1, baseG = 1, baseB = 1, baseA = 1;
      float emisR = 0, emisG = 0, emisB = 0;
      float emissiveIntensity = 1;
      int useEmissiveIntensity = 0;
      float thickness = 0;
      float transR = 1, transG = 0, transB = 0;
      int surfaceType = 0, doubleSided = 0, alphaCutoffEnable = 0;
      float cutoff = 0.5f;
      int transmissionEnable = 0;
      float coatMask = 0;
   };

   bool LoadUnityMaterial(const string& guid, UnityMat& out) { return LoadUnityMaterialRec(guid, out, 0); }

   bool LoadUnityMaterialRec(const string& guid, UnityMat& out, int depth)
   {
      const auto it = m_guidToPath.find(guid);
      if (it == m_guidToPath.end())
      {
         Warn("unresolved material guid " + guid);
         return false;
      }
      if (depth > 4)
      {
         Warn("material variant chain too deep at '" + PathToUTF8(it->second) + "'");
         return false;
      }
      std::ifstream f(it->second, std::ios::binary);
      if (!f)
      {
         Warn("cannot open material '" + PathToUTF8(it->second) + "'");
         return false;
      }
      std::ostringstream ss;
      ss << f.rdbuf();
      vector<UnityYaml::Document> docs;
      try
      {
         docs = UnityYaml::Parse(ss.str());
      }
      catch (const std::exception& e)
      {
         Warn("YAML parse failed in '" + PathToUTF8(it->second) + "': " + e.what());
         return false;
      }
      if (docs.empty())
         return false;
      const UnityYaml::Value& mat = docs[0].root;
      // Material variants only serialize the properties they override (empty lists inherit
      // everything): resolve the m_Parent chain first, then apply local entries on top.
      UnityYaml::Ref parentRef;
      if (UnityYaml::AsRef(mat["m_Parent"], parentRef) && !parentRef.guid.empty() && parentRef.guid != guid && !LoadUnityMaterialRec(parentRef.guid, out, depth + 1))
         Warn("unresolved parent material guid " + parentRef.guid + " in '" + PathToUTF8(it->second) + "'");
      out.name = mat["m_Name"].AsString(PathToUTF8(it->second.stem()));
      const UnityYaml::Value& saved = mat["m_SavedProperties"];
      std::unordered_set<string> keywords;
      for (size_t i = 0; i < mat["m_ValidKeywords"].Size(); i++)
         keywords.insert(mat["m_ValidKeywords"].At(i).AsString());
      // texture bindings
      const auto texGuid = [&](const UnityYaml::Value& env, std::filesystem::path& dst) -> bool
      {
         UnityYaml::Ref r;
         if (!env.IsMap() || !UnityYaml::AsRef(env["m_Texture"], r) || r.guid.empty())
            return false;
         const auto gi = m_guidToPath.find(r.guid);
         if (gi == m_guidToPath.end())
            return false;
         dst = gi->second;
         return true;
      };
      for (size_t i = 0; i < saved["m_TexEnvs"].Size(); i++)
      {
         const UnityYaml::Value& env = saved["m_TexEnvs"].At(i);
         if (!env.IsMap())
            continue;
         const UnityYaml::Value& inner = env.map.begin()->second;
         const string key = env.map.begin()->first;
         if (key == "_BaseColorMap")
            out.hasBaseMap = texGuid(inner, out.baseMap);
         else if (key == "_NormalMap")
            out.hasNormalMap = texGuid(inner, out.normalMap);
         else if (key == "_MaskMap")
            out.hasMaskMap = texGuid(inner, out.maskMap);
         else if (key == "_EmissiveColorMap")
            out.hasEmissiveMap = texGuid(inner, out.emissiveMap);
      }
      for (size_t i = 0; i < saved["m_Floats"].Size(); i++)
      {
         const UnityYaml::Value& f = saved["m_Floats"].At(i);
         if (!f.IsMap())
            continue;
         const string key = f.map.begin()->first;
         const float v = static_cast<float>(f.map.begin()->second.AsFloat());
         if (key == "_Metallic")
            out.metallic = v;
         else if (key == "_Smoothness")
            out.smoothness = v;
         else if (key == "_AORemapMin")
            out.aoMin = v;
         else if (key == "_AORemapMax")
            out.aoMax = v;
         else if (key == "_SmoothnessRemapMin")
            out.smoothMin = v;
         else if (key == "_SmoothnessRemapMax")
            out.smoothMax = v;
         else if (key == "_MetallicRemapMin")
            out.metalMin = v;
         else if (key == "_MetallicRemapMax")
            out.metalMax = v;
         else if (key == "_SurfaceType")
            out.surfaceType = static_cast<int>(v);
         else if (key == "_DoubleSidedEnable")
            out.doubleSided = static_cast<int>(v);
         else if (key == "_AlphaCutoffEnable")
            out.alphaCutoffEnable = static_cast<int>(v);
         else if (key == "_AlphaCutoff" || key == "_Cutoff")
            out.cutoff = v;
         else if (key == "_Thickness")
            out.thickness = v;
         else if (key == "_CoatMask")
            out.coatMask = v;
         else if (key == "_EmissiveIntensity")
            out.emissiveIntensity = v;
         else if (key == "_UseEmissiveIntensity")
            out.useEmissiveIntensity = static_cast<int>(v);
         else if (key == "_TransmissionEnable")
            out.transmissionEnable = static_cast<int>(v);
      }
      for (size_t i = 0; i < saved["m_Colors"].Size(); i++)
      {
         const UnityYaml::Value& c = saved["m_Colors"].At(i);
         if (!c.IsMap())
            continue;
         const string key = c.map.begin()->first;
         float r, g, b, a;
         if (!UnityYaml::AsColor(c.map.begin()->second, r, g, b, a))
            continue;
         if (key == "_BaseColor" || key == "_Color")
         {
            out.baseR = r;
            out.baseG = g;
            out.baseB = b;
            out.baseA = a;
         }
         else if (key == "_EmissiveColor")
         {
            out.emisR = r;
            out.emisG = g;
            out.emisB = b;
         }
         else if (key == "_TransmittanceColor")
         {
            out.transR = r;
            out.transG = g;
            out.transB = b;
         }
      }
      (void)keywords;
      return true;
   }

   static uint8_t LinearToSrgb8(const float c)
   {
      const float s = (c <= 0.0031308f) ? c * 12.92f : 1.055f * powf(c, 1.f / 2.4f) - 0.055f;
      return static_cast<uint8_t>(saturate(s) * 255.f + 0.5f);
   }

   // Creates (or deduplicates) a PBR Material from a Unity .mat guid. Returns the final material name.
   string AddMaterial(const string& guid)
   {
      const auto cached = m_materialNames.find(guid);
      if (cached != m_materialNames.end())
         return cached->second;
      UnityMat um;
      string out;
      if (!LoadUnityMaterial(guid, um))
      {
         m_materialNames[guid] = string();
         return string();
      }

      // Textures
      string baseTex, normalTex, ormTex, emisTex;
      if (um.hasBaseMap)
         baseTex = AddImage(Texture::CreateFromFile(um.baseMap), PathToUTF8(um.baseMap.stem()), um.baseMap);
      if (um.hasNormalMap)
         normalTex = AddImage(Texture::CreateFromFile(um.normalMap), PathToUTF8(um.normalMap.stem()), um.normalMap);
      if (um.hasEmissiveMap)
         emisTex = AddImage(Texture::CreateFromFile(um.emissiveMap), PathToUTF8(um.emissiveMap.stem()), um.emissiveMap);
      if (um.hasMaskMap)
         ormTex = AddImage(ConvertMaskMap(um.maskMap, PathToUTF8(um.maskMap.stem()) + " - ORM", um.aoMin, um.aoMax, um.smoothMin, um.smoothMax, um.metalMin, um.metalMax),
            PathToUTF8(um.maskMap.stem()) + " - ORM", um.maskMap);

      Material* mat = new Material();
      mat->m_type = Material::PBR;
      mat->m_cBase = RGB(LinearToSrgb8(um.baseR), LinearToSrgb8(um.baseG), LinearToSrgb8(um.baseB));
      mat->m_bOpacityActive = um.surfaceType == 1;
      mat->m_fOpacity = um.surfaceType == 1 ? saturate(um.baseA) : 1.f;
      mat->m_fRoughness = um.hasMaskMap ? 1.f : 1.f - saturate(um.smoothness);
      mat->m_fMetallic = um.hasMaskMap ? 1.f : saturate(um.metallic);
      mat->m_fOcclusion = 1.f;
      mat->m_cEmission = RGB(LinearToSrgb8(um.emisR), LinearToSrgb8(um.emisG), LinearToSrgb8(um.emisB));
      const float emisMax = std::max({ um.emisR, um.emisG, um.emisB });
      mat->m_fEmissionIntensity = um.useEmissiveIntensity ? um.emissiveIntensity : std::max(1.f, emisMax);
      mat->m_fWrapLighting = um.transmissionEnable ? 0.5f : 0.f;
      mat->m_fThickness = um.thickness;
      mat->m_cRefractionTint = RGB(LinearToSrgb8(um.transR), LinearToSrgb8(um.transG), LinearToSrgb8(um.transB));
      mat->m_fEdge = 1.f;
      mat->m_fEdgeAlpha = 1.f;
      mat->m_fElasticity = 0.3f;
      mat->m_fElasticityFalloff = 0.f;
      mat->m_fFriction = 0.3f;
      mat->m_fScatterAngle = 0.f;
      mat->m_name = um.name;
      m_table->AddMaterial(mat); // uniquifies the name
      m_result.materials++;
      out = mat->m_name;

      m_materialNames[guid] = out;
      m_materialTextures[guid] = { baseTex, normalTex, ormTex, emisTex, um.alphaCutoffEnable ? um.cutoff : -1.f };
      return out;
   }

   std::unordered_map<string, string> m_materialNames; // mat guid -> table material name
   std::unordered_map<string, std::tuple<string, string, string, string, float>> m_materialTextures; // guid -> base/normal/orm/emis + alphaTest

   // ----- emit -----------------------------------------------------------------------------------

   void FillMesh(const URenderNode& node, ufbx_mesh* mesh, size_t materialSlot, Mesh& dst)
   {
      // Gather the faces of this material slot
      const bool useAll = materialSlot == SIZE_MAX || mesh->materials.count <= 1;
      dst.m_vertices.clear();
      dst.m_indices.clear();
      std::unordered_map<uint32_t, uint32_t> cornerToIndex;
      const auto emitCorner = [&](uint32_t ci)
      {
         const auto it = cornerToIndex.find(ci);
         if (it != cornerToIndex.end())
            return it->second;
         const ufbx_vec3 p = ufbx_get_vertex_vec3(&mesh->vertex_position, ci);
         ufbx_vec3 n { 0, 0, 1 };
         if (mesh->vertex_normal.exists)
            n = ufbx_get_vertex_vec3(&mesh->vertex_normal, ci);
         ufbx_vec2 uv { 0, 0 };
         if (mesh->vertex_uv.exists)
            uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, ci);
         const vec3 wp = node.world.TransformPoint(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z));
         const vec3 wn = node.world.TransformDir(static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z));
         const vec3 vp = UnityToVpxPos(wp.x, wp.y, wp.z);
         vec3 vn = UnityToVpxDir(wn.x, wn.y, wn.z);
         const float nl = sqrtf(vn.x * vn.x + vn.y * vn.y + vn.z * vn.z);
         if (nl > 0)
            vn = { vn.x / nl, vn.y / nl, vn.z / nl };
         Vertex3D_NoTex2 v;
         v.x = vp.x;
         v.y = vp.y;
         v.z = vp.z;
         v.nx = vn.x;
         v.ny = vn.y;
         v.nz = vn.z;
         v.tu = static_cast<float>(uv.x);
         v.tv = kFlipV ? 1.f - static_cast<float>(uv.y) : static_cast<float>(uv.y);
         const uint32_t idx = static_cast<uint32_t>(dst.m_vertices.size());
         dst.m_vertices.push_back(v);
         cornerToIndex[ci] = idx;
         return idx;
      };

      vector<uint32_t> tri;
      for (size_t fi = 0; fi < mesh->faces.count; fi++)
      {
         if (!useAll && (fi >= mesh->face_material.count || (size_t)mesh->face_material.data[fi] != materialSlot))
            continue;
         const ufbx_face face = mesh->faces.data[fi];
         tri.resize(face.num_indices * 3); // triangulated output is 3*(n-2) indices
         const uint32_t nIdx = ufbx_triangulate_face(tri.data(), tri.size(), mesh, face) * 3; // returns a triangle count
         for (uint32_t i = 0; i + 2 < nIdx; i += 3)
         {
            const uint32_t a = emitCorner(tri[i]);
            const uint32_t b = emitCorner(tri[i + 1]);
            const uint32_t c = emitCorner(tri[i + 2]);
            if (kFlipWinding)
            {
               dst.m_indices.push_back(a);
               dst.m_indices.push_back(c);
               dst.m_indices.push_back(b);
            }
            else
            {
               dst.m_indices.push_back(a);
               dst.m_indices.push_back(b);
               dst.m_indices.push_back(c);
            }
         }
      }
      if (dst.m_vertices.empty() || dst.m_indices.empty())
         return;
      if (!mesh->vertex_normal.exists)
         ComputeNormals(dst.m_vertices, dst.m_indices);
   }

   Primitive* EmitPrimitive(const string& name, const string& materialName, const string* texNames, PartGroup* group)
   {
      IEditable* const pie = EditableRegistry::CreateAndInit(eItemPrimitive, m_table, 0.f, 0.f);
      if (pie == nullptr)
         return nullptr;
      Primitive* const prim = static_cast<Primitive*>(pie);
      string n = name;
      m_table->GetUniqueName(eItemPrimitive, n);
      pie->GetIScriptable()->m_name = n;
      prim->m_d.m_use3DMesh = true;
      prim->m_d.m_vPosition = { 0.f, 0.f, 0.f };
      prim->m_d.m_vSize = { 1.f, 1.f, 1.f };
      memset(prim->m_d.m_aRotAndTra, 0, sizeof(prim->m_d.m_aRotAndTra));
      prim->m_d.m_szMaterial = materialName;
      prim->m_d.m_szImage = texNames[0];
      prim->m_d.m_szNormalMap = texNames[1];
      prim->m_d.m_szORMMap = texNames[2];
      prim->m_d.m_szEmissionMap = texNames[3];
      prim->m_d.m_collidable = true;
      prim->m_d.m_hitEvent = false;
      prim->m_d.m_visible = true;
      prim->m_d.m_staticRendering = true;
      prim->m_d.m_toy = false;
      prim->m_d.m_backfacesEnabled = false; // set by caller from material flags
      prim->m_d.m_drawTexturesInside = true;
      prim->m_d.m_alpha = 100.f;
      m_table->AddPart(pie);
      pie->SetPartGroup(group);
      return prim;
   }

   UnityImportResult Run(const std::filesystem::path& prefabPath, PartGroup* targetGroup, vector<IEditable*>& created)
   {
      m_created = &created;
      if (!BuildGuidIndex(prefabPath))
      {
         m_result.error = "could not index any .meta files above '" + PathToUTF8(prefabPath) + "' (is it inside a Unity assets tree?)";
         return m_result;
      }

      vector<URenderNode> nodes;
      CollectRenderNodes(prefabPath, Mat4::Identity(), nodes, 0);
      if (nodes.empty())
      {
         m_result.error = "no renderable mesh nodes found in '" + PathToUTF8(prefabPath) + "'";
         return m_result;
      }

      vector<Primitive*> prims;

      // Part group for multi-node prefabs
      PartGroup* group = targetGroup;
      if (nodes.size() > 1)
      {
         IEditable* const pie = EditableRegistry::CreateAndInit(eItemPartGroup, m_table, 0.f, 0.f);
         if (pie != nullptr)
         {
            group = static_cast<PartGroup*>(pie);
            string gname = PathToUTF8(prefabPath.stem());
            m_table->GetUniqueName(eItemPartGroup, gname);
            pie->GetIScriptable()->m_name = gname;
            m_table->AddPart(pie);
            pie->SetPartGroup(targetGroup);
            created.push_back(pie);
            m_result.partGroups++;
         }
      }

      for (URenderNode& node : nodes)
      {
         ufbx_scene* scene = GetFbx(node.fbxGuid);
         if (scene == nullptr)
            continue;
         ufbx_mesh* mesh = FindMesh(scene, node.meshFileId, node.name);
         if (mesh == nullptr)
         {
            Warn("mesh fileID " + std::to_string(node.meshFileId) + " not found in '" + PathToUTF8(m_guidToPath[node.fbxGuid]) + "' for '" + node.name + "'");
            continue;
         }

         // Emit one primitive per material slot (single primitive when no materials)
         const size_t slots = std::max<size_t>(mesh->materials.count, 1);
         for (size_t slot = 0; slot < slots; slot++)
         {
            // Unity's m_Materials[i] binds to the i-th submesh in first-face-usage order (FBX SDK
            // convention, exposed by ufbx as material_part_usage_order), not in materials order.
            const size_t matIndex = slot < mesh->material_part_usage_order.count ? mesh->material_part_usage_order.data[slot] : slot;
            string materialName;
            string texNames[4] = { "", "", "", "" };
            if (slot < node.materialGuids.size() && !node.materialGuids[slot].empty())
            {
               materialName = AddMaterial(node.materialGuids[slot]);
               const auto tit = m_materialTextures.find(node.materialGuids[slot]);
               if (tit != m_materialTextures.end())
               {
                  texNames[0] = std::get<0>(tit->second);
                  texNames[1] = std::get<1>(tit->second);
                  texNames[2] = std::get<2>(tit->second);
                  texNames[3] = std::get<3>(tit->second);
               }
            }
            Mesh dstMesh;
            FillMesh(node, mesh, (mesh->materials.count > 1) ? matIndex : SIZE_MAX, dstMesh);
            if (dstMesh.m_vertices.empty())
            {
               Warn("mesh for '" + node.name + "' produced no geometry (" + std::to_string(mesh->faces.count) + " faces)");
               continue;
            }
            const string partName = (slots > 1) ? node.name + " (" + std::to_string(slot) + ")" : node.name;
            Primitive* const prim = EmitPrimitive(partName, materialName, texNames, group);
            if (prim == nullptr)
               continue;
            prim->m_mesh = std::move(dstMesh);
            created.push_back(prim);
            prims.push_back(prim);
            m_result.parts++;
         }
      }

      for (auto& [guid, scene] : m_fbxCache)
         if (scene != nullptr)
            ufbx_free_scene(scene);
      m_fbxCache.clear();

      if (m_result.parts == 0)
      {
         m_result.error = "no primitives could be emitted (see warnings)";
         return m_result;
      }

      // Default drop point: middle of the playfield, 3/4 of the way toward the player — the open
      // area between the slings on a standard layout. The imported content is re-centered on it
      // horizontally and rests on the playfield surface, whatever the prefab's authored pivot is.
      const Vertex3Ds drop(m_table->m_left + (m_table->m_right - m_table->m_left) * 0.5f, m_table->m_top + (m_table->m_bottom - m_table->m_top) * 0.75f,
         0.f);
      Vertex3Ds bmin(FLT_MAX, FLT_MAX, FLT_MAX), bmax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
      for (const Primitive* const prim : prims)
         for (const Vertex3D_NoTex2& v : prim->m_mesh.m_vertices)
         {
            bmin.x = min(bmin.x, v.x);
            bmin.y = min(bmin.y, v.y);
            bmin.z = min(bmin.z, v.z);
            bmax.x = max(bmax.x, v.x);
            bmax.y = max(bmax.y, v.y);
            bmax.z = max(bmax.z, v.z);
         }
      const Vertex3Ds anchor((bmin.x + bmax.x) * 0.5f, (bmin.y + bmax.y) * 0.5f, bmin.z);
      for (Primitive* const prim : prims)
      {
         for (Vertex3D_NoTex2& v : prim->m_mesh.m_vertices)
         {
            v.x -= anchor.x;
            v.y -= anchor.y;
            v.z -= anchor.z;
         }
         prim->m_d.m_vPosition = drop;
      }

      m_result.success = true;
      return m_result;
   }
};

} // anonymous namespace

UnityImportResult UnityImportPrefab(PinTable* table, const std::filesystem::path& path, PartGroup* targetGroup, vector<IEditable*>& createdParts)
{
   Importer importer(table);
   try
   {
      return importer.Run(path, targetGroup, createdParts);
   }
   catch (const std::exception& e)
   {
      UnityImportResult res;
      res.error = e.what();
      return res;
   }
}
