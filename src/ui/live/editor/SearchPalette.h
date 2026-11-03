// license:GPLv3+

#pragma once

#include "PinnedPanel.h"

#include <unordered_map>

namespace VPX::EditorUI
{

class EditorUI;

// Cmd/Ctrl+K palette: type part of an object name and/or a field label, Enter selects the object and scrolls the properties
// panel to the field (flashing it), Cmd/Ctrl+Enter pins the field to the overlay. The index is the pin field registry: field
// labels per object type, learned from harvests of the objects' panes.
class SearchPalette
{
public:
   explicit SearchPalette(EditorUI &editor);
   ~SearchPalette();

   void Open();
   void Close() { m_open = false; }
   void Toggle() { m_open ? Close() : Open(); }
   bool IsOpen() const { return m_open; }

   void Render();

private:
   struct Field
   {
      string label, group;
      string labelLower, groupLower;
   };
   // Object type (table, material or part item type) with the fields learned from the pin registry
   struct TypeInfo
   {
      string name, lower;
      vector<Field> fields;
      bool queued = false; // A representative object was queued for harvest
   };
   struct Object
   {
      PinKind kind;
      string name; // Registry name (empty for the table)
      string display, lower;
      int type;
   };
   struct Result
   {
      int obj; // Index in m_objects (-1 for operators)
      int field; // Index in the type's fields, -1: object-only or recent field not in the catalog
      int score;
      bool isField;
      string prop; // Field label (field rows)
      string label, group; // Cached display strings
      string right;
      int op = -1; // Index in m_ops
   };
   // Menu command (Blender's menu search); path is the full menu path, its last component is the label
   struct Operator
   {
      string path, label, shortcut;
      string labelLower, pathLower;
      std::function<bool()> enabled;
      std::function<void()> action;
   };
   struct Recent
   {
      PinKind kind;
      string name;
      string prop;
      bool isField;
      string opId; // Operator path (operator entries)
   };
   struct Candidate
   {
      int obj, field, score;
      int op = -1;
   };

   void BuildOperators();
   void BuildIndex();
   int TypeIndex(const string &key, const string &name);
   void RefreshCatalog();
   void QueueMissingTypes();
   void Harvest();
   void Recompute();
   void MakeResult(Result &r) const;
   void Activate(const Result &r, bool pin);
   void Remember(const Result &r);

   EditorUI &m_editor;
   bool m_open = false;
   bool m_focusInput = false;
   bool m_firstFrame = false;
   bool m_indexStale = true;
   bool m_dirty = true;
   bool m_harvestSelection = false;
   bool m_scrollToSel = false;
   bool m_hasPending = false;
   bool m_pendingPin = false;
   size_t m_pending = 0;
   int m_sel = 0;
   size_t m_indexParts = 0, m_indexMats = 0;
   string m_query;
   string m_lastQuery;

   vector<TypeInfo> m_types;
   std::unordered_map<string, int> m_typeByKey;
   std::unordered_map<string, int> m_partType; // Part name -> type
   int m_tableType = -1, m_matType = -1;
   vector<Object> m_objects;
   vector<Operator> m_ops;
   struct QueueItem
   {
      PinKind kind;
      string name;
   };
   vector<QueueItem> m_queue;
   vector<Result> m_results;
   vector<Recent> m_recent;
};

}
