// license:GPLv3+

#pragma once

class Material;
class Light;

enum ShadowMode : int;

namespace VPX::EditorUI
{

class EditorUI;

// Whole-table "Convert to PBR" tool: previews and applies the legacy material
// and light migration to the PBR shading model (see .local/specs/Convert to PBR.md)
class ConvertToPBRModal
{
public:
   explicit ConvertToPBRModal(EditorUI &editor);

   void Show()
   {
      m_visible = true;
      Scan();
   }
   void Close();
   bool IsVisible() const { return m_visible; }

   void Render();

private:
   struct MaterialItem
   {
      Material *mat;
      bool convert;
      string note;
      int visualUsers = 0, otherUsers = 0;
      float newRough = 0.f, newMetal = 0.f;
   };
   struct LightItem
   {
      Light *light;
      int fromType; // LightType
      bool convert;
      string note;
      ShadowMode shadow;
      bool scriptBulbRef;
   };

   void Scan();
   void Apply();

   EditorUI &m_editor;
   bool m_visible = false;
   vector<MaterialItem> m_materials;
   vector<LightItem> m_lights;
   // Options
   bool m_convertUnreferenced = true;
   bool m_convertUnsupported = false;
   bool m_deriveMetallic = false;
   bool m_convertHalo = true;
   bool m_convertClassic = true;
   bool m_convertImageMode = false;
   float m_intensityScale = 0.1f;
   int m_shadowScene = 0; // 0=Heuristic,1=None,2=Static,3=Static+Ball,4=Static+Dynamic
   int m_shadowNear = 0;
   int m_shadowInsert = 0;
   float m_insertRange = 200.f;
   string m_result;
};

}
