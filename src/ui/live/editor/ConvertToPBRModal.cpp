// license:GPLv3+

#include "core/stdafx.h"

#include "ConvertToPBRModal.h"
#include "parts/PBRConversion.h"
#include "parts/light.h"
#include "parts/Material.h"
#include "ui/live/EditorUI.h"
#include "ui/live/LiveUI.h"
#include "PropertyPane.h"

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

// Right-justified gray text (same look as TextDisabled hints)
static void TextDisabledRight(const string &text)
{
   const float offset = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(text.c_str()).x;
   ImGui::SetCursorPosX(ImGui::GetCursorPosX() + max(0.f, offset));
   ImGui::TextDisabled("%s", text.c_str());
}

// Underlined label, same style as PropertyPane::Separator
static void UnderlinedLabel(const char *label)
{
   ImGui::TextUnformatted(label);
   const ImVec2 headerMin = ImGui::GetItemRectMin();
   const ImVec2 headerMax = ImGui::GetItemRectMax();
   ImGui::GetWindowDrawList()->AddLine(ImVec2(headerMin.x, headerMax.y), ImVec2(headerMax.x, headerMax.y), ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
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
      item.scene = light->GetCurrentHeight() > table->m_glassTopHeight;
      item.insert = !item.scene && PBRIsInsertLight(light->GetCurrentHeight(), light->m_d.m_falloff, m_insertRange);
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
      else if (item.insert)
      {
         item.convert = false; // Insert lights keep their rendered surface: never converted
         item.note = "insert";
      }
      else if (light->m_d.m_imageMode)
      {
         item.convert = m_convertClassic && m_convertImageMode; // pass-through is a Classic light mode
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
      m_lights.push_back(item);
   }
}

void ConvertToPBRModal::Apply()
{
   PinTable *const table = m_editor.m_table;
   PBRConvertMaterialOptions opts;
   opts.deriveMetallicFromGlossy = m_deriveMetallic;
   vector<std::pair<uint64_t, Material>> snapshots; // (runtime identity of the material, its state)
   int nMat = 0, nLight = 0;
   for (const MaterialItem &it : m_materials)
      if (it.convert)
         snapshots.emplace_back(it.mat->m_uid.Value(), *it.mat);
   m_editor.m_undo.BeginUndo();
   for (const LightItem &it : m_lights)
      if (it.convert)
         m_editor.m_undo.MarkForUndo(it.light);
   m_editor.m_undo.MarkForAction(
      [table, snapshots]()
      {
         // A material deleted since (not undoable) is gone: skip it (found by its runtime identity, as a new one may reuse its name or address)
         for (const auto &[uid, snapshot] : snapshots)
         {
            if (Material *const mat = MaterialEditUndo::Find(table, uid))
               *mat = snapshot;
            else
               MaterialEditUndo::NoteSkipped();
         }
      },
      // Redo: the converted state of the materials, captured when the step is undone
      [table, snapshots]() -> std::function<void()>
      {
         vector<std::pair<uint64_t, Material>> converted;
         for (const auto &[uid, snapshot] : snapshots)
         {
            if (const Material *const mat = MaterialEditUndo::Find(table, uid))
               converted.emplace_back(uid, *mat); // (a deleted material is already reported by the undo)
         }
         return [table, converted]()
         {
            for (const auto &[uid, state] : converted)
               if (Material *const mat = MaterialEditUndo::Find(table, uid))
                  *mat = state;
               else
                  MaterialEditUndo::NoteSkipped();
         };
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
         const int sel = it.scene ? m_shadowScene : m_shadowNear;
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
         TextDisabledRight(std::to_string(nConvert) + " will convert, "s + std::to_string(nPBR) + " already PBR, "s + std::to_string(nSkip) + " skipped"s);
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
                  if (ImGui::Selectable(it.mat->m_name.c_str(), m_editor.m_selection.GetMaterial() == it.mat, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                     m_editor.SetSelection(Selection(it.mat));
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
         TextDisabledRight(std::to_string(nL) + " will convert"s);
         dirty |= ImGui::Checkbox("Convert Halo lights", &m_convertHalo);
         ImGui::SetItemTooltip("A converted Halo light loses its rendered halo surface: only its light contribution remains.");
         dirty |= ImGui::Checkbox("Convert Classic lights", &m_convertClassic);
         ImGui::SetItemTooltip("Off by default: converting removes the rendered light surface (usually the insert artwork), so most Classic lights should be kept as-is.");
         if (m_convertClassic) // pass-through only applies to Classic lights
            dirty |= ImGui::Checkbox("Convert pass-through (Image Mode) lights", &m_convertImageMode);
         ImGui::SliderFloat("Intensity scale", &m_intensityScale, 0.05f, 10.f, "%.2f", ImGuiSliderFlags_Logarithmic);
         UnderlinedLabel("Shadow Mode");
         dirty |= ImGui::InputFloat("Insert range (VPU)", &m_insertRange);
         ImGui::SetItemTooltip(
            "Falloff distance (in VPU) below which a light under the glass is classified as an insert. Lights below the playfield are always inserts. Insert lights are never converted.");
         static const char *tierNames = "Heuristic\0None\0Static\0Static+Ball\0Static+Dynamic\0";
         ImGui::Combo("Scene lights (above glass)", &m_shadowScene, tierNames);
         ImGui::Combo("Near lights", &m_shadowNear, tierNames);
         UnderlinedLabel("Light Conversion");
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
                  const int sel = it.scene ? m_shadowScene : m_shadowNear;
                  const ShadowMode eff = sel == 0 ? PBRSuggestShadowMode(l->GetCurrentHeight(), l->m_d.m_falloff, table->m_glassTopHeight, m_insertRange) : tiers[sel - 1];
                  ImGui::TableNextRow();
                  ImGui::PushID(l);
                  if (pbr || it.insert)
                     ImGui::BeginDisabled();
                  ImGui::TableNextColumn();
                  ImGui::Checkbox("##c", &it.convert);
                  if (pbr || it.insert)
                     ImGui::EndDisabled();
                  ImGui::TableNextColumn();
                  if (ImGui::Selectable(l->GetName().c_str(), m_editor.IsEditableSelected(l), ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                  {
                     const auto itPart = m_editor.m_editableMap.find(l);
                     if (itPart != m_editor.m_editableMap.end())
                        m_editor.SetSelection(Selection(itPart->second));
                  }
                  ImGui::TableNextColumn();
                  ImGui::TextUnformatted(typeNames[it.fromType]);
                  ImGui::TableNextColumn();
                  ImGui::TextUnformatted(it.scene ? "Scene" : it.insert ? "Insert" : "Near");
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
