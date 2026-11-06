// license:GPLv3+

#include "core/stdafx.h"
#include "../vpx-test.h"

#include "parts/pintable.h"
#include "parts/primitive.h"
#include "parts/Material.h"
#include "renderer/Texture.h"
#include "utils/UnityPrefabImporter.h"
#include "utils/UnityYaml.h"

#include <FreeImage.h>
#include <fstream>
#include "ufbx/ufbx.h"

#include "doctest.h"

namespace
{

void WriteFile(const std::filesystem::path& path, const string& content)
{
   std::filesystem::create_directories(path.parent_path());
   std::ofstream f(path, std::ios::binary | std::ios::trunc);
   f << content;
}

// Writes a solid color RGBA png through FreeImage
void WritePng(const std::filesystem::path& path, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
   std::filesystem::create_directories(path.parent_path());
   FIBITMAP* dib = FreeImage_AllocateT(FIT_BITMAP, 4, 4, 32, FI_RGBA_RED_MASK, FI_RGBA_GREEN_MASK, FI_RGBA_BLUE_MASK);
   REQUIRE(dib != nullptr);
   for (unsigned y = 0; y < 4; y++)
   {
      BYTE* row = FreeImage_GetScanLine(dib, y);
      for (unsigned x = 0; x < 4; x++, row += 4)
      {
         row[FI_RGBA_RED] = r;
         row[FI_RGBA_GREEN] = g;
         row[FI_RGBA_BLUE] = b;
         row[FI_RGBA_ALPHA] = a;
      }
   }
   FreeImage_Save(FIF_PNG, dib, PathToUTF8(path).c_str(), PNG_DEFAULT);
   FreeImage_Unload(dib);
}

// A minimal ASCII FBX: one "Cube" model node with a quad-mesh. Vertex 2 is pulled out to make the
// shape asymmetric, so that handedness conversion (Unity mirrors X when importing FBX) is testable.
const char* kCubeFbx = R"FBX(
FBXHeaderExtension: {
   FBXHeaderVersion: 1003
   FBXVersion: 7400
}
GlobalSettings: {
   Version: 1000
   Properties70: {
      P: "UnitScaleFactor", "double", "Number", "",100
      P: "UpAxis", "int", "Integer", "",1
      P: "UpAxisSign", "int", "Integer", "",1
      P: "FrontAxis", "int", "Integer", "",2
      P: "FrontAxisSign", "int", "Integer", "",1
      P: "CoordAxis", "int", "Integer", "",0
      P: "CoordAxisSign", "int", "Integer", "",1
   }
}
Objects: {
   Geometry: 1000001, "Geometry::", "Mesh" {
      Vertices: *24 {
         a: -0.5,-0.5,-0.5,0.5,-0.5,-0.5,0.9,0.5,-0.5,-0.5,0.5,-0.5,-0.5,-0.5,0.5,0.5,-0.5,0.5,0.5,0.5,0.5,-0.5,0.5,0.5
      }
      PolygonVertexIndex: *24 {
         a: 0,3,2,-2,4,5,6,-8,0,1,5,-5,1,2,6,-6,2,3,7,-7,3,0,4,-8
      }
      LayerElementUV: 0 {
         MappingInformationType: "ByPolygonVertex"
         ReferenceInformationType: "IndexToDirect"
         UV: *8 { a: 0,0,1,0,1,1,0,1 }
         UVIndex: *24 { a: 0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3 }
      }
      LayerElementMaterial: 0 {
         MappingInformationType: "ByPolygon"
         ReferenceInformationType: "IndexToDirect"
         Materials: *6 { a: 1,1,1,1,1,0 }
      }
   }
   Material: 1000003, "Material::MatA", "" {
   }
   Material: 1000004, "Material::MatB", "" {
   }
   Model: 1000002, "Model::Cube", "Mesh" {
      Version: 232
   }
}
Connections: {
   C: "OO",1000001,1000002
   C: "OO",1000003,1000002
   C: "OO",1000004,1000002
   C: "OO",1000002,0
}
)FBX";

} // namespace

TEST_CASE("UnityYaml")
{
   const string doc = R"YAML(
%YAML 1.1
%TAG !u! tag:unity3d.com,2011:
--- !u!1 &12345
GameObject:
  m_Name: Foo Bar (baz)
  m_IsActive: 1
  m_Component:
  - component: {fileID: 222, guid: abc, type: 3}
--- !u!4 &222
Transform:
  m_LocalPosition: {x: 1.5, y: -2, z: 3e2}
  m_Children: []
  m_Modifications:
  - target: {fileID: 1}
    propertyPath: m_LocalPosition.x
    value: 4.25
)YAML";
   const vector<UnityYaml::Document> docs = UnityYaml::Parse(doc);
   REQUIRE(docs.size() == 2);
   CHECK(docs[0].classId == 1);
   CHECK(docs[0].fileId == 12345);
   CHECK(docs[0].rootName == "GameObject");
   CHECK(docs[0].root["m_Name"].AsString() == "Foo Bar (baz)");
   UnityYaml::Ref r;
   CHECK(UnityYaml::AsRef(docs[0].root["m_Component"].At(0)["component"], r));
   CHECK(r.fileID == 222);
   CHECK(r.guid == "abc");
   float x, y, z;
   CHECK(UnityYaml::AsVec3(docs[1].root["m_LocalPosition"], x, y, z));
   CHECK(x == doctest::Approx(1.5f));
   CHECK(z == doctest::Approx(300.f));
   CHECK(docs[1].root["m_Modifications"].At(0)["value"].AsFloat() == doctest::Approx(4.25));
   CHECK(docs[1].root["m_Children"].IsList());
   // meta file (tagless)
   CHECK(UnityYaml::MetaGuid("fileFormatVersion: 2\nguid: 0123456789abcdef0123456789abcdef\nfoo: 1\n") == "0123456789abcdef0123456789abcdef");
}

TEST_CASE("Unity prefab import")
{
   // Synthesized Unity asset tree (ASCII FBX + YAML + PNG textures)
   const std::filesystem::path root = GetAssetPath() / "unity-fixture";
   std::filesystem::remove_all(root);
   const std::filesystem::path dir = root / "Assets" / "Test";

   const string fbxGuid = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1";
   const string matGuid = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb2";
   const string baseMatGuid = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb6";
   const string altMatGuid = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb7";
   const string baseGuid = "ccccccccccccccccccccccccccccccc3";
   const string maskGuid = "ddddddddddddddddddddddddddddddd4";
   const string normalGuid = "ddddddddddddddddddddddddddddd8";
   const string prefabGuid = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeee5";

   WriteFile(dir / "cube.fbx", kCubeFbx);
   WriteFile(dir / "cube.fbx.meta", "fileFormatVersion: 2\nguid: " + fbxGuid + "\n");
   WritePng(dir / "BaseMap.png", 200, 30, 30, 255);
   WriteFile(dir / "BaseMap.png.meta", "fileFormatVersion: 2\nguid: " + baseGuid + "\n");
   WritePng(dir / "MaskMap.png", 255, 128, 0, 204); // metal=1, ao=0.5, smooth=0.8
   WriteFile(dir / "MaskMap.png.meta", "fileFormatVersion: 2\nguid: " + maskGuid + "\n");
   WritePng(dir / "Normal.png", 128, 200, 128, 255); // tangent-space normal, OpenGL green up
   WriteFile(dir / "Normal.png.meta", "fileFormatVersion: 2\nguid: " + normalGuid + "\n");
   // base.mat holds all shader properties; test.mat is a material variant (m_Parent) which only
   // serializes overridden properties — its texture/float lists are empty and inherit the parent's.
   WriteFile(dir / "base.mat",
      "%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n"
      "--- !u!21 &2100000\n"
      "Material:\n"
      "  m_Name: basemat\n"
      "  m_ValidKeywords:\n"
      "  - _MASKMAP\n"
      "  m_SavedProperties:\n"
      "    m_TexEnvs:\n"
      "    - _BaseColorMap:\n"
      "        m_Texture: {fileID: 2800000, guid: "
         + baseGuid
         + ", type: 3}\n"
           "    - _MaskMap:\n"
           "        m_Texture: {fileID: 2800000, guid: "
         + maskGuid
         + ", type: 3}\n"
           "    - _NormalMap:\n"
           "        m_Texture: {fileID: 2800000, guid: "
         + normalGuid
         + ", type: 3}\n"
           "    m_Floats:\n"
           "    - _Metallic: 0.9\n"
           "    - _Smoothness: 0.8\n"
           "    - _SurfaceType: 0\n"
           "    m_Colors:\n"
           "    - _BaseColor: {r: 0, g: 1, b: 0, a: 1}\n");
   WriteFile(dir / "base.mat.meta", "fileFormatVersion: 2\nguid: " + baseMatGuid + "\n");
   WriteFile(dir / "test.mat",
      "%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n"
      "--- !u!21 &2100000\n"
      "Material:\n"
      "  m_Name: testmat\n"
      "  m_Parent: {fileID: 2100000, guid: "
         + baseMatGuid
         + ", type: 2}\n"
           "  m_SavedProperties:\n"
           "    m_TexEnvs: []\n"
           "    m_Floats: []\n"
           "    m_Colors:\n"
           "    - _BaseColor: {r: 1, g: 0, b: 0, a: 1}\n");
   WriteFile(dir / "test.mat.meta", "fileFormatVersion: 2\nguid: " + matGuid + "\n");
   WriteFile(dir / "alt.mat",
      "%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n"
      "--- !u!21 &2100000\n"
      "Material:\n"
      "  m_Name: altmat\n"
      "  m_SavedProperties:\n"
      "    m_Colors:\n"
      "    - _BaseColor: {r: 0, g: 0, b: 1, a: 1}\n");
   WriteFile(dir / "alt.mat.meta", "fileFormatVersion: 2\nguid: " + altMatGuid + "\n");
   // fileID for mesh "Cube" = XXH64("Type:Mesh->Cube0") = -5495902117074765545
   WriteFile(dir / "test.prefab",
      "%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n"
      "--- !u!1 &1001\n"
      "GameObject:\n"
      "  m_Component:\n"
      "  - component: {fileID: 1002}\n"
      "  - component: {fileID: 1003}\n"
      "  - component: {fileID: 1004}\n"
      "  m_Name: Cube\n"
      "  m_IsActive: 1\n"
      "--- !u!4 &1002\n"
      "Transform:\n"
      "  m_GameObject: {fileID: 1001}\n"
      "  m_LocalRotation: {x: 0, y: 0, z: 0, w: 1}\n"
      "  m_LocalPosition: {x: 0, y: 0, z: 0}\n"
      "  m_LocalScale: {x: 1, y: 1, z: 1}\n"
      "  m_Father: {fileID: 0}\n"
      "--- !u!33 &1003\n"
      "MeshFilter:\n"
      "  m_GameObject: {fileID: 1001}\n"
      "  m_Mesh: {fileID: -5495902117074765545, guid: "
         + fbxGuid
         + ", type: 3}\n"
           "--- !u!23 &1004\n"
           "MeshRenderer:\n"
           "  m_GameObject: {fileID: 1001}\n"
           "  m_Enabled: 1\n"
           "  m_Materials:\n"
           "  - {fileID: 2100000, guid: "
         + matGuid
         + ", type: 2}\n"
           "  - {fileID: 2100000, guid: "
         + altMatGuid + ", type: 2}\n");
   WriteFile(dir / "test.prefab.meta", "fileFormatVersion: 2\nguid: " + prefabGuid + "\n");

   CComObject<PinTable>* table = nullptr;
   CComObject<PinTable>::CreateInstance(&table);
   REQUIRE(table != nullptr);
   table->AddRef();
   table->m_right = 952.5f; // standard playfield size in VPU
   table->m_bottom = 1905.f;

   vector<IEditable*> created;
   const UnityImportResult res = UnityImportPrefab(table, dir / "test.prefab", nullptr, created);

   INFO("error: ", res.error);
   for (const string& w : res.warnings)
      MESSAGE("warning: ", w);
   REQUIRE(res.success);
   CHECK(res.parts == 2); // one primitive per material slot
   CHECK(res.materials == 2);
   CHECK(res.images == 3); // base map + ORM + normal map, only testmat has textures
   REQUIRE(created.size() == 2);
   Primitive* prim = static_cast<Primitive*>(created[0]);
   Primitive* prim2 = static_cast<Primitive*>(created[1]);

   // The FBX material layer uses index 1 for all but the last face: Unity's m_Materials order is
   // by first-face usage, so testmat (slot 0) must bind to the 5-face part, altmat to the 1-face
   // part (material-layer order, not FBX connection order, decides the binding).
   CHECK(prim->m_d.m_szMaterial == "testmat"s);
   CHECK(prim2->m_d.m_szMaterial == "altmat"s);
   CHECK(prim->m_mesh.m_indices.size() == 5 * 6); // 5 quads -> 10 triangles
   CHECK(prim2->m_mesh.m_indices.size() == 6); // 1 quad -> 2 triangles

   CHECK(prim->m_mesh.m_vertices.size() > 0);
   CHECK(prim->m_d.m_szORMMap.empty() == false);
   CHECK(prim->m_d.m_szImage.empty() == false);
   CHECK(prim->m_d.m_szNormalMap.empty() == false);
   CHECK(prim2->m_d.m_szImage.empty());
   CHECK(prim2->m_d.m_szORMMap.empty());
   CHECK(prim2->m_d.m_szNormalMap.empty());

   // The imported normal map is bound as-is (raw PNG bytes)
   Texture* const nmap = table->GetImage(prim->m_d.m_szNormalMap);
   REQUIRE(nmap != nullptr);

   // Default placement: parts are dropped centered at mid-playfield x, 3/4 toward the player,
   // resting on the playfield (world-space bounds: min z = 0, xy bounds centered on the drop point)
   const Vertex3Ds& pos = prim->m_d.m_vPosition;
   CHECK(pos.x == doctest::Approx(952.5f * 0.5f));
   CHECK(pos.y == doctest::Approx(1905.f * 0.75f));
   CHECK(pos.z == doctest::Approx(0.f));
   CHECK(prim2->m_d.m_vPosition == pos);
   float wminx = FLT_MAX, wminy = FLT_MAX, wminz = FLT_MAX, wmaxx = -FLT_MAX, wmaxy = -FLT_MAX, wmaxz = -FLT_MAX;
   for (const Primitive* p : { prim, prim2 })
      for (const Vertex3D_NoTex2& v : p->m_mesh.m_vertices)
      {
         wminx = min(wminx, pos.x + v.x);
         wminy = min(wminy, pos.y + v.y);
         wminz = min(wminz, pos.z + v.z);
         wmaxx = max(wmaxx, pos.x + v.x);
         wmaxy = max(wmaxy, pos.y + v.y);
         wmaxz = max(wmaxz, pos.z + v.z);
      }
   CHECK(wminz == doctest::Approx(0.f).epsilon(1e-4));
   CHECK((wminx + wmaxx) * 0.5f == doctest::Approx(pos.x));
   CHECK((wminy + wmaxy) * 0.5f == doctest::Approx(pos.y));
   // scale sanity: VPU magnitudes (1m wedge => ~1852 VPU tall)
   CHECK(wmaxz - wminz == doctest::Approx(MTOVPU(1.0)));
   // Unity's FBX handedness conversion mirrors X: the fixture's lone +X bulge vertex must land on
   // -X. Extents around the bounds-centered drop point are symmetric by construction, so check the
   // vertex count at each extreme instead: mirrored => few verts at min-x (the bulge), many at
   // max-x (the original -X quad face). UV seams split vertices, so compare counts rather than
   // asserting exact ones.
   int nMinX = 0, nMaxX = 0;
   for (const Primitive* p : { prim, prim2 })
      for (const Vertex3D_NoTex2& v : p->m_mesh.m_vertices)
      {
         if (pos.x + v.x == doctest::Approx(wminx))
            nMinX++;
         if (pos.x + v.x == doctest::Approx(wmaxx))
            nMaxX++;
      }
   CHECK(nMinX < nMaxX);

   // Faces must be front-facing: the geometric (right handed) normal of each triangle must point
   // away from the part's centroid (inverted meshes would point inward)
   Vertex3Ds center(0.f, 0.f, 0.f);
   size_t totalVerts = 0;
   for (const Primitive* p : { prim, prim2 })
      for (const Vertex3D_NoTex2& v : p->m_mesh.m_vertices)
      {
         center += Vertex3Ds(v.x, v.y, v.z);
         totalVerts++;
      }
   center /= static_cast<float>(totalVerts);
   int outward = 0, inward = 0;
   for (const Primitive* p : { prim, prim2 })
      for (size_t i = 0; i + 2 < p->m_mesh.m_indices.size(); i += 3)
      {
         const Vertex3D_NoTex2& a = p->m_mesh.m_vertices[p->m_mesh.m_indices[i]];
         const Vertex3D_NoTex2& b = p->m_mesh.m_vertices[p->m_mesh.m_indices[i + 1]];
         const Vertex3D_NoTex2& c = p->m_mesh.m_vertices[p->m_mesh.m_indices[i + 2]];
         const Vertex3Ds e0(b.x - a.x, b.y - a.y, b.z - a.z), e1(c.x - a.x, c.y - a.y, c.z - a.z);
         const Vertex3Ds n = CrossProduct(e0, e1);
         if (n.x * (a.x - center.x) + n.y * (a.y - center.y) + n.z * (a.z - center.z) > 0.f)
            outward++;
         else
            inward++;
      }
   CHECK(outward > inward);

   Material* mat = table->GetMaterial(prim->m_d.m_szMaterial);
   REQUIRE(mat != nullptr);
   CHECK(mat->m_type == Material::PBR);
   CHECK(mat->m_fMetallic == doctest::Approx(1.f));
   CHECK(mat->m_fRoughness == doctest::Approx(1.f));
   CHECK(mat->m_cBase == RGB(255, 0, 0));
   CHECK(!mat->m_bOpacityActive);

   const Material* altMat = table->GetMaterial(prim2->m_d.m_szMaterial);
   REQUIRE(altMat != nullptr);
   CHECK(altMat->m_cBase == RGB(0, 0, 255));

   table->Release();
}
