// license:GPLv3+

#include "core/stdafx.h"
#include "../vpx-test.h"
#include "doctest.h"
#include "parts/PBRConversion.h"
#include "parts/Material.h"
#include "parts/light.h"

TEST_CASE("PBR conversion: legacy roughness to perceptual roughness")
{
   CHECK(PBRLegacyRoughnessToPerceptual(0.f) == doctest::Approx(0.84f).epsilon(0.05));
   CHECK(PBRLegacyRoughnessToPerceptual(1.f) == doctest::Approx(0.18f).epsilon(0.05));
   CHECK(PBRLegacyRoughnessToPerceptual(0.5f) < PBRLegacyRoughnessToPerceptual(0.4f)); // monotone decreasing
}

TEST_CASE("PBR conversion: material in-place mapping")
{
   Material basic;
   basic.m_type = Material::MaterialType::BASIC;
   basic.m_fRoughness = 0.5f;
   basic.m_fOpacity = 0.5f;
   PBRConvertMaterial(&basic, {});
   CHECK(basic.m_type == Material::MaterialType::PBR);
   CHECK(basic.m_fMetallic == 0.f);
   CHECK(basic.m_fOcclusion == 1.f);
   CHECK(basic.m_fEmissionIntensity == 1.f);
   CHECK(basic.m_cEmission == 0);
   CHECK(basic.m_fOpacity == 0.5f); // untouched
   CHECK(basic.m_fRoughness == doctest::Approx(0.42f).epsilon(0.05));

   Material metal;
   metal.m_type = Material::MaterialType::METAL;
   PBRConvertMaterial(&metal, {});
   CHECK(metal.m_fMetallic == 1.f);

   Material alreadyPBR;
   alreadyPBR.m_type = Material::MaterialType::PBR;
   alreadyPBR.m_fMetallic = 0.3f;
   PBRConvertMaterial(&alreadyPBR, {});
   CHECK(alreadyPBR.m_fMetallic == 0.3f); // untouched

   Material glossy;
   glossy.m_type = Material::MaterialType::BASIC;
   glossy.m_cGlossy = 0xFFFFFF;
   PBRConvertMaterialOptions opts;
   opts.deriveMetallicFromGlossy = true;
   PBRConvertMaterial(&glossy, opts);
   CHECK(glossy.m_fMetallic == doctest::Approx(1.f));
}

TEST_CASE("PBR conversion: shadow mode heuristic")
{
   CHECK(PBRSuggestShadowMode(100.f, 400.f, 50.f, 200.f) == ShadowMode::PBR_STATIC_N_BALL_SHADOWS);
   CHECK(PBRSuggestShadowMode(10.f, 100.f, 50.f, 200.f) == ShadowMode::NONE);
   CHECK(PBRSuggestShadowMode(10.f, 400.f, 50.f, 200.f) == ShadowMode::PBR_STATIC_SHADOWS);
}

TEST_CASE("PBR conversion: part capability")
{
   CHECK(PBRIsCapablePart(ItemTypeEnum::eItemGate));
   CHECK(PBRIsCapablePart(ItemTypeEnum::eItemTrigger));
   CHECK(PBRIsCapablePart(ItemTypeEnum::eItemKicker));
   CHECK(PBRIsCapablePart(ItemTypeEnum::eItemSurface));
   CHECK(PBRIsCapablePart(ItemTypeEnum::eItemBumper));
   CHECK(PBRIsCapablePart(ItemTypeEnum::eItemPrimitive));
   CHECK(PBRIsCapablePart(ItemTypeEnum::eItemRamp));
   CHECK(PBRIsCapablePart(ItemTypeEnum::eItemRubber));
   CHECK(PBRIsCapablePart(ItemTypeEnum::eItemSpinner));
   CHECK(PBRIsCapablePart(ItemTypeEnum::eItemFlipper));
   CHECK(PBRIsCapablePart(ItemTypeEnum::eItemHitTarget));
   CHECK(PBRIsCapablePart(ItemTypeEnum::eItemPlunger));
   CHECK(PBRIsCapablePart(ItemTypeEnum::eItemDecal));
   CHECK_FALSE(PBRIsCapablePart(ItemTypeEnum::eItemFlasher));
   CHECK_FALSE(PBRIsCapablePart(ItemTypeEnum::eItemLight));
   CHECK_FALSE(PBRIsCapablePart(ItemTypeEnum::eItemTimer));
   CHECK_FALSE(PBRIsCapablePart(ItemTypeEnum::eItemDispReel));
   CHECK_FALSE(PBRIsCapablePart(ItemTypeEnum::eItemLightSeq));
   CHECK_FALSE(PBRIsCapablePart(ItemTypeEnum::eItemTextbox));
   CHECK_FALSE(PBRIsCapablePart(ItemTypeEnum::eItemBall));
}
