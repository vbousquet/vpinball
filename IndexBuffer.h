#pragma once

#include "stdafx.h"
#include "typedefs3D.h"

class VertexBuffer;

class IndexBuffer final
{
public:
   enum Format
   {
      FMT_INDEX16,
      FMT_INDEX32
   };

   enum LockFlags
   {
#if defined(ENABLE_BGFX) || defined(ENABLE_SDL) // BGFX & OpenGL
      WRITEONLY,
      DISCARDCONTENTS
#else
      WRITEONLY = 0, // in DX9, this is specified during VB creation
      DISCARDCONTENTS = D3DLOCK_DISCARD // discard previous contents; only works with dynamic VBs
#endif
   };

   IndexBuffer(RenderDevice* rd, const unsigned int numIndices, const bool isDynamic = false, const IndexBuffer::Format format = IndexBuffer::Format::FMT_INDEX16);
   IndexBuffer(RenderDevice* rd, const unsigned int numIndices, const unsigned int* indices);
   IndexBuffer(RenderDevice* rd, const unsigned int numIndices, const WORD* indices);
   IndexBuffer(RenderDevice* rd, const vector<unsigned int>& indices);
   IndexBuffer(RenderDevice* rd, const vector<WORD>& indices);
   ~IndexBuffer();

   unsigned int GetOffset() const { return m_offset; }
   unsigned int GetIndexOffset() const { return m_indexOffset; }
   bool IsSharedBuffer() const { return m_sharedBuffer->buffers.size() > 1; }

   void lock(const unsigned int offsetToLock, const unsigned int sizeToLock, void** dataBuffer, const DWORD flags);
   void unlock();
   void ApplyOffset(VertexBuffer* vb);
   void Upload();

   RenderDevice* const m_rd;
   const unsigned int m_indexCount;
   const unsigned int m_sizePerIndex;
   const unsigned int m_size;
   const bool m_isStatic;
   const Format m_indexFormat;

private:
   struct SharedBuffer
   {
      vector<IndexBuffer*> buffers;
      unsigned int count = 0;
      Format format;
      bool isStatic;
   };

   bool IsCreated() const
   {
#if defined(ENABLE_BGFX)
      return m_isStatic ? bgfx::isValid(m_ib) : bgfx::isValid(m_dib);
#else
      return m_ib;
#endif
   }

   SharedBuffer* m_sharedBuffer = nullptr;
   unsigned int m_offset = 0; // Offset in bytes of the data inside the native GPU array
   unsigned int m_indexOffset = 0; // Offset in indices of the data inside the native GPU array

   static void CreateSharedBuffer(SharedBuffer* sharedBuffer);
   static vector<SharedBuffer*> pendingSharedBuffers;

   struct PendingUpload
   {
      unsigned int offset;
      unsigned int size;
      BYTE* data;
      #ifdef ENABLE_BGFX
      const bgfx::Memory* buffer = nullptr;
      #endif
   };
   vector<PendingUpload> m_pendingUploads;
   PendingUpload m_lock = { 0, 0, nullptr };

#if defined(ENABLE_BGFX) // BGFX
   bgfx::IndexBufferHandle m_ib = BGFX_INVALID_HANDLE;
   bgfx::DynamicIndexBufferHandle m_dib = BGFX_INVALID_HANDLE; 
   int* m_sharedBufferRefCount = nullptr;

public:
   bgfx::IndexBufferHandle GetStaticBuffer() const { return m_ib; }
   bgfx::DynamicIndexBufferHandle GetDynamicBuffer() const { return m_dib; }

#elif defined(ENABLE_SDL) // OpenGL
   GLuint m_ib = 0;

public:
   GLuint GetBuffer() const { return m_ib; }

#else // DirectX 9
   IDirect3DIndexBuffer9* m_ib = nullptr;

public:
   IDirect3DIndexBuffer9* GetBuffer() const { return m_ib; }
#endif
};
