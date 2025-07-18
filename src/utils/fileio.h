#pragma once
#define FID(A) (int)((unsigned int)(#A[0])|((unsigned int)(#A[1])<<8)|((unsigned int)(#A[2])<<16)|((unsigned int)(#A[3])<<24))

bool DirExists(const string& dirPath);
bool FileExists(const string& filePath);
string TitleFromFilename(const string& filename);
string ExtensionFromFilename(const string& filename);
string PathFromFilename(const string& filename);
string TitleAndPathFromFilename(const string& filename);
bool ReplaceExtensionFromFilename(string& filename, const string& newextension);

class BiffReader;

class ILoadable
{
public:
   virtual bool LoadToken(const int id, BiffReader * const pbr) = 0;
};

class BiffWriter final
{
public:
   BiffWriter(IStream *pistream, const HCRYPTHASH hcrypthash);

   HRESULT WriteInt(const int id, const int value);
   HRESULT WriteString(const int id, const char * const szvalue);
   HRESULT WriteString(const int id, const string& szvalue);

   HRESULT WriteWideString(const int id, const WCHAR * const wzvalue);
   HRESULT WriteWideString(const int id, const std::basic_string<WCHAR>& wzvalue);
   HRESULT WriteBool(const int id, const BOOL value);
   HRESULT WriteFloat(const int id, const float value);
   HRESULT WriteStruct(const int id, const void * const pvalue, const int size);
   HRESULT WriteVector2(const int id, const Vertex2D& vec);
   HRESULT WriteVector3(const int id, const Vertex3Ds& vec);
   HRESULT WriteVector3Padded(const int id, const Vertex3Ds& vec);
   HRESULT WriteTag(const int id);

   HRESULT WriteBytes(const void *pv, const ULONG count, ULONG *foo);

   HRESULT WriteRecordSize(const int size);

   IStream *m_pistream;
   HCRYPTHASH m_hcrypthash;
};

class BiffReader final
{
public:
   BiffReader(IStream *pistream, ILoadable *piloadable, void *ppassdata, const int version, const HCRYPTHASH hcrypthash, const HCRYPTKEY hcryptkey);

   int GetBytesInRecordRemaining() const { return m_bytesinrecordremaining; }

   HRESULT GetIntNoHash(int &value);
   HRESULT GetInt(void * const value);
   HRESULT GetInt(int &value);
   HRESULT GetInt(uint32_t &value)
   {
      int val;
      const HRESULT hr = GetInt(val);
      value = val;
      return hr;
   }
#ifndef __STANDALONE__
   HRESULT GetInt(COLORREF &value)
   {
      int val;
      const HRESULT hr = GetInt(val);
      value = val;
      return hr;
   }
#endif
   HRESULT GetString(char * const szvalue, const size_t szvalue_maxlength);
   HRESULT GetString(string& szvalue);
   HRESULT GetWideString(WCHAR* wzvalue, const size_t wzvalue_maxlength);
   HRESULT GetWideString(std::basic_string<WCHAR>& wzvalue);
   HRESULT GetFloat(float &value);
   HRESULT GetBool(BOOL &value);
   HRESULT GetBool(bool &value)
   {
      BOOL val;
      const HRESULT hr = GetBool(val);
      value = !!val;
      return hr;
   }
   HRESULT GetStruct(void *pvalue, const int size);
   HRESULT GetVector2(Vertex2D& vec);
   HRESULT GetVector3(Vertex3Ds& vec);
   HRESULT GetVector3Padded(Vertex3Ds& vec);

   HRESULT ReadBytes(void * const pv, const uint32_t count);

   HRESULT Load(std::function<bool(const int id, BiffReader *const pbr)> processToken = nullptr);

   IStream *m_pistream;
   void *m_pdata;
   int m_version;

   HCRYPTHASH m_hcrypthash;
   HCRYPTKEY m_hcryptkey;

private:
   ILoadable *m_piloadable;
   int m_bytesinrecordremaining;
};

class FastIStream;

class FastIStorage : public IStorage
{
public:
   FastIStorage();
   virtual ~FastIStorage();

   HRESULT __stdcall QueryInterface(const struct _GUID &, void **) override;
   ULONG __stdcall AddRef() override;
   ULONG __stdcall Release() override;

   HRESULT __stdcall CreateStream(const WCHAR *, ULONG, ULONG, ULONG, struct IStream **) override;
   HRESULT __stdcall OpenStream(const WCHAR *, void *, ULONG, ULONG, struct IStream **) override;
   HRESULT __stdcall CreateStorage(const WCHAR *, ULONG, ULONG, ULONG, struct IStorage **) override;
   HRESULT __stdcall OpenStorage(const WCHAR *, struct IStorage *, ULONG, WCHAR **, ULONG, struct IStorage **) override;
   HRESULT __stdcall CopyTo(ULONG, const struct _GUID *, WCHAR **, struct IStorage *) override;
   HRESULT __stdcall MoveElementTo(const WCHAR *, struct IStorage *, const WCHAR *, ULONG) override;
   HRESULT __stdcall Commit(ULONG) override;
   HRESULT __stdcall Revert() override;
   HRESULT __stdcall EnumElements(ULONG, void *, ULONG, struct IEnumSTATSTG **) override;
   HRESULT __stdcall DestroyElement(const WCHAR *) override;
   HRESULT __stdcall RenameElement(const WCHAR *, const WCHAR *) override;
   HRESULT __stdcall SetElementTimes(const WCHAR *, const struct _FILETIME *, const struct _FILETIME *, const struct _FILETIME *) override;
   HRESULT __stdcall SetClass(const struct _GUID &) override;
   HRESULT __stdcall SetStateBits(ULONG, ULONG) override;
   HRESULT __stdcall Stat(struct tagSTATSTG *, ULONG) override;

private:
   int m_cref;

   vector<FastIStorage*> m_vstg;
   vector<FastIStream*> m_vstm;

   WCHAR *m_wzName;
};

class FastIStream : public IStream
{
public:
   FastIStream();
   virtual ~FastIStream();

   HRESULT __stdcall QueryInterface(const struct _GUID &, void **) override;
   ULONG __stdcall AddRef() override;
   ULONG __stdcall Release() override;
   HRESULT __stdcall Read(void *pv, ULONG count, ULONG *foo) override;
   HRESULT __stdcall Write(const void *pv, ULONG count, ULONG *foo) override;
   HRESULT __stdcall Seek(union _LARGE_INTEGER, ULONG, union _ULARGE_INTEGER *) override;
   HRESULT __stdcall SetSize(union _ULARGE_INTEGER) override;
   HRESULT __stdcall CopyTo(struct IStream *, union _ULARGE_INTEGER, union _ULARGE_INTEGER *, union _ULARGE_INTEGER *) override;
   HRESULT __stdcall Commit(ULONG) override;
   HRESULT __stdcall Revert() override;

   HRESULT __stdcall LockRegion(union _ULARGE_INTEGER, union _ULARGE_INTEGER, ULONG) override;
   HRESULT __stdcall UnlockRegion(union _ULARGE_INTEGER, union _ULARGE_INTEGER, ULONG) override;
   HRESULT __stdcall Stat(struct tagSTATSTG *, ULONG) override;
   HRESULT __stdcall Clone(struct IStream **) override;

   char  *m_rg;          // Data buffer
   WCHAR *m_wzName;
   unsigned int m_cSize; // Size of stream

private:
   void SetSize(const unsigned int i);

   int m_cref;

   unsigned int m_cMax;  // Number of elements allocated
   unsigned int m_cSeek; // Last element used
};
