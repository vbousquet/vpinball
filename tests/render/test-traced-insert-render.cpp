// license:GPLv3+

#include "core/stdafx.h"
#include "../vpx-test.h"

#include "core/VPApp.h"
#include "parts/light.h"
#include "parts/Material.h"
#include "parts/pintable.h"
#include "parts/primitive.h"
#include "renderer/ViewSetup.h"
#include "renderer/Renderer.h"
#include "utils/bulb.h"
#include "plugins/MsgPluginManager.h"

#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <sstream>

// Traced insert prototype: one AFM 'round 1.19in starburst' insert (l63) drawn three ways side by side on a plain
// playfield: lamp-off body only (left), a classic single-view lightmap (middle) and the traced insert (right).
// The bake folder holds light.exr (4 elevations in RGBA), light0.exr (a single top-down render), facets.png and
// env45.exr (lamp-off look), as written by the LuxCore solo bake. Point TRACED_BAKE_DIR at it.

static constexpr float mmToVPU = 1.f / 0.5396875f;

// Where the insert row sits, as a fraction of the table height (TRACED_ROW_Y, default 0.55)
static float RowY(const PinTable* table)
{
   const char* const y = std::getenv("TRACED_ROW_Y");
   return table->m_bottom * (y ? (float)atof(y) : 0.55f);
}

static std::filesystem::path BakeDir()
{
   const char* const dir = std::getenv("TRACED_BAKE_DIR");
   return dir ? std::filesystem::path(dir) : std::filesystem::path();
}

// The lamp-off body image: the bake's 45 deg env render, or TRACED_OFF_IMAGE (e.g. a frosted variant)
static std::filesystem::path OffImage()
{
   const char* const path = std::getenv("TRACED_OFF_IMAGE");
   return path ? std::filesystem::path(path) : BakeDir() / "env45.exr";
}

// A flat disc of the insert's outline, its UV the bake's square (u right, v down the table)
static void MakeInsertDisc(Primitive* prim, float diameterMm, float squareMm)
{
   prim->m_mesh.Clear();
   const int n = 96;
   const float r = 0.5f * diameterMm * mmToVPU;
   const float s = squareMm * mmToVPU;
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

static Primitive* AddDisc(PinTable* table, const string& name, float x, float y, float z, const string& image, const string& normalMap, bool additive, COLORREF color)
{
   Primitive* const prim = Primitive::COMCreate();
   prim->Init(x, y, false);
   prim->SetName(name);
   MakeInsertDisc(prim, 30.16f, 34.f);
   prim->m_d.m_vPosition = Vertex3Ds(x, y, z);
   prim->m_d.m_vSize = Vertex3Ds(1.f, 1.f, 1.f); // the mesh is in table units already
   prim->m_d.m_szImage = image;
   prim->m_d.m_szNormalMap = normalMap;
   prim->m_d.m_addBlend = additive;
   prim->m_d.m_color = color;
   prim->m_d.m_disableLightingTop = 1.f;
   prim->m_d.m_staticRendering = false;
   prim->m_d.m_depthBias = -z;
   table->AddPart(prim);
   prim->Release();
   return prim;
}

static std::filesystem::path CreateTracedInsertTable()
{
   CComObject<PinTable>* table;
   CComObject<PinTable>::CreateInstance(&table);
   table->AddRef();
   TestFileFeedback feedback;
   REQUIRE(SUCCEEDED(table->LoadGameFromFilename(GetAssetPath() / "test000-default-table.vpx", feedback)));

   const std::filesystem::path bake = BakeDir();
   REQUIRE(table->ImportImage(bake / "light.exr", "TRACED_l63") != nullptr);
   REQUIRE(table->ImportImage(bake / "light0.exr", "l63_classic") != nullptr);
   REQUIRE(table->ImportImage(std::getenv("TRACED_FACETS") ? std::filesystem::path(std::getenv("TRACED_FACETS")) : bake / "facets.png", "l63_facets") != nullptr);
   REQUIRE(table->ImportImage(OffImage(), "l63_off") != nullptr);

   const float cx = (table->m_left + table->m_right) * 0.5f;
   const float y = RowY(table);
   const float dx = 34.f * mmToVPU * 1.2f;
   const COLORREF lamp = RGB(255, 96, 40);
   for (int i = 0; i < 3; i++)
      AddDisc(table, "l63_off_" + std::to_string(i), cx + (float)(i - 1) * dx, y, 0.1f, "l63_off"s, ""s, false, RGB(255, 255, 255));
   AddDisc(table, "l63_classic"s, cx, y, 0.2f, "l63_classic"s, ""s, true, lamp);
   AddDisc(table, "l63_traced"s, cx + dx, y, 0.2f, "TRACED_l63"s, "l63_facets"s, true, lamp);

   const std::filesystem::path file = GetTestTmpDir() / "traced-insert-table.vpx";
   table->m_filename = file;
   TestFileFeedback saveFeedback;
   REQUIRE(SUCCEEDED(table->Save(saveFeedback)));
   table->Release();
   return file;
}

// Camera mode view of the insert row, from 'elevation' degrees off vertical at 'distance' table units
static std::filesystem::path WithCamera(const std::filesystem::path& tableFile, float elevation, float distance, float fov, const string& suffix)
{
   CComObject<PinTable>* table;
   CComObject<PinTable>::CreateInstance(&table);
   table->AddRef();
   TestFileFeedback feedback;
   REQUIRE(SUCCEEDED(table->LoadGameFromFilename(tableFile, feedback)));
   const float y = RowY(table);
   const float e = ANGTORAD(elevation);
   for (ViewSetup& vs : table->mViewSetups)
   {
      vs.mMode = VLM_CAMERA;
      vs.mViewX = 0.f;
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
   const std::filesystem::path file = GetTestTmpDir() / ("traced-insert-table-" + suffix + ".vpx");
   table->m_filename = file;
   TestFileFeedback saveFeedback;
   REQUIRE(SUCCEEDED(table->Save(saveFeedback)));
   table->Release();
   return file;
}

TEST_CASE("Traced insert render [render][traced]")
{
   if (BakeDir().empty())
   {
      MESSAGE("TRACED_BAKE_DIR not set, skipped");
      return;
   }
   ResetVPX();
   const std::filesystem::path tableFile = CreateTracedInsertTable();
#if defined(ENABLE_BGFX)
   // Shots as 'elevation,distance,fov,name;...' (TRACED_SHOTS), elevation off vertical in degrees, distance in table units
   const char* const shotsEnv = std::getenv("TRACED_SHOTS");
   std::istringstream shots(shotsEnv ? shotsEnv : "20,450,22,e20;45,450,22,e45;62,520,20,e62");
   string shot;
   while (std::getline(shots, shot, ';'))
   {
      float elevation, distance, fov;
      char name[32];
      if (sscanf(shot.c_str(), "%f,%f,%f,%31s", &elevation, &distance, &fov, name) != 4)
         continue;
      const std::filesystem::path file = WithCamera(tableFile, elevation, distance, fov, name);
      CaptureRender(file.string(), "traced-insert-"s + name + ".webp");
      CHECK(std::filesystem::exists(GetAssetPath() / ("traced-insert-"s + name + ".webp")));
   }
#endif
   ResetVPX();
}

// Measures the average GPU frame time over 300 frames (after 120 frames of warm up) of a table
static double MeasureGPUFrameTime(const std::filesystem::path& tablePath)
{
   struct PerfState { uint64_t sum = 0; int count = 0; int frame = 0; } state;
   msgpi_msg_callback onPrepareFrame = [](const unsigned int msgId, void* context, void* msgData)
   {
      PerfState* const state = reinterpret_cast<PerfState*>(context);
      state->frame++;
      if (state->frame > 120)
      {
         state->sum += g_pplayer->m_renderer->m_renderDevice->m_lastGPUFrameLength;
         state->count++;
      }
      if (state->count >= 300)
         g_pplayer->SetCloseState(Player::CS_STOP_PLAY);
   };
   CComObject<PinTable>* table;
   CComObject<PinTable>::CreateInstance(&table);
   table->AddRef();
   TestFileFeedback feedback;
   REQUIRE(SUCCEEDED(table->LoadGameFromFilename(tablePath, feedback)));
   LoadProgress loadProgress;
   auto player = std::make_unique<Player>(table, Player::PlayMode::Play, loadProgress);
   const unsigned int msgId = player->m_pluginManager.GetMsgAPI().GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_PREPARE_FRAME);
   player->m_pluginManager.GetMsgAPI().SubscribeMsg(player->m_pluginAPI.GetVPXEndPointId(), msgId, onPrepareFrame, &state);
   player->GameLoop();
   player->m_pluginManager.GetMsgAPI().UnsubscribeMsg(msgId, onPrepareFrame, &state);
   player->m_pluginManager.GetMsgAPI().ReleaseMsgID(msgId);
   player = nullptr;
   table->Release();
   return state.count ? (double)state.sum / state.count / 1000.0 : -1.0; // ms
}

// mode: 0 none, 1 classic lightmap cards, 2 traced cards. fill: one card scaled up to cover the screen (per pixel worst case)
static std::filesystem::path CreatePerfTable(int mode, bool fill)
{
   CComObject<PinTable>* table;
   CComObject<PinTable>::CreateInstance(&table);
   table->AddRef();
   TestFileFeedback feedback;
   REQUIRE(SUCCEEDED(table->LoadGameFromFilename(GetAssetPath() / "test000-default-table.vpx", feedback)));
   const std::filesystem::path bake = BakeDir();
   REQUIRE(table->ImportImage(bake / "light.exr", "TRACED_l63") != nullptr);
   REQUIRE(table->ImportImage(bake / "light0.exr", "l63_classic") != nullptr);
   REQUIRE(table->ImportImage(std::getenv("TRACED_FACETS") ? std::filesystem::path(std::getenv("TRACED_FACETS")) : bake / "facets.png", "l63_facets") != nullptr);
   const COLORREF lamp = RGB(255, 96, 40);
   if (mode != 0)
   {
      const string image = mode == 2 ? "TRACED_l63"s : "l63_classic"s;
      const string normal = mode == 2 ? "l63_facets"s : ""s;
      if (fill)
      {
         Primitive* const prim = AddDisc(table, "fill"s, (table->m_left + table->m_right) * 0.5f, table->m_bottom * 0.5f, 0.2f, image, normal, true, lamp);
         prim->m_d.m_vSize = Vertex3Ds(40.f, 40.f, 1.f);
      }
      else
      {
         for (int i = 0; i < 64; i++)
         {
            const float x = table->m_right * (0.15f + 0.7f * (float)(i % 8) / 7.f);
            const float y = table->m_bottom * (0.12f + 0.6f * (float)(i / 8) / 7.f);
            AddDisc(table, "ins" + std::to_string(i), x, y, 0.2f, image, normal, true, lamp);
         }
      }
   }
   const std::filesystem::path file = GetTestTmpDir() / ("traced-perf-" + std::to_string(mode) + (fill ? "-fill" : "") + ".vpx");
   table->m_filename = file;
   TestFileFeedback saveFeedback;
   REQUIRE(SUCCEEDED(table->Save(saveFeedback)));
   table->Release();
   return file;
}

TEST_CASE("Traced insert perf [render][traced][perf]")
{
   if (BakeDir().empty())
      return;
   ResetVPX();
#if defined(ENABLE_BGFX)
   const char* const names[] = { "none", "classic", "traced" };
   for (int fill = 0; fill < 2; fill++)
      for (int mode = 0; mode < 3; mode++)
      {
         if (fill && mode == 0)
            continue;
         const double ms = MeasureGPUFrameTime(CreatePerfTable(mode, fill != 0));
         const string line = string("PERF ") + (fill ? "fill " : "64ins ") + names[mode] + " gpu_ms=" + std::to_string(ms);
         MESSAGE(line);
      }
#endif
   ResetVPX();
}

// One traced insert over its lamp-off body at the row centre, turned by 'turn' degrees, optionally driven by an
// incandescent light blinking on and off
static std::filesystem::path CreateSingleTable(float turn, bool blink, const string& suffix)
{
   CComObject<PinTable>* table;
   CComObject<PinTable>::CreateInstance(&table);
   table->AddRef();
   TestFileFeedback feedback;
   REQUIRE(SUCCEEDED(table->LoadGameFromFilename(GetAssetPath() / "test000-default-table.vpx", feedback)));
   const std::filesystem::path bake = BakeDir();
   REQUIRE(table->ImportImage(bake / "light.exr", "TRACED_l63") != nullptr);
   REQUIRE(table->ImportImage(std::getenv("TRACED_FACETS") ? std::filesystem::path(std::getenv("TRACED_FACETS")) : bake / "facets.png", "l63_facets") != nullptr);
   REQUIRE(table->ImportImage(OffImage(), "l63_off") != nullptr);
   const float cx = (table->m_left + table->m_right) * 0.5f;
   const float y = RowY(table);
   // With TRACED_ENV_ATLAS the lamp-off look is drawn by the traced card from the 4-elevation atlas, over a black body;
   // otherwise the body is the flat 45 deg lamp-off render
   const char* const atlas = std::getenv("TRACED_ENV_ATLAS");
   if (atlas)
   {
      REQUIRE(table->ImportImage(atlas, "l63_env") != nullptr);
      REQUIRE(table->ImportImage(std::filesystem::path(atlas).parent_path() / "black.png", "black") != nullptr);
   }
   Primitive* const body = AddDisc(table, "l63_off"s, cx, y, 0.1f, atlas ? "black"s : "l63_off"s, ""s, false, RGB(255, 255, 255));
   Primitive* const card = AddDisc(table, "l63_traced"s, cx, y, 0.2f, "TRACED_l63"s, "l63_facets"s, true, RGB(255, 96, 40));
   if (atlas)
      card->m_d.m_szEmissionMap = "l63_env"s;
   body->m_d.m_aRotAndTra[2] = turn;
   card->m_d.m_aRotAndTra[2] = turn;
   if (blink)
   {
      Light* const light = Light::COMCreate();
      light->Init(cx, y, false);
      light->SetName("l63"s);
      light->m_d.m_state = 2.f; // blinking
      light->m_d.m_rgblinkpattern = "10";
      light->m_d.m_blinkinterval = 700;
      light->m_d.m_fader = FADER_INCANDESCENT;
      light->m_d.m_visible = false; // the traced card is the lamp's only visual
      table->AddPart(light);
      light->Release();
      card->m_d.m_szLightmap = "l63"s;
   }
   // PBR bench (TRACED_PBR=<folder with pf_normal.png, pf_orm.png>): an explicit playfield mesh carrying the wood image,
   // normal and ORM maps on a PBR material, lit by two shadowed PBR lights above the table
   if (const char* const pbr = std::getenv("TRACED_PBR"))
   {
      REQUIRE(table->ImportImage(std::filesystem::path(pbr) / "pf_normal.png", "pf_normal") != nullptr);
      REQUIRE(table->ImportImage(std::filesystem::path(pbr) / "pf_orm.png", "pf_orm") != nullptr);
      Material* const pfMat = table->GetMaterial(table->m_playfieldMaterial);
      REQUIRE(pfMat != nullptr);
      pfMat->m_type = Material::MaterialType::PBR;
      pfMat->m_fRoughness = 1.f; // scaled by the ORM map
      pfMat->m_fMetallic = 0.f;
      pfMat->m_fOcclusion = 1.f;
      pfMat->m_fEmissionIntensity = 0.f;
      pfMat->m_cBase = RGB(255, 255, 255);
      pfMat->m_bOpacityActive = false;
      Primitive* const pf = Primitive::COMCreate();
      pf->Init(0.f, 0.f, false);
      pf->SetName("playfield_mesh"s);
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
      pf->m_d.m_vPosition = Vertex3Ds(0.f, 0.f, 0.f);
      pf->m_d.m_vSize = Vertex3Ds(1.f, 1.f, 1.f);
      pf->m_d.m_szImage = table->m_image;
      pf->m_d.m_szMaterial = table->m_playfieldMaterial;
      pf->m_d.m_szNormalMap = "pf_normal"s;
      pf->m_d.m_szORMMap = "pf_orm"s;
      pf->m_d.m_staticRendering = false;
      pf->m_d.m_reflectionEnabled = true;
      pf->m_d.m_collidable = false;
      pf->m_d.m_toy = true;
      pf->m_d.m_depthBias = 100000.f;
      pf->m_d.m_disableLightingBelow = 1.f;
      table->AddPart(pf);
      pf->Release();
      for (int i = 0; i < 2; i++)
      {
         Light* const light = Light::COMCreate();
         light->Init(cx + (i == 0 ? -250.f : 300.f), y - (i == 0 ? 500.f : 250.f), false);
         light->SetName("pbr_key_" + std::to_string(i));
         light->m_d.m_type = LightType::PBR;
         light->m_d.m_state = 1.f;
         light->m_d.m_intensity = i == 0 ? 6.f : 3.f;
         light->m_d.m_falloff = 3000.f;
         light->m_d.m_falloff_power = 2.f;
         light->m_d.m_height = i == 0 ? 600.f : 400.f;
         light->m_d.m_color = i == 0 ? RGB(255, 226, 190) : RGB(200, 220, 255);
         light->m_d.m_showBulbMesh = false;
         light->m_d.m_visible = true;
         light->m_d.m_shadows = ShadowMode::PBR_STATIC_SHADOWS;
         table->AddPart(light);
         light->Release();
      }
   }

   const std::filesystem::path file = GetTestTmpDir() / ("traced-single-" + suffix + ".vpx");
   table->m_filename = file;
   TestFileFeedback saveFeedback;
   REQUIRE(SUCCEEDED(table->Save(saveFeedback)));
   table->Release();
   return file;
}

// Orbit: the insert turns under a fixed camera, which is the same as the camera orbiting it (the wood does not turn)
TEST_CASE("Traced insert orbit [render][traced][clip]")
{
   if (BakeDir().empty())
      return;
   ResetVPX();
#if defined(ENABLE_BGFX)
   const char* const stepsEnv = std::getenv("TRACED_ORBIT_STEPS");
   const int steps = stepsEnv ? atoi(stepsEnv) : 36;
   for (int i = 0; i < steps; i++)
   {
      const string name = "orbit" + std::to_string(100 + i).substr(1);
      const std::filesystem::path file = WithCamera(CreateSingleTable(360.f * (float)i / (float)steps, false, name), 45.f, 230.f, 14.f, name);
      CaptureRender(file.string(), "traced-" + name + ".webp");
   }
#endif
   ResetVPX();
}

// Blink: an incandescent lamp blinking on and off. Screenshots are slower than real time, so the lamp runs on a virtual
// clock: VPX's own #44 filament model is stepped 1/30 s per captured frame, and the light's fade is frozen in between
TEST_CASE("Traced insert blink [render][traced][clip]")
{
   if (BakeDir().empty())
      return;
   ResetVPX();
#if defined(ENABLE_BGFX)
   g_settingsService.GetAppSettings().SetPlayer_PlayfieldWidth(1280, false);
   g_settingsService.GetAppSettings().SetPlayer_PlayfieldHeight(720, false);
   const std::filesystem::path tablePath = WithCamera(CreateSingleTable(0.f, true, "blink"), 45.f, 230.f, 14.f, "blink");
   struct SeqState
   {
      int frame = 0, shot = 0;
      bool pending = false;
      double T = 293.0; // filament temperature (K)
      float fadeUp = 0.f, fadeDown = 0.f;
      std::ofstream log;
   } state;
   state.log.open(GetAssetPath() / "traced-blink-log.txt");
   msgpi_msg_callback onPrepareFrame = [](const unsigned int msgId, void* context, void* msgData)
   {
      SeqState* const state = reinterpret_cast<SeqState*>(context);
      Light* const light = g_pplayer->m_ptable->GetLight("l63"s);
      if (state->frame++ == 0)
      {
         state->fadeUp = light->m_d.m_fadeSpeedUp;
         state->fadeDown = light->m_d.m_fadeSpeedDown;
         light->m_d.m_fadeSpeedUp = light->m_d.m_fadeSpeedDown = 1e-6f; // freeze the real-time fade
         light->m_d.m_state = 0.f;
      }
      const float scale = light->m_d.m_intensity * light->m_d.m_intensity_scale;
      light->m_currentFilamentTemperature = state->T;
      light->m_currentIntensity = bulb_filament_temperature_to_emission(BULB_44, (float)state->T) * scale;
      if (state->frame < 60 || state->pending)
         return;
      if (state->shot >= (std::getenv("TRACED_BLINK_SHOTS") ? atoi(std::getenv("TRACED_BLINK_SHOTS")) : 150))
      {
         g_pplayer->SetCloseState(Player::CS_STOP_PLAY);
         return;
      }
      state->pending = true;
      state->log << state->shot << " T=" << state->T << " I=" << light->m_currentIntensity << " scale=" << scale << " up=" << state->fadeUp << " down=" << state->fadeDown
                 << " inPlay=" << light->m_inPlayState << '\n';
      const string name = "traced-blink-" + std::to_string(1000 + state->shot).substr(1) + ".png";
      g_pplayer->m_renderer->m_renderDevice->CaptureScreenshot({ g_pplayer->m_playfieldWnd }, { GetAssetPath() / name },
         [state, scale](bool)
         {
            // Advance the virtual clock by 1/30 s: 0.7 s on, 0.7 s off, as LightData's own incandescent update does it
            const bool on = ((state->shot / 21) % 2) == 0; // 21 frames = 0.7 s
            const float dt = 1000.f / 30.f;
            const float remaining = min(0.5f, dt * 0.04f * (on ? state->fadeUp : state->fadeDown) / scale);
            state->T = on ? bulb_heat_up(BULB_44, state->T, remaining, 6.3f, 0.f) : bulb_cool_down(BULB_44, state->T, remaining);
            state->shot++;
            state->pending = false;
         });
   };
   CComObject<PinTable>* table;
   CComObject<PinTable>::CreateInstance(&table);
   table->AddRef();
   TestFileFeedback feedback;
   REQUIRE(SUCCEEDED(table->LoadGameFromFilename(tablePath, feedback)));
   LoadProgress loadProgress;
   auto player = std::make_unique<Player>(table, Player::PlayMode::Play, loadProgress);
   const unsigned int msgId = player->m_pluginManager.GetMsgAPI().GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_PREPARE_FRAME);
   player->m_pluginManager.GetMsgAPI().SubscribeMsg(player->m_pluginAPI.GetVPXEndPointId(), msgId, onPrepareFrame, &state);
   player->GameLoop();
   player->m_pluginManager.GetMsgAPI().UnsubscribeMsg(msgId, onPrepareFrame, &state);
   player->m_pluginManager.GetMsgAPI().ReleaseMsgID(msgId);
   player = nullptr;
   table->Release();
#endif
   ResetVPX();
}

// Camera orbit: a free look-at camera (as the live editor's) circles the fixed insert at 'elevation' degrees off vertical,
// in one play session with the static prepass off. TRACED_CAM_ORBIT="elevation,distance,fov,steps"
TEST_CASE("Traced insert camera orbit [render][traced][clip]")
{
   if (BakeDir().empty())
      return;
   ResetVPX();
#if defined(ENABLE_BGFX)
   g_settingsService.GetAppSettings().SetPlayer_PlayfieldWidth(1280, false);
   g_settingsService.GetAppSettings().SetPlayer_PlayfieldHeight(720, false);
   // Anti-aliasing for the capture: TRACED_AA="<supersampling factor>,<post AA mode>" (e.g. "2,0" or "1,6" for SMAA)
   if (const char* const aa = std::getenv("TRACED_AA"))
   {
      float factor = 1.f;
      int post = 0;
      sscanf(aa, "%f,%d", &factor, &post);
      g_settingsService.GetAppSettings().SetPlayer_AAFactor(factor, false);
      g_settingsService.GetAppSettings().SetPlayer_FXAA(post, false);
   }
   struct OrbitState
   {
      float elevation = 45.f, distance = 230.f, fov = 14.f;
      int steps = 24, frame = 0, step = 0, settle = 0;
      bool pending = false;
      Vertex3Ds target;
   } state;
   if (const char* const env = std::getenv("TRACED_CAM_ORBIT"))
      sscanf(env, "%f,%f,%f,%d", &state.elevation, &state.distance, &state.fov, &state.steps);
   const std::filesystem::path tablePath = CreateSingleTable(0.f, false, "camorbit");
   msgpi_msg_callback onPrepareFrame = [](const unsigned int msgId, void* context, void* msgData)
   {
      OrbitState* const state = reinterpret_cast<OrbitState*>(context);
      if (state->frame++ == 0)
      {
         g_pplayer->m_renderer->DisableStaticPrePass(true);
         const PinTable* const table = g_pplayer->m_ptable;
         state->target = Vertex3Ds((table->m_left + table->m_right) * 0.5f, RowY(table), 0.f);
      }
      if (state->step >= state->steps)
      {
         if (!state->pending)
            g_pplayer->SetCloseState(Player::CS_STOP_PLAY);
         return;
      }
      // Azimuth 0 is the player's side (+y), turning counter-clockwise seen from above
      const float e = ANGTORAD(state->elevation), a = ANGTORAD(360.f * (float)state->step / (float)state->steps);
      const Vertex3Ds eye(state->target.x + state->distance * sinf(e) * sinf(a), state->target.y + state->distance * sinf(e) * cosf(a), state->distance * cosf(e));
      // The live editor's convention: a right handed look-at with z reversed, converted to VPX's left handed system
      const Matrix3D RH2LH = Matrix3D::MatrixScale(1.f, 1.f, -1.f);
      const Matrix3D YAxis = Matrix3D::MatrixScale(1.f, -1.f, 1.f);
      const Matrix3D camView = Matrix3D::MatrixLookAtRH(Vertex3Ds(eye.x, eye.y, -eye.z), Vertex3Ds(state->target.x, state->target.y, 0.f), Vertex3Ds(0.f, 0.f, -1.f));
      const Matrix3D camProj = Matrix3D::MatrixPerspectiveFovRH(state->fov, 1280.f / 720.f, 5.f, 50000.f);
      g_pplayer->m_renderer->SetViewProj(RH2LH * camView * YAxis, YAxis * camProj);
      if (state->frame < 60 || state->pending || ++state->settle < 12)
         return;
      state->pending = true;
      const string name = "traced-camorbit" + std::to_string(1000 + state->step).substr(1) + ".png";
      g_pplayer->m_renderer->m_renderDevice->CaptureScreenshot({ g_pplayer->m_playfieldWnd }, { GetAssetPath() / name },
         [state](bool)
         {
            state->step++;
            state->settle = 0;
            state->pending = false;
         });
   };
   CComObject<PinTable>* table;
   CComObject<PinTable>::CreateInstance(&table);
   table->AddRef();
   TestFileFeedback feedback;
   REQUIRE(SUCCEEDED(table->LoadGameFromFilename(tablePath, feedback)));
   LoadProgress loadProgress;
   auto player = std::make_unique<Player>(table, Player::PlayMode::Play, loadProgress);
   const unsigned int msgId = player->m_pluginManager.GetMsgAPI().GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_PREPARE_FRAME);
   player->m_pluginManager.GetMsgAPI().SubscribeMsg(player->m_pluginAPI.GetVPXEndPointId(), msgId, onPrepareFrame, &state);
   player->GameLoop();
   player->m_pluginManager.GetMsgAPI().UnsubscribeMsg(msgId, onPrepareFrame, &state);
   player->m_pluginManager.GetMsgAPI().ReleaseMsgID(msgId);
   player = nullptr;
   table->Release();
#endif
   ResetVPX();
}
