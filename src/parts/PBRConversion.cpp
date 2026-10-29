// license:GPLv3+

#include "core/stdafx.h"

#include "PBRConversion.h"
#include "Material.h"
#include "light.h"
#include "core/ieditable.h"

float PBRLegacyRoughnessToPerceptual(const float legacyRoughness)
{
   const float N = exp2f(10.f * legacyRoughness + 1.f);
   return powf(2.f / (N + 2.f), 0.25f);
}

void PBRConvertMaterial(Material *mat, const PBRConvertMaterialOptions &opts)
{
   if (mat->m_type == Material::MaterialType::PBR)
      return;
   const bool wasMetal = mat->m_type == Material::MaterialType::METAL;
   mat->m_type = Material::MaterialType::PBR;
   mat->m_fRoughness = PBRLegacyRoughnessToPerceptual(mat->m_fRoughness);
   mat->m_fMetallic = wasMetal ? 1.f
      : opts.deriveMetallicFromGlossy
         ? clamp((0.299f * GetRValue(mat->m_cGlossy) + 0.587f * GetGValue(mat->m_cGlossy) + 0.114f * GetBValue(mat->m_cGlossy)) / 255.f, 0.f, 1.f)
         : 0.f;
   mat->m_fOcclusion = 1.f;
   mat->m_cEmission = 0;
   mat->m_fEmissionIntensity = 1.f;
}

ShadowMode PBRSuggestShadowMode(const float lightZ, const float falloff, const float glassTop, const float insertRange)
{
   if (lightZ > glassTop)
      return ShadowMode::PBR_STATIC_N_BALL_SHADOWS; // scene light
   if (falloff < insertRange)
      return ShadowMode::NONE; // insert-like, too many lights
   return ShadowMode::PBR_STATIC_SHADOWS; // GI / near light
}

bool PBRIsCapablePart(const ItemTypeEnum type)
{
   switch (type)
   {
   case ItemTypeEnum::eItemBumper:
   case ItemTypeEnum::eItemDecal:
   case ItemTypeEnum::eItemFlipper:
   case ItemTypeEnum::eItemGate:
   case ItemTypeEnum::eItemHitTarget:
   case ItemTypeEnum::eItemKicker:
   case ItemTypeEnum::eItemPlunger:
   case ItemTypeEnum::eItemPrimitive:
   case ItemTypeEnum::eItemRamp:
   case ItemTypeEnum::eItemRubber:
   case ItemTypeEnum::eItemSpinner:
   case ItemTypeEnum::eItemSurface:
   case ItemTypeEnum::eItemTrigger: return true;
   default: return false;
   }
}
