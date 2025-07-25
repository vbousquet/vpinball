// license:GPLv3+

#include "core/stdafx.h"
#include "renderer/Shader.h"
#include "renderer/IndexBuffer.h"
#include "renderer/VertexBuffer.h"
#ifndef __STANDALONE__
#include "renderer/captureExt.h"
#endif
#include "core/VPXPluginAPIImpl.h"

Flasher::Flasher()
{
   m_menuid = IDR_SURFACEMENU;
}

Flasher::~Flasher()
{
   assert(m_rd == nullptr); // RenderRelease must be explicitely called before deleting this object
}

Flasher *Flasher::CopyForPlay(PinTable *live_table) const
{
   STANDARD_EDITABLE_WITH_DRAGPOINT_COPY_FOR_PLAY_IMPL(Flasher, live_table, m_vdpoint)
   return dst;
}

void Flasher::InitShape()
{
   if (m_vdpoint.empty())
   {
      // First time shape has been set to custom - set up some points
      const float x = m_d.m_vCenter.x;
      const float y = m_d.m_vCenter.y;
      constexpr float size = 100.0f;

      CComObject<DragPoint> *pdp;
      CComObject<DragPoint>::CreateInstance(&pdp);
      if (pdp)
      {
         pdp->AddRef();
         pdp->Init(this, x - size*0.5f, y - size*0.5f, 0.f, false);
         m_vdpoint.push_back(pdp);
      }
      CComObject<DragPoint>::CreateInstance(&pdp);
      if (pdp)
      {
         pdp->AddRef();
         pdp->Init(this, x - size*0.5f, y + size*0.5f, 0.f, false);
         m_vdpoint.push_back(pdp);
      }
      CComObject<DragPoint>::CreateInstance(&pdp);
      if (pdp)
      {
         pdp->AddRef();
         pdp->Init(this, x + size*0.5f, y + size*0.5f, 0.f, false);
         m_vdpoint.push_back(pdp);
      }
      CComObject<DragPoint>::CreateInstance(&pdp);
      if (pdp)
      {
         pdp->AddRef();
         pdp->Init(this, x + size*0.5f, y - size*0.5f, 0.f, false);
         m_vdpoint.push_back(pdp);
      }
   }
}

HRESULT Flasher::Init(PinTable *const ptable, const float x, const float y, const bool fromMouseClick, const bool forPlay)
{
   m_ptable = ptable;
   SetDefaults(fromMouseClick);
   m_d.m_isVisible = true;
   m_d.m_vCenter.x = x;
   m_d.m_vCenter.y = y;
   m_d.m_rotX = 0.0f;
   m_d.m_rotY = 0.0f;
   m_d.m_rotZ = 0.0f;
   InitShape();
   return forPlay ? S_OK : InitVBA(fTrue, 0, nullptr);
}

void Flasher::SetDefaults(const bool fromMouseClick)
{
#define regKey Settings::DefaultPropsFlasher

   m_d.m_height = fromMouseClick ? g_pvp->m_settings.LoadValueWithDefault(regKey, "Height"s, 50.f) : 50.f;
   m_d.m_rotX = fromMouseClick ? g_pvp->m_settings.LoadValueWithDefault(regKey, "RotX"s, 0.f) : 0.f;
   m_d.m_rotY = fromMouseClick ? g_pvp->m_settings.LoadValueWithDefault(regKey, "RotY"s, 0.f) : 0.f;
   m_d.m_rotZ = fromMouseClick ? g_pvp->m_settings.LoadValueWithDefault(regKey, "RotZ"s, 0.f) : 0.f;
   m_d.m_color = fromMouseClick ? g_pvp->m_settings.LoadValueWithDefault(regKey, "Color"s, (int)RGB(50,200,50)) : RGB(50,200,50);
   m_d.m_tdr.m_TimerEnabled = fromMouseClick ? g_pvp->m_settings.LoadValueWithDefault(regKey, "TimerEnabled"s, false) : false;
   m_d.m_tdr.m_TimerInterval = fromMouseClick ? g_pvp->m_settings.LoadValueWithDefault(regKey, "TimerInterval"s, 100) : 100;

   bool hr = g_pvp->m_settings.LoadValue(regKey, "ImageA"s, m_d.m_szImageA);
   if (!hr || !fromMouseClick)
      m_d.m_szImageA.clear();

   hr = g_pvp->m_settings.LoadValue(regKey, "ImageB"s, m_d.m_szImageB);
   if (!hr || !fromMouseClick)
      m_d.m_szImageB.clear();

   m_d.m_alpha = fromMouseClick ? g_pvp->m_settings.LoadValueWithDefault(regKey, "Opacity"s, 100) : 100;

   m_d.m_intensity_scale = 1.0f;

   m_d.m_modulate_vs_add = fromMouseClick ? g_pvp->m_settings.LoadValueWithDefault(regKey, "ModulateVsAdd"s, 0.9f) : 0.9f;
   m_d.m_filterAmount = fromMouseClick ? g_pvp->m_settings.LoadValueWithDefault(regKey, "FilterAmount"s, 100) : 100;
   m_d.m_isVisible = fromMouseClick ? g_pvp->m_settings.LoadValueWithDefault(regKey, "Visible"s, true) : true;
   m_inPlayState = m_d.m_isVisible;
   m_d.m_addBlend = fromMouseClick ? g_pvp->m_settings.LoadValueWithDefault(regKey, "AddBlend"s, false) : false;
   m_d.m_renderMode = fromMouseClick ? static_cast<FlasherData::RenderMode>(g_pvp->m_settings.LoadValueWithDefault(regKey, "RenderMode"s, 0)) : FlasherData::FLASHER;
   m_d.m_displayTexture = fromMouseClick ? g_pvp->m_settings.LoadValueWithDefault(regKey, "DisplayTexture"s, false) : false;
   m_d.m_imagealignment = fromMouseClick ? (RampImageAlignment)g_pvp->m_settings.LoadValueWithDefault(regKey, "ImageMode"s, (int)ImageModeWrap) : ImageModeWrap;
   m_d.m_filter = fromMouseClick ? (Filters)g_pvp->m_settings.LoadValueWithDefault(regKey, "Filter"s, (int)Filter_Overlay) : Filter_Overlay;

#undef regKey
}

void Flasher::WriteRegDefaults()
{
#define regKey Settings::DefaultPropsFlasher

   g_pvp->m_settings.SaveValue(regKey, "Height"s, m_d.m_height);
   g_pvp->m_settings.SaveValue(regKey, "RotX"s, m_d.m_rotX);
   g_pvp->m_settings.SaveValue(regKey, "RotY"s, m_d.m_rotY);
   g_pvp->m_settings.SaveValue(regKey, "RotZ"s, m_d.m_rotZ);
   g_pvp->m_settings.SaveValue(regKey, "Color"s, (int)m_d.m_color);
   g_pvp->m_settings.SaveValue(regKey, "TimerEnabled"s, m_d.m_tdr.m_TimerEnabled);
   g_pvp->m_settings.SaveValue(regKey, "TimerInterval"s, m_d.m_tdr.m_TimerInterval);
   g_pvp->m_settings.SaveValue(regKey, "ImageA"s, m_d.m_szImageA);
   g_pvp->m_settings.SaveValue(regKey, "ImageB"s, m_d.m_szImageB);
   g_pvp->m_settings.SaveValue(regKey, "Alpha"s, m_d.m_alpha);
   g_pvp->m_settings.SaveValue(regKey, "ModulateVsAdd"s, m_d.m_modulate_vs_add);
   g_pvp->m_settings.SaveValue(regKey, "Visible"s, m_d.m_isVisible);
   g_pvp->m_settings.SaveValue(regKey, "DisplayTexture"s, m_d.m_displayTexture);
   g_pvp->m_settings.SaveValue(regKey, "AddBlend"s, m_d.m_addBlend);
   g_pvp->m_settings.SaveValue(regKey, "RenderMode"s, m_d.m_renderMode);
   g_pvp->m_settings.SaveValue(regKey, "ImageMode"s, (int)m_d.m_imagealignment);
   g_pvp->m_settings.SaveValue(regKey, "Filter"s, m_d.m_filter);
   g_pvp->m_settings.SaveValue(regKey, "FilterAmount"s, (int)m_d.m_filterAmount);

#undef regKey
}

void Flasher::UIRenderPass1(Sur * const psur)
{
   if (m_vdpoint.empty())
      InitShape();

   psur->SetFillColor(m_ptable->RenderSolid() ? m_vpinball->m_fillColor: -1);
   psur->SetObject(this);
   // Don't want border color to be over-ridden when selected - that will be drawn later
   psur->SetBorderColor(-1, false, 0);

   vector<RenderVertex> vvertex;
   GetRgVertex(vvertex);
   if (!m_ptable->RenderSolid() || !m_d.m_displayTexture)
      psur->Polygon(vvertex);
   else if (const Texture *const ppi = m_ptable->GetImage(m_d.m_szImageA); ppi && ppi->GetGDIBitmap())
   {
      if (m_d.m_imagealignment == ImageModeWrap)
      {
         float _minx = FLT_MAX;
         float _miny = FLT_MAX;
         float _maxx = -FLT_MAX;
         float _maxy = -FLT_MAX;
         for (size_t i = 0; i < vvertex.size(); i++)
         {
            if (vvertex[i].x < _minx) _minx = vvertex[i].x;
            if (vvertex[i].x > _maxx) _maxx = vvertex[i].x;
            if (vvertex[i].y < _miny) _miny = vvertex[i].y;
            if (vvertex[i].y > _maxy) _maxy = vvertex[i].y;
         }

         psur->PolygonImage(vvertex, ppi->GetGDIBitmap(), _minx, _miny, _minx + (_maxx - _minx), _miny + (_maxy - _miny), ppi->m_width, ppi->m_height);
      }
      else
      {
         psur->PolygonImage(vvertex, ppi->GetGDIBitmap(), m_ptable->m_left, m_ptable->m_top, m_ptable->m_right, m_ptable->m_bottom, ppi->m_width, ppi->m_height);
      }
   }
   else
      psur->Polygon(vvertex);
}

void Flasher::UIRenderPass2(Sur * const psur)
{
   psur->SetFillColor(-1);
   psur->SetBorderColor(RGB(0, 0, 0), false, 0);
   psur->SetObject(this); // For selected formatting
   psur->SetObject(nullptr);

   {
      vector<RenderVertex> vvertex; //!! check/reuse from UIRenderPass1
      GetRgVertex(vvertex);
      psur->Polygon(vvertex);
   }

   // if the item is selected then draw the dragpoints (or if we are always to draw dragpoints)
   bool drawDragpoints = ((m_selectstate != eNotSelected) || m_vpinball->m_alwaysDrawDragPoints);

   if (!drawDragpoints)
   {
      // if any of the dragpoints of this object are selected then draw all the dragpoints
      for (size_t i = 0; i < m_vdpoint.size(); i++)
      {
         const CComObject<DragPoint> * const pdp = m_vdpoint[i];
         if (pdp->m_selectstate != eNotSelected)
         {
            drawDragpoints = true;
            break;
         }
      }
   }

   if (drawDragpoints)
   for (size_t i = 0; i < m_vdpoint.size(); i++)
   {
      CComObject<DragPoint> * const pdp = m_vdpoint[i];
      psur->SetFillColor(-1);
      psur->SetBorderColor(pdp->m_dragging ? RGB(0, 255, 0) : RGB(255, 0, 0), false, 0);
      psur->SetObject(pdp);

      psur->Ellipse2(pdp->m_v.x, pdp->m_v.y, 8);
   }
}

void Flasher::RenderBlueprint(Sur *psur, const bool solid)
{
}


void Flasher::PhysicSetup(PhysicsEngine* physics, const bool isUI)
{
   if (isUI)
   {
      vector<RenderVertex> vvertex;
      GetRgVertex(vvertex);
      if (vvertex.empty())
         return;

      const int cvertex = (int)vvertex.size();
      Vertex3Ds *const rgv3d = new Vertex3Ds[cvertex];

      const float height = m_d.m_height;
      const float movx = m_minx + (m_maxx - m_minx)*0.5f;
      const float movy = m_miny + (m_maxy - m_miny)*0.5f;
      const Matrix3D tempMatrix = Matrix3D::MatrixTranslate(-movx /* -m_d.m_vCenter.x */, -movy /* -m_d.m_vCenter.y */, 0.f)
                             * (((Matrix3D::MatrixRotateZ(ANGTORAD(m_d.m_rotZ))
                                * Matrix3D::MatrixRotateY(ANGTORAD(m_d.m_rotY)))
                                * Matrix3D::MatrixRotateX(ANGTORAD(m_d.m_rotX)))
                                * Matrix3D::MatrixTranslate(m_d.m_vCenter.x, m_d.m_vCenter.y, height));

      for (int i = 0; i < cvertex; i++)
      {
         Vertex3Ds v = tempMatrix *Vertex3Ds(vvertex[i].x, vvertex[i].y, 0.f);
         rgv3d[i].x = v.x;
         rgv3d[i].y = v.y;
         rgv3d[i].z = v.z;
      }

      Hit3DPoly *const ph3dp = new Hit3DPoly(this, rgv3d, cvertex);
      physics->AddCollider(ph3dp, isUI);
   }
}

void Flasher::PhysicRelease(PhysicsEngine* physics, const bool isUI)
{
}

void Flasher::SetObjectPos()
{
    m_vpinball->SetObjectPosCur(0, 0);
}

void Flasher::FlipY(const Vertex2D& pvCenter)
{
   IHaveDragPoints::FlipPointY(pvCenter);
}

void Flasher::FlipX(const Vertex2D& pvCenter)
{
   IHaveDragPoints::FlipPointX(pvCenter);
}

void Flasher::Rotate(const float ang, const Vertex2D& pvCenter, const bool useElementCenter)
{
   IHaveDragPoints::RotatePoints(ang, pvCenter, useElementCenter);
}

void Flasher::Scale(const float scalex, const float scaley, const Vertex2D& pvCenter, const bool useElementCenter)
{
   IHaveDragPoints::ScalePoints(scalex, scaley, pvCenter, useElementCenter);
}

void Flasher::Translate(const Vertex2D &pvOffset)
{
   IHaveDragPoints::TranslatePoints(pvOffset);
}

void Flasher::MoveOffset(const float dx, const float dy)
{
   m_d.m_vCenter.x += dx;
   m_d.m_vCenter.y += dy;
   for (size_t i = 0; i < m_vdpoint.size(); i++)
   {
      CComObject<DragPoint> * const pdp = m_vdpoint[i];

      pdp->m_v.x += dx;
      pdp->m_v.y += dy;
   }
}

void Flasher::DoCommand(int icmd, int x, int y)
{
   ISelect::DoCommand(icmd, x, y);

   switch (icmd)
   {
   case ID_WALLMENU_FLIP:
      FlipPointY(GetPointCenter());
      break;

   case ID_WALLMENU_MIRROR:
      FlipPointX(GetPointCenter());
      break;

   case ID_WALLMENU_ROTATE:
      RotateDialog();
      break;

   case ID_WALLMENU_SCALE:
      ScaleDialog();
      break;

   case ID_WALLMENU_TRANSLATE:
      TranslateDialog();
      break;

   case ID_WALLMENU_ADDPOINT:
      AddPoint(x, y, false);
      break;
   }
}

void Flasher::AddPoint(int x, int y, const bool smooth)
{
      STARTUNDO
      const Vertex2D v = m_ptable->TransformPoint(x, y);

      vector<RenderVertex> vvertex;
      GetRgVertex(vvertex);

      Vertex2D vOut;
      int iSeg;
      ClosestPointOnPolygon(vvertex, v, vOut, iSeg, true);

      // Go through vertices (including iSeg itself) counting control points until iSeg
      int icp = 0;
      for (int i = 0; i < (iSeg + 1); i++)
         if (vvertex[i].controlPoint)
            icp++;

      CComObject<DragPoint> *pdp;
      CComObject<DragPoint>::CreateInstance(&pdp);
      if (pdp)
      {
         pdp->AddRef();
         pdp->Init(this, vOut.x, vOut.y, 0.f, smooth);
         m_vdpoint.insert(m_vdpoint.begin() + icp, pdp); // push the second point forward, and replace it with this one.  Should work when index2 wraps.
      }

      STOPUNDO
}

void Flasher::UpdatePoint(int index, float x, float y)
{
     CComObject<DragPoint> *pdp = m_vdpoint[index];
     pdp->m_v.x = x;
     pdp->m_v.y = y;
}

HRESULT Flasher::SaveData(IStream *pstm, HCRYPTHASH hcrypthash, const bool saveForUndo)
{
   BiffWriter bw(pstm, hcrypthash);

   bw.WriteFloat(FID(FHEI), m_d.m_height);
   bw.WriteFloat(FID(FLAX), m_d.m_vCenter.x);
   bw.WriteFloat(FID(FLAY), m_d.m_vCenter.y);
   bw.WriteFloat(FID(FROX), m_d.m_rotX);
   bw.WriteFloat(FID(FROY), m_d.m_rotY);
   bw.WriteFloat(FID(FROZ), m_d.m_rotZ);
   bw.WriteInt(FID(COLR), m_d.m_color);
   bw.WriteBool(FID(TMON), m_d.m_tdr.m_TimerEnabled);
   bw.WriteInt(FID(TMIN), m_d.m_tdr.m_TimerInterval);
   bw.WriteWideString(FID(NAME), m_wzName);
   bw.WriteString(FID(IMAG), m_d.m_szImageA);
   bw.WriteString(FID(IMAB), m_d.m_szImageB);
   bw.WriteInt(FID(FALP), m_d.m_alpha);
   bw.WriteFloat(FID(MOVA), m_d.m_modulate_vs_add);
   bw.WriteBool(FID(FVIS), m_d.m_isVisible);
   bw.WriteBool(FID(DSPT), m_d.m_displayTexture);
   bw.WriteBool(FID(ADDB), m_d.m_addBlend);
   bw.WriteInt(FID(RDMD), m_d.m_renderMode);
   bw.WriteInt(FID(RSTL), m_d.m_renderStyle);
   bw.WriteFloat(FID(GRGH), m_d.m_glassRoughness);
   bw.WriteInt(FID(GAMB), m_d.m_glassAmbient);
   bw.WriteFloat(FID(GTOP), m_d.m_glassPadTop);
   bw.WriteFloat(FID(GBOT), m_d.m_glassPadBottom);
   bw.WriteFloat(FID(GLFT), m_d.m_glassPadLeft);
   bw.WriteFloat(FID(GRHT), m_d.m_glassPadRight);
   bw.WriteString(FID(LINK), m_d.m_imageSrcLink);
   bw.WriteFloat(FID(FLDB), m_d.m_depthBias);
   bw.WriteInt(FID(ALGN), m_d.m_imagealignment);
   bw.WriteInt(FID(FILT), m_d.m_filter);
   bw.WriteInt(FID(FIAM), m_d.m_filterAmount);
   bw.WriteString(FID(LMAP), m_d.m_szLightmap);
   bw.WriteBool(FID(BGLS), m_backglass);
   ISelect::SaveData(pstm, hcrypthash);

   HRESULT hr;
   if (FAILED(hr = SavePointData(pstm, hcrypthash)))
      return hr;

   bw.WriteTag(FID(ENDB));

   return S_OK;
}

void Flasher::ClearForOverwrite()
{
   ClearPointsForOverwrite();
}


HRESULT Flasher::InitLoad(IStream *pstm, PinTable *ptable, int *pid, int version, HCRYPTHASH hcrypthash, HCRYPTKEY hcryptkey)
{
   SetDefaults(false);

   BiffReader br(pstm, this, pid, version, hcrypthash, hcryptkey);

   m_ptable = ptable;

   br.Load();

   m_inPlayState = m_d.m_isVisible;

   return S_OK;
}

bool Flasher::LoadToken(const int id, BiffReader * const pbr)
{
   switch(id)
   {
   case FID(PIID): pbr->GetInt(pbr->m_pdata); break;
   case FID(FHEI): pbr->GetFloat(m_d.m_height); break;
   case FID(FLAX): pbr->GetFloat(m_d.m_vCenter.x); break;
   case FID(FLAY): pbr->GetFloat(m_d.m_vCenter.y); break;
   case FID(FROX): pbr->GetFloat(m_d.m_rotX); break;
   case FID(FROY): pbr->GetFloat(m_d.m_rotY); break;
   case FID(FROZ): pbr->GetFloat(m_d.m_rotZ); break;
   case FID(COLR): pbr->GetInt(m_d.m_color); break;
   case FID(TMON): pbr->GetBool(m_d.m_tdr.m_TimerEnabled); break;
   case FID(TMIN): pbr->GetInt(m_d.m_tdr.m_TimerInterval); break;
   case FID(IMAG): pbr->GetString(m_d.m_szImageA); break;
   case FID(IMAB): pbr->GetString(m_d.m_szImageB); break;
   case FID(FALP):
   {
      int iTmp;
      pbr->GetInt(iTmp);
      //if (iTmp>100) iTmp=100;
      if (iTmp < 0) iTmp = 0;
      m_d.m_alpha = iTmp;
      break;
   }
   case FID(MOVA): pbr->GetFloat(m_d.m_modulate_vs_add); break;
   case FID(NAME): pbr->GetWideString(m_wzName, std::size(m_wzName)); break;
   case FID(FVIS): pbr->GetBool(m_d.m_isVisible); break;
   case FID(ADDB): pbr->GetBool(m_d.m_addBlend); break;
   case FID(IDMD): { bool m; pbr->GetBool(m); m_d.m_renderMode = m ? FlasherData::DMD : FlasherData::FLASHER; } break; // Backward compatibility for table 10.8 and before
   case FID(RDMD): pbr->GetInt(&m_d.m_renderMode); break;
   case FID(RSTL): pbr->GetInt(&m_d.m_renderStyle); break;
   case FID(LINK): pbr->GetString(m_d.m_imageSrcLink); break;
   case FID(GRGH): pbr->GetFloat(m_d.m_glassRoughness); break;
   case FID(GAMB): pbr->GetInt(m_d.m_glassAmbient); break;
   case FID(GTOP): pbr->GetFloat(m_d.m_glassPadTop); break;
   case FID(GBOT): pbr->GetFloat(m_d.m_glassPadBottom); break;
   case FID(GLFT): pbr->GetFloat(m_d.m_glassPadLeft); break;
   case FID(GRHT): pbr->GetFloat(m_d.m_glassPadRight); break;
   case FID(BGLS): pbr->GetBool(m_backglass); break;
   case FID(DSPT): pbr->GetBool(m_d.m_displayTexture); break;
   case FID(FLDB): pbr->GetFloat(m_d.m_depthBias); break;
   case FID(ALGN): pbr->GetInt(&m_d.m_imagealignment); break;
   case FID(FILT): pbr->GetInt(&m_d.m_filter); break;
   case FID(FIAM): pbr->GetInt(m_d.m_filterAmount); break;
   case FID(LMAP): pbr->GetString(m_d.m_szLightmap); break;
   default:
   {
      LoadPointToken(id, pbr, pbr->m_version);
      ISelect::LoadToken(id, pbr);
      break;
   }
   }
   return true;
}

HRESULT Flasher::InitPostLoad()
{
   return S_OK;
}

STDMETHODIMP Flasher::InterfaceSupportsErrorInfo(REFIID riid)
{
   static const IID* arr[] =
   {
      &IID_IFlasher
   };

   for (size_t i = 0; i < std::size(arr); i++)
      if (InlineIsEqualGUID(*arr[i], riid))
         return S_OK;

   return S_FALSE;
}

STDMETHODIMP Flasher::get_X(float *pVal)
{
   *pVal = m_d.m_vCenter.x;
   m_vpinball->SetStatusBarUnitInfo(string(), true);

   return S_OK;
}

STDMETHODIMP Flasher::put_X(float newVal)
{
   if (m_d.m_vCenter.x != newVal)
   {
      m_d.m_vCenter.x = newVal;
      m_dynamicVertexBufferRegenerate = true;
   }

   return S_OK;
}

STDMETHODIMP Flasher::get_Y(float *pVal)
{
   *pVal = m_d.m_vCenter.y;
   return S_OK;
}

STDMETHODIMP Flasher::put_Y(float newVal)
{
   if (m_d.m_vCenter.y != newVal)
   {
      m_d.m_vCenter.y = newVal;
      m_dynamicVertexBufferRegenerate = true;
   }

   return S_OK;
}

STDMETHODIMP Flasher::get_RotX(float *pVal)
{
   *pVal = m_d.m_rotX;
   return S_OK;
}

STDMETHODIMP Flasher::put_RotX(float newVal)
{
   if (m_d.m_rotX != newVal)
   {
      m_d.m_rotX = newVal;
      m_dynamicVertexBufferRegenerate = true;
   }

   return S_OK;
}

STDMETHODIMP Flasher::get_RotY(float *pVal)
{
   *pVal = m_d.m_rotY;
   return S_OK;
}

STDMETHODIMP Flasher::put_RotY(float newVal)
{
   if (m_d.m_rotY != newVal)
   {
      m_d.m_rotY = newVal;
      m_dynamicVertexBufferRegenerate = true;
   }

   return S_OK;
}

STDMETHODIMP Flasher::get_RotZ(float *pVal)
{
   *pVal = m_d.m_rotZ;
   return S_OK;
}

STDMETHODIMP Flasher::put_RotZ(float newVal)
{
   if (m_d.m_rotZ != newVal)
   {
      m_d.m_rotZ = newVal;
      m_dynamicVertexBufferRegenerate = true;
   }

   return S_OK;
}

STDMETHODIMP Flasher::get_Height(float *pVal)
{
   *pVal = m_d.m_height;
   return S_OK;
}

STDMETHODIMP Flasher::put_Height(float newVal)
{
   if (m_d.m_height != newVal)
   {
      m_d.m_height = newVal;
      m_dynamicVertexBufferRegenerate = true;
   }

   return S_OK;
}

STDMETHODIMP Flasher::get_Color(OLE_COLOR *pVal)
{
   *pVal = m_d.m_color;
   return S_OK;
}

STDMETHODIMP Flasher::put_Color(OLE_COLOR newVal)
{
   m_d.m_color = newVal;
   return S_OK;
}

STDMETHODIMP Flasher::get_ImageA(BSTR *pVal)
{
   *pVal = MakeWideBSTR(m_d.m_szImageA);
   return S_OK;
}

STDMETHODIMP Flasher::put_ImageA(BSTR newVal)
{
   m_d.m_szImageA = MakeString(newVal);
   return S_OK;
}

STDMETHODIMP Flasher::get_ImageB(BSTR *pVal)
{
   *pVal = MakeWideBSTR(m_d.m_szImageB);
   return S_OK;
}

STDMETHODIMP Flasher::put_ImageB(BSTR newVal)
{
   m_d.m_szImageB = MakeString(newVal);
   return S_OK;
}

STDMETHODIMP Flasher::get_Filter(BSTR *pVal)
{
   const WCHAR *wz;

   switch (m_d.m_filter)
   {
   case Filter_Additive: wz = L"Additive"; break;
   case Filter_Multiply: wz = L"Multiply"; break;
   case Filter_Overlay:  wz = L"Overlay"; break;
   case Filter_Screen:   wz = L"Screen"; break;
   default:
      assert(!"Invalid Flasher Filter");
   case Filter_None:     wz = L"None"; break;
   }
   *pVal = SysAllocString(wz);

   return S_OK;
}

STDMETHODIMP Flasher::put_Filter(BSTR newVal)
{
   const string szFilter = lowerCase(MakeString(newVal));

   if (szFilter == "additive" && m_d.m_filter != Filter_Additive)
      m_d.m_filter = Filter_Additive;
   else if (szFilter == "multiply" && m_d.m_filter != Filter_Multiply)
      m_d.m_filter = Filter_Multiply;
   else if (szFilter == "overlay" && m_d.m_filter != Filter_Overlay)
      m_d.m_filter = Filter_Overlay;
   else if (szFilter == "screen" && m_d.m_filter != Filter_Screen)
      m_d.m_filter = Filter_Screen;
   else if (szFilter == "none" && m_d.m_filter != Filter_None)
      m_d.m_filter = Filter_None;

   return S_OK;
}

STDMETHODIMP Flasher::get_Opacity(LONG *pVal)
{
   *pVal = m_d.m_alpha;
   return S_OK;
}

STDMETHODIMP Flasher::put_Opacity(LONG newVal)
{
   SetAlpha(newVal);
   return S_OK;
}

STDMETHODIMP Flasher::get_IntensityScale(float *pVal)
{
   *pVal = m_d.m_intensity_scale;
   return S_OK;
}

STDMETHODIMP Flasher::put_IntensityScale(float newVal)
{
   m_d.m_intensity_scale = newVal;
   return S_OK;
}

STDMETHODIMP Flasher::get_ModulateVsAdd(float *pVal)
{
   *pVal = m_d.m_modulate_vs_add;
   return S_OK;
}

STDMETHODIMP Flasher::put_ModulateVsAdd(float newVal)
{
   m_d.m_modulate_vs_add = newVal;
   return S_OK;
}

STDMETHODIMP Flasher::get_Amount(LONG *pVal)
{
   *pVal = m_d.m_filterAmount;
   return S_OK;
}

STDMETHODIMP Flasher::put_Amount(LONG newVal)
{
   SetFilterAmount(newVal);
   return S_OK;
}

STDMETHODIMP Flasher::get_Visible(VARIANT_BOOL *pVal)
{
   *pVal = FTOVB(m_d.m_isVisible);
   return S_OK;
}

STDMETHODIMP Flasher::put_Visible(VARIANT_BOOL newVal)
{
   m_d.m_isVisible = VBTOb(newVal); // set visibility
   return S_OK;
}

STDMETHODIMP Flasher::get_DisplayTexture(VARIANT_BOOL *pVal)
{
   *pVal = FTOVB(m_d.m_displayTexture);
   return S_OK;
}

STDMETHODIMP Flasher::put_DisplayTexture(VARIANT_BOOL newVal)
{
   m_d.m_displayTexture = VBTOb(newVal); // set visibility
   return S_OK;
}

STDMETHODIMP Flasher::get_AddBlend(VARIANT_BOOL *pVal)
{
   *pVal = FTOVB(m_d.m_addBlend);
   return S_OK;
}

STDMETHODIMP Flasher::put_AddBlend(VARIANT_BOOL newVal)
{
   m_d.m_addBlend = VBTOb(newVal);
   return S_OK;
}

STDMETHODIMP Flasher::get_DMD(VARIANT_BOOL *pVal)
{
   *pVal = FTOVB(m_d.m_renderMode == FlasherData::DMD);
   return S_OK;
}

STDMETHODIMP Flasher::put_DMD(VARIANT_BOOL newVal)
{
   m_d.m_renderMode = VBTOb(newVal) ? FlasherData::DMD : FlasherData::FLASHER;
   return S_OK;
}

STDMETHODIMP Flasher::put_DMDWidth(int pVal)
{
   m_dmdSize.x = pVal;
   return S_OK;
}

STDMETHODIMP Flasher::put_DMDHeight(int pVal)
{
   m_dmdSize.y = pVal;
   return S_OK;
}

// Implementation included in pintable.cpp
void upscale(uint32_t *const data, const int2 &res, const bool is_brightness_data);

STDMETHODIMP Flasher::put_DMDPixels(VARIANT pVal) // assumes VT_UI1 as input //!! use 64bit instead of 8bit to reduce overhead??
{

   SAFEARRAY *psa = V_ARRAY(&pVal);
   if (psa == nullptr || m_dmdSize.x <= 0 || m_dmdSize.y <= 0)
      return E_FAIL;

   #ifdef DMD_UPSCALE
      constexpr int scale = 3;
   #else
      constexpr int scale = 1;
   #endif

   BaseTexture::Update(m_dmdFrame, m_dmdSize.x * scale, m_dmdSize.y * scale, BaseTexture::BW, nullptr);
   const int size = m_dmdSize.x * m_dmdSize.y;
   // Convert from gamma compressed [0..100] luminance to linear [0..255] luminance, eventually applying ScaleFX upscaling
   VARIANT *p;
   SafeArrayAccessData(psa, (void **)&p);
   if (g_pplayer->m_scaleFX_DMD)
   {
      uint32_t * const __restrict rgba = new uint32_t[size * scale * scale];
      for (int ofs = 0; ofs < size; ++ofs)
         rgba[ofs] = V_UI4(&p[ofs]); 
      upscale(rgba, m_dmdSize, true);
      uint8_t * const __restrict data = m_dmdFrame->data();
      for (int ofs = 0; ofs < size; ++ofs)
         data[ofs] = static_cast<uint8_t>(InvsRGB((float)(rgba[ofs] & 0xFF) * (float)(1.0 / 100.)) * 255.f);
      delete[] rgba;
   }
   else
   {
      uint8_t *const data = m_dmdFrame->data();
      for (int ofs = 0; ofs < size; ++ofs)
         data[ofs] = static_cast<uint8_t>(InvsRGB((float)V_UI4(&p[ofs]) * (float)(1.0 / 100.)) * 255.f);
   }
   SafeArrayUnaccessData(psa);
   m_dmdFrameId++;
   VPXPluginAPIImpl::GetInstance().UpdateDMDSource(this, true);
   return S_OK;
}

STDMETHODIMP Flasher::put_DMDColoredPixels(VARIANT pVal) //!! assumes VT_UI4 as input //!! use 64bit instead of 32bit to reduce overhead??
{
   SAFEARRAY *psa = V_ARRAY(&pVal);
   if (psa == nullptr || m_dmdSize.x <= 0 || m_dmdSize.y <= 0)
      return E_FAIL;

   #ifdef DMD_UPSCALE
      constexpr int scale = 3;
   #else
      constexpr int scale = 1;
   #endif

   BaseTexture::Update(m_dmdFrame, m_dmdSize.x * scale, m_dmdSize.y * scale, BaseTexture::SRGBA, nullptr);
   const int size = m_dmdSize.x * m_dmdSize.y;
   uint32_t *const __restrict data = reinterpret_cast<uint32_t *>(m_dmdFrame->data());
   VARIANT *p;
   SafeArrayAccessData(psa, (void **)&p);
   for (int ofs = 0; ofs < size; ++ofs)
      data[ofs] = V_UI4(&p[ofs]) | 0xFF000000u;
   SafeArrayUnaccessData(psa);
   if (g_pplayer->m_scaleFX_DMD)
      upscale(data, m_dmdSize, false);
   m_dmdFrameId++;
   return S_OK;
}

STDMETHODIMP Flasher::put_VideoCapWidth(LONG cWidth)
{
    if (m_videoCapWidth != cWidth) ResetVideoCap(); //resets capture
    m_videoCapWidth = cWidth;

    return S_OK;
}

STDMETHODIMP Flasher::put_VideoCapHeight(LONG cHeight)
{
    if (m_videoCapHeight != cHeight) ResetVideoCap(); //resets capture
    m_videoCapHeight = cHeight;

    return S_OK;
}

void Flasher::ResetVideoCap()
{
   m_isVideoCap = false;
   if (m_videoCapTex)
   {
      //  m_rd->m_flasherShader->SetTextureNull(SHADER_tex_flasher_A); //!! ??
      m_rd->m_texMan.UnloadTexture(m_videoCapTex.get());
      m_videoCapTex = nullptr;
   }
}

// if PASSED a blank title then we treat this as STOP capture and free resources.
STDMETHODIMP Flasher::put_VideoCapUpdate(BSTR cWinTitle)
{
#ifndef __STANDALONE__
    if (m_videoCapWidth == 0 || m_videoCapHeight == 0) return S_FALSE; //safety.  VideoCapWidth/Height needs to be set prior to this call

    // if PASS blank title then we treat as STOP capture and free resources.  Should be called on table1_exit
    if (SysStringLen(cWinTitle) == 0 || cWinTitle[0] == L'\0')
    {
        ResetVideoCap();
        return S_OK;
    }

    if (m_isVideoCap == false) {  // VideoCap has not started because no sourcewin found
        char * const szWinTitle = MakeChar(cWinTitle);
        m_videoCapHwnd = ::FindWindow(nullptr, szWinTitle);
        delete [] szWinTitle;
        if (m_videoCapHwnd == nullptr)
            return S_FALSE;

        // source videocap found.  lets start!
        GetClientRect(m_videoCapHwnd, &m_videoSourceRect);
        ResetVideoCap();
        try
        {
           m_videoCapTex = BaseTexture::Create(m_videoCapWidth, m_videoCapHeight, BaseTexture::SRGBA);
        }
        catch (...)
        {
           m_videoCapTex = nullptr;
           return S_FAIL;
        }
    }

    // Retrieve the handle to a display device context for the client area of the window.
    const HDC hdcWindow = GetDC(m_videoCapHwnd);

    // Create a compatible DC, which is used in a BitBlt from the window DC.
    const HDC hdcMemDC = CreateCompatibleDC(hdcWindow);

    // Get the client area for size calculation.
    const int pWidth = m_videoCapWidth;
    const int pHeight = m_videoCapHeight;

    // Create a compatible bitmap from the Window DC.
    const HBITMAP hbmScreen = CreateCompatibleBitmap(hdcWindow, pWidth, pHeight);

    // Select the compatible bitmap into the compatible memory DC.
    SelectObject(hdcMemDC, hbmScreen);
    SetStretchBltMode(hdcMemDC, HALFTONE);
    // Bit block transfer into our compatible memory DC.
    m_isVideoCap = StretchBlt(hdcMemDC, 0, 0, pWidth, pHeight, hdcWindow, 0, 0, m_videoSourceRect.right - m_videoSourceRect.left, m_videoSourceRect.bottom - m_videoSourceRect.top, SRCCOPY);
    if (m_isVideoCap)
    {
        // Get the BITMAP from the HBITMAP.
        BITMAP bmpScreen;
        GetObject(hbmScreen, sizeof(BITMAP), &bmpScreen);

        BITMAPINFOHEADER bi;
        bi.biSize = sizeof(BITMAPINFOHEADER);
        bi.biWidth = bmpScreen.bmWidth;
        bi.biHeight = -bmpScreen.bmHeight;
        bi.biPlanes = 1;
        bi.biBitCount = 32;
        bi.biCompression = BI_RGB;
        bi.biSizeImage = 0;
        bi.biXPelsPerMeter = 0;
        bi.biYPelsPerMeter = 0;
        bi.biClrUsed = 0;
        bi.biClrImportant = 0;

        const size_t dwBmpSize = ((bmpScreen.bmWidth * bi.biBitCount + 31) / 32) * 4 * bmpScreen.bmHeight;

        const HANDLE hDIB = GlobalAlloc(GHND, dwBmpSize);
        char* lpbitmap = (char*)GlobalLock(hDIB);

        // Gets the "bits" from the bitmap, and copies them into a buffer 
        // that's pointed to by lpbitmap.
        GetDIBits(hdcWindow, hbmScreen, 0, (UINT)bmpScreen.bmHeight, lpbitmap, (BITMAPINFO*)&bi, DIB_RGB_COLORS);

        // copy bitmap pixels to texture, reversing BGR to RGB and adding an opaque alpha channel
        copy_bgra_rgba<true>((unsigned int*)(m_videoCapTex->data()), (const unsigned int*)lpbitmap, pWidth * pHeight);

        GlobalUnlock(hDIB);
        GlobalFree(hDIB);

        m_rd->m_texMan.SetDirty(m_videoCapTex.get());
    }

    ReleaseDC(m_videoCapHwnd, hdcWindow);
    DeleteObject(hbmScreen);
    DeleteObject(hdcMemDC);
#endif

    return S_OK;
}

STDMETHODIMP Flasher::get_DepthBias(float *pVal)
{
   *pVal = m_d.m_depthBias;
   return S_OK;
}

STDMETHODIMP Flasher::put_DepthBias(float newVal)
{
   m_d.m_depthBias = newVal;
   return S_OK;
}

STDMETHODIMP Flasher::get_ImageAlignment(RampImageAlignment *pVal)
{
   *pVal = m_d.m_imagealignment;
   return S_OK;
}

STDMETHODIMP Flasher::put_ImageAlignment(RampImageAlignment newVal)
{
   m_d.m_imagealignment = newVal;
   return S_OK;
}

//Sets the in play state for light sequencing rendering
void Flasher::setInPlayState(const bool newVal)
{
   m_inPlayState = newVal;
}

#pragma region Rendering

void Flasher::RenderSetup(RenderDevice *device)
{
   assert(m_rd == nullptr);
   m_rd = device;

   m_lightmap = m_ptable->GetLight(m_d.m_szLightmap);

   vector<RenderVertex> vvertex;
   GetRgVertex(vvertex);

   m_numVertices = (unsigned int)vvertex.size();
   if (m_numVertices == 0)
   {
       // no polys to render leave vertex buffer undefined 
       m_numPolys = 0;
       return;
   }

   vector<WORD> vtri;
   
   {
      vector<unsigned int> vpoly(m_numVertices);
      for (unsigned int i = 0; i < m_numVertices; i++)
         vpoly[i] = i;

      PolygonToTriangles(vvertex, vpoly, vtri, false);
   }

   m_numPolys = (int)(vtri.size()/3);
   if (m_numPolys == 0)
   {
      // no polys to render leave vertex buffer undefined 
      return;
   }

   IndexBuffer* dynamicIndexBuffer = new IndexBuffer(m_rd, m_numPolys * 3, false, IndexBuffer::FMT_INDEX16);

   WORD* bufi;
   dynamicIndexBuffer->Lock(bufi);
   memcpy(bufi, vtri.data(), vtri.size()*sizeof(WORD));
   dynamicIndexBuffer->Unlock();

   VertexBuffer* dynamicVertexBuffer = new VertexBuffer(m_rd, m_numVertices, nullptr, true);

   delete m_meshBuffer;
   m_meshBuffer = new MeshBuffer(m_wzName, dynamicVertexBuffer, dynamicIndexBuffer, true);

   m_vertices = new Vertex3D_NoTex2[m_numVertices];
   m_transformedVertices = new Vertex3D_NoTex2[m_numVertices];

   m_minx = FLT_MAX;
   m_miny = FLT_MAX;
   m_maxx = -FLT_MAX;
   m_maxy = -FLT_MAX;

   for (unsigned int i = 0; i < m_numVertices; i++)
   {
      const RenderVertex * const pv0 = &vvertex[i];

      m_vertices[i].x = pv0->x;
      m_vertices[i].y = pv0->y;
      m_vertices[i].z = 0;

      if (pv0->x > m_maxx) m_maxx = pv0->x;
      if (pv0->x < m_minx) m_minx = pv0->x;
      if (pv0->y > m_maxy) m_maxy = pv0->y;
      if (pv0->y < m_miny) m_miny = pv0->y;
   }
   
   const float inv_width = 1.0f / (m_maxx - m_minx);
   const float inv_height = 1.0f / (m_maxy - m_miny);
   const float inv_tablewidth = 1.0f / (m_ptable->m_right - m_ptable->m_left);
   const float inv_tableheight = 1.0f / (m_ptable->m_bottom - m_ptable->m_top);
   m_d.m_vCenter.x = m_minx + ((m_maxx - m_minx)*0.5f);
   m_d.m_vCenter.y = m_miny + ((m_maxy - m_miny)*0.5f);

   for (unsigned int i = 0; i < m_numVertices; i++)
   {
      if (m_d.m_imagealignment == ImageModeWrap)
      {
         m_vertices[i].tu = (m_vertices[i].x - m_minx)*inv_width;
         m_vertices[i].tv = (m_vertices[i].y - m_miny)*inv_height;
      }
      else
      {
         m_vertices[i].tu = m_vertices[i].x*inv_tablewidth;
         m_vertices[i].tv = m_vertices[i].y*inv_tableheight;
      }
   }

   m_dynamicVertexBufferRegenerate = true;
}

void Flasher::RenderRelease()
{
   assert(m_rd != nullptr);
   ResetVideoCap();
   delete m_meshBuffer;
   delete[] m_vertices;
   delete[] m_transformedVertices;
   m_meshBuffer = nullptr;
   m_vertices = nullptr;
   m_transformedVertices = nullptr;
   m_renderFrame = nullptr;
   m_dmdFrame = nullptr;
   m_dmdSize = int2(0, 0);
   m_lightmap = nullptr;
   m_rd = nullptr;
}

void Flasher::UpdateAnimation(const float diff_time_msec)
{
}

void Flasher::Render(const unsigned int renderMask)
{
   assert(m_rd != nullptr);
   const bool isStaticOnly = renderMask & Renderer::STATIC_ONLY;
   const bool isReflectionPass = renderMask & Renderer::REFLECTION_PASS;
   TRACE_FUNCTION();

   // FIXME BGFX DX12 will crash on this
   #ifdef ENABLE_BGFX
   if (bgfx::getRendererType() == bgfx::RendererType::Direct3D12)
      return;
   #endif

   // Flashers are always dynamic parts
   if (isStaticOnly)
      return;

   // Don't render if invisible, degenerated or in reflection pass
   // TODO shouldn't we handle flashers like lights and therefore process them according to disable lightmap flag ?
   if (!m_d.m_isVisible || m_meshBuffer == nullptr || isReflectionPass)
      return;

   // Don't render if LightSequence in play and state is off
   if (m_lockedByLS && !m_inPlayState)
      return;

   // Update lightmap before checking anything that uses alpha
   float alpha = (float) m_d.m_alpha;
   if (m_lightmap)
   {
      if (m_lightmap->m_d.m_intensity != 0.f && m_lightmap->m_d.m_intensity_scale != 0.f)
         alpha *= m_lightmap->m_currentIntensity / (m_lightmap->m_d.m_intensity * m_lightmap->m_d.m_intensity_scale);
      else
         alpha = 0.f;
   }

   if (m_d.m_color == 0 || alpha == 0.0f || m_d.m_intensity_scale == 0.0f)
      return;

   if (m_dynamicVertexBufferRegenerate)
   {
      m_dynamicVertexBufferRegenerate = false;
      const float height = m_d.m_height;
      const float movx = m_minx + (m_maxx - m_minx)*0.5f;
      const float movy = m_miny + (m_maxy - m_miny)*0.5f;
      const Matrix3D tempMatrix = Matrix3D::MatrixTranslate(-movx /* -m_d.m_vCenter.x */, -movy /* -m_d.m_vCenter.y */, 0.f)
                             * (((Matrix3D::MatrixRotateZ(ANGTORAD(m_d.m_rotZ))
                                * Matrix3D::MatrixRotateY(ANGTORAD(m_d.m_rotY)))
                                * Matrix3D::MatrixRotateX(ANGTORAD(m_d.m_rotX)))
                                * Matrix3D::MatrixTranslate(m_d.m_vCenter.x, m_d.m_vCenter.y, height));

      Vertex3D_NoTex2 *buf;
      m_meshBuffer->m_vb->Lock(buf);
      for (unsigned int i = 0; i < m_numVertices; i++)
      {
         Vertex3D_NoTex2 vert = m_vertices[i];
         tempMatrix.MultiplyVector(vert);
         if (m_backglass)
            vert.z = 1.f;
         m_transformedVertices[i] = vert;
         buf[i] = vert;
      }
      m_meshBuffer->m_vb->Unlock();
   }

   if (m_backglass)
   {
      Matrix3D matWorldViewProj[2];
      matWorldViewProj[0] = Matrix3D::MatrixIdentity(); // MVP to move from back buffer space (0..w, 0..h) to clip space (-1..1, -1..1)
      matWorldViewProj[0]._11 = 2.0f / static_cast<float>(EDITOR_BG_WIDTH /* m_rd->GetCurrentRenderTarget()->GetWidth() */);
      matWorldViewProj[0]._41 = -1.0f;
      matWorldViewProj[0]._22 = -2.0f / static_cast<float>(EDITOR_BG_HEIGHT /* m_rd->GetCurrentRenderTarget()->GetHeight() */);
      matWorldViewProj[0]._42 = 1.0f;
      const int eyes = m_rd->GetCurrentRenderTarget()->m_nLayers;
      if (eyes > 1)
         matWorldViewProj[1] = matWorldViewProj[0];
      m_rd->m_flasherShader->SetMatrix(SHADER_matWorldViewProj, &matWorldViewProj[0], eyes);
      m_rd->m_DMDShader->SetMatrix(SHADER_matWorldViewProj, &matWorldViewProj[0], eyes);
   }

   m_rd->ResetRenderState();
   m_rd->SetRenderState(RenderState::CULLMODE, RenderState::CULL_NONE);

   Vertex3Ds pos(m_d.m_vCenter.x, m_d.m_vCenter.y, m_d.m_height);
   const vec4 color = convertColor(m_d.m_color, alpha * m_d.m_intensity_scale / 100.0f);
   const float clampedModulateVsAdd = min(max(m_d.m_modulate_vs_add, 0.00001f), 0.9999f); // avoid 0, as it disables the blend and avoid 1 as it looks not good with day->night changes
   switch (m_d.m_renderMode)
   {
      case FlasherData::FLASHER:
      {
         Texture *const pinA = m_ptable->GetImage(m_d.m_szImageA);
         Texture *const pinB = m_ptable->GetImage(m_d.m_szImageB);

         m_rd->m_flasherShader->SetVector(SHADER_staticColor_Alpha, &color);

         vec4 flasherData(-1.f, -1.f, (float)m_d.m_filter, m_d.m_addBlend ? 1.f : 0.f);
         m_rd->m_flasherShader->SetTechnique(SHADER_TECHNIQUE_basic_noLight);

         float flasherMode;
         if ((pinA || m_isVideoCap) && !pinB)
         {
            flasherMode = 0.f;
            if (m_isVideoCap)
               m_rd->m_flasherShader->SetTexture(SHADER_tex_flasher_A, m_videoCapTex.get());
            else
               m_rd->m_flasherShader->SetTexture(SHADER_tex_flasher_A, pinA);

            if (!m_d.m_addBlend)
               flasherData.x = !m_isVideoCap ? pinA->m_alphaTestValue : 0.f;
         }
         else if (!(pinA || m_isVideoCap) && pinB)
         {
            flasherMode = 0.f;
            m_rd->m_flasherShader->SetTexture(SHADER_tex_flasher_A, pinB);

            if (!m_d.m_addBlend)
               flasherData.x = pinB->m_alphaTestValue;
         }
         else if ((pinA || m_isVideoCap) && pinB)
         {
            flasherMode = 1.f;
            if (m_isVideoCap)
               m_rd->m_flasherShader->SetTexture(SHADER_tex_flasher_A, m_videoCapTex.get());
            else
               m_rd->m_flasherShader->SetTexture(SHADER_tex_flasher_A, pinA);
            m_rd->m_flasherShader->SetTexture(SHADER_tex_flasher_B, pinB);

            if (!m_d.m_addBlend)
            {
               flasherData.x = !m_isVideoCap ? pinA->m_alphaTestValue : 0.f;
               flasherData.y = pinB->m_alphaTestValue;
            }
         }
         else
            flasherMode = 2.f;

         m_rd->m_flasherShader->SetVector(SHADER_alphaTestValueAB_filterMode_addBlend, &flasherData);
         m_rd->m_flasherShader->SetVector(SHADER_amount_blend_modulate_vs_add_flasherMode, static_cast<float>(m_d.m_filterAmount) / 100.0f, clampedModulateVsAdd, flasherMode, 0.f);

         // Check if this flasher is used as a lightmap and should be convoluted with the light shadows
         if (m_lightmap != nullptr && m_lightmap->m_d.m_shadows == ShadowMode::RAYTRACED_BALL_SHADOWS)
            m_rd->m_flasherShader->SetVector(SHADER_lightCenter_doShadow, m_lightmap->m_d.m_vCenter.x, m_lightmap->m_d.m_vCenter.y, m_lightmap->GetCurrentHeight(), 1.0f);

         m_rd->SetRenderState(RenderState::ZWRITEENABLE, RenderState::RS_FALSE);
         m_rd->SetRenderState(RenderState::ALPHABLENDENABLE, RenderState::RS_TRUE);
         m_rd->SetRenderState(RenderState::SRCBLEND, RenderState::SRC_ALPHA);
         m_rd->SetRenderState(RenderState::DESTBLEND, m_d.m_addBlend ? RenderState::INVSRC_COLOR : RenderState::INVSRC_ALPHA);
         m_rd->SetRenderState(RenderState::BLENDOP, m_d.m_addBlend ? RenderState::BLENDOP_REVSUBTRACT : RenderState::BLENDOP_ADD);

         m_rd->DrawMesh(m_rd->m_flasherShader, true, pos, m_d.m_depthBias, m_meshBuffer, RenderDevice::TRIANGLELIST, 0, m_numPolys * 3);

         m_rd->m_flasherShader->SetVector(SHADER_lightCenter_doShadow, 0.0f, 0.0f, 0.0f, 0.0f);
         break;
      }

      case FlasherData::DMD:
      {
         if (m_dmdFrame)
            m_renderFrame = m_dmdFrame;
         else
         {
            ResURIResolver::DisplayState dmd { nullptr };
            if (!m_d.m_imageSrcLink.empty())
               dmd = g_pplayer->m_resURIResolver.GetDisplayState(m_d.m_imageSrcLink);
            if (dmd.state.frame == nullptr)
               dmd = g_pplayer->m_resURIResolver.GetDisplayState("ctrl://default/display");
            if (dmd.state.frame != nullptr)
               BaseTexture::Update(m_renderFrame, dmd.source->width, dmd.source->height,
                              dmd.source->frameFormat == CTLPI_DISPLAY_FORMAT_LUM8    ? BaseTexture::BW
                            : dmd.source->frameFormat == CTLPI_DISPLAY_FORMAT_SRGB565 ? BaseTexture::SRGB565
                                                                                      : BaseTexture::SRGB, dmd.state.frame);
         }
         if (m_renderFrame == nullptr)
         {
            if (m_backglass)
               g_pplayer->m_renderer->UpdateBasicShaderMatrix();
            return;
         }
         Texture *const glass = m_ptable->GetImage(m_d.m_szImageA);
         if (m_d.m_modulate_vs_add < 1.f)
            m_rd->EnableAlphaBlend(m_d.m_addBlend);
         else
            m_rd->SetRenderState(RenderState::ALPHABLENDENABLE, RenderState::RS_FALSE);
         m_rd->SetRenderState(RenderState::ZWRITEENABLE, RenderState::RS_FALSE);
         const vec3 dotTint = m_renderFrame->m_format == BaseTexture::BW ? vec3(color.x, color.y, color.z) : vec3(1.f, 1.f, 1.f);
         const int dmdProfile = clamp(m_d.m_renderStyle, 0, 7);
         g_pplayer->m_renderer->SetupDMDRender(dmdProfile, false, dotTint, color.w, m_renderFrame, m_d.m_modulate_vs_add, m_backglass ? Renderer::Reinhard : Renderer::Linear, m_transformedVertices,
            vec4(m_d.m_glassPadLeft, m_d.m_glassPadTop, m_d.m_glassPadRight, m_d.m_glassPadBottom),
            vec3(1.f, 1.f, 1.f), m_d.m_glassRoughness, 
            glass ? glass : nullptr, vec4(0.f, 0.f, 1.f, 1.f), vec3(GetRValue(m_d.m_glassAmbient) / 255.f, GetGValue(m_d.m_glassAmbient) / 255.f, GetBValue(m_d.m_glassAmbient) / 255.f));
         // DMD flasher are rendered transparent. They used to be drawn as a separate pass after opaque parts and before other transparents.
         // There we shift the depthbias to reproduce this behavior for backward compatibility.
         m_rd->DrawMesh(m_rd->m_DMDShader, true, pos, m_d.m_depthBias - 10000.f, m_meshBuffer, RenderDevice::TRIANGLELIST, 0, m_numPolys * 3);
         break;
      }

      case FlasherData::DISPLAY:
      {
         ResURIResolver::DisplayState display = g_pplayer->m_resURIResolver.GetDisplayState(m_d.m_imageSrcLink);
         if (display.state.frame == nullptr)
         {
            if (m_backglass)
               g_pplayer->m_renderer->UpdateBasicShaderMatrix();
            return;
         }
         BaseTexture::Update(m_renderFrame, display.source->width, display.source->height, 
                          display.source->frameFormat == CTLPI_DISPLAY_FORMAT_LUM8 ? BaseTexture::BW
                     : display.source->frameFormat == CTLPI_DISPLAY_FORMAT_SRGB565 ? BaseTexture::SRGB565
                                                                                   : BaseTexture::SRGB,
            display.state.frame);
         if (m_d.m_modulate_vs_add < 1.f)
            m_rd->EnableAlphaBlend(m_d.m_addBlend);
         else
            m_rd->SetRenderState(RenderState::ALPHABLENDENABLE, RenderState::RS_FALSE);
         const int displayProfile = clamp(m_d.m_renderStyle, 0, 1);  // 4);
         switch (displayProfile)
         {
         case 0: // Pixelated
            m_rd->m_flasherShader->SetTexture(SHADER_tex_flasher_A, m_renderFrame.get(), false, SF_NONE);
            m_rd->m_flasherShader->SetVector(SHADER_staticColor_Alpha, color.x * color.w, color.y * color.w, color.z * color.w, 1.f);
            m_rd->m_flasherShader->SetVector(SHADER_alphaTestValueAB_filterMode_addBlend, -1.f, -1.f, 0.f, m_d.m_addBlend ? 1.f : 0.f);
            m_rd->m_flasherShader->SetVector(SHADER_amount_blend_modulate_vs_add_flasherMode, 0.f, clampedModulateVsAdd, 0.f, 0.f);
            m_rd->SetRenderState(RenderState::ZWRITEENABLE, RenderState::RS_FALSE);
            m_rd->SetRenderState(RenderState::ALPHABLENDENABLE, RenderState::RS_TRUE);
            m_rd->SetRenderState(RenderState::SRCBLEND, RenderState::SRC_ALPHA);
            m_rd->SetRenderState(RenderState::DESTBLEND, m_d.m_addBlend ? RenderState::INVSRC_COLOR : RenderState::INVSRC_ALPHA);
            m_rd->SetRenderState(RenderState::BLENDOP, m_d.m_addBlend ? RenderState::BLENDOP_REVSUBTRACT : RenderState::BLENDOP_ADD);
            m_rd->m_flasherShader->SetTechnique(SHADER_TECHNIQUE_basic_noLight);
            break;
         case 1: // Smoothed
            m_rd->m_flasherShader->SetTexture(SHADER_tex_flasher_A, m_renderFrame.get(), false, SF_TRILINEAR);
            m_rd->m_flasherShader->SetVector(SHADER_staticColor_Alpha, color.x * color.w, color.y * color.w, color.z * color.w, 1.f);
            m_rd->m_flasherShader->SetVector(SHADER_alphaTestValueAB_filterMode_addBlend, -1.f, -1.f, 0.f, m_d.m_addBlend ? 1.f : 0.f);
            m_rd->m_flasherShader->SetVector(SHADER_amount_blend_modulate_vs_add_flasherMode, 0.f, clampedModulateVsAdd, 0.f, 0.f);
            m_rd->SetRenderState(RenderState::ZWRITEENABLE, RenderState::RS_FALSE);
            m_rd->SetRenderState(RenderState::ALPHABLENDENABLE, RenderState::RS_TRUE);
            m_rd->SetRenderState(RenderState::SRCBLEND, RenderState::SRC_ALPHA);
            m_rd->SetRenderState(RenderState::DESTBLEND, m_d.m_addBlend ? RenderState::INVSRC_COLOR : RenderState::INVSRC_ALPHA);
            m_rd->SetRenderState(RenderState::BLENDOP, m_d.m_addBlend ? RenderState::BLENDOP_REVSUBTRACT : RenderState::BLENDOP_ADD);
            m_rd->m_flasherShader->SetTechnique(SHADER_TECHNIQUE_basic_noLight);
            break;
         case 2: // ScaleFX
            assert(false); // Not yet implemented
            break;
         case 3: // Vertical CRT
            assert(false); // Not yet implemented
            break;
         case 4: // Horizontal CRT
            assert(false); // Not yet implemented
            break;
         }
         // We also apply the depth bias shift, not for backward compatibility (as display did not exist before 10.8.1) but for consistency between DMD and Display mode
         m_rd->DrawMesh(m_rd->m_flasherShader, true, pos, m_d.m_depthBias - 10000.f, m_meshBuffer, RenderDevice::TRIANGLELIST, 0, m_numPolys * 3);
         break;
      }

      case FlasherData::ALPHASEG:
      {
         ResURIResolver::SegDisplayState segs = g_pplayer->m_resURIResolver.GetSegDisplayState(m_d.m_imageSrcLink);
         if (segs.state.frame == nullptr || segs.source->nElements == 0)
            return;
         Texture *const glass = m_ptable->GetImage(m_d.m_szImageA);
         // We always use max blending as segment may overlap in the glass diffuse: we retain the most lighted one which is wrong but looks ok (otherwise we would have to deal with colorspace conversions and layering between glass and emitter)
         m_rd->SetRenderState(RenderState::BLENDOP, RenderState::BLENDOP_MAX);
         m_rd->SetRenderState(RenderState::ALPHABLENDENABLE, RenderState::RS_TRUE);
         m_rd->SetRenderState(RenderState::SRCBLEND, RenderState::SRC_ALPHA);
         m_rd->SetRenderState(RenderState::DESTBLEND, RenderState::ONE);
         m_rd->SetRenderState(RenderState::ZWRITEENABLE, RenderState::RS_FALSE);
         const int renderStyle = clamp(m_d.m_renderStyle % 8, 0, 7); // Shading settings
         const Renderer::SegmentFamily segFamily = static_cast<Renderer::SegmentFamily>(clamp(m_d.m_renderStyle / 8, 0, 4)); // Segments shape
         g_pplayer->m_renderer->SetupSegmentRenderer(renderStyle, false, vec3(color.x, color.y, color.z), color.w,
            segFamily, segs.source->elementType[0], segs.state.frame, m_backglass ? Renderer::Reinhard : Renderer::Linear, m_transformedVertices,
            vec4(m_d.m_glassPadLeft, m_d.m_glassPadTop, m_d.m_glassPadRight, m_d.m_glassPadBottom),
            vec3(1.f, 1.f, 1.f), m_d.m_glassRoughness, glass ? glass : nullptr, vec4(0.f, 0.f , 1.f, 1.f), 
            vec3(GetRValue(m_d.m_glassAmbient)/255.f, GetGValue(m_d.m_glassAmbient)/255.f, GetBValue(m_d.m_glassAmbient)/255.f));
         // We also apply the depth bias shift, not for backward compatibility (as alphaseg display did not exist before 10.8.1) but for consistency between DMD and Display mode
         m_rd->DrawMesh(m_rd->m_DMDShader, true, pos, m_d.m_depthBias - 10000.f, m_meshBuffer, RenderDevice::TRIANGLELIST, 0, m_numPolys * 3);
         break;
      }
   }

   if (m_backglass)
      g_pplayer->m_renderer->UpdateBasicShaderMatrix();
}

#pragma endregion
