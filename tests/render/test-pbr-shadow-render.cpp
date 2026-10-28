// license:GPLv3+

#include "core/stdafx.h"
#include "../vpx-test.h"

#include "core/VPApp.h"
#include "parts/light.h"
#include "parts/Material.h"
#include "parts/pintable.h"
#include "parts/primitive.h"
#include "parts/surface.h"
#include "parts/flipper.h"
#include "renderer/Renderer.h"

#include "doctest.h"

#include <filesystem>

// Builds a table exercising the PBR orthographic shadow map path: the playfield has a PBR material
// (the shadow receiver), one static and one dynamic-tier shadowed PBR light high above the table
// (ortho layout), plus static (wall) and dynamic (flipper) casters inside their footprint.
static std::filesystem::path CreatePBRShadowTable()
{
   CComObject<PinTable>* table;
   CComObject<PinTable>::CreateInstance(&table);
   table->AddRef();
   TestFileFeedback feedback;
   REQUIRE(SUCCEEDED(table->LoadGameFromFilename(GetAssetPath() / "test000-default-table.vpx", feedback)));
   const float cx = (table->m_left + table->m_right) * 0.5f;
   const float cy = (table->m_top + table->m_bottom) * 0.5f;

   Material* const pfMat = new Material();
   pfMat->m_name = "pbr_playfield";
   pfMat->m_type = Material::MaterialType::PBR;
   pfMat->m_fRoughness = 0.5f;
   pfMat->m_fMetallic = 0.f;
   pfMat->m_fOcclusion = 1.f;
   pfMat->m_fEmissionIntensity = 0.f;
   pfMat->m_cBase = RGB(230, 230, 230);
   pfMat->m_bOpacityActive = false;
   table->AddMaterial(pfMat);
   table->m_playfieldMaterial = pfMat->m_name;

   // Static caster: a wall in the middle of the light footprint
   Surface* const wall = Surface::COMCreate();
   wall->Init(cx, cy, false);
   wall->SetName("caster_wall"s);
   wall->m_d.m_heighttop = 100.f;
   table->AddPart(wall);
   wall->Release();

   // Dynamic caster
   Flipper* const flipper = Flipper::COMCreate();
   flipper->Init(cx - 150.f, cy + 300.f, false);
   flipper->SetName("caster_flipper"s);
   table->AddPart(flipper);
   flipper->Release();

   // Two shadowed PBR lights far above the glass top => ortho shadow layout, one static tier, one static+dynamic tier
   for (int i = 0; i < 2; i++)
   {
      Light* const light = Light::COMCreate();
      light->Init(cx + (i == 0 ? -200.f : 200.f), cy - 400.f, false);
      light->SetName("pbr_shadow_light_" + std::to_string(i));
      light->m_d.m_type = LightType::PBR;
      light->m_d.m_state = 1.f;
      light->m_d.m_intensity = 5.f;
      light->m_d.m_falloff = 3000.f;
      light->m_d.m_falloff_power = 2.f;
      light->m_d.m_height = 500.f;
      light->m_d.m_color = i == 0 ? RGB(255, 220, 180) : RGB(200, 220, 255);
      light->m_d.m_showBulbMesh = false;
      light->m_d.m_visible = true;
      light->m_d.m_shadows = i == 0 ? ShadowMode::PBR_STATIC_SHADOWS : ShadowMode::PBR_STATIC_N_DYNAMIC_SHADOWS;
      table->AddPart(light);
      light->Release();
   }

   // A near light under the glass top: takes the cube layout (6 face layers in the near array).
   // Placed close to the wall so its shadow is unambiguous: light at (cx+150, cy), wall at (cx, cy)
   // should project the wall's shadow toward -X/+spread on the playfield.
   {
      Light* const light = Light::COMCreate();
      light->Init(cx + 200.f, cy - 100.f, false);
      light->SetName("pbr_shadow_light_near"s);
      light->m_d.m_type = LightType::PBR;
      light->m_d.m_state = 1.f;
      light->m_d.m_intensity = 12.f;
      light->m_d.m_falloff = 500.f;
      light->m_d.m_falloff_power = 2.f;
      light->m_d.m_height = 60.f;
      light->m_d.m_color = RGB(255, 255, 255);
      light->m_d.m_showBulbMesh = false;
      light->m_d.m_visible = true;
      light->m_d.m_shadows = ShadowMode::PBR_STATIC_SHADOWS;
      table->AddPart(light);
      light->Release();
   }

   const std::filesystem::path file = GetTestTmpDir() / "pbr-shadow-table.vpx";
   table->m_filename = file;
   TestFileFeedback saveFeedback;
   REQUIRE(SUCCEEDED(table->Save(saveFeedback)));
   CHECK(std::filesystem::exists(file));
   table->Release();
   return file;
}

TEST_CASE("PBR shadow map render [render]")
{
   ResetVPX();
   const std::filesystem::path tableFile = CreatePBRShadowTable();

#if defined(ENABLE_BGFX)
   g_settingsService.GetAppSettings().SetPlayer_GfxBackend("Direct3D11"s, false);
   g_settingsService.GetAppSettings().SetPlayer_DepthPrepass(2, false);
   CaptureRender(tableFile.string(), "pbr-shadow-Test.webp"s);
   CHECK(std::filesystem::exists(GetAssetPath() / "pbr-shadow-Test.webp"));
#endif

   ResetVPX();
}

// Same render, but the near light is moved mid-play: its baked shadow map must be invalidated and
// re-rendered, keeping the wall shadow anchored to the wall instead of following the light.
TEST_CASE("PBR shadow map follows light move [render]")
{
   ResetVPX();
   const std::filesystem::path tableFile = CreatePBRShadowTable();

#if defined(ENABLE_BGFX)
   g_settingsService.GetAppSettings().SetPlayer_GfxBackend("Direct3D11"s, false);
   g_settingsService.GetAppSettings().SetPlayer_DepthPrepass(2, false);

   struct MoveState
   {
      std::filesystem::path tmpScreenshotPath;
      bool done;
      bool captureRequested;
      bool moved;
   } state = { GetAssetPath() / "pbr-shadow-move-Test.webp"s, false, false, false };
   msgpi_msg_callback onPrepareFrame = [](const unsigned int msgId, void* context, void* msgData)
   {
      MoveState* state = reinterpret_cast<MoveState*>(context);
      if (state->captureRequested || g_pplayer->m_overall_frames < 25)
         return;
      if (!state->moved)
      {
         // Move the near light once the first frames rendered (its shadow map was baked at the old spot)
         for (auto part : g_pplayer->m_ptable->GetParts())
         {
            if (part->GetItemType() != eItemLight)
               continue;
            Light* const light = static_cast<Light*>(part);
            if (light->GetName() == "pbr_shadow_light_near"s)
            {
               light->m_d.m_vCenter.y += 300.f;
               state->moved = true;
            }
         }
         return; // Give a few frames for the re-bake to land before capturing
      }
      if (g_pplayer->m_overall_frames < 40)
         return;
      if (g_pplayer->m_renderer->IsTemporalAccumulationInProgress() && g_pplayer->m_overall_frames < 512)
         return;
      state->captureRequested = true;
      g_pplayer->m_renderer->m_renderDevice->CaptureScreenshot({ g_pplayer->m_playfieldWnd }, { state->tmpScreenshotPath },
         [state](bool success)
         {
            g_pplayer->SetCloseState(Player::CS_STOP_PLAY);
            state->done = true;
         });
   };

   CComObject<PinTable>* table;
   CComObject<PinTable>::CreateInstance(&table);
   table->AddRef();
   TestFileFeedback feedback;
   REQUIRE(SUCCEEDED(table->LoadGameFromFilename(tableFile, feedback)));
   LoadProgress loadProgress;
   auto player = std::make_unique<Player>(table, Player::PlayMode::Play, loadProgress);
   const unsigned int onPrepareFrameMsgId = player->m_pluginManager.GetMsgAPI().GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_PREPARE_FRAME);
   player->m_pluginManager.GetMsgAPI().SubscribeMsg(player->m_pluginAPI.GetVPXEndPointId(), onPrepareFrameMsgId, onPrepareFrame, &state);
   player->GameLoop();
   player->m_pluginManager.GetMsgAPI().UnsubscribeMsg(onPrepareFrameMsgId, onPrepareFrame, &state);
   player->m_pluginManager.GetMsgAPI().ReleaseMsgID(onPrepareFrameMsgId);
   player = nullptr;
   table->Release();
   CHECK(state.done);
   CHECK(std::filesystem::exists(GetAssetPath() / "pbr-shadow-move-Test.webp"s));
#endif

   ResetVPX();
}
