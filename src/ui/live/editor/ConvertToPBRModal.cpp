// license:GPLv3+

#include "core/stdafx.h"

#include "ConvertToPBRModal.h"
#include "parts/PBRConversion.h"
#include "parts/light.h"
#include "parts/Material.h"
#include "ui/live/EditorUI.h"
#include "ui/live/LiveUI.h"

namespace VPX::EditorUI
{

static const char *ID_CONVERT_TO_PBR = "Convert to PBR";

ConvertToPBRModal::ConvertToPBRModal(EditorUI &editor)
   : m_editor(editor)
{
}

void ConvertToPBRModal::Close()
{
   m_visible = false;
}

static bool IsPhysicsOnly(const string &propertyName)
{
   return propertyName.find("hysics") != string::npos; // 'Physics Material' & friends
}

void ConvertToPBRModal::Scan()
{
   m_materials.clear();
   m_lights.clear();
   PinTable *const table = m_editor.m_table;
   for (Material *mat : table->m_materials)
   {
      MaterialItem item;
      item.mat = mat;
      vector<WhereUsedInfo> used;
      table->ShowWhereMaterialUsed(used, mat);
      for (const WhereUsedInfo &u : used)
      {
         if (IsPhysicsOnly(u.whereUsedPropertyName))
            item.otherUsers++;
         else
            item.visualUsers++;
      }
      if (mat->m_type == Material::MaterialType::PBR)
      {
         item.convert = false;
         item.note = "already PBR";
      }
      else if (used.empty())
      {
         item.convert = m_convertUnreferenced;
         item.note = m_convertUnreferenced ? "unreferenced" : "unreferenced (skipped)";
      }
      else if (item.visualUsers == 0)
      {
         item.convert = m_convertUnsupported;
         item.note = item.otherUsers > 0 ? "physics-only usage" : "unsupported usage";
      }
      else
      {
         item.convert = true;
         item.note = std::to_string(item.visualUsers) + " visual usage(s)";
      }
      item.newRough = PBRLegacyRoughnessToPerceptual(mat->m_fRoughness);
      item.newMetal = mat->m_type == Material::MaterialType::METAL ? 1.f : 0.f;
      m_materials.push_back(item);
   }

   for (IEditable *part : table->GetParts())
   {
      if (part->GetItemType() != ItemTypeEnum::eItemLight)
         continue;
      Light *const light = static_cast<Light *>(part);
      LightItem item;
      item.light = light;
      item.fromType = (int)light->m_d.m_type;
      item.scriptBulbRef = false;
      if (light->m_d.m_type == LightType::PBR)
      {
         item.convert = false;
         item.note = "already PBR";
      }
      else if (light->m_desktopBackdrop)
      {
         item.convert = false;
         item.note = "hidden";
      }
      else if (light->m_d.m_imageMode)
      {
         item.convert = m_convertImageMode;
         item.note = "artwork light (Image Mode)";
      }
      else
      {
         item.convert = light->m_d.m_type == LightType::Halo ? m_convertHalo : m_convertClassic;
         item.note = "loses rendered surface";
         // advisory check for script '.Bulb' writes that would revert the type
         const string needle = string(light->GetName()) + ".Bulb"s;
         string script = table->m_script_text;
         std::transform(script.begin(), script.end(), script.begin(), ::tolower);
         string nl = needle;
         std::transform(nl.begin(), nl.end(), nl.begin(), ::tolower);
         item.scriptBulbRef = script.find(nl) != string::npos;
         if (item.scriptBulbRef)
            item.note += ", script assigns .Bulb";
      }
      item.shadow = PBRSuggestShadowMode(light->GetCurrentHeight(), light->m_d.m_falloff, table->m_glassTopHeight, m_insertRange);
      m_lights.push_back(item);
   }
}

void ConvertToPBRModal::Apply()
{
   PinTable *const table = m_editor.m_table;
   PBRConvertMaterialOptions opts;
   opts.deriveMetallicFromGlossy = m_deriveMetallic;
   vector<Material> snapshots;
   int nMat = 0, nLight = 0;
   for (const MaterialItem &it : m_materials)
      if (it.convert)
         snapshots.push_back(*it.mat);
   m_editor.m_undo.BeginUndo();
   for (const LightItem &it : m_lights)
      if (it.convert)
         m_editor.m_undo.MarkForUndo(it.light);
   m_editor.m_undo.MarkForAction([this, snapshots]() {
      int i = 0;
      for (const MaterialItem &it : m_materials)
         if (it.convert)
            *it.mat = snapshots[i++];
   });
   for (MaterialItem &it : m_materials)
      if (it.convert)
      {
         PBRConvertMaterial(it.mat, opts);
         nMat++;
      }
   static const ShadowMode tiers[4] = { ShadowMode::NONE, ShadowMode::PBR_STATIC_SHADOWS, ShadowMode::PBR_STATIC_N_BALL_SHADOWS, ShadowMode::PBR_STATIC_N_DYNAMIC_SHADOWS };
   for (LightItem &it : m_lights)
      if (it.convert)
      {
         Light *const l = it.light;
         const bool scene = l->GetCurrentHeight() > table->m_glassTopHeight;
         const int sel = scene ? m_shadowScene : l->m_d.m_falloff < m_insertRange ? m_shadowInsert : m_shadowNear;
         l->m_d.m_shadows = sel == 0 ? PBRSuggestShadowMode(l->GetCurrentHeight(), l->m_d.m_falloff, table->m_glassTopHeight, m_insertRange) : tiers[sel - 1];
         l->m_d.m_type = LightType::PBR;
         l->m_d.m_intensity *= m_intensityScale;
         nLight++;
      }
   m_editor.m_undo.EndUndo();
   for (const LightItem &it : m_lights)
      if (it.convert)
         m_editor.m_renderer->ReinitRenderable(it.light);
   table->SetNonUndoableDirty(SaveDirtyState::Dirty);
   m_result = "Converted "s + std::to_string(nMat) + " materials and "s + std::to_string(nLight) + " lights"s;
   Scan();
}

void ConvertToPBRModal::Render()
{
   if (!m_visible)
      return;

   ImGui::SetNextWindowSize(ImVec2(420.f * m_editor.m_liveUI.GetDPI(), 0));
   if (ImGui::Begin(ID_CONVERT_TO_PBR, &m_visible))
   {
      bool dirty = false;
      if (m_editor.m_table->IsLocked() || m_editor.IsInspectMode())
         ImGui::BeginDisabled();

      static const ShadowMode tiers[4] = { ShadowMode::NONE, ShadowMode::PBR_STATIC_SHADOWS, ShadowMode::PBR_STATIC_N_BALL_SHADOWS, ShadowMode::PBR_STATIC_N_DYNAMIC_SHADOWS };
      const auto shadowName = [](ShadowMode s)
      {
         return s == ShadowMode::NONE ? "None" : s == ShadowMode::PBR_STATIC_SHADOWS ? "Static" : s == ShadowMode::PBR_STATIC_N_BALL_SHADOWS ? "Static+Ball" : "Static+Dyn";
      };
      PinTable *const table = m_editor.m_table;

      // Materials
      int nConvert = 0, nPBR = 0, nSkip = 0;
      for (const MaterialItem &it : m_materials)
         it.convert ? nConvert++ : (it.note == "already PBR" ? nPBR++ : nSkip++);
      if (ImGui::CollapsingHeader("Materials", ImGuiTreeNodeFlags_DefaultOpen))
      {
         ImGui::Text("%d will convert, %d already PBR, %d skipped", nConvert, nPBR, nSkip);
         dirty |= ImGui::Checkbox("Convert unreferenced materials", &m_convertUnreferenced);
         dirty |= ImGui::Checkbox("Convert materials used only by non-PBR parts", &m_convertUnsupported);
         dirty |= ImGui::Checkbox("Derive metallic from glossy color", &m_deriveMetallic);
         if (ImGui::BeginChild("##mats", ImVec2(0.f, 140.f * m_editor.m_liveUI.GetDPI()), ImGuiChildFlags_Borders))
         {
            if (ImGui::BeginTable("##matTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY))
            {
               ImGui::TableSetupColumn("Use", ImGuiTableColumnFlags_WidthFixed);
               ImGui::TableSetupColumn("Name");
               ImGui::TableSetupColumn("From");
               ImGui::TableSetupColumn("Rough");
               ImGui::TableSetupColumn("Metal");
               ImGui::TableSetupColumn("Usage");
               ImGui::TableHeadersRow();
               for (MaterialItem &it : m_materials)
               {
                  const bool pbr = it.note == "already PBR";
                  ImGui::TableNextRow();
                  ImGui::PushID(it.mat);
                  if (pbr)
                     ImGui::BeginDisabled();
                  ImGui::TableNextColumn();
                  ImGui::Checkbox("##c", &it.convert);
                  if (pbr)
                     ImGui::EndDisabled();
                  ImGui::TableNextColumn();
                  ImGui::TextUnformatted(it.mat->m_name.c_str());
                  ImGui::TableNextColumn();
                  ImGui::TextUnformatted(pbr ? "PBR" : it.mat->m_type == Material::MaterialType::METAL ? "Metal" : "Basic");
                  ImGui::TableNextColumn();
                  ImGui::Text("%.2f", it.newRough);
                  ImGui::TableNextColumn();
                  ImGui::Text("%.1f", it.newMetal);
                  ImGui::TableNextColumn();
                  ImGui::TextUnformatted(it.note.c_str());
                  ImGui::PopID();
               }
               ImGui::EndTable();
            }
         }
         ImGui::EndChild();
      }

      // Lights
      if (ImGui::CollapsingHeader("Lights", ImGuiTreeNodeFlags_DefaultOpen))
      {
         int nL = 0;
         for (const LightItem &it : m_lights)
            nL += it.convert ? 1 : 0;
         ImGui::Text("%d will convert", nL);
         dirty |= ImGui::Checkbox("Convert Halo lights", &m_convertHalo);
         if (m_convertHalo)
            ImGui::TextDisabled("  rendered surface disappears");
         dirty |= ImGui::Checkbox("Convert Classic lights", &m_convertClassic);
         dirty |= ImGui::Checkbox("Convert pass-through (Image Mode) lights", &m_convertImageMode);
         ImGui::SliderFloat("Intensity scale", &m_intensityScale, 0.05f, 10.f, "%.2f", ImGuiSliderFlags_Logarithmic);
         static const char *tierNames = "Heuristic\0None\0Static\0Static+Ball\0Static+Dynamic\0";
         ImGui::Combo("Scene lights (above glass)", &m_shadowScene, tierNames);
         ImGui::Combo("Near lights", &m_shadowNear, tierNames);
         ImGui::Combo("Insert-like lights (short range)", &m_shadowInsert, tierNames);
         ImGui::InputFloat("Insert range (VPU)", &m_insertRange);
         if (ImGui::BeginChild("##lights", ImVec2(0.f, 160.f * m_editor.m_liveUI.GetDPI()), ImGuiChildFlags_Borders))
         {
            if (ImGui::BeginTable("##lightTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY))
            {
               ImGui::TableSetupColumn("Use", ImGuiTableColumnFlags_WidthFixed);
               ImGui::TableSetupColumn("Name");
               ImGui::TableSetupColumn("From");
               ImGui::TableSetupColumn("Class");
               ImGui::TableSetupColumn("Shadow");
               ImGui::TableSetupColumn("Note");
               ImGui::TableHeadersRow();
               static const char *typeNames[] = { "Classic", "Halo", "PBR" };
               for (LightItem &it : m_lights)
               {
                  const bool pbr = it.fromType == (int)LightType::PBR;
                  Light *const l = it.light;
                  const bool scene = l->GetCurrentHeight() > table->m_glassTopHeight;
                  const bool insertLike = !scene && l->m_d.m_falloff < m_insertRange;
                  const int sel = scene ? m_shadowScene : insertLike ? m_shadowInsert : m_shadowNear;
                  const ShadowMode eff = sel == 0 ? PBRSuggestShadowMode(l->GetCurrentHeight(), l->m_d.m_falloff, table->m_glassTopHeight, m_insertRange) : tiers[sel - 1];
                  ImGui::TableNextRow();
                  ImGui::PushID(l);
                  if (pbr)
                     ImGui::BeginDisabled();
                  ImGui::TableNextColumn();
                  ImGui::Checkbox("##c", &it.convert);
                  if (pbr)
                     ImGui::EndDisabled();
                  ImGui::TableNextColumn();
                  ImGui::TextUnformatted(l->GetName().c_str());
                  ImGui::TableNextColumn();
                  ImGui::TextUnformatted(typeNames[it.fromType]);
                  ImGui::TableNextColumn();
                  ImGui::TextUnformatted(scene ? "Scene" : insertLike ? "Insert" : "Near");
                  ImGui::TableNextColumn();
                  ImGui::TextUnformatted(it.convert ? shadowName(eff) : "-");
                  ImGui::TableNextColumn();
                  ImGui::TextUnformatted(it.note.c_str());
                  ImGui::PopID();
               }
               ImGui::EndTable();
            }
         }
         ImGui::EndChild();
      }

      if (!m_editor.m_player->m_renderer->m_renderDevice->m_pbrActive)
         ImGui::TextWrapped("PBR shading is off (Player_PBR setting or non-BGFX backend) - conversion still applies, rendering stays legacy.");

      const int nTotal = [&] { int n = 0; for (const MaterialItem &i : m_materials) n += i.convert; for (const LightItem &i : m_lights) n += i.convert; return n; }();
      ImGui::BeginDisabled(nTotal == 0);
      if (ImGui::Button(("Convert "s + std::to_string(nTotal) + " items"s).c_str()))
         Apply();
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button("Close"))
         Close();
      if (!m_result.empty())
         ImGui::TextWrapped("%s", m_result.c_str());

      if (m_editor.m_table->IsLocked() || m_editor.IsInspectMode())
         ImGui::EndDisabled();
      if (dirty)
         Scan();
   }
   ImGui::End();
}

}
