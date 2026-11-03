// license:GPLv3+

#include "core/stdafx.h"
#include "UnityYaml.h"

#include <charconv>
#include <cstdlib>
#include <string>
#include <stdexcept>

namespace UnityYaml
{

namespace
{

   const Value s_null;

   struct Line
   {
      int indent = 0;
      string text; // without indentation, trailing \r removed
      int number = 0;
   };

   [[noreturn]] void Fail(int line, const string& what) { throw std::runtime_error("UnityYaml: line " + std::to_string(line) + ": " + what); }

   // Splits "key: rest" at the first ':' followed by space/end. Returns the key (unquoted) and the rest (trimmed, may be empty).
   void SplitKey(const Line& line, string& key, string& rest)
   {
      const string& t = line.text;
      const size_t colon = t.find(':');
      if (colon == string::npos || (colon + 1 < t.size() && t[colon + 1] != ' '))
         Fail(line.number, "expected 'key: value', got '" + t + "'");
      key = t.substr(0, colon);
      if (!key.empty() && key.front() == '"' && key.back() == '"')
         key = key.substr(1, key.size() - 2);
      rest = t.substr(colon + 1);
      const size_t start = rest.find_first_not_of(' ');
      rest = (start == string::npos) ? string() : rest.substr(start);
      if (!rest.empty() && rest.back() == '\r')
         rest.pop_back();
   }

   string Unquote(const string& s)
   {
      if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
      {
         string out;
         out.reserve(s.size() - 2);
         for (size_t i = 1; i + 1 < s.size(); i++)
         {
            if (s[i] == '\\' && i + 2 < s.size())
               out += s[++i];
            else
               out += s[i];
         }
         return out;
      }
      if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'')
         return s.substr(1, s.size() - 2);
      return s;
   }

   // ---- flow collections: {k: v, ...} and [v, ...] ---------------------------------------------

   struct FlowCursor
   {
      const string& s;
      size_t pos = 0;
      int line;
      void SkipWs()
      {
         while (pos < s.size() && (s[pos] == ' '))
            pos++;
      }
      char Peek()
      {
         SkipWs();
         return pos < s.size() ? s[pos] : '\0';
      }
      bool Consume(char c)
      {
         if (Peek() == c)
         {
            pos++;
            return true;
         }
         return false;
      }
      void Expect(char c)
      {
         if (!Consume(c))
            Fail(line, string("expected '") + c + "' in flow collection '" + s + "'");
      }
   };

   Value ParseFlowValue(FlowCursor& c);

   Value ParseFlowMap(FlowCursor& c)
   {
      Value v;
      v.type = Value::Type::Map;
      c.Expect('{');
      if (c.Consume('}'))
         return v;
      for (;;)
      {
         Value key = ParseFlowValue(c);
         c.Expect(':');
         v.map[key.scalar] = ParseFlowValue(c);
         if (c.Consume(','))
            continue;
         c.Expect('}');
         return v;
      }
   }

   Value ParseFlowList(FlowCursor& c)
   {
      Value v;
      v.type = Value::Type::List;
      c.Expect('[');
      if (c.Consume(']'))
         return v;
      for (;;)
      {
         v.list.push_back(ParseFlowValue(c));
         if (c.Consume(','))
            continue;
         c.Expect(']');
         return v;
      }
   }

   // A flow scalar ends at , } ] or : (when followed by space or end) at the same nesting level.
   Value ParseFlowScalar(FlowCursor& c)
   {
      Value v;
      v.type = Value::Type::Scalar;
      const char open = c.Peek();
      if (open == '"' || open == '\'')
      {
         const char quote = open;
         c.pos++;
         size_t start = c.pos;
         while (c.pos < c.s.size() && c.s[c.pos] != quote)
            c.pos += (c.s[c.pos] == '\\' && quote == '"') ? 2 : 1;
         v.scalar = (quote == '"') ? Unquote('"' + c.s.substr(start, c.pos - start) + '"') : c.s.substr(start, c.pos - start);
         c.pos++;
         return v;
      }
      const size_t start = c.pos;
      while (c.pos < c.s.size())
      {
         const char ch = c.s[c.pos];
         if (ch == ',' || ch == '}' || ch == ']')
            break;
         if (ch == ':' && (c.pos + 1 >= c.s.size() || c.s[c.pos + 1] == ' '))
            break;
         c.pos++;
      }
      size_t end = c.pos;
      while (end > start && c.s[end - 1] == ' ')
         end--;
      v.scalar = c.s.substr(start, end - start);
      return v;
   }

   Value ParseFlowValue(FlowCursor& c)
   {
      const char ch = c.Peek();
      if (ch == '{')
         return ParseFlowMap(c);
      if (ch == '[')
         return ParseFlowList(c);
      return ParseFlowScalar(c);
   }

   Value ParseInline(const string& text, int line)
   {
      FlowCursor c { text, 0, line };
      Value v = ParseFlowValue(c);
      return v;
   }

   // ---- block parser --------------------------------------------------------------------------

   struct BlockCursor
   {
      const vector<Line>& lines;
      size_t pos = 0;
   };

   void ParseMap(BlockCursor& c, int indent, Value& out);
   void ParseList(BlockCursor& c, int indent, Value& out);

   bool ConsumeDocHeader(const string& text, int& classId, int64_t& fileId, bool& stripped)
   {
      // "--- !u!<classId> &<fileId> [stripped]"
      if (!text.starts_with("---"))
         return false;
      size_t p = text.find("!u!", 3);
      if (p == string::npos)
         return text.find("---") == 0; // plain "---" doc separator
      p += 3;
      classId = atoi(text.c_str() + p);
      const size_t amp = text.find('&', p);
      if (amp != string::npos)
         fileId = strtoll(text.c_str() + amp + 1, nullptr, 10);
      stripped = text.find("stripped") != string::npos;
      return true;
   }

   // Parses a mapping starting at indent level. Stops on dedent or list item.
   void ParseMap(BlockCursor& c, int indent, Value& out)
   {
      out.type = Value::Type::Map;
      while (c.pos < c.lines.size())
      {
         const Line& line = c.lines[c.pos];
         if (line.indent < indent || (line.indent == indent && line.text.starts_with("- ")))
            return;
         if (line.indent > indent)
            Fail(line.number, "unexpected indentation in '" + line.text + "'");
         string key, rest;
         SplitKey(line, key, rest);
         c.pos++;
         Value& child = out.map[key];
         if (!rest.empty())
            child = ParseInline(rest, line.number);
         else if (c.pos < c.lines.size() && (c.lines[c.pos].indent > indent || (c.lines[c.pos].indent == indent && c.lines[c.pos].text.starts_with("- "))))
         {
            // Nested block (deeper indent) or a list at the same indent as its parent key (Unity style)
            if (c.lines[c.pos].text.starts_with("- "))
               ParseList(c, c.lines[c.pos].indent, child);
            else
               ParseMap(c, c.lines[c.pos].indent, child);
         }
         else
            child.type = Value::Type::Null;
      }
   }

   // Parses a list ("- item") starting at indent level.
   void ParseList(BlockCursor& c, int indent, Value& out)
   {
      out.type = Value::Type::List;
      while (c.pos < c.lines.size())
      {
         const Line& line = c.lines[c.pos];
         if (line.indent != indent || !line.text.starts_with("- "))
            return;
         const string item = line.text.substr(2);
         c.pos++;
         // "- key: value" begins a map item whose following keys live at deeper indent.
         // The ':' must be at flow depth 0 ('- {fileID: 1}' is a single flow-map item).
         size_t colon = string::npos;
         int depth = 0;
         for (size_t i = 0; i < item.size(); i++)
         {
            const char ch = item[i];
            if (ch == '{' || ch == '[')
               depth++;
            else if (ch == '}' || ch == ']')
               depth--;
            else if (ch == ':' && depth == 0 && (i + 1 >= item.size() || item[i + 1] == ' '))
            {
               colon = i;
               break;
            }
         }
         if (colon != string::npos && (colon + 1 >= item.size() || item[colon + 1] == ' '))
         {
            Value& elem = out.list.emplace_back();
            elem.type = Value::Type::Map;
            Line first = line;
            first.text = item;
            string key, rest;
            SplitKey(first, key, rest);
            Value& child = elem.map[key];
            if (!rest.empty())
               child = ParseInline(rest, line.number);
            else if (c.pos < c.lines.size() && c.lines[c.pos].indent > indent)
            {
               if (c.lines[c.pos].text.starts_with("- "))
                  ParseList(c, c.lines[c.pos].indent, child);
               else
                  ParseMap(c, c.lines[c.pos].indent, child);
            }
            // sibling keys of the same map item (indent > list indent, e.g. "- a: 1\n  b: 2")
            while (c.pos < c.lines.size() && c.lines[c.pos].indent > indent && !c.lines[c.pos].text.starts_with("- "))
            {
               const Line& kl = c.lines[c.pos];
               string k2, r2;
               SplitKey(kl, k2, r2);
               c.pos++;
               if (!r2.empty())
                  elem.map[k2] = ParseInline(r2, kl.number);
               else if (c.pos < c.lines.size() && c.lines[c.pos].indent > kl.indent)
               {
                  if (c.lines[c.pos].text.starts_with("- "))
                     ParseList(c, c.lines[c.pos].indent, elem.map[k2]);
                  else
                     ParseMap(c, c.lines[c.pos].indent, elem.map[k2]);
               }
               else
                  elem.map[k2].type = Value::Type::Null;
            }
         }
         else
         {
            out.list.push_back(ParseInline(item, line.number));
         }
      }
   }

   vector<Line> MakeLines(const string& text)
   {
      vector<Line> lines;
      size_t pos = 0;
      int number = 0;
      while (pos <= text.size())
      {
         const size_t eol = text.find('\n', pos);
         const size_t end = (eol == string::npos) ? text.size() : eol;
         number++;
         string t = text.substr(pos, end - pos);
         pos = end + 1;
         if (!t.empty() && t.back() == '\r')
            t.pop_back();
         size_t indent = 0;
         while (indent < t.size() && t[indent] == ' ')
            indent++;
         if (indent < t.size() && t[indent] == '\t')
            Fail(number, "tab indentation is not supported");
         string body = t.substr(indent);
         if (body.empty() || body[0] == '#')
            continue; // blank line / comment
         lines.push_back(Line { static_cast<int>(indent), body, number });
         if (eol == string::npos)
            break;
      }
      return lines;
   }

   void ParseDocBody(const vector<Line>& lines, Document& doc)
   {
      if (lines.empty())
      {
         doc.root.type = Value::Type::Null;
         return;
      }
      BlockCursor c { lines, 0 };
      const Line& first = lines[0];
      string key, rest;
      SplitKey(first, key, rest);
      if (rest.empty() && lines.size() > 1 && lines[1].indent > first.indent && !first.text.starts_with("- "))
      {
         // Single root key ("GameObject:", ...) wrapping the whole document
         doc.rootName = key;
         c.pos = 1; // skip the root key line itself
         if (lines[1].text.starts_with("- "))
            ParseList(c, lines[1].indent, doc.root);
         else
            ParseMap(c, lines[1].indent, doc.root);
         if (c.pos != lines.size())
            Fail(lines[c.pos].number, "unexpected content after document root");
      }
      else
      {
         ParseMap(c, first.indent, doc.root);
         if (c.pos != lines.size())
            Fail(lines[c.pos].number, "unexpected content after document root");
      }
   }

} // anonymous namespace

vector<Document> Parse(const string& text)
{
   const vector<Line> lines = MakeLines(text);
   vector<Document> docs;
   vector<Line> body;
   Document current;
   bool inDoc = false;
   const auto flush = [&]()
   {
      if (!inDoc)
         return;
      ParseDocBody(body, current);
      docs.push_back(std::move(current));
      current = Document {};
      body.clear();
      inDoc = false;
   };
   for (const Line& line : lines)
   {
      if (line.indent == 0 && line.text[0] == '%')
         continue; // %YAML / %TAG directives
      if (line.indent == 0)
      {
         int classId = 0;
         int64_t fileId = 0;
         bool stripped = false;
         if (line.text.starts_with("---") && ConsumeDocHeader(line.text, classId, fileId, stripped))
         {
            flush();
            current.classId = classId;
            current.fileId = fileId;
            current.stripped = stripped;
            inDoc = true;
            continue;
         }
      }
      body.push_back(line);
   }
   // tagless file (.meta): treat as a single document
   if (docs.empty() && !body.empty())
   {
      current.classId = 0;
      ParseDocBody(body, current);
      docs.push_back(std::move(current));
      return docs;
   }
   flush();
   return docs;
}

string MetaGuid(const string& text)
{
   for (size_t pos = 0; pos < text.size();)
   {
      const size_t eol = text.find('\n', pos);
      const string line = text.substr(pos, (eol == string::npos ? text.size() : eol) - pos);
      if (line.starts_with("guid:"))
      {
         string g = line.substr(5);
         const size_t a = g.find_first_not_of(' ');
         const size_t b = g.find_last_not_of(" \r");
         return (a == string::npos) ? string() : g.substr(a, b - a + 1);
      }
      if (eol == string::npos)
         break;
      pos = eol + 1;
   }
   return string();
}

const Value& Value::operator[](const string& key) const
{
   if (type != Type::Map)
      return s_null;
   const auto it = map.find(key);
   return it == map.end() ? s_null : it->second;
}

const Value& Value::At(size_t i) const
{
   if (type != Type::List || i >= list.size())
      return s_null;
   return list[i];
}

double Value::AsFloat(double def) const
{
   if (type != Type::Scalar)
      return def;
#if defined(__clang__)
   char* end;
   const double out = std::strtod(scalar.data(), &end); // no float from_chars in libc++
   return (end != scalar.data()) ? out : def;
#else
   double out;
   const char* b = scalar.data();
   const char* e = b + scalar.size();
   if (std::from_chars(b, e, out).ec == std::errc())
      return out;
   return def;
#endif
}

int64_t Value::AsInt(int64_t def) const
{
   if (type != Type::Scalar)
      return def;
   int64_t out;
   const char* b = scalar.data();
   const char* e = b + scalar.size();
   if (std::from_chars(b, e, out).ec == std::errc())
      return out;
   // Unity writes some ids/floats in exponent form; fall back to double
   return static_cast<int64_t>(AsFloat(static_cast<double>(def)));
}

string Value::AsString(const string& def) const { return (type == Type::Scalar) ? scalar : def; }

bool Value::AsBool(bool def) const
{
   if (type != Type::Scalar)
      return def;
   if (scalar == "1" || scalar == "true" || scalar == "True")
      return true;
   if (scalar == "0" || scalar == "false" || scalar == "False")
      return false;
   return def;
}

bool AsRef(const Value& v, Ref& out)
{
   if (!v.IsMap())
      return false;
   out.fileID = v["fileID"].AsInt();
   out.guid = v["guid"].AsString();
   out.type = static_cast<int>(v["type"].AsInt());
   return true;
}

bool AsVec3(const Value& v, float& x, float& y, float& z)
{
   if (!v.IsMap())
      return false;
   x = static_cast<float>(v["x"].AsFloat());
   y = static_cast<float>(v["y"].AsFloat());
   z = static_cast<float>(v["z"].AsFloat());
   return true;
}

bool AsVec4(const Value& v, float& x, float& y, float& z, float& w)
{
   if (!v.IsMap())
      return false;
   x = static_cast<float>(v["x"].AsFloat());
   y = static_cast<float>(v["y"].AsFloat());
   z = static_cast<float>(v["z"].AsFloat());
   w = static_cast<float>(v["w"].AsFloat());
   return true;
}

bool AsColor(const Value& v, float& r, float& g, float& b, float& a)
{
   if (!v.IsMap())
      return false;
   r = static_cast<float>(v["r"].AsFloat());
   g = static_cast<float>(v["g"].AsFloat());
   b = static_cast<float>(v["b"].AsFloat());
   a = static_cast<float>(v["a"].AsFloat(1.0));
   return true;
}

float MemberFloat(const Value& v, const string& key, float def) { return static_cast<float>(v[key].AsFloat(def)); }

} // namespace UnityYaml
