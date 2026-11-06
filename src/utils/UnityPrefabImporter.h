// license:GPLv3+

#pragma once

// Imports Unity .prefab files (e.g. from the VisualPinball.Unity.Assets library) into a PinTable:
// each renderable GameObject becomes a Primitive, each HDRP Lit .mat becomes a Material of type PBR,
// and the referenced textures (base/normal/mask->ORM/emission) are imported as images.
struct UnityImportResult
{
   bool success = false;
   string error; // hard failure reason when success is false
   int parts = 0;
   int partGroups = 0;
   int materials = 0;
   int images = 0;
   vector<string> warnings;
};

// Imports the .prefab file at 'path' into 'table'. The created parts are appended to 'createdParts'
// (for undo/selection bookkeeping); materials and images are added to the table directly.
// 'targetGroup' is the fallback group for imported top-level parts (may be nullptr).
// Must be called on the main thread, only when the table is editable.
UnityImportResult UnityImportPrefab(PinTable *table, const std::filesystem::path &path, PartGroup *targetGroup, vector<IEditable *> &createdParts);
