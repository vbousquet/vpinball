#include "core/stdafx.h"
#include "PrimitiveUIPart.h"

#include "core/extern.h"
#include "core/player.h"
#include "math/matrix.h"
#include "utils/color.h"


namespace VPX::EditorUI
{

PrimitiveUIPart::PrimitiveUIPart(Primitive* primitive)
   : EditableUIPart(primitive, { &Data::m_visible })
{
}

PrimitiveUIPart::TransformMask PrimitiveUIPart::GetTransform(Matrix3D& transform)
{
   const Matrix3D Smatrix = Matrix3D::MatrixScale(m_part->m_d.m_vSize.x, m_part->m_d.m_vSize.y, m_part->m_d.m_vSize.z);
   const Matrix3D Tmatrix = Matrix3D::MatrixTranslate(m_part->m_d.m_vPosition);
   const Matrix3D Rmatrix = (Matrix3D::MatrixRotateZ(ANGTORAD(m_part->m_d.m_aRotAndTra[2])) //
                               * Matrix3D::MatrixRotateY(ANGTORAD(m_part->m_d.m_aRotAndTra[1]))) //
      * Matrix3D::MatrixRotateX(ANGTORAD(m_part->m_d.m_aRotAndTra[0]));
   transform = (Smatrix * Rmatrix) * Tmatrix; // fullMatrix = Scale * Rotate * Translate
   return TM_TRSAny;
}

void PrimitiveUIPart::SetTransform(const vec3& pos, const vec3& scale, const vec3& rot)
{
   m_part->m_d.m_vPosition = pos;
   m_part->m_d.m_aRotAndTra[0] = rot.x;
   m_part->m_d.m_aRotAndTra[1] = rot.y;
   m_part->m_d.m_aRotAndTra[2] = rot.z;
   m_part->m_d.m_vSize = scale;
}

void PrimitiveUIPart::RenderOverlay(const EditorRenderContext& ctx)
{
   ctx.DrawWireframe(m_part);
}

void PrimitiveUIPart::UpdatePropertyPane(PropertyPane& props)
{
   props.EditableHeader("Primitive"s, m_part);

   // Apply asynchronous mesh file dialog results (not in the hidden pin harvest pane, which would open the dialogs out of sight)
   if (!props.IsHarvest() && m_pendingMeshImport && !m_pendingMeshImport->empty())
   {
      // A file was selected: reset the options to their defaults and show the import option dialog
      m_meshImportFileName = *m_pendingMeshImport;
      m_pendingMeshImport = nullptr;
      m_meshUnitsMeters = false;
      m_meshImportAbsolutePosition = false;
      m_meshImportCenterMesh = false;
      m_meshImportMaterial = false;
      m_meshImportAnimation = false;
      m_meshImportNoForsyth = false;
      m_meshImportFailed = false;
      ImGui::OpenPopup("Wavefront OBJ Importer");
   }
   if (!props.IsHarvest() && m_pendingMeshExport && !m_pendingMeshExport->empty())
   {
      // A file was selected: reset the options to their defaults and show the export option dialog
      m_meshExportFileName = *m_pendingMeshExport;
      m_pendingMeshExport = nullptr;
      m_meshUnitsMeters = false;
      m_meshExportFailed = false;
      ImGui::OpenPopup("Wavefront OBJ Exporter");
   }

   // Import option dialog (same options as the WinUI 'Wavefront OBJ Importer' dialog)
   if (!m_meshImportFileName.empty())
   {
      if (ImGui::BeginPopupModal("Wavefront OBJ Importer", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
      {
         ImGui::TextWrapped("%s", m_meshImportFileName.c_str());
         ImGui::Spacing();
         UpdateMeshUnitsUI();
         ImGui::SeparatorText("Options");
         ImGui::Columns(2, nullptr, true);
         ImGui::Checkbox("Center mesh to its midpoint", &m_meshImportCenterMesh);
         int position = m_meshImportAbsolutePosition ? 1 : 0;
         ImGui::RadioButton("Place at primitive's position", &position, 0);
         ImGui::RadioButton("Place at mesh's absolute position (use mesh's midpoint)", &position, 1);
         m_meshImportAbsolutePosition = position != 0;
         ImGui::NextColumn();
         ImGui::Checkbox("Import mesh's material", &m_meshImportMaterial);
         ImGui::Checkbox("Import Animation Sequence", &m_meshImportAnimation);
         ImGui::Checkbox("Do not reorder/optimize data", &m_meshImportNoForsyth);
         ImGui::Columns(1);
         if (m_meshImportFailed)
            ImGui::TextColored(ImVec4(1.f, 0.2f, 0.2f, 1.f), "Failed to import file!");
         ImGui::Separator();
         if (ImGui::Button("Import"))
         {
            const std::filesystem::path meshPath = PathFromUTF8(m_meshImportFileName); // The dialog and the table use UTF-8
            m_part->m_d.m_meshFileName = PathToUTF8(meshPath.filename());
            m_meshImportFailed = !m_part->LoadMesh(meshPath, m_meshUnitsMeters ? MeshUnits::Meters : MeshUnits::VPUnits, m_meshImportAbsolutePosition, m_meshImportCenterMesh,
               m_meshImportMaterial, m_meshImportAnimation, !m_meshImportNoForsyth);
            if (!m_meshImportFailed)
            {
               m_meshImportFileName.clear();
               ImGui::CloseCurrentPopup();
            }
         }
         ImGui::SameLine();
         if (ImGui::Button("Cancel"))
         {
            m_meshImportFileName.clear();
            ImGui::CloseCurrentPopup();
         }
         ImGui::EndPopup();
      }
      else
         m_meshImportFileName.clear();
   }

   // Export option dialog (same options as the WinUI 'Wavefront OBJ Exporter' dialog)
   if (!m_meshExportFileName.empty())
   {
      if (ImGui::BeginPopupModal("Wavefront OBJ Exporter", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
      {
         ImGui::TextWrapped("%s", m_meshExportFileName.c_str());
         ImGui::Spacing();
         UpdateMeshUnitsUI();
         if (m_meshExportFailed)
            ImGui::TextColored(ImVec4(1.f, 0.2f, 0.2f, 1.f), "Failed to export file!");
         ImGui::Separator();
         if (ImGui::Button("Export"))
         {
            m_meshExportFailed = !m_part->m_mesh.SaveWavefrontObj(
               PathFromUTF8(m_meshExportFileName), m_part->m_d.m_use3DMesh ? m_part->m_name : "Primitive"s, m_meshUnitsMeters ? MeshUnits::Meters : MeshUnits::VPUnits);
            if (!m_meshExportFailed)
            {
               m_meshExportFileName.clear();
               ImGui::CloseCurrentPopup();
            }
         }
         ImGui::SameLine();
         if (ImGui::Button("Cancel"))
         {
            m_meshExportFileName.clear();
            ImGui::CloseCurrentPopup();
         }
         ImGui::EndPopup();
      }
      else
         m_meshExportFileName.clear();
   }

   if (props.BeginSection("Visuals"s))
   {
      props.Separator("Geometry"s);
      if (ImGui::Button("Import Mesh"))
         ImportMesh();
      ImGui::SameLine();
      ImGui::BeginDisabled(!props.GetEditedPart<Primitive>(m_part)->m_d.m_use3DMesh);
      if (ImGui::Button("Export Mesh"))
         ExportMesh();
      ImGui::EndDisabled();
      if (props.GetEditedPart<Primitive>(m_part)->m_d.m_use3DMesh)
      {
         ImGui::BeginDisabled();
         props.InputString<Primitive>(
            m_part, "Mesh File"s, //
            [](const Primitive* primitive) { return primitive->m_d.m_meshFileName; }, //
            [](Primitive*, const string&) {});
         ImGui::EndDisabled();
      }
      else
      {
         props.InputInt<Primitive>(
            m_part, "Sides"s, //
            [](const Primitive* primitive) { return primitive->m_d.m_Sides; }, //
            [](Primitive* primitive, int v) { primitive->SetSides(v); });
         props.Checkbox<Primitive>(
            m_part, "Draw Textures Inside"s, //
            [](const Primitive* primitive) { return primitive->m_d.m_drawTexturesInside; }, //
            [](Primitive* primitive, bool v) { primitive->m_d.m_drawTexturesInside = v; }, "Render the image on the inside faces of the primitive");
      }

      props.Separator("Render Options"s);
      props.Checkbox<Primitive>(
         m_part, "Visible"s, //
         [this](const Primitive* primitive) { return GetVisibility(primitive); }, //
         [this](Primitive* primitive, bool v) { SetVisibility(primitive, v); });
      props.Checkbox<Primitive>(
         m_part, "Static Rendering"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_staticRendering; }, //
         [](Primitive* primitive, bool v) { primitive->m_d.m_staticRendering = v; }, "Bake this part into the static prerender pass (faster, but it cannot move or change at runtime)");
      props.Checkbox<Primitive>(
         m_part, "Reflection Enabled"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_reflectionEnabled; }, //
         [](Primitive* primitive, bool v) { primitive->m_d.m_reflectionEnabled = v; }, "Render this part in the table's reflections");
      props.InputFloat<Primitive>(
         m_part, "Depth Bias"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_depthBias; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_depthBias = v; }, PropertyPane::Unit::None, 0,
         "Depth offset applied at render time to fix flickering between overlapping parts");
      props.Checkbox<Primitive>(
         m_part, "Depth Mask"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_useDepthMask; }, //
         [](Primitive* primitive, bool v) { primitive->m_d.m_useDepthMask = v; },
         "Write this part to the depth buffer (off lets parts behind it show through, for hollow or transparent meshes)");
      props.Checkbox<Primitive>(
         m_part, "Render Backfaces"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_backfacesEnabled; }, //
         [](Primitive* primitive, bool v) { primitive->m_d.m_backfacesEnabled = v; }, "Draw the back faces of the mesh as well as the front ones");
      props.Checkbox<Primitive>(
         m_part, "Cull Back Faces"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_cullBackfaces; }, //
         [](Primitive* primitive, bool v) { primitive->m_d.m_cullBackfaces = v; },
         "Draw front faces only when Depth Mask is off; use for closed, double-walled transparent meshes. Also sorts its triangles back to front for the camera.");
      props.Checkbox<Primitive>(
         m_part, "Additive Blend"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_addBlend; }, //
         [](Primitive* primitive, bool v) { primitive->m_d.m_addBlend = v; }, "Add this part's light over the scene instead of drawing it opaque");
      props.LightmapCombo<Primitive>(
         m_part, "Lightmap"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_szLightmap; }, //
         [](Primitive* primitive, const string& v) { primitive->m_d.m_szLightmap = v; }, "Use this light's illumination as the part's baked lighting");
      props.InputResURI<Primitive>(
         m_part, "State Link"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_stateSrcLink; }, //
         [](Primitive* primitive, const string& v) { primitive->m_d.m_stateSrcLink = v; }, ResURIKind::State,
         "URI linking this part's state to a shared state source (plugin or external controller)", DisplayFamily::Any, -1, false, -1);

      props.Separator("Material"s);
      if (props.GetEditedPart<Primitive>(m_part)->IsPlayfield())
      {
         // The playfield mesh's material and image are overridden by the table's playfield definition
         props.MaterialCombo<Primitive>(
            m_part, "Material"s, //
            [](const Primitive* primitive) { return primitive->m_ptable->m_playfieldMaterial; }, //
            [](Primitive* primitive, const string& v) { primitive->m_ptable->m_playfieldMaterial = v; });
         props.ImageCombo<Primitive>(
            m_part, "Image"s, //
            [](const Primitive* primitive) { return primitive->m_ptable->m_image; }, //
            [](Primitive* primitive, const string& v) { primitive->m_ptable->m_image = v; });
         ImGui::BeginDisabled(m_part->m_ptable->m_image.empty());
         props.Checkbox<Primitive>(
            m_part, "Show in Editor"s, //
            [](const Primitive* primitive) { return primitive->m_ptable->m_winEditorBackdrop; }, //
            [](Primitive* primitive, bool v) { primitive->m_ptable->m_winEditorBackdrop = v; });
         ImGui::EndDisabled();
         // The implicit playfield mesh does not support normal mapping
         ImGui::BeginDisabled(m_part == g_pplayer->m_implicitPlayfieldMesh);
         props.MaterialVisuals(m_part, {}, nullptr, {}, nullptr, nullptr, &Data::m_szNormalMap, &Data::m_objectSpaceNormalMap);
         ImGui::EndDisabled();
      }
      else
         props.MaterialVisuals(m_part, "Material"s, &Data::m_szMaterial, "Image"s, &Data::m_szImage, &Data::m_displayTexture, &Data::m_szNormalMap, &Data::m_objectSpaceNormalMap);
      props.ImageCombo<Primitive>(
         m_part, "ORM Map"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_szORMMap; }, //
         [](Primitive* primitive, const string& v) { primitive->m_d.m_szORMMap = v; });
      props.ImageCombo<Primitive>(
         m_part, "Emission Map"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_szEmissionMap; }, //
         [](Primitive* primitive, const string& v) { primitive->m_d.m_szEmissionMap = v; });
      props.InputFloat<Primitive>(
         m_part, "Disable Spot Lights"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_disableLightingTop; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_disableLightingTop = v; }, PropertyPane::Unit::None, 3,
         "Reduce the effect of dynamic light sources on the top of this part (0 = fully lit)");
      props.InputFloat<Primitive>(
         m_part, "Translucency"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_disableLightingBelow; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_disableLightingBelow = v; }, PropertyPane::Unit::None, 3, "Let light pass through the part from below, faking translucency");
      props.InputFloat<Primitive>(
         m_part, "Modulate Opacity"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_alpha; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_alpha = v; }, PropertyPane::Unit::None, 3, "Scale the part's opacity on top of the image's alpha");
      props.InputRGB<Primitive>(
         m_part, "Modulate Color"s, //
         [](const Primitive* primitive) { return convertColor(primitive->m_d.m_color); }, //
         [](Primitive* primitive, const vec3& v) { primitive->m_d.m_color = convertColorRGB(v); }, "Color multiplied with the part's image and material");

      props.Separator("Reflections");
      props.RenderProbeCombo<Primitive>(
         m_part, "Reflection Probe"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_szReflectionProbe; }, //
         [](Primitive* primitive, const string& v) { primitive->m_d.m_szReflectionProbe = v; }, "Render probe providing this part's reflection image");
      props.InputFloat<Primitive>(
         m_part, "Reflection Strength"s, //
         [](const Primitive* primitive) { return primitive->IsPlayfield() ? primitive->m_ptable->m_playfieldReflectionStrength : primitive->m_d.m_reflectionStrength; }, //
         [](Primitive* primitive, float v)
         {
            if (primitive->IsPlayfield())
               primitive->m_ptable->m_playfieldReflectionStrength = v;
            else
               primitive->m_d.m_reflectionStrength = v;
         },
         PropertyPane::Unit::None, 3);

      props.Separator("Refractions"s);
      props.RenderProbeCombo<Primitive>(
         m_part, "Refraction Probe"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_szRefractionProbe; }, //
         [](Primitive* primitive, const string& v) { primitive->m_d.m_szRefractionProbe = v; }, "Render probe providing the image seen through this part");
      props.InputFloat<Primitive>(
         m_part, "Refraction Thickness"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_refractionThickness; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_refractionThickness = v; }, PropertyPane::Unit::None, 3, "Simulated thickness used to refract light through this part");
      props.SliderFloat<Primitive>(
         m_part, "Refraction Curvature"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_refractionCurvature; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_refractionCurvature = max(v, 0.f); }, 0.f, 8.f, 2, "Curvature-driven refraction strength (0 = off)");
      props.SliderFloat<Primitive>(
         m_part, "Refraction Anisotropy"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_refractionAnisotropy; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_refractionAnisotropy = clamp(v, 0.f, 1.f); }, 0.f, 1.f, 2);
      props.SliderFloat<Primitive>(
         m_part, "Refraction Wave"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_refractionWave; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_refractionWave = max(v, 0.f); }, 0.f, 3.f, 2);
      props.SliderFloat<Primitive>(
         m_part, "Refraction Wave Flow"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_refractionWaveFlow; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_refractionWaveFlow = clamp(v, 0.f, 1.f); }, 0.f, 1.f, 2);
      props.SliderFloat<Primitive>(
         m_part, "Refraction Noise"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_refractionNoise; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_refractionNoise = max(v, 0.f); }, 0.f, 10.f, 2); // 1 = ~0.7 deg tilt; range far past "sensible" on purpose (Ben: dial past, then back)
      props.SliderFloat<Primitive>(
         m_part, "Refraction Noise Scale (mm)"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_refractionNoiseScale; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_refractionNoiseScale = max(v, 0.01f); }, 0.5f, 10.f, 2);

      // Traced insert (an additive card with a TRACED_ image): the lit insert's hot spot over the bulb, parallaxing with the view
      props.Separator("Traced Insert Bulb"s);
      props.SliderFloat<Primitive>(
         m_part, "Bulb Depth (mm)"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_bulbDepth; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_bulbDepth = max(v, 0.f); }, 0.f, 30.f, 1, "A soft hot spot in the lit insert over the bulb, this far below the top, shifting with the view (0 = off)");
      props.SliderFloat<Primitive>(
         m_part, "Bulb Spot Size"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_bulbScale; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_bulbScale = max(v, 0.05f); }, 0.2f, 2.f, 2, "Hot spot size (1 = about 19 mm across at half height)");
      props.SliderFloat<Primitive>(
         m_part, "Bulb Brightness"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_bulbBrightness; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_bulbBrightness = max(v, 0.f); }, 0.f, 4.f, 2, "Hot spot brightness (1 = +80 % of the glow's peak at its centre)");
      props.SliderFloat<Primitive>(
         m_part, "Bulb Stretch X"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_bulbStretchX; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_bulbStretchX = max(v, 0.05f); }, 0.2f, 4.f, 2, "Hot spot stretch along the card's X (1 = round)");
      props.SliderFloat<Primitive>(
         m_part, "Bulb Stretch Y"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_bulbStretchY; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_bulbStretchY = max(v, 0.05f); }, 0.2f, 4.f, 2, "Hot spot stretch along the card's Y (1 = round)");
      props.SliderFloat<Primitive>(
         m_part, "Bulb Offset X (mm)"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_bulbOffsetX; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_bulbOffsetX = v; }, -25.f, 25.f, 1, "Moves the hot spot along the card's X (the bulb is not under the insert's centre)");
      props.SliderFloat<Primitive>(
         m_part, "Bulb Offset Y (mm)"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_bulbOffsetY; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_bulbOffsetY = v; }, -25.f, 25.f, 1, "Moves the hot spot along the card's Y");

      props.SliderFloat<Primitive>(
      props.SliderFloat<Primitive>(
      props.SliderFloat<Primitive>(

      props.Separator("Flasher Dome"s);
      props.SliderFloat<Primitive>(
         m_part, "Dome Body"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeBody; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domeBody = clamp(v, 0.f, 1.f); }, 0.f, 1.f, 2,
         "Thick body: 0 = see-through (the scene behind filters through the plastic), 1 = the scene behind is replaced by the scattered interior");
      props.SliderFloat<Primitive>(
         m_part, "Dome Scatter"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeScatter; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domeScatter = max(v, 0.f); }, 0.f, 4.f, 2,
         "Body scatter driven by the dome's own lamp (milky glow in the lamp's colour; 0 = off)");
      props.SliderFloat<Primitive>(
         m_part, "Dome Cap Highlight"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeCap; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domeCap = max(v, 0.f); }, 0.f, 4.f, 2, "Highlight on the cap seam/bevel at the top (0 = off)");
      props.InputInt<Primitive>(
         m_part, "Dome Flutes"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeFlutes; }, //
         [](Primitive* primitive, int v) { primitive->m_d.m_domeFlutes = clamp(v, 0, 96); }, "Flute count override, 0..96 (0 = measured from the mesh)");
      props.SliderFloat<Primitive>(
         m_part, "Dome Flute Depth"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeFluteDepth; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domeFluteDepth = clamp(v, 0.f, 3.f); }, 0.f, 3.f, 2, "Flute depth scale (1 = measured from the mesh)");
      props.SliderFloat<Primitive>(
         m_part, "Dome Peak White"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domePeak; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domePeak = clamp(v, 0.f, 3.f); }, 0.f, 3.f, 2,
         "At full power the lit body and the top go white-hot like a real flash (0 = stays red at the peak); also a hot spot in the middle of the cap");
      props.SliderFloat<Primitive>(
         m_part, "Dome Orb"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeOrb; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domeOrb = clamp(v, 0.f, 4.f); }, 0.f, 4.f, 2,
         "The lit bulb seen through the ridges as a hot glowing ball that moves with the view (0 = off)");
      props.SliderFloat<Primitive>(
         m_part, "Dome Cooldown"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeCooldown; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domeCooldown = clamp(v, 0.f, 1.f); }, 0.f, 1.f, 2,
         "How the white-hot peak follows a flash: 0 = it switches on only near full power (short flashes snap), 1 = a smooth blackbody pulse that cools from white through yellow and orange to red");
      props.SliderFloat<Primitive>(
         m_part, "Dome Flash Boost"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeBoost; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domeBoost = clamp(v, 0.f, 5.f); }, 0.f, 5.f, 2,
         "Gain on the lamp level the dome sees: a flash the game drives below full (short pulses) reads hotter; the lamp's own light on the table is unchanged (1 = as is)");
      props.SliderFloat<Primitive>(
         m_part, "Dome Depth"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeDepth; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domeDepth = clamp(v, 0.f, 3.f); }, 0.f, 3.f, 2,
         "Lamp off: the far wall's ridges seen through the cavity, each flute lensing its own slice, so something moves inside as you move (0 = off)");
      props.SliderFloat<Primitive>(
         m_part, "Dome Glints"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeGlint; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domeGlint = clamp(v, 0.f, 5.f); }, 0.f, 5.f, 2,
         "The lamp-off sparkle: facet glints, ridge glints and needles along the flutes, the room seen in the plastic (1 = as tuned, 0 = none)");
      props.SliderFloat<Primitive>(
         m_part, "Dome Glint Lights"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeGlintLights; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domeGlintLights = clamp(v, 0.f, 1.f); }, 0.f, 1.f, 2,
         "What the lamp-off sparkle reflects: 0 = a fixed studio room, 1 = the table's own lights near the dome (GI bulbs, inserts, other flashers), so the glints move and change as they light up");
      props.SliderFloat<Primitive>(
         m_part, "Dome Frost"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeFrost; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domeFrost = clamp(v, 0.f, 3.f); }, 0.f, 3.f, 2,
         "Clear and pale domes: the thick parts of the plastic (ridges, top, base, rim) scatter the room's light to a milky white so the dome reads frosted, not grey (0 = off)");
      props.InputRGB<Primitive>(
         m_part, "Dome Tint"s, //
         [](const Primitive* primitive) { return convertColor(primitive->m_d.m_domeTint); }, //
         [](Primitive* primitive, const vec3& v) { primitive->m_d.m_domeTint = convertColorRGB(v); },
         "Plastic colour at 2 mm thickness; black = the built-in red acrylic preset");

      props.EndSection();
   }

   if (props.BeginSection("Position"s))
   {
      props.Separator("Position, Rotation & Size"s);
      props.InputFloat3<Primitive>(
         m_part, "Position"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_vPosition; }, //
         [](Primitive* primitive, const vec3& v) { primitive->m_d.m_vPosition = v; }, PropertyPane::Unit::VPLength, 1);
      props.InputFloat3<Primitive>(
         m_part, "Orientation"s, //
         [](const Primitive* primitive) { return vec3(primitive->m_d.m_aRotAndTra[0], primitive->m_d.m_aRotAndTra[1], primitive->m_d.m_aRotAndTra[2]); }, //
         [](Primitive* primitive, const vec3& v) { 
            primitive->m_d.m_aRotAndTra[0] = v.x; 
            primitive->m_d.m_aRotAndTra[1] = v.y;
            primitive->m_d.m_aRotAndTra[2] = v.z;
         },
         PropertyPane::Unit::Degree, 2);
      props.InputFloat3<Primitive>(
         m_part, "Scale"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_vSize; }, //
         [](Primitive* primitive, const vec3& v) { primitive->m_d.m_vSize = v; }, PropertyPane::Unit::PercentX100, 2);

      props.Separator("Additional Transform"s);
      props.InputFloat3<Primitive>(
         m_part, "Translation"s, //
         [](const Primitive* primitive) { return vec3(primitive->m_d.m_aRotAndTra[3], primitive->m_d.m_aRotAndTra[4], primitive->m_d.m_aRotAndTra[5]); }, //
         [](Primitive* primitive, const vec3& v)
         {
            primitive->m_d.m_aRotAndTra[3] = v.x;
            primitive->m_d.m_aRotAndTra[4] = v.y;
            primitive->m_d.m_aRotAndTra[5] = v.z;
         },
         PropertyPane::Unit::VPLength, 1, "Additional offset applied after the base transform (can be animated by script without changing Position)");
      props.InputFloat3<Primitive>(
         m_part, "Rotation"s, //
         [](const Primitive* primitive) { return vec3(primitive->m_d.m_aRotAndTra[6], primitive->m_d.m_aRotAndTra[7], primitive->m_d.m_aRotAndTra[8]); }, //
         [](Primitive* primitive, const vec3& v)
         {
            primitive->m_d.m_aRotAndTra[6] = v.x;
            primitive->m_d.m_aRotAndTra[7] = v.y;
            primitive->m_d.m_aRotAndTra[8] = v.z;
         },
         PropertyPane::Unit::Degree, 2, "Additional rotation applied on top of the base orientation");
      props.EndSection();
   }

   if (props.BeginSection("Physics"s))
   {
      props.Checkbox<Primitive>(
         m_part, "Has Hit Event"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_hitEvent; }, //
         [](Primitive* primitive, bool v) { primitive->m_d.m_hitEvent = v; }, "Fire a Hit event to the script when the ball hits this part");
      props.InputFloat<Primitive>(
         m_part, "Hit Threshold"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_threshold; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_threshold = v; }, PropertyPane::Unit::None, 3, "Minimum ball speed needed to trigger this part's hit event");
      props.MaterialCombo<Primitive>(
         m_part, "Physics Material"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_szPhysicsMaterial; }, //
         [](Primitive* primitive, const string& v) { primitive->m_d.m_szPhysicsMaterial = v; }, "Material preset used for this part's physics values");
      props.Checkbox<Primitive>(
         m_part, "Overwrite Material"s, //
         [](const Primitive* surf) { return surf->m_d.m_overwritePhysics; }, //
         [](Primitive* surf, bool v) { surf->m_d.m_overwritePhysics = v; }, "Use the fields below instead of the physics material's values");
      props.InputFloat<Primitive>(
         m_part, "Elasticity"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_elasticity; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_elasticity = v; }, PropertyPane::Unit::None, 3);
      props.InputFloat<Primitive>(
         m_part, "Elasticity Falloff"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_elasticityFalloff; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_elasticityFalloff = v; }, PropertyPane::Unit::None, 3, "Reduce elasticity for slower impacts (0 = constant elasticity)");
      props.InputFloat<Primitive>(
         m_part, "Friction"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_friction; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_friction = max(v, 0.f); }, PropertyPane::Unit::None, 3);
      props.InputFloat<Primitive>(
         m_part, "Scatter Angle"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_scatter; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_scatter = v; }, PropertyPane::Unit::None, 3, "Random angle added to the ball's bounce direction");
      props.Checkbox<Primitive>(
         m_part, "Collidable"s, //
         [](const Primitive* surf) { return surf->m_d.m_collidable; }, //
         [](Primitive* surf, bool v) { surf->m_d.m_collidable = v; });
      props.Checkbox<Primitive>(
         m_part, "Toy (never collidable)"s, //
         [](const Primitive* surf) { return surf->m_d.m_toy; }, //
         [](Primitive* surf, bool v) { surf->m_d.m_toy = v; });
      props.InputFloat<Primitive>(
         m_part, "Reduction Factor"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_collision_reductionFactor; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_collision_reductionFactor = v; }, PropertyPane::Unit::None, 3,
         "Simplify the physics collision mesh (higher = fewer collision points, faster but less accurate)");
      props.EndSection();
   }

   //props.TimerSection(m_part);
}

void PrimitiveUIPart::UpdateMeshUnitsUI()
{
   ImGui::SeparatorText("Units and axes");
   int units = m_meshUnitsMeters ? 1 : 0;
   ImGui::RadioButton("VPUnits (Up -Z, Visual Pinball default)", &units, 0);
   ImGui::Indent();
   ImGui::TextDisabled("Recommended Blender Import/Export Transformations:\nScale 0.00054, Forward -Y, Up -Z");
   ImGui::Unindent();
   ImGui::RadioButton("Meters (Up Y, Blender default)", &units, 1);
   ImGui::Indent();
   ImGui::TextDisabled("Matches Blender's default Import/Export Transformations:\nScale 1.0, Forward -Z, Up Y. Primitive scale stays 1.");
   ImGui::Unindent();
   m_meshUnitsMeters = units != 0;
}

void PrimitiveUIPart::ImportMesh()
{
   if (g_pplayer == nullptr || g_pplayer->m_playfieldWnd == nullptr)
      return;
   m_pendingMeshImport = std::make_shared<string>();
   const SDL_DialogFileFilter filters[] = { { "Wavefront obj file", "obj" } };
   SDL_ShowOpenFileDialog(
      [](void* userdata, const char* const* filelist, int filter)
      {
         auto* res = static_cast<std::shared_ptr<string>*>(userdata);
         if (filelist != nullptr && filelist[0] != nullptr)
            **res = filelist[0];
         delete res;
      },
      new std::shared_ptr<string>(m_pendingMeshImport), //
      g_pplayer->m_playfieldWnd->GetCore(), filters, 1, nullptr, false);
}

void PrimitiveUIPart::ExportMesh()
{
   if (g_pplayer == nullptr || g_pplayer->m_playfieldWnd == nullptr)
      return;
   m_pendingMeshExport = std::make_shared<string>();
   const SDL_DialogFileFilter filters[] = { { "Wavefront obj file", "obj" } };
   SDL_ShowSaveFileDialog(
      [](void* userdata, const char* const* filelist, int filter)
      {
         auto* res = static_cast<std::shared_ptr<string>*>(userdata);
         if (filelist != nullptr && filelist[0] != nullptr)
            **res = filelist[0];
         delete res;
      },
      new std::shared_ptr<string>(m_pendingMeshExport), //
      g_pplayer->m_playfieldWnd->GetCore(), filters, 1, nullptr);
}
}
