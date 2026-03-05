// license:GPLv3+

#pragma once

#include "nlohmann/json.hpp"
#include "fileio.h"

class JSONSerializer
{
public:
   class Serializer
   {
   public:
      virtual ~Serializer() = default;
      virtual void AddTextFile(const std::filesystem::path& path, const std::string& content) = 0;
      virtual void AddBinaryFile(const std::filesystem::path& path, const std::vector<uint8_t>& data) = 0;
   };

   static std::unique_ptr<Serializer> Create(const std::filesystem::path& path);

   static const string& GetFieldName(int fieldId);
   static int GetFieldId(const string& filedName);
};

class JSONObjectWriter final :
   public JSONSerializer,
   public IObjectWriter
{
public:
   explicit JSONObjectWriter(Serializer& serializer, const std::filesystem::path& path);
   explicit JSONObjectWriter(nlohmann::ordered_json &array);
   bool HasError() const override { return m_hasError; }

   void BeginObject(int objectId, bool isArray, bool isSkippable) override;
   void WriteBool(int id, bool value) override;
   void WriteInt(int id, int value) override;
   void WriteUInt(int id, unsigned int value) override;
   void WriteFloat(int id, float value) override;
   void WriteString(int id, const string& value) override;
   void WriteWideString(int id, const wstring& value) override;
   void WriteVector2(int id, const Vertex2D& value) override;
   void WriteVector3(int id, const vec3& value) override;
   void WriteVector4(int id, const vec4& value) override;
   void WriteScript(int fieldId, const string &value) override;
   void WriteFontDescriptor(int fieldId, const FontDesc &value) override;
   void WriteRaw(const int id, const void* pvalue, const int size) override;
   void EndObject() override;

private:
   nlohmann::ordered_json m_json;
   std::optional<std::reference_wrapper<nlohmann::ordered_json>> m_array;
   bool m_hasError = false;
};
