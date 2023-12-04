#include "stdafx.h"

#include <algorithm>

#include "FlexDMD.h"
#include "UltraDMD.h"
#include "actors/Group.h"
#include "actors/Image.h"
#include "actors/Frame.h"
#include "actors/Label.h"
#include "actors/ImageSequence.h"
#include "actors/GIFImage.h"
#include "actors/Video.h"

#include "AssetManager.h"

FlexDMD::FlexDMD()
{
   m_width = 128;
   m_height = 32;
   m_frameRate = 60;
   m_run = false;
   m_runtimeVersion = 1008;
   m_clear = false;
   m_renderLockCount = 0;
   m_pThread = NULL;
   m_pGraphics = NULL;
   m_pStage = new Group(this, "Stage");
   m_pStage->AddRef();
   m_renderMode = RenderMode_DMD_GRAY_4;
   m_dmdColor = RGB(255, 88, 32);
   m_pAssetManager = new AssetManager();
   m_pDmd = NULL;
}

FlexDMD::~FlexDMD()
{
   m_run = false;

   if (m_pThread) {
      m_pThread->join();
      delete m_pThread;
   }

   m_pStage->Release();

   delete m_pAssetManager;

   if (m_pDmd) {
      m_pDmd->Cleanup();
      m_pDmd = NULL;
   }
}

STDMETHODIMP FlexDMD::get_Version(LONG *pRetVal)
{
   *pRetVal = 9999;

   return S_OK;
}

STDMETHODIMP FlexDMD::get_RuntimeVersion(LONG *pRetVal)
{
   *pRetVal = m_runtimeVersion;

   return S_OK;
}

STDMETHODIMP FlexDMD::put_RuntimeVersion(LONG pRetVal)
{
   m_runtimeVersion = pRetVal;

   return S_OK;
}

STDMETHODIMP FlexDMD::get_Run(VARIANT_BOOL *pRetVal)
{
   *pRetVal = m_run;

   return S_OK;
}

STDMETHODIMP FlexDMD::put_Run(VARIANT_BOOL pRetVal)
{
   if (!m_run && pRetVal == VARIANT_FALSE)
      return S_OK;

   if (m_run && pRetVal == VARIANT_TRUE)
      return S_OK;

   m_run = (pRetVal == VARIANT_TRUE);

   if (m_run) {
      PLOGI.printf("Starting render thread");

      m_pThread = new std::thread([this](){
         RenderLoop();

         PLOGI.printf("Render thread finished");
      });
   }
   else {
      PLOGI.printf("Stopping render thread");

      m_pThread->join();
      delete m_pThread;

      m_pThread = NULL;

      m_pAssetManager->ClearAll();
   }

   return S_OK;
}

STDMETHODIMP FlexDMD::get_Show(VARIANT_BOOL *pRetVal)
{
   *pRetVal = VARIANT_FALSE;

   return S_OK;
}

STDMETHODIMP FlexDMD::put_Show(VARIANT_BOOL pRetVal)
{
   return S_OK;
}

STDMETHODIMP FlexDMD::get_GameName(BSTR *pRetVal)
{
   CComBSTR Val(m_szGameName.c_str());
   *pRetVal = Val.Detach();

   return S_OK;
}

STDMETHODIMP FlexDMD::put_GameName(BSTR pRetVal)
{
   m_szGameName = MakeString(pRetVal);

   PLOGI.printf("Game name set to %s", m_szGameName.c_str());

   return S_OK;
}

STDMETHODIMP FlexDMD::get_Width(unsigned short *pRetVal)
{
   *pRetVal = m_width;

   return S_OK;
}

STDMETHODIMP FlexDMD::put_Width(unsigned short pRetVal)
{
   m_width = pRetVal;

   return S_OK;
}

STDMETHODIMP FlexDMD::get_Height(unsigned short *pRetVal)
{
   *pRetVal = m_height;

   return S_OK;
}

STDMETHODIMP FlexDMD::put_Height(unsigned short pRetVal)
{
   m_height = pRetVal;

   return S_OK;
}

STDMETHODIMP FlexDMD::get_Color(OLE_COLOR *pRetVal)
{
   *pRetVal = m_dmdColor;

   return S_OK;
}

STDMETHODIMP FlexDMD::put_Color(OLE_COLOR pRetVal)
{
   m_dmdColor = pRetVal;

   return S_OK;
}

STDMETHODIMP FlexDMD::get_RenderMode(RenderMode *pRetVal)
{
   *pRetVal = m_renderMode;

   return S_OK;
}

STDMETHODIMP FlexDMD::put_RenderMode(RenderMode pRetVal)
{
   m_renderMode = pRetVal;

   return S_OK;
}

STDMETHODIMP FlexDMD::get_ProjectFolder(BSTR *pRetVal)
{
   CComBSTR Val(m_pAssetManager->GetBasePath().c_str());
   *pRetVal = Val.Detach();

   return S_OK;
}

STDMETHODIMP FlexDMD::put_ProjectFolder(BSTR pRetVal)
{
   m_pAssetManager->SetBasePath(MakeString(pRetVal));

   return S_OK;
}

STDMETHODIMP FlexDMD::get_TableFile(BSTR *pRetVal)
{
   CComBSTR Val(m_szTableFile.c_str());
   *pRetVal = Val.Detach();

   return S_OK;
}

STDMETHODIMP FlexDMD::put_TableFile(BSTR pRetVal)
{
   m_szTableFile = MakeString(pRetVal);

   PLOGI.printf("Table file set to %s", m_szTableFile.c_str());

   return S_OK;
}

STDMETHODIMP FlexDMD::get_Clear(VARIANT_BOOL *pRetVal)
{
   *pRetVal = m_clear ? VARIANT_TRUE : VARIANT_FALSE;

   return S_OK;
}

STDMETHODIMP FlexDMD::put_Clear(VARIANT_BOOL pRetVal)
{
   m_clear = (pRetVal == VARIANT_TRUE);

   return S_OK;
}

STDMETHODIMP FlexDMD::get_DmdColoredPixels(VARIANT* pRetVal)
{
   if (!m_pDmd || !m_pDmd->IsUpdated())
      return S_FALSE;

   const UINT32 end = m_width * m_height;

   SAFEARRAY* psa = SafeArrayCreateVector(VT_VARIANT, 0, end);
   VARIANT* pData;

   SafeArrayAccessData(psa, (void **)&pData);

   if (m_pDmd->IsRGB24()) {
      UINT8* ptr = m_pDmd->GetRGB24PixelData();
      if (!ptr)
         return S_FALSE;
      UINT8 r, g, b;
      for (UINT32 i = 0; i < end; i++) {
         r = *ptr++;
         g = *ptr++;
         b = *ptr++;
         V_VT(&pData[i]) = VT_UI4;
         V_UI4(&pData[i]) = r | g << 8 | b << 16;
      }
   }
   else {
       UINT8* pDmdData = m_pDmd->GetData();
       if (!pDmdData)
         return S_FALSE;
       UINT8* pDmdPalette = m_pDmd->GetPalette();
       UINT8 pos, r, g, b;
       for (UINT32 i = 0; i < end; i++) {
          pos = pDmdData[i] * 3;
          r = pDmdPalette[pos];
          g = pDmdPalette[pos + 1];
          b = pDmdPalette[pos + 2];
          V_VT(&pData[i]) = VT_UI4;
          V_UI4(&pData[i]) = r | g << 8 | b << 16;
       }
   }

   SafeArrayUnaccessData(psa);

   V_VT(pRetVal) = VT_ARRAY | VT_VARIANT;
   V_ARRAY(pRetVal) = psa;

   m_pDmd->ResetUpdated();

   return S_OK;
}

STDMETHODIMP FlexDMD::get_DmdPixels(VARIANT* pRetVal)
{
   if (!m_pDmd || !m_pDmd->IsUpdated() || m_pDmd->IsRGB24())
      return S_FALSE;

   const UINT32 end = m_width * m_height;
   UINT8* pDmdData = m_pDmd->GetData();
   if (!pDmdData)
      return S_FALSE;
   UINT8* pDmdPalette = m_pDmd->GetPalette();

   SAFEARRAY* psa = SafeArrayCreateVector(VT_VARIANT, 0, end);
   VARIANT* pData;

   SafeArrayAccessData(psa, (void **)&pData);

   UINT8 pos, r, g, b;
   for (UINT32 i = 0; i < end; i++) {
      pos = pDmdData[i] * 3;
      r = pDmdPalette[pos];
      g = pDmdPalette[pos + 1];
      b = pDmdPalette[pos + 2];
      V_VT(&pData[i]) = VT_UI1;
      V_UI1(&pData[i]) = r | g << 8 | b << 16;
   }

   SafeArrayUnaccessData(psa);

   V_VT(pRetVal) = VT_ARRAY | VT_VARIANT;
   V_ARRAY(pRetVal) = psa;

   m_pDmd->ResetUpdated();

   return S_OK;
}

STDMETHODIMP FlexDMD::putref_Segments(VARIANT rhs)
{
   int size1 = 0;
   int size2 = 0;

   switch (m_renderMode) {
      case RenderMode_SEG_2x16Alpha:
         size1 = 2 * 16;
         break;
      case RenderMode_SEG_2x20Alpha:
         size1 = 2 * 20;
         break;
      case RenderMode_SEG_2x7Alpha_2x7Num:
         size1 = 2 * 7 + 2 * 7;
         break;
      case RenderMode_SEG_2x7Alpha_2x7Num_4x1Num:
         size1 = 2 * 7 + 2 * 7 + 4;
         break;
      case RenderMode_SEG_2x7Num_2x7Num_4x1Num:
         size1 = 2 * 7 + 2 * 7 + 4;
         break;
      case RenderMode_SEG_2x7Num_2x7Num_10x1Num:
         size1 = 2 * 7 + 2 * 7 + 4;
         size2 = 6;
         break;
      case RenderMode_SEG_2x7Num_2x7Num_4x1Num_gen7:
         size1 = 2 * 7 + 2 * 7 + 4;
         break;
      case RenderMode_SEG_2x7Num10_2x7Num10_4x1Num:
         size1 = 2 * 7 + 2 * 7 + 4;
         break;
      case RenderMode_SEG_2x6Num_2x6Num_4x1Num:
         size1 = 2 * 6 + 2 * 6 + 4;
         break;
      case RenderMode_SEG_2x6Num10_2x6Num10_4x1Num:
         size1 = 2 * 6 + 2 * 6 + 4;
         break;
      case RenderMode_SEG_4x7Num10:
         size1 = 4 * 7;
         break;
      case RenderMode_SEG_6x4Num_4x1Num:
         size1 = 6 * 4 + 4;
         break;
      case RenderMode_SEG_2x7Num_4x1Num_1x16Alpha:
         size1 = 2 * 7 + 4 + 1;
         break;
      case RenderMode_SEG_1x16Alpha_1x16Num_1x7Num:
         size1 = 1 + 1 + 7;
         break;
      default:
         break;
   }

   SAFEARRAY* psa = V_ARRAY(&rhs);

   VARIANT* p;
   SafeArrayAccessData(psa, (void **)&p);

   for (int i = 0; i < size1; i++)
      m_segData1[i] = V_I2(&p[i]);

   for (int i = 0; i < size2; i++)
      m_segData2[i] = V_I2(&p[size1 + i]);

   SafeArrayUnaccessData(psa);

   return S_OK;
}

STDMETHODIMP FlexDMD::get_Stage(IGroupActor **pRetVal)
{
   return m_pStage->QueryInterface(IID_IGroupActor, (void**)pRetVal);
}

STDMETHODIMP FlexDMD::LockRenderThread()
{
   m_renderLockCount++;

   return S_OK;
}

STDMETHODIMP FlexDMD::UnlockRenderThread()
{
   m_renderLockCount--;

   return S_OK;
}

STDMETHODIMP FlexDMD::NewGroup(BSTR Name, IGroupActor **pRetVal)
{
   Group* obj = new Group(this, MakeString(Name));
   return obj->QueryInterface(IID_IGroupActor, (void**)pRetVal);
}

STDMETHODIMP FlexDMD::NewFrame(BSTR Name, IFrameActor **pRetVal)
{
   Frame* obj = new Frame(this, MakeString(Name));
   return obj->QueryInterface(IID_IFrameActor, (void**)pRetVal); 
}

STDMETHODIMP FlexDMD::NewLabel(BSTR Name, IUnknown *Font_,BSTR Text, ILabelActor **pRetVal)
{
   Label* obj = new Label(this,  dynamic_cast<Font*>(Font_), MakeString(Text),  MakeString(Name));
   return obj->QueryInterface(IID_ILabelActor, (void**)pRetVal);
}

STDMETHODIMP FlexDMD::NewVideo(BSTR Name, BSTR video, IVideoActor **pRetVal)
{
   AnimatedActor* obj = NewVideo(MakeString(video), MakeString(Name));
   if (obj)
      return obj->QueryInterface(IID_IVideoActor, (void**)pRetVal);

   *pRetVal = NULL;

   return S_OK;
}

STDMETHODIMP FlexDMD::NewImage(BSTR Name, BSTR image, IImageActor **pRetVal)
{
   Image* obj = Image::Create(this, m_pAssetManager, MakeString(image), MakeString(Name));
   if (obj)
      return obj->QueryInterface(IID_IVideoActor, (void**)pRetVal);

   *pRetVal = NULL;

   return S_OK;
}

STDMETHODIMP FlexDMD::NewFont(BSTR font, OLE_COLOR tint, OLE_COLOR borderTint, LONG borderSize, IUnknown **pRetVal)
{
   return NewFont(MakeString(font), tint, borderTint, borderSize)->QueryInterface(IID_IUnknown, (void**)pRetVal);
}

STDMETHODIMP FlexDMD::NewUltraDMD(IUltraDMD **pRetVal)
{
   UltraDMD* obj = new UltraDMD(this);
   return obj->QueryInterface(IID_IDMDObject, (void**)pRetVal);
}

void FlexDMD::RenderLoop()
{
   DMDUtil* pDmd = DMDUtil::GetInstance();
   
   if (!pDmd->IsActive()) {
      m_pDmd = pDmd;
      m_pDmd->Register(m_width, m_height);
   }
   else {
      PLOGE.printf("Another DMD is already active, FlexDMD will not render.");
   }

   SDL_Surface* pSurface = SDL_CreateRGBSurfaceWithFormat(0, m_width, m_height, 24, SDL_PIXELFORMAT_RGB24);
   m_pGraphics = new VP::Graphics(pSurface);

   m_pStage->SetSize(m_width, m_height);
   m_pStage->SetOnStage(true);

   memset(m_segData1, 0, sizeof(m_segData1));
   memset(m_segData2, 0, sizeof(m_segData2));

   double elapsedMs = 0.0;

   while (m_run) {
      Uint64 startTime = SDL_GetTicks64();

      if (!m_renderLockCount) {
         if (m_clear) {
            m_pGraphics->SetColor(RGB(0, 0, 0));
            m_pGraphics->Clear();
         }

         m_pStage->Update((float)(elapsedMs / 1000.0));
         m_pStage->Draw(m_pGraphics);

         if (m_pDmd) {
            switch (m_renderMode) {
               case RenderMode_DMD_GRAY_2:
                  m_pDmd->SetPixelData((UINT8*)pSurface->pixels, m_dmdColor, 2);
                  break;
               case RenderMode_DMD_GRAY_4:
                  m_pDmd->SetPixelData((UINT8*)pSurface->pixels, m_dmdColor, 4);
                  break;
               case RenderMode_DMD_RGB:
                  m_pDmd->SetRGB24PixelData((UINT8*)pSurface->pixels);
                  break;
               case RenderMode_SEG_2x16Alpha:
               case RenderMode_SEG_2x20Alpha:
               case RenderMode_SEG_2x7Alpha_2x7Num:
               case RenderMode_SEG_2x7Alpha_2x7Num_4x1Num:
               case RenderMode_SEG_2x7Num_2x7Num_4x1Num:
               case RenderMode_SEG_2x7Num_2x7Num_10x1Num:
               case RenderMode_SEG_2x7Num_2x7Num_4x1Num_gen7:
               case RenderMode_SEG_2x7Num10_2x7Num10_4x1Num:
               case RenderMode_SEG_2x6Num_2x6Num_4x1Num:
               case RenderMode_SEG_2x6Num10_2x6Num10_4x1Num:
               case RenderMode_SEG_4x7Num10:
               case RenderMode_SEG_6x4Num_4x1Num:
               case RenderMode_SEG_2x7Num_4x1Num_1x16Alpha:
               case RenderMode_SEG_1x16Alpha_1x16Num_1x7Num:
                  m_pDmd->SetAlphaNumericLayout((NumericalLayout)(m_renderMode - 2));
                  m_pDmd->SetAlphaNumericData(m_segData1, m_segData2, true, m_dmdColor);
                  break;
               default: break;
            }
         }
      }

      double renderingDuration = SDL_GetTicks64() - startTime;

      int sleepMs = (1000 / m_frameRate) - (int)renderingDuration;

      if (sleepMs > 1)
         SDL_Delay(sleepMs);

      elapsedMs = SDL_GetTicks64() - startTime;

      if (elapsedMs > 4000 / m_frameRate) {
         PLOGI.printf("Abnormally long elapsed time between frames of %fs (rendering lasted %fms, sleeping was %dms), limiting to %dms", 
            elapsedMs / 1000.0, renderingDuration, std::max(0, sleepMs), 4000 / m_frameRate);

         elapsedMs = 4000 / m_frameRate;
      }
   }

   m_pStage->SetOnStage(false);

   SDL_FreeSurface(pSurface);

   delete m_pGraphics;
   m_pGraphics = NULL;

   if (m_pDmd) {
      m_pDmd->Cleanup();
      m_pDmd = NULL;
   }
}

Font* FlexDMD::NewFont(string font, OLE_COLOR tint, OLE_COLOR borderTint, LONG borderSize)
{
   AssetSrc* pAssetSrc = m_pAssetManager->ResolveSrc(
        font + "&tint=" + color_to_hex(tint) + "&border_size=" + std::to_string(borderSize) + "&border_tint=" + color_to_hex(borderTint), NULL);
   Font* pFont = m_pAssetManager->GetFont(pAssetSrc);
   delete pAssetSrc;

   return pFont;
}

AnimatedActor* FlexDMD::NewVideo(string szVideo, string szName)
{
   if (szVideo.find("|") != string::npos)
      return (AnimatedActor*)ImageSequence::Create(this, m_pAssetManager, szVideo, szName, 30, true);
   else {
      AssetSrc* pAssetSrc = m_pAssetManager->ResolveSrc(szVideo, NULL);
      AssetType assetType = pAssetSrc->GetAssetType();
      delete pAssetSrc;

      if (assetType == AssetType_Video)
         return (AnimatedActor*)Video::Create(this, szVideo, szName, true);
      else if (assetType == AssetType_GIF)
         return (AnimatedActor*)GIFImage::Create(this, m_pAssetManager, szVideo, szName);
      else if (assetType == AssetType_Image)
         return (AnimatedActor*)ImageSequence::Create(this, m_pAssetManager, szVideo, szName, 30, true);
   }

   return NULL;
}