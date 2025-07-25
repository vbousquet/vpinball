#include "common.h"

#include <algorithm>
#include <filesystem>
#include <charconv>
#if defined(__APPLE__) || defined(__linux__) || defined(__ANDROID__)
#include <pthread.h>
#endif

namespace PUP {

string trim_string(const string& str)
{
   size_t start = 0;
   size_t end = str.length();
   while (start < end && (str[start] == ' ' || str[start] == '\t' || str[start] == '\r' || str[start] == '\n'))
      ++start;
   while (end > start && (str[end - 1] == ' ' || str[end - 1] == '\t' || str[end - 1] == '\r' || str[end - 1] == '\n'))
      --end;
   return str.substr(start, end - start);
}

static bool try_parse_int(const string& str, int& value)
{
   const string tmp = trim_string(str);
   return (std::from_chars(tmp.c_str(), tmp.c_str() + tmp.length(), value).ec == std::errc{});
}

static bool try_parse_float(const string& str, float& value)
{
   const string tmp = trim_string(str);
#if defined(__clang__)
   const char* const p = tmp.c_str();
   char* e;
   value = std::strtof(p, &e);
   return (p != e);
#else
   return (std::from_chars(tmp.c_str(), tmp.c_str() + tmp.length(), value).ec == std::errc{});
#endif
}

int string_to_int(const string& str, int default_value)
{
   int value;
   return try_parse_int(str, value) ? value : default_value;
}

float string_to_float(const string& str, float default_value)
{
   float value;
   return try_parse_float(str, value) ? value : default_value;
}

vector<string> parse_csv_line(const string& line)
{
   vector<string> parts;
   string field;
   enum State { Normal, Quoted };
   State currentState = Normal;

   for (char c : trim_string(line)) {
      switch (currentState) {
         case Normal:
            if (c == '"') {
               currentState = Quoted;
            } else if (c == ',') {
               parts.push_back(field);
               field.clear();
            } else {
               field += c;
            }
            break;
         case Quoted:
            if (c == '"') {
               currentState = Normal;
            } else {
               field += c;
            }
            break;
      }
   }

   parts.push_back(field);

   return parts;
}

string string_replace_all(const string& szStr, const string& szFrom, const string& szTo, const size_t offs)
{
   size_t startPos = szStr.find(szFrom, offs);
   if (startPos == string::npos)
      return szStr;

   string szNewStr = szStr;
   szNewStr.replace(startPos, szFrom.length(), szTo);
   return string_replace_all(szNewStr, szFrom, szTo, startPos+szTo.length());
}

constexpr inline char cLower(char c)
{
   if (c >= 'A' && c <= 'Z')
      c ^= 32; //ASCII convention
   return c;
}

CONSTEXPR inline void StrToLower(string& str) {
   std::ranges::transform(str.begin(), str.end(), str.begin(), cLower);
}

string lowerCase(string input)
{
   StrToLower(input);
   return input;
}

string extension_from_path(const string& path)
{
   const size_t pos = path.find_last_of('.');
   return pos != string::npos ? lowerCase(path.substr(pos + 1)) : string();
}

string normalize_path_separators(const string& szPath)
{
   string szResult = szPath;

   if (PATH_SEPARATOR_CHAR == '/')
      std::ranges::replace(szResult.begin(), szResult.end(), '\\', PATH_SEPARATOR_CHAR);
   else
      std::ranges::replace(szResult.begin(), szResult.end(), '/', PATH_SEPARATOR_CHAR);

   auto end = std::unique(szResult.begin(), szResult.end(),
      [](char a, char b) { return a == b && a == PATH_SEPARATOR_CHAR; });
   szResult.erase(end, szResult.end());

   return szResult;
}

string find_case_insensitive_file_path(const string& szPath)
{
   auto fn = [&](auto& self, const string& s) -> string {
      string path = normalize_path_separators(s);
      std::filesystem::path p = std::filesystem::path(path).lexically_normal();
      std::error_code ec;

      if (std::filesystem::exists(p, ec))
         return p.string();

      auto parent = p.parent_path();
      string base;
      if (parent.empty() || parent == p) {
         base = ".";
      } else {
         base = self(self, parent.string());
         if (base.empty())
            return string();
      }

      for (auto& ent : std::filesystem::directory_iterator(base, ec)) {
         if (!ec && StrCompareNoCase(ent.path().filename().string(), p.filename().string())) {
            auto found = ent.path().string();
            if (found != path) {
               LOGI("case insensitive file match: requested \"%s\", actual \"%s\"", path.c_str(), found.c_str());
            }
            return found;
         }
      }

      return string();
   };

   string result = fn(fn, szPath);
   if (!result.empty()) {
      std::filesystem::path p = std::filesystem::absolute(result);
      return p.string();
   }
   return string();
}

string find_case_insensitive_directory_path(const string& szPath)
{
   auto fn = [&](auto& self, const string& s) -> string {
      string path = normalize_path_separators(s);
      std::filesystem::path p = std::filesystem::path(path).lexically_normal();
      std::error_code ec;

      if (std::filesystem::exists(p, ec) && std::filesystem::is_directory(p, ec)) {
         string exact = p.string();
         if (!exact.empty() && exact.back() != PATH_SEPARATOR_CHAR)
            exact.push_back(PATH_SEPARATOR_CHAR);
         return exact;
      }

      auto parent = p.parent_path();
      string base;
      if (parent.empty() || parent == p) {
         base = ".";
      } else {
         base = self(self, parent.string());
         if (base.empty())
            return string();
      }

      for (auto& ent : std::filesystem::directory_iterator(base, ec)) {
         if (ec || !ent.is_directory(ec))
            continue;
         if (StrCompareNoCase(ent.path().filename().string(), p.filename().string())) {
            string found = ent.path().string();
            if (!found.empty() && found.back() != PATH_SEPARATOR_CHAR)
               found.push_back(PATH_SEPARATOR_CHAR);
            if (found != path) {
               LOGI("case insensitive directory match: requested \"%s\", actual \"%s\"", path.c_str(), found.c_str());
            }
            return found;
         }
      }

      return string();
   };

   string result = fn(fn, szPath);
   if (!result.empty()) {
      std::filesystem::path p = std::filesystem::absolute(result);
      string exact = p.string();
      if (!exact.empty() && exact.back() != PATH_SEPARATOR_CHAR)
         exact.push_back(PATH_SEPARATOR_CHAR);
      return exact;
   }
   return string();
}

bool StrCompareNoCase(const string& strA, const string& strB)
{
   return strA.length() == strB.length()
      && std::equal(strA.begin(), strA.end(), strB.begin(),
         [](char a, char b) { return cLower(a) == cLower(b); });
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <locale>
void SetThreadName(const string& name)
{
   const int size_needed = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
   if (size_needed <= 1)
      return;
   std::wstring wstr(size_needed - 1, L'\0');
   if (MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wstr.data(), size_needed) == 0)
      return;
   HRESULT hr = SetThreadDescription(GetCurrentThread(), wstr.c_str());
}
#else
void SetThreadName(const string& name)
{
#ifdef __APPLE__
   pthread_setname_np(name.c_str());
#elif defined(__linux__) || defined(__ANDROID__)
   pthread_setname_np(pthread_self(), name.c_str());
#endif
}
#endif

}
