// license:GPLv3+

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Minimal parser for Unity's machine-generated serialization YAML (.meta, .mat, .prefab, .asset).
// Supports the subset Unity emits: multi-document files with "--- !u!<classId> &<fileId> [stripped]"
// headers, block mappings/lists, flow mappings/sequences and plain/quoted scalars. Anchors, aliases,
// multiline scalars and tags other than the document ones are intentionally unsupported.
namespace UnityYaml
{

using std::map;
using std::string;
using std::vector;

struct Value;

struct Value
{
   enum class Type
   {
      Null,
      Scalar, // raw text (numbers/bools are parsed on demand)
      Map,
      List
   };
   Type type = Type::Null;
   string scalar;
   map<string, Value> map;
   vector<Value> list;

   bool IsNull() const { return type == Type::Null; }
   bool IsScalar() const { return type == Type::Scalar; }
   bool IsMap() const { return type == Type::Map; }
   bool IsList() const { return type == Type::List; }
   size_t Size() const { return type == Type::List ? list.size() : 0; }

   const Value& operator[](const string& key) const;
   const Value& At(size_t i) const;

   double AsFloat(double def = 0.0) const;
   int64_t AsInt(int64_t def = 0) const;
   string AsString(const string& def = string()) const;
   bool AsBool(bool def = false) const;
};

// {fileID: <i64>, guid: <hex>, type: <i>} object reference
struct Ref
{
   int64_t fileID = 0;
   string guid;
   int type = 0;
};

struct Document
{
   int classId = 0; // !u!N class id (e.g. 1=GameObject, 4=Transform, 1001=PrefabInstance); 0 for tagless files (.meta)
   int64_t fileId = 0; // &anchor
   bool stripped = false;
   string rootName; // top-level key ("GameObject", "Material", ...) when present
   Value root; // document content (the mapping under the root key when there is one)
};

// Parses a Unity text serialization file. Throws std::runtime_error with a line number on malformed input.
vector<Document> Parse(const string& text);

// Extracts the guid of a .meta file (empty when absent).
string MetaGuid(const string& text);

// Helpers for common value shapes (return false/default on mismatch)
bool AsRef(const Value& v, Ref& out);
bool AsVec3(const Value& v, float& x, float& y, float& z);
bool AsVec4(const Value& v, float& x, float& y, float& z, float& w);
bool AsColor(const Value& v, float& r, float& g, float& b, float& a);
// Reads a float member of a flow map, e.g. v["m_LocalPosition"]["x"]
float MemberFloat(const Value& v, const string& key, float def = 0.f);

} // namespace UnityYaml
