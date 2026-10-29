// license:GPLv3+

#include "core/stdafx.h"
#include "../vpx-test.h"

#include "parts/pintable.h"

#include "doctest.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

// Police Force PBR conversion tooling (local, not a regular test): these cases only run when their environment
// variables are set, and are skipped otherwise.
//
//   PF_SRC_VPX=<table.vpx> PF_PACK_DIR=<folder.vpz> vpx-test -tc='PF pack convert*'
//
// Loads the table, saves it as a folder pack, reloads the pack, saves it again to <PF_PACK_DIR>.2 and checks that
// both packs are identical apart from the date/revision fields, and that the part counts survive the round trip.

static std::string EnvStr(const char* name)
{
   const char* const v = std::getenv(name);
   return v ? std::string(v) : std::string();
}

// Remove the volatile fields (save date and revision) from a pack document
static void StripVolatile(nlohmann::ordered_json& j)
{
   if (j.is_object())
   {
      for (auto it = j.begin(); it != j.end();)
      {
         if (it.key() == "save_date" || it.key() == "date_saved" || it.key() == "save_rev")
            it = j.erase(it);
         else
         {
            StripVolatile(it.value());
            ++it;
         }
      }
   }
   else if (j.is_array())
      for (auto& e : j)
         StripVolatile(e);
}

static std::string ReadAll(const std::filesystem::path& p)
{
   std::ifstream f(p, std::ios::binary);
   std::stringstream ss;
   ss << f.rdbuf();
   return ss.str();
}

// Two GLBs are the same mesh if their structure is identical and their float buffers match to a tolerance: the vertices
// go VPX units -> metres on save and back on load, so a second save differs by about 1 ulp.
static bool SameGlb(const std::string& a, const std::string& b)
{
   if (a == b)
      return true;
   if (a.size() < 28 || b.size() < 28)
      return false;
   auto u32 = [](const std::string& s, size_t o) { uint32_t v; memcpy(&v, s.data() + o, 4); return v; };
   const uint32_t ja = u32(a, 12), jb = u32(b, 12);
   if (20 + (size_t)ja > a.size() || 20 + (size_t)jb > b.size())
      return false;
   nlohmann::json docA = nlohmann::json::parse(a.substr(20, ja), nullptr, false);
   nlohmann::json docB = nlohmann::json::parse(b.substr(20, jb), nullptr, false);
   if (docA.is_discarded() || docB.is_discarded() || !docA.contains("accessors") || !docB.contains("accessors"))
      return false;
   for (auto* d : { &docA, &docB })
      for (auto& acc : (*d)["accessors"])
      {
         acc.erase("min");
         acc.erase("max");
      }
   if (docA != docB)
      return false;
   const size_t binA = 20 + ja + 8, binB = 20 + jb + 8; // BIN chunk data
   if (binA > a.size() || binB > b.size() || a.size() - binA != b.size() - binB)
      return false;
   for (const auto& acc : docA["accessors"])
   {
      const auto& bv = docA["bufferViews"][acc["bufferView"].get<int>()];
      const size_t off = binA + bv.value("byteOffset", 0) + acc.value("byteOffset", 0);
      const int comps = acc["type"] == "VEC3" ? 3 : acc["type"] == "VEC2" ? 2 : acc["type"] == "VEC4" ? 4 : 1;
      const size_t count = (size_t)acc["count"].get<int>() * comps;
      const size_t width = acc["componentType"].get<int>() == 5126 ? 4 : acc["componentType"].get<int>() == 5125 ? 4 : 2;
      const size_t offB = off - binA + binB;
      if (off + count * width > a.size())
         return false;
      if (acc["componentType"].get<int>() != 5126)
      {
         if (memcmp(a.data() + off, b.data() + offB, count * width) != 0)
            return false;
         continue;
      }
      for (size_t i = 0; i < count; i++)
      {
         float x, y;
         memcpy(&x, a.data() + off + i * 4, 4);
         memcpy(&y, b.data() + offB + i * 4, 4);
         if (fabsf(x - y) > 1e-6f * fmaxf(1.f, fmaxf(fabsf(x), fabsf(y))))
            return false;
      }
   }
   return true;
}

static std::map<std::string, std::string> PackFiles(const std::filesystem::path& root)
{
   std::map<std::string, std::string> files;
   for (const auto& e : std::filesystem::recursive_directory_iterator(root))
      if (e.is_regular_file())
         files[std::filesystem::relative(e.path(), root).generic_string()] = e.path().string();
   return files;
}

TEST_CASE("PF pack convert [pf][tool]")
{
   const std::string src = EnvStr("PF_SRC_VPX");
   const std::string packDir = EnvStr("PF_PACK_DIR");
   if (src.empty() || packDir.empty())
      return;

   std::error_code ec;
   const std::filesystem::path pack1 = packDir;
   const std::filesystem::path pack2 = packDir + ".2";
   std::filesystem::remove_all(pack1, ec);
   std::filesystem::remove_all(pack2, ec);
   REQUIRE(std::filesystem::create_directories(pack1, ec));
   REQUIRE(std::filesystem::create_directories(pack2, ec));

   CComObject<PinTable>* table;
   CComObject<PinTable>::CreateInstance(&table);
   table->AddRef();
   TestFileFeedback feedback;
   REQUIRE(SUCCEEDED(table->LoadGameFromFilename(std::filesystem::path(src), feedback)));

   std::map<int, int> srcCounts;
   for (IEditable* part : table->GetParts())
      srcCounts[(int)part->GetItemType()]++;
   const size_t srcParts = table->GetParts().size();
   INFO("source table parts: " << srcParts << ", images " << table->m_vimage.size() << ", materials " << table->m_materials.size());

   TestFileFeedback save1;
   REQUIRE(SUCCEEDED(table->SaveToJSON(pack1, save1)));

   CComObject<PinTable>* reloaded;
   CComObject<PinTable>::CreateInstance(&reloaded);
   reloaded->AddRef();
   TestFileFeedback load1;
   REQUIRE(SUCCEEDED(reloaded->LoadGameFromFilename(pack1, load1)));

   std::map<int, int> packCounts;
   for (IEditable* part : reloaded->GetParts())
      packCounts[(int)part->GetItemType()]++;
   CHECK(reloaded->GetParts().size() == srcParts);
   CHECK(packCounts == srcCounts);
   CHECK(reloaded->m_vimage.size() == table->m_vimage.size());
   CHECK(reloaded->m_materials.size() == table->m_materials.size());

   TestFileFeedback save2;
   REQUIRE(SUCCEEDED(reloaded->SaveToJSON(pack2, save2)));

   const auto f1 = PackFiles(pack1);
   const auto f2 = PackFiles(pack2);
   CHECK(f1.size() == f2.size());
   int differing = 0;
   for (const auto& [rel, path1] : f1)
   {
      const auto it = f2.find(rel);
      if (it == f2.end())
      {
         MESSAGE("missing in second pack: " << rel);
         differing++;
         continue;
      }
      bool same;
      if (std::filesystem::path(rel).extension() == ".json")
      {
         nlohmann::ordered_json a = nlohmann::ordered_json::parse(ReadAll(path1), nullptr, false);
         nlohmann::ordered_json b = nlohmann::ordered_json::parse(ReadAll(it->second), nullptr, false);
         StripVolatile(a);
         StripVolatile(b);
         same = a.dump() == b.dump();
      }
      else if (std::filesystem::path(rel).extension() == ".glb")
         same = SameGlb(ReadAll(path1), ReadAll(it->second));
      else
         same = ReadAll(path1) == ReadAll(it->second);
      if (!same)
      {
         if (differing < 20)
            MESSAGE("differs: " << rel);
         differing++;
      }
   }
   CHECK(differing == 0);
   MESSAGE("PF pack: " << f1.size() << " files, " << srcParts << " parts, differing " << differing);
}

// ---------------------------------------------------------------------------------------------------------------------
// T3a first look: PBR playfield, constant PBR materials on the BM_ meshes, PBR GI lights, Traced starburst inserts.
//
//   PF_PACK_DIR=<pack in> PF_OUT_DIR=<pack out> PF_T3A=<assets folder> PF_BENCH=<bench folder> PF_BAKE=<l63 bake folder>
//   PF_INSERTS_CSV=<inserts.csv> vpx-test -tc='PF first look build*'
//
// The renders then load the saved pack and apply one variant (HDRI, env level, GI, camera) in memory, see 'PF first look render'.
// ---------------------------------------------------------------------------------------------------------------------

#include "parts/Material.h"
#include "parts/flasher.h"
#include "parts/light.h"
#include "parts/primitive.h"
#include "renderer/ViewSetup.h"
#include "renderer/Renderer.h"
#include "plugins/MsgPluginManager.h"
#include "core/VPApp.h"

static constexpr float PFmmToVPU = 1.f / 0.5396875f;
static constexpr float PFinToVPU = 25.4f * PFmmToVPU; // 47.06 units per inch

static std::vector<std::vector<std::string>> ReadCsv(const std::filesystem::path& path)
{
   std::vector<std::vector<std::string>> rows;
   std::ifstream f(path);
   std::string line;
   while (std::getline(f, line))
   {
      std::vector<std::string> cells;
      std::string cur;
      bool quoted = false;
      for (const char c : line)
      {
         if (c == '"')
            quoted = !quoted;
         else if (c == ',' && !quoted)
         {
            cells.push_back(cur);
            cur.clear();
         }
         else if (c != '\r')
            cur += c;
      }
      cells.push_back(cur);
      rows.push_back(cells);
   }
   return rows;
}

static Primitive* FindPrim(PinTable* table, const std::string& name)
{
   for (IEditable* part : table->GetParts())
      if (part->GetItemType() == eItemPrimitive && static_cast<Primitive*>(part)->GetName() == name)
         return static_cast<Primitive*>(part);
   return nullptr;
}

static bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

static Material* AddPBR(PinTable* table, const std::string& name, COLORREF base, float roughness, float metallic)
{
   Material* const m = new Material();
   m->m_name = name;
   m->m_type = Material::MaterialType::PBR;
   m->m_cBase = base;
   m->m_fRoughness = roughness;
   m->m_fMetallic = metallic;
   m->m_fOcclusion = 1.f;
   m->m_fEmissionIntensity = 0.f;
   m->m_cEmission = 0;
   m->m_bOpacityActive = false;
   m->m_fOpacity = 1.f;
   table->AddMaterial(m);
   return table->GetMaterial(name);
}

// A flat disc of the bake's insert outline (l63: 30.16 mm round, UV the bake's 34 mm square), scaled to an aperture by the part's size
static void MakeCardDisc(Primitive* prim)
{
   constexpr float diameterMm = 30.16f, squareMm = 34.f;
   prim->m_mesh.Clear();
   const int n = 96;
   const float r = 0.5f * diameterMm * PFmmToVPU;
   const float s = squareMm * PFmmToVPU;
   auto vert = [&](float x, float y)
   {
      Vertex3D_NoTex2 v;
      v.x = x; v.y = y; v.z = 0.f;
      v.nx = 0.f; v.ny = 0.f; v.nz = -1.f;
      v.tu = x / s + 0.5f;
      v.tv = y / s + 0.5f;
      return v;
   };
   prim->m_mesh.m_vertices.push_back(vert(0.f, 0.f));
   for (int i = 0; i < n; i++)
   {
      const float a = (float)(2.0 * M_PI) * (float)i / (float)n;
      prim->m_mesh.m_vertices.push_back(vert(r * cosf(a), r * sinf(a)));
   }
   for (int i = 0; i < n; i++)
   {
      prim->m_mesh.m_indices.push_back(0);
      prim->m_mesh.m_indices.push_back(1 + (i + 1) % n);
      prim->m_mesh.m_indices.push_back(1 + i);
   }
   prim->m_mesh.middlePoint = Vertex3Ds(0.f, 0.f, 0.f);
   prim->m_mesh.UpdateBounds();
   prim->m_d.m_use3DMesh = true;
   prim->m_d.m_useDepthMask = false;
   prim->m_d.m_backfacesEnabled = true;
}

TEST_CASE("PF first look build [pf][tool]")
{
   const std::string packIn = EnvStr("PF_PACK_DIR"), packOut = EnvStr("PF_OUT_DIR");
   if (packIn.empty() || packOut.empty())
      return;
   const std::filesystem::path t3a = EnvStr("PF_T3A"), bench = EnvStr("PF_BENCH"), bake = EnvStr("PF_BAKE");

   CComObject<PinTable>* table;
   CComObject<PinTable>::CreateInstance(&table);
   table->AddRef();
   TestFileFeedback feedback;
   REQUIRE(SUCCEEDED(table->LoadGameFromFilename(std::filesystem::path(packIn), feedback)));

   // ---- images
   REQUIRE(table->ImportImage(t3a / "pf_art_3k.jpg", "PF_art") != nullptr);
   REQUIRE(table->ImportImage(t3a / "pf_normal_combined.png", "PF_normal") != nullptr);
   REQUIRE(table->ImportImage(bench / "pf_orm.png", "PF_orm") != nullptr);
   for (const char* hdri : { "dancing_hall", "tv_studio", "machine_shop", "artist_workshop" })
      REQUIRE(table->ImportImage(t3a / (std::string(hdri) + "_2k.hdr"), "env_"s + hdri) != nullptr);
   REQUIRE(table->ImportImage(bake / "light.exr", "TRACED_l63") != nullptr);
   REQUIRE(table->ImportImage(bench / "facets_aa.png", "l63_facets") != nullptr);
   REQUIRE(table->ImportImage(bench / "env_atlas.exr", "l63_env") != nullptr);

   // ---- playfield
   Material* const pfMat = AddPBR(table, "PF_Playfield", RGB(255, 255, 255), 1.f, 0.f); // roughness scaled by the ORM map
   table->m_image = "PF_art";
   table->m_playfieldMaterial = pfMat->m_name;
   int nPrim = 0;
   {
      Primitive* const pf = FindPrim(table, "playfield_mesh");
      REQUIRE(pf != nullptr);
      // The pack's playfield_mesh is the VLM export: 5 vertices whose UVs do not span the table linearly, so the art would be
      // sheared. Replace it by a clean quad whose UV spans 0-1 over the table (u right, v down the table)
      pf->m_mesh.Clear();
      for (unsigned int i = 0; i < 4; ++i)
      {
         Vertex3D_NoTex2 v;
         v.x = (i & 1) ? table->m_right : table->m_left;
         v.y = (i & 2) ? table->m_bottom : table->m_top;
         v.z = 0.f;
         v.tu = (i & 1) ? 1.f : 0.f;
         v.tv = (i & 2) ? 1.f : 0.f;
         v.nx = 0.f; v.ny = 0.f; v.nz = 1.f;
         pf->m_mesh.m_vertices.push_back(v);
      }
      pf->m_mesh.m_indices = { 0, 1, 2, 2, 1, 3 };
      pf->m_mesh.UpdateBounds();
      pf->m_d.m_use3DMesh = true;
      pf->m_d.m_visible = true;
      pf->m_d.m_szImage = "PF_art"s;
      pf->m_d.m_szNormalMap = "PF_normal"s;
      pf->m_d.m_szORMMap = "PF_orm"s;
      pf->m_d.m_szMaterial = pfMat->m_name;
      pf->m_d.m_staticRendering = false;
      pf->m_d.m_reflectionEnabled = true;
      pf->m_d.m_depthBias = 100000.f;
      pf->m_d.m_objectSpaceNormalMap = false; // tangent space maps
      pf->m_d.m_disableLightingTop = 0.f; // the VLM pack had these meshes emissive (baked light)
      pf->m_d.m_disableLightingBelow = 1.f;
      Primitive* const bmPf = FindPrim(table, "BM_Playfield");
      if (bmPf)
         bmPf->m_d.m_visible = false;
   }

   // ---- constant PBR materials on the BM_ meshes (their UVs are VLM atlas UVs)
   Material* const mMetal = AddPBR(table, "PF_Metal", RGB(191, 191, 191), 0.25f, 1.f);
   Material* const mPlastic = AddPBR(table, "PF_Plastic", RGB(150, 150, 150), 0.1f, 0.f);
   Material* const mPlasticWhite = AddPBR(table, "PF_PlasticWhite", RGB(215, 215, 210), 0.1f, 0.f);
   Material* const mRubber = AddPBR(table, "PF_Rubber", RGB(24, 24, 24), 0.6f, 0.f);
   Material* const mFlipper = AddPBR(table, "PF_Flipper", RGB(235, 230, 205), 0.6f, 0.f);
   Material* const mParts = AddPBR(table, "PF_Parts", RGB(128, 128, 128), 0.35f, 0.5f);
   Material* const mWood = AddPBR(table, "PF_DarkWood", RGB(60, 38, 22), 0.5f, 0.f);
   Material* const mRamp = AddPBR(table, "PF_CenterRamp", RGB(255, 255, 255), 0.1f, 0.f);
   std::map<std::string, std::string> assigned;
   for (IEditable* part : table->GetParts())
   {
      if (part->GetItemType() != eItemPrimitive)
         continue;
      Primitive* const prim = static_cast<Primitive*>(part);
      const std::string name = prim->GetName();
      if (!StartsWith(name, "BM_") || name == "BM_Playfield")
         continue;
      Material* mat = nullptr;
      auto has = [&](const char* s) { return name.find(s) != std::string::npos; };
      if (name == "BM_CRamp" || name == "BM_CRampEdges")
         mat = mRamp; // keeps its CenterRampBM image (UVs span 0-1) and normal map
      else if (name == "BM_Sideblades")
         mat = mWood;
      else if (name == "BM_Parts")
         mat = mParts;
      else if (name == "BM_FlipperL" || name == "BM_FlipperR")
         mat = mFlipper;
      else if (has("LeftSling") || has("Rightsling"))
         mat = mRubber;
      else if (name == "BM_Rails" || name == "BM_WireRamp" || has("Wire") || name == "BM_Spinner" || name == "BM_DiverterArm" || (has("Bumper") && has("Socket")) || name == "BM_GateFlap")
         mat = mMetal;
      else if (name == "BM_Car")
         mat = mPlasticWhite;
      else
         mat = mPlastic; // RPlastics, Slings, RmpCovers, StickersCRamp, Bumper rings/inner/outer, Targets, Rollovers, SpecialPost, RRmp
      prim->m_d.m_szMaterial = mat->m_name;
      if (mat != mRamp)
      {
         prim->m_d.m_szImage.clear();
         prim->m_d.m_szNormalMap.clear();
      }
      else
      {
         prim->m_d.m_szORMMap.clear();
      }
      prim->m_d.m_staticRendering = false;
      prim->m_d.m_disableLightingTop = 0.f;
      prim->m_d.m_disableLightingBelow = 0.f;
      prim->m_d.m_szORMMap.clear();
      assigned[name] = mat->m_name;
      nPrim++;
   }
   MESSAGE("BM_ meshes re-materialled: " << nPrim);
   for (const auto& [n, m] : assigned)
      MESSAGE("  " << n << " -> " << m);

   // ---- GI lights: PBR point lights above the playfield
   const float giIntensity = EnvStr("PF_GI_INTENSITY").empty() ? 0.03f : (float)atof(EnvStr("PF_GI_INTENSITY").c_str());
   int nGI = 0;
   for (IEditable* part : table->GetParts())
   {
      if (part->GetItemType() != eItemLight)
         continue;
      Light* const light = static_cast<Light*>(part);
      const std::string name = light->GetName();
      if (!(name.size() == 4 && name[0] == 'g' && name[1] == 'i' && isdigit((unsigned char)name[2])))
         continue;
      const bool shadowed = light->m_d.m_shadows != ShadowMode::NONE; // the main GI ring (the lights that had ball shadows)
      light->m_d.m_type = LightType::PBR;
      light->m_d.m_visible = true;
      light->m_d.m_showBulbMesh = false;
      light->m_d.m_state = 1.f;
      light->m_d.m_color = RGB(255, 169, 87); // warm 2700 K white
      light->m_d.m_intensity = giIntensity;
      light->m_d.m_falloff = 400.f;
      light->m_d.m_falloff_power = 2.f;
      light->m_d.m_height = 40.f;
      if (shadowed && !EnvStr("PF_GI_RING_H").empty())
      {
         light->m_d.m_height = (float)atof(EnvStr("PF_GI_RING_H").c_str());
         light->m_d.m_intensity = (float)atof(EnvStr("PF_GI_RING_I").c_str());
         light->m_d.m_falloff = 700.f;
      }
      light->m_d.m_shadows = (shadowed && EnvStr("PF_GI_SHADOWS") != "0") ? ShadowMode::PBR_STATIC_SHADOWS : ShadowMode::NONE;
      nGI++;
   }
   MESSAGE("GI lights converted: " << nGI);

   // ---- Traced starburst rounds
   const auto csv = ReadCsv(EnvStr("PF_INSERTS_CSV"));
   REQUIRE(csv.size() > 2);
   std::map<std::string, int> col;
   for (size_t i = 0; i < csv[0].size(); i++)
      col[csv[0][i]] = (int)i;
   const std::set<int> lamps = { 14, 24, 25, 26, 27, 28, 29, 30, 49, 50, 51, 52, 54, 55, 56, 57, 58, 59 };
   int nCards = 0;
   for (size_t r = 1; r < csv.size(); r++)
   {
      const auto& row = csv[r];
      if ((int)row.size() <= col["light_rgb"] || row[col["shape"]] != "round" || row[col["lamp"]].empty())
         continue;
      const int lampNo = atoi(row[col["lamp"]].c_str() + 1);
      if (!lamps.count(lampNo))
         continue;
      const float longIn = (float)atof(row[col["long_in"]].c_str());
      const float xIn = (float)atof(row[col["x_in"]].c_str()), yIn = (float)atof(row[col["y_in_from_top"]].c_str());
      float rgb[3] = { 1.f, 1.f, 1.f };
      sscanf(row[col["light_rgb"]].c_str(), "%f/%f/%f", &rgb[0], &rgb[1], &rgb[2]);
      const std::string lampName = "l" + std::to_string(lampNo);
      Light* const lamp = table->GetLight(lampName);
      REQUIRE_MESSAGE(lamp != nullptr, "missing light " << lampName);
      const float apertureMm = (longIn - 0.05f) * 25.4f;
      const float k = apertureMm / 30.16f;
      Primitive* const card = Primitive::COMCreate();
      const float x = xIn * PFinToVPU, y = yIn * PFinToVPU;
      card->Init(x, y, false);
      card->SetName("TC_" + lampName);
      MakeCardDisc(card);
      card->m_d.m_vPosition = Vertex3Ds(x, y, 0.2f);
      card->m_d.m_vSize = Vertex3Ds(k, k, 1.f);
      card->m_d.m_szImage = "TRACED_l63"s;
      card->m_d.m_szNormalMap = "l63_facets"s;
      card->m_d.m_szEmissionMap = "l63_env"s;
      card->m_d.m_szLightmap = lampName;
      card->m_d.m_addBlend = true;
      card->m_d.m_color = RGB((int)(rgb[0] * 255.f + 0.5f), (int)(rgb[1] * 255.f + 0.5f), (int)(rgb[2] * 255.f + 0.5f));
      card->m_d.m_disableLightingTop = 1.f;
      card->m_d.m_staticRendering = false;
      card->m_d.m_depthBias = -0.2f;
      card->m_d.m_collidable = false;
      card->m_d.m_toy = true;
      table->AddPart(card);
      card->Release();
      nCards++;
      MESSAGE("card " << lampName << " at " << x << "," << y << " scale " << k << " (aperture " << apertureMm << " mm)");
   }
   MESSAGE("Traced cards: " << nCards);

   // ---- save
   std::error_code ec;
   std::filesystem::remove_all(packOut, ec);
   REQUIRE(std::filesystem::create_directories(packOut, ec));
   TestFileFeedback save;
   REQUIRE(SUCCEEDED(table->SaveToJSON(std::filesystem::path(packOut), save)));
   table->Release();
}

// ---------------------------------------------------------------------------------------------------------------------
// T3a first look render: loads the pack, applies one variant in memory and saves a PNG screenshot (plus a one line GPU
// frame time file next to it). The script is removed so the ROM does not drive the lamps: the light states are set here.
//
//   PF_PACK_DIR=<pack> PF_SHOT=<out.png> [PF_HDRI=orig|dancing_hall|tv_studio|machine_shop|artist_workshop] [PF_ENV_SCALE=<unset: the pack's own value>]
//   [PF_GI=1|0] [PF_GI_INTENSITY=<value>] [PF_VIEW=main|lower|gun|mid] [PF_AO=1] [PF_SSR=1] [PF_LAMPS=odd|even|all|every|none]
//   (all = the 18 Traced rounds, every = every insert lamp l1-l64 + the Top Cop flasher f114) [PF_LAMP_LEVEL=1 (the real game's full lamp is 0.706)]
//   [PF_KEEP_PARTS=1: hide nothing (to render the VPW vpx's own VLM build as a reference)]
//   [PF_TONEMAP=0..3] [PF_AA="1,6"] [PF_ROOM=0..1] [PF_RES="w,h"] [PF_FLASH=<dome lamp level>] [PF_LAMPS_ON="37,38"] [PF_CAM="elevation,distance,fov,yFraction"]
// ---------------------------------------------------------------------------------------------------------------------

static const int PFRoundLamps[] = { 14, 24, 25, 26, 27, 28, 29, 30, 49, 50, 51, 52, 54, 55, 56, 57, 58, 59 };

TEST_CASE("PF first look render [pf][render]")
{
#if defined(ENABLE_BGFX)
   const std::string pack = EnvStr("PF_PACK_DIR"), shot = EnvStr("PF_SHOT");
   if (pack.empty() || shot.empty())
      return;
   ResetVPX();
   const std::string hdri = EnvStr("PF_HDRI").empty() ? "orig" : EnvStr("PF_HDRI");
   const bool envScaleSet = !EnvStr("PF_ENV_SCALE").empty(); // unset: keep the pack's own env_emission_scale (T10c)
   const float envScale = envScaleSet ? (float)atof(EnvStr("PF_ENV_SCALE").c_str()) : 0.f;
   const bool gi = EnvStr("PF_GI") != "0";
   const float giMul = EnvStr("PF_GI_INTENSITY").empty() ? 1.f : (float)atof(EnvStr("PF_GI_INTENSITY").c_str());
   const std::string view = EnvStr("PF_VIEW").empty() ? "main" : EnvStr("PF_VIEW");
   const std::string lampMode = EnvStr("PF_LAMPS").empty() ? "even" : EnvStr("PF_LAMPS");
   const bool ao = EnvStr("PF_AO") == "1", ssr = EnvStr("PF_SSR") == "1";
   const float lampLevel = EnvStr("PF_LAMP_LEVEL").empty() ? 1.f : (float)atof(EnvStr("PF_LAMP_LEVEL").c_str());

   {
      Settings& settings = g_settingsService.GetAppSettings();
      settings.SetPlayer_DisableAO(!ao, false);
      settings.SetPlayer_SSRefl(ssr, false);
      float factor = 1.f;
      int post = 6;
      if (!EnvStr("PF_AA").empty())
         sscanf(EnvStr("PF_AA").c_str(), "%f,%d", &factor, &post);
      settings.SetPlayer_AAFactor(factor, false);
      settings.SetPlayer_FXAA(post, false);
      // PF_RES="w,h": the window size (Ben's 4K: 3840,2160)
      if (!EnvStr("PF_RES").empty())
      {
         int w = 1920, h = 1080;
         sscanf(EnvStr("PF_RES").c_str(), "%d,%d", &w, &h);
         settings.SetPlayer_PlayfieldWidth(w, false);
         settings.SetPlayer_PlayfieldHeight(h, false);
      }
      // PF_ROOM=<0..1>: the player's Scene Lighting set to a custom fixed level (the room brightness slider)
      if (!EnvStr("PF_ROOM").empty())
      {
         settings.SetPlayer_OverrideTableEmissionScale(true, false);
         settings.SetPlayer_DynamicDayNight(false, false);
         settings.SetPlayer_EmissionScale((float)atof(EnvStr("PF_ROOM").c_str()), false);
      }
   }

   CComObject<PinTable>* table;
   CComObject<PinTable>::CreateInstance(&table);
   table->AddRef();
   TestFileFeedback feedback;
   REQUIRE(SUCCEEDED(table->LoadGameFromFilename(std::filesystem::path(pack), feedback)));

   // No script: nothing drives the lamps, the VR/backglass/desktop clutter the script would hide stays hidden here
   table->m_script_text.clear();
   int hidden = 0;
   std::set<int> on;
   for (size_t i = 0; i < std::size(PFRoundLamps); i++)
      if (lampMode == "all" || (lampMode == "even" && i % 2 == 0) || (lampMode == "odd" && i % 2 == 1))
         on.insert(PFRoundLamps[i]);
   // PF_LAMPS_ON="37,38,39,40": these lamps on too, whatever PF_LAMPS says (lamps outside the round inserts: jail cells...)
   const std::string lampsOn = EnvStr("PF_LAMPS_ON");
   for (const char* p = lampsOn.c_str(); p && *p;)
   {
      on.insert(atoi(p));
      p = strchr(p, ',');
      if (p)
         p++;
   }
   for (IEditable* part : table->GetParts())
   {
      switch (part->GetItemType())
      {
      case eItemFlasher:
         if (EnvStr("PF_KEEP_PARTS") == "1") // the VLM build (VPW vpx) for reference stills: keep its lightmaps and blooms
            break;
         static_cast<Flasher*>(part)->m_d.m_isVisible = false;
         hidden++;
         break;
      case eItemPrimitive:
      {
         Primitive* const prim = static_cast<Primitive*>(part);
         const std::string n = prim->GetName();
         if (prim->m_d.m_visible && EnvStr("PF_KEEP_PARTS") != "1" && !StartsWith(n, "BM_") && !StartsWith(n, "PBR_") && !StartsWith(n, "TC_") && n != "playfield_mesh")
         {
            prim->m_d.m_visible = false;
            hidden++;
         }
         break;
      }
      case eItemLight:
      {
         Light* const light = static_cast<Light*>(part);
         const std::string n = light->GetName();
         light->m_d.m_fadeSpeedUp = light->m_d.m_fadeSpeedDown = 1000.f; // settle at once
         if (light->m_d.m_type == LightType::PBR)
         {
            if (n.size() == 4 && n[0] == 'g' && n[1] == 'i')
            {
               static const std::set<std::string> ring = { "gi01", "gi02", "gi03", "gi04", "gi05", "gi06", "gi11", "gi14", "gi16", "gi21", "gi23" };
               if (EnvStr("PF_RING_SHADOWS") == "1" && ring.count(n))
               {
                  light->m_d.m_shadows = ShadowMode::PBR_STATIC_SHADOWS;
                  light->m_d.m_height = 110.f;
                  light->m_d.m_falloff = 700.f;
                  light->m_d.m_intensity = (float)atof(EnvStr("PF_RING_I").c_str());
               }
               light->m_d.m_state = light->m_inPlayState = gi ? 1.f : 0.f;
               light->m_d.m_intensity *= giMul;
            }
         }
         else if (light->m_d.m_visible && EnvStr("PF_KEEP_PARTS") != "1")
         {
            light->m_d.m_visible = false;
            hidden++;
         }
         if (n.size() > 1 && n[0] == 'l' && isdigit((unsigned char)n[1]))
            light->m_d.m_state = light->m_inPlayState = (lampMode == "every" || on.count(atoi(n.c_str() + 1))) ? lampLevel : 0.f;
         else if (n == "f114")
            light->m_d.m_state = light->m_inPlayState = lampMode == "every" ? lampLevel : 0.f;
         // PF_FLASH=<level>: the three flasher dome lamps (f125 shark, f126 croc, f127 rat) at that level
         if (!EnvStr("PF_FLASH").empty() && (n == "f125" || n == "f126" || n == "f127"))
            light->m_d.m_state = light->m_inPlayState = (float)atof(EnvStr("PF_FLASH").c_str());
         break;
      }
      default: break;
      }
   }
   std::ofstream dbg(shot + ".dbg");
   dbg << "hidden parts: " << hidden << ", lamps on: " << on.size() << "\n";
   if (Primitive* const pf = FindPrim(table, "playfield_mesh"))
   {
      Material* const m = table->GetMaterial(pf->m_d.m_szMaterial);
      for (const auto& v : pf->m_mesh.m_vertices)
         dbg << "  pf vertex " << v.x << "," << v.y << "," << v.z << " uv " << v.tu << "," << v.tv << " n " << v.nx << "," << v.ny << "," << v.nz << "\n";
      dbg << "  indices:";
      for (const auto i : pf->m_mesh.m_indices)
         dbg << " " << i;
      dbg << "\n  pos " << pf->m_d.m_vPosition.x << "," << pf->m_d.m_vPosition.y << " size " << pf->m_d.m_vSize.x << "," << pf->m_d.m_vSize.y << " rot " << pf->m_d.m_aRotAndTra[0] << "," << pf->m_d.m_aRotAndTra[1] << "," << pf->m_d.m_aRotAndTra[2] << " tra " << pf->m_d.m_aRotAndTra[3] << "," << pf->m_d.m_aRotAndTra[4] << "," << pf->m_d.m_aRotAndTra[5] << "\n";
      dbg << "playfield_mesh visible=" << pf->m_d.m_visible << " material=" << pf->m_d.m_szMaterial << " type=" << (m ? (int)m->m_type : -1) << " image=" << pf->m_d.m_szImage << " lights=" << table->m_lightEmissionScale << "\n";
   }

   if (!EnvStr("PF_DBG_PFBASE").empty())
      if (Material* const m = table->GetMaterial("PF_Playfield"s))
         m->m_cBase = (COLORREF)atoi(EnvStr("PF_DBG_PFBASE").c_str());
   // World
   if (hdri != "orig")
      table->m_envImage = "env_" + hdri;
   if (envScaleSet)
      table->m_envEmissionScale = envScale;
   if (!EnvStr("PF_LIGHT_SCALE").empty())
      table->m_lightEmissionScale = (float)atof(EnvStr("PF_LIGHT_SCALE").c_str());
   table->m_enableAO = ao;
   table->m_enableSSR = ssr;
   if (!EnvStr("PF_EXPOSURE").empty())
      table->SetExposure((float)atof(EnvStr("PF_EXPOSURE").c_str()));
   if (!EnvStr("PF_TONEMAP").empty())
      table->SetToneMapper((ToneMapper)atoi(EnvStr("PF_TONEMAP").c_str()));

   // Camera: 'main' is the table's own desktop view; the others are free camera views
   if (view != "main")
   {
      float elevation = 30.f, distance = 650.f, fov = 36.f, yFrac = 0.82f;
      if (view == "lower") { elevation = 32.f; distance = 640.f; fov = 36.f; yFrac = 0.86f; }
      else if (view == "gun") { elevation = 32.f; distance = 560.f; fov = 36.f; yFrac = 0.14f; }
      else if (view == "mid") { elevation = 38.f; distance = 900.f; fov = 36.f; yFrac = 0.5f; }
      if (!EnvStr("PF_CAM").empty())
         sscanf(EnvStr("PF_CAM").c_str(), "%f,%f,%f,%f", &elevation, &distance, &fov, &yFrac);
      const float y = table->m_bottom * yFrac;
      const float e = ANGTORAD(elevation);
      for (ViewSetup& vs : table->mViewSetups)
      {
         vs.mMode = VLM_CAMERA;
         vs.mViewX = EnvStr("PF_CAM_X").empty() ? 0.f : (float)atof(EnvStr("PF_CAM_X").c_str()); // VPX units off the table centre
         vs.mViewY = (y + distance * sinf(e)) - table->m_bottom;
         vs.mViewZ = distance * cosf(e);
         vs.mLookAt = 100.f * (table->m_bottom - y) / table->m_bottom;
         vs.mFOV = fov;
         vs.mLayback = 0.f;
         vs.mViewHOfs = 0.f;
         vs.mViewVOfs = 0.f;
         vs.mViewportRotation = 0.f;
         vs.mSceneScaleX = vs.mSceneScaleY = vs.mSceneScaleZ = 1.f;
      }
   }

   struct ShotState
   {
      std::string path;
      int frame = 0;
      bool pending = false;
      uint64_t gpuSum = 0;
      int gpuCount = 0;
      std::chrono::steady_clock::time_point start;
      int captureOk = -1;
      std::string lampInfo;
   } state;
   state.path = shot;
   msgpi_msg_callback onPrepareFrame = [](const unsigned int msgId, void* context, void* msgData)
   {
      ShotState* const s = reinterpret_cast<ShotState*>(context);
      if (s->frame == 0)
         s->start = std::chrono::steady_clock::now();
      s->frame++;
      if (s->frame > 90 && s->frame <= 210)
      {
         s->gpuSum += g_pplayer->m_renderer->m_renderDevice->m_lastGPUFrameLength;
         s->gpuCount++;
      }
      const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - s->start).count();
      if (s->pending || s->frame < 220 || secs < 4.0)
         return;
      s->pending = true;
      for (const char* ln : { "l14", "l24", "gi01" })
         if (Light* const l = g_pplayer->m_ptable->GetLight(ln))
            s->lampInfo += std::string(ln) + ":inPlay=" + std::to_string(l->m_inPlayState) + ",cur=" + std::to_string(l->m_currentIntensity) + ",fader=" + std::to_string((int)l->m_d.m_fader) + " ";
      g_pplayer->m_renderer->m_renderDevice->CaptureScreenshot({ g_pplayer->m_playfieldWnd }, { std::filesystem::path(s->path) },
         [s](bool ok) { s->captureOk = ok ? 1 : 0; g_pplayer->SetCloseState(Player::CS_STOP_PLAY); });
   };
   LoadProgress loadProgress;
   auto player = std::make_unique<Player>(table, Player::PlayMode::Play, loadProgress);
   dbg << "pbrActive=" << player->m_renderer->m_renderDevice->m_pbrActive << " staticPrepass=" << player->m_renderer->IsUsingStaticPrepass() << "\n";
   dbg.flush();
   const unsigned int msgId = player->m_pluginManager.GetMsgAPI().GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_PREPARE_FRAME);
   player->m_pluginManager.GetMsgAPI().SubscribeMsg(player->m_pluginAPI.GetVPXEndPointId(), msgId, onPrepareFrame, &state);
   player->GameLoop();
   player->m_pluginManager.GetMsgAPI().UnsubscribeMsg(msgId, onPrepareFrame, &state);
   player->m_pluginManager.GetMsgAPI().ReleaseMsgID(msgId);
   player = nullptr;
   table->Release();
   std::ofstream(shot + ".txt") << "gpu_ms=" << (state.gpuCount ? (double)state.gpuSum / state.gpuCount / 1000.0 : -1.0) << " frames=" << state.frame << " captureOk=" << state.captureOk << " " << state.lampInfo << '\n';
   CHECK(std::filesystem::exists(shot));
   ResetVPX();
#endif
}
