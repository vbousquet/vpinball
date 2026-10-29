// license:GPLv3+

#pragma once

class Material;
class PinTable;
enum ItemTypeEnum : uint32_t;

enum ShadowMode : int;

struct PBRConvertMaterialOptions
{
   bool deriveMetallicFromGlossy = false; // optional spec-gloss -> metal heuristic
};

// Legacy gloss factor r (0..1) -> PBR perceptual roughness:
// legacy shader uses N = exp2(10r+1) as a specular (Blinn-Phong-like) exponent;
// GGX equivalence alpha = sqrt(2/(N+2)); perceptual roughness = sqrt(alpha) = (2/(N+2))^0.25.
float PBRLegacyRoughnessToPerceptual(float legacyRoughness);

// In-place conversion: sets m_type=PBR and fills the PBR factors; leaves all
// legacy-only fields and physics/transparency fields untouched.
void PBRConvertMaterial(Material *mat, const PBRConvertMaterialOptions &opts);

// Shadow tier suggestion from geometry (same above-glass test as the VSM pass)
ShadowMode PBRSuggestShadowMode(float lightZ, float falloff, float glassTop, float insertRange);

// True if this part type shades through the PBR path for PBR materials (the 13 GetPBRTextureSet callers)
bool PBRIsCapablePart(ItemTypeEnum type);
