#include "stdafx.h"
#include "Sampler.h"
#include "RenderDevice.h"
#include <fstream>
#include <filesystem>
#include "miniz/miniz.h"

Sampler::Sampler(RenderDevice* rd, BaseTexture* const surf, const bool force_linear_rgb, const SamplerAddressMode clampu, const SamplerAddressMode clampv, const SamplerFilter filter, const bool compress) : 
   m_type(SurfaceType::RT_DEFAULT), 
   m_rd(rd),
   m_dirty(false),
   m_ownTexture(true),
   m_width(surf->width()),
   m_height(surf->height()),
   m_clampu(clampu),
   m_clampv(clampv),
   m_filter(filter)
{
#ifdef ENABLE_SDL
   m_texTarget = GL_TEXTURE_2D;
   colorFormat format;
   if (surf->m_format == BaseTexture::SRGBA)
      format = colorFormat::SRGBA;
   else if (surf->m_format == BaseTexture::RGBA)
      format = colorFormat::RGBA;
   else if (surf->m_format == BaseTexture::SRGB)
      format = colorFormat::SRGB;
   else if (surf->m_format == BaseTexture::RGB)
      format = colorFormat::RGB;
   else if (surf->m_format == BaseTexture::RGB_FP16)
      format = colorFormat::RGB16F;
   else if (surf->m_format == BaseTexture::RGBA_FP16)
      format = colorFormat::RGBA16F;
   else if (surf->m_format == BaseTexture::RGB_FP32)
      format = colorFormat::RGB32F;
   else if (surf->m_format == BaseTexture::BW)
      format = colorFormat::GREY8;
   else
      assert(false); // Unsupported image format
   if (force_linear_rgb)
   {
      if (format == colorFormat::SRGB)
         format = colorFormat::RGB;
      else if (format == colorFormat::SRGBA)
         format = colorFormat::RGBA;
   }
   m_texture = CreateTexture(surf, 0, format, 0, compress);
   m_isLinear = format != colorFormat::SRGB && format != colorFormat::SRGBA;
#else
   colorFormat texformat;
   IDirect3DTexture9* sysTex = CreateSystemTexture(surf, force_linear_rgb, texformat, compress);

   m_isLinear = texformat == colorFormat::RGBA16F || texformat == colorFormat::RGBA32F || force_linear_rgb;

   HRESULT hr = m_rd->GetCoreDevice()->CreateTexture(m_width, m_height, (texformat != colorFormat::DXT5 && m_rd->m_autogen_mipmap) ? 0 : sysTex->GetLevelCount(),
      (texformat != colorFormat::DXT5 && m_rd->m_autogen_mipmap) ? textureUsage::AUTOMIPMAP : 0, (D3DFORMAT)texformat, (D3DPOOL)memoryPool::DEFAULT, &m_texture, nullptr);
   if (FAILED(hr))
      ReportError("Fatal Error: out of VRAM!", hr, __FILE__, __LINE__);

   m_rd->m_curTextureUpdates++;
   hr = m_rd->GetCoreDevice()->UpdateTexture(sysTex, m_texture);
   if (FAILED(hr))
      ReportError("Fatal Error: uploading texture failed!", hr, __FILE__, __LINE__);

   SAFE_RELEASE(sysTex);

   if (texformat != colorFormat::DXT5 && m_rd->m_autogen_mipmap)
      m_texture->GenerateMipSubLevels(); // tell driver that now is a good time to generate mipmaps
#endif
}

#ifdef ENABLE_SDL
Sampler::Sampler(RenderDevice* rd, SurfaceType type, GLuint glTexture, bool ownTexture, bool force_linear_rgb, const SamplerAddressMode clampu, const SamplerAddressMode clampv, const SamplerFilter filter)
   : m_type(type)
   , m_rd(rd)
   , m_dirty(false)
   , m_ownTexture(ownTexture)
   , m_clampu(clampu)
   , m_clampv(clampv)
   , m_filter(filter)
{
   switch (m_type)
   {
   case RT_DEFAULT: m_texTarget = GL_TEXTURE_2D; break;
   case RT_STEREO: m_texTarget = GL_TEXTURE_2D_ARRAY; break;
   case RT_CUBEMAP: m_texTarget = GL_TEXTURE_CUBE_MAP; break;
   default: assert(false);
   }
   glGetTexLevelParameteriv(m_texTarget, 0, GL_TEXTURE_WIDTH, &m_width);
   glGetTexLevelParameteriv(m_texTarget, 0, GL_TEXTURE_HEIGHT, &m_height);
   int internal_format;
   glGetTexLevelParameteriv(m_texTarget, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal_format);
   m_isLinear = !((internal_format == SRGB) || (internal_format == SRGBA) || (internal_format == SDXT5) || (internal_format == SBC7)) || force_linear_rgb;
   m_texture = glTexture;
}
#else
Sampler::Sampler(RenderDevice* rd, IDirect3DTexture9* dx9Texture, bool ownTexture, bool force_linear_rgb, const SamplerAddressMode clampu, const SamplerAddressMode clampv, const SamplerFilter filter)
   : m_type(SurfaceType::RT_DEFAULT)
   , m_rd(rd)
   , m_dirty(false)
   , m_ownTexture(ownTexture)
   , m_clampu(clampu)
   , m_clampv(clampv)
   , m_filter(filter)
{
   D3DSURFACE_DESC desc;
   dx9Texture->GetLevelDesc(0, &desc);
   m_width = desc.Width;
   m_height = desc.Height;
   m_isLinear = desc.Format == D3DFMT_A16B16G16R16F || desc.Format == D3DFMT_A32B32G32R32F || force_linear_rgb;
   m_texture = dx9Texture;
}
#endif

Sampler::~Sampler()
{
#ifdef ENABLE_SDL
   Unbind();
   if (m_ownTexture)
      glDeleteTextures(1, &m_texture);
#else
   if (m_ownTexture)
      SAFE_RELEASE(m_texture);
#endif
}

void Sampler::Unbind()
{
#ifdef ENABLE_SDL
   for (auto binding : m_bindings)
   {
      binding->sampler = nullptr;
      glActiveTexture(GL_TEXTURE0 + binding->unit);
      glBindTexture(m_texTarget, 0);
   }
   m_bindings.clear();
#endif
}

void Sampler::UpdateTexture(BaseTexture* const surf, const bool force_linear_rgb, const bool compress)
{
#ifdef ENABLE_SDL
   colorFormat format;
   if (surf->m_format == BaseTexture::RGBA)
      format = colorFormat::RGBA;
   else if (surf->m_format == BaseTexture::SRGBA)
      format = colorFormat::SRGBA;
   else if (surf->m_format == BaseTexture::RGB)
      format = colorFormat::RGB;
   else if (surf->m_format == BaseTexture::SRGB)
      format = colorFormat::SRGB;
   else if (surf->m_format == BaseTexture::RGB_FP16)
      format = colorFormat::RGB16F;
   else if (surf->m_format == BaseTexture::RGBA_FP16)
      format = colorFormat::RGBA16F;
   else if (surf->m_format == BaseTexture::RGB_FP32)
      format = colorFormat::RGB32F;
   else if (surf->m_format == BaseTexture::BW)
      format = colorFormat::GREY8;
   else
      assert(false); // Unsupported image format
   if (force_linear_rgb)
   {
      if (format == colorFormat::SRGB)
         format = colorFormat::RGB;
      else if (format == colorFormat::SRGBA)
         format = colorFormat::RGBA;
   }
   const GLuint col_type = ((format == RGBA32F) || (format == RGB32F)) ? GL_FLOAT : ((format == RGBA16F) || (format == RGB16F)) ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE;
   const GLuint col_format = ((format == GREY8) || (format == RED16F))                                                                                                      ? GL_RED
      : ((format == GREY_ALPHA) || (format == RG16F))                                                                                                                       ? GL_RG
      : ((format == RGB) || (format == RGB8) || (format == SRGB) || (format == SRGB8) || (format == RGB5) || (format == RGB10) || (format == RGB16F) || (format == RGB32F)) ? GL_RGB
                                                                                                                                                                            : GL_RGBA;
   // Update bind cache
   auto tex_unit = m_rd->m_samplerBindings.back();
   if (tex_unit->sampler != nullptr)
      tex_unit->sampler->m_bindings.erase(tex_unit);
   tex_unit->sampler = nullptr;
   glActiveTexture(GL_TEXTURE0 + tex_unit->unit);

   glBindTexture(m_texTarget, m_texture);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
   glTexSubImage2D(m_texTarget, 0, 0, 0, surf->width(), surf->height(), col_format, col_type, surf->data());
   glGenerateMipmap(m_texTarget); // Generate mip-maps
   glBindTexture(m_texTarget, 0);
#else
   colorFormat texformat;
   IDirect3DTexture9* sysTex = CreateSystemTexture(surf, force_linear_rgb, texformat, compress);
   CHECKD3D(m_rd->GetCoreDevice()->UpdateTexture(sysTex, m_texture));
   SAFE_RELEASE(sysTex);
#endif
   m_rd->m_curTextureUpdates++;
}

void Sampler::SetClamp(const SamplerAddressMode clampu, const SamplerAddressMode clampv)
{
   m_clampu = clampu;
   m_clampv = clampv;
}

void Sampler::SetFilter(const SamplerFilter filter)
{
   m_filter = filter;
}

void Sampler::SetName(const string& name)
{
   #ifdef ENABLE_SDL
   if (GLAD_GL_VERSION_4_3)
      glObjectLabel(GL_TEXTURE, m_texture, (GLsizei) name.length(), name.c_str());
   #endif
}

#ifdef ENABLE_SDL
GLuint Sampler::CreateTexture(BaseTexture* const surf, unsigned int Levels, colorFormat Format, int stereo, const bool compress)
{
   unsigned int Width = surf->width();
   unsigned int Height = surf->height();
   void* data = surf->data();

   const GLuint col_type = ((Format == RGBA32F) || (Format == RGB32F)) ? GL_FLOAT : ((Format == RGBA16F) || (Format == RGB16F)) ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE;
   const GLuint col_format = ((Format == GREY8) || (Format == RED16F))                                                                                                      ? GL_RED
      : ((Format == GREY_ALPHA) || (Format == RG16F))                                                                                                                       ? GL_RG
      : ((Format == RGB) || (Format == RGB8) || (Format == SRGB) || (Format == SRGB8) || (Format == RGB5) || (Format == RGB10) || (Format == RGB16F) || (Format == RGB32F)) ? GL_RGB
                                                                                                                                                                            : GL_RGBA;
   const bool col_is_linear = (Format == GREY8) || (Format == RED16F) || (Format == GREY_ALPHA) || (Format == RG16F) || (Format == RGB5) || (Format == RGB) || (Format == RGB8)
      || (Format == RGB10) || (Format == RGB16F) || (Format == RGB32F) || (Format == RGBA16F) || (Format == RGBA32F) || (Format == RGBA) || (Format == RGBA8) || (Format == RGBA10)
      || (Format == DXT5) || (Format == BC6U) || (Format == BC6S) || (Format == BC7);

   // Update bind cache
   auto tex_unit = m_rd->m_samplerBindings.back();
   if (tex_unit->sampler != nullptr)
      tex_unit->sampler->m_bindings.erase(tex_unit);
   tex_unit->sampler = nullptr;
   glActiveTexture(GL_TEXTURE0 + tex_unit->unit);

   GLuint texture;
   glGenTextures(1, &texture);
   glBindTexture(m_texTarget, texture);

   if (Format == GREY8)
   { //Hack so that GL_RED behaves as GL_GREY
      glTexParameteri(m_texTarget, GL_TEXTURE_SWIZZLE_R, GL_RED);
      glTexParameteri(m_texTarget, GL_TEXTURE_SWIZZLE_G, GL_RED);
      glTexParameteri(m_texTarget, GL_TEXTURE_SWIZZLE_B, GL_RED);
      glTexParameteri(m_texTarget, GL_TEXTURE_SWIZZLE_A, GL_ALPHA);
      Format = RGB8;
   }
   else if (Format == GREY_ALPHA)
   { //Hack so that GL_RG behaves as GL_GREY_ALPHA
      glTexParameteri(m_texTarget, GL_TEXTURE_SWIZZLE_R, GL_RED);
      glTexParameteri(m_texTarget, GL_TEXTURE_SWIZZLE_G, GL_RED);
      glTexParameteri(m_texTarget, GL_TEXTURE_SWIZZLE_B, GL_RED);
      glTexParameteri(m_texTarget, GL_TEXTURE_SWIZZLE_A, GL_GREEN);
      Format = RGB8;
   }
   else
   { //Default
      glTexParameteri(m_texTarget, GL_TEXTURE_SWIZZLE_R, GL_RED);
      glTexParameteri(m_texTarget, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
      glTexParameteri(m_texTarget, GL_TEXTURE_SWIZZLE_B, GL_BLUE);
      glTexParameteri(m_texTarget, GL_TEXTURE_SWIZZLE_A, GL_ALPHA);
   }

   colorFormat comp_format = Format;
   if (compress && ((Width & 3) == 0) && ((Height & 3) == 0) && (Width > 256) && (Height > 256))
   {
      if (col_type == GL_FLOAT || col_type == GL_HALF_FLOAT)
      {
#ifndef __OPENGLES__
         if (GLAD_GL_ARB_texture_compression_bptc)
            comp_format = surf->IsSigned() ? colorFormat::BC6S : colorFormat::BC6U;
#endif
      }
#ifndef __OPENGLES__
      else if (GLAD_GL_ARB_texture_compression_bptc)
         comp_format = col_is_linear ? colorFormat::BC7 : colorFormat::SBC7;
#endif
      else
         comp_format = col_is_linear ? colorFormat::DXT5 : colorFormat::SDXT5;
   }

   const int num_mips = (int)std::log2(float(max(Width, Height))) + 1;
#ifndef __OPENGLES__
   if (m_rd->getGLVersion() >= 403)
#endif
      glTexStorage2D(m_texTarget, num_mips, comp_format, Width, Height);
#ifndef __OPENGLES__
   else
   { // should never be triggered nowadays
      GLsizei w = Width;
      GLsizei h = Height;
      for (int i = 0; i < num_mips; i++)
      {
         glTexImage2D(m_texTarget, i, comp_format, w, h, 0, col_format, col_type, nullptr);
         w = max(1, (w / 2));
         h = max(1, (h / 2));
      }
   }
#endif

   if (data)
   {
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      #ifdef CACHE_COMPRESSED_TEX
      if (comp_format == Format)
      #endif
      {
         // This line causes a false GLIntercept error log on OpenGL >= 403 since the image is initialized through TexStorage and not TexImage (expected by GLIntercept)
         // InterceptImage::SetImageDirtyPost - Flagging an image as dirty when it is not ready/init?
         glTexSubImage2D(m_texTarget, 0, 0, 0, Width, Height, col_format, col_type, data);
         glGenerateMipmap(m_texTarget); // Generate mip-maps, when using TexStorage will generate same amount as specified in TexStorage, otherwise good idea to limit by GL_TEXTURE_MAX_LEVEL
      }
      #ifdef CACHE_COMPRESSED_TEX
      else
      {
         bool loadedFromCache = false;

         string dir = g_pvp->m_szMyPrefPath + "Cache" + PATH_SEPARATOR_CHAR + g_pplayer->m_ptable->m_szTitle + PATH_SEPARATOR_CHAR;
         std::filesystem::create_directories(std::filesystem::path(dir));
         uint8_t* md5 = surf->GetMD5Hash();
         char buf[32+4+1] = { 0 };
         sprintf_s(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X.tex", 
            md5[0], md5[1], md5[2], md5[3], md5[4], md5[5], md5[6], md5[7],
            md5[8], md5[9], md5[10], md5[11], md5[12], md5[13], md5[14], md5[15]);
         string filename = dir + buf;
         if (FileExists(filename))
         {
            std::ifstream fin;
            fin.open(filename, std::ios::binary | std::ios::in);
            uint8_t* buffer = nullptr;
            int buffer_size = 0, size, level = 0;
            GLsizei w = Width, h = Height, numMips, texFormat, fileFormat;
            fin.read((char*)&fileFormat, sizeof(fileFormat));
            if (fileFormat == 1)
            {
               fin.read((char*)&texFormat, sizeof(texFormat));
               fin.read((char*)&numMips, sizeof(numMips));
               while (!fin.eof())
               {
                  fin.read((char*)&size, sizeof(size));
                  if (size > buffer_size)
                  {
                     delete[] buffer;
                     buffer = new uint8_t[size];
                  }
                  //fin.read((char*)buffer, size);
                  mz_ulong clen;
                  fin.read((char*)&clen, sizeof(clen));
                  mz_uint8* c = (mz_uint8*)malloc(clen);
                  fin.read((char*)c, clen);
                  mz_ulong uclen = size;
                  const int error = uncompress2((unsigned char*)buffer, &uclen, c, &clen);
                  if (error != Z_OK)
                     ShowError("Could not unzip compressed texture data, error " + std::to_string(error));
                  free(c);
                  glCompressedTexSubImage2D(m_texTarget, level, 0, 0, w, h, texFormat, size, buffer);
                  w = max(1, (w / 2));
                  h = max(1, (h / 2));
                  level++;
               }
               fin.close();
               delete[] buffer;
               loadedFromCache = true;
            }
         }
         if (!loadedFromCache)
         {
            glTexSubImage2D(m_texTarget, 0, 0, 0, Width, Height, col_format, col_type, data);
            glGenerateMipmap(m_texTarget);
            std::ofstream fout;
            fout.open(filename, std::ios::binary | std::ios::out);
            uint8_t* buffer = nullptr;
            int buffer_size = 0, size, texFormat, fileFormat = 1;
            glGetTexLevelParameteriv(m_texTarget, 0, GL_TEXTURE_INTERNAL_FORMAT, &texFormat);
            fout.write((char*)&fileFormat, sizeof(fileFormat));
            fout.write((char*)&texFormat, sizeof(texFormat));
            fout.write((char*)&num_mips, sizeof(num_mips));
            for (int level = 0; level < num_mips; level++)
            {
               glGetTexLevelParameteriv(m_texTarget, level, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &size);
               if (size > buffer_size)
               {
                  delete[] buffer;
                  buffer = new uint8_t[size];
               }
               glGetCompressedTexImage(m_texTarget, level, buffer);
               fout.write((char*)&size, sizeof(size));
               //fout.write((char*)buffer, size);
               // We compress the data to limit the cache size
               const mz_ulong slen = (mz_ulong)(size);
               mz_ulong clen = compressBound(slen);
               mz_uint8* c = (mz_uint8*)malloc(clen);
               if (compress2(c, &clen, (const unsigned char*)buffer, slen, MZ_BEST_COMPRESSION) != Z_OK)
                  ShowError("Could not zip compressed texture data");
               fout.write((char*)&clen, sizeof(clen));
               fout.write((char*)c, clen);
               free(c);
            }
            fout.close();
            delete[] buffer;
         }
      }
      #endif
   }
   return texture;
}

#else

IDirect3DTexture9* Sampler::CreateSystemTexture(BaseTexture* const surf, const bool force_linear_rgb, colorFormat& texformat)
{
   const unsigned int texwidth = surf->width();
   const unsigned int texheight = surf->height();
   const BaseTexture::Format basetexformat = surf->m_format;

   if (basetexformat == BaseTexture::RGB_FP16)
   {
      texformat = colorFormat::RGBA16F;
   }
   else if (basetexformat == BaseTexture::RGB_FP32)
   {
      texformat = colorFormat::RGBA32F;
   }
   else
   {
      texformat = colorFormat::RGBA8;
      if (compress && ((texwidth & 3) == 0) && ((texheight & 3) == 0) && (texwidth > 256) && (texheight > 256))
         texformat = colorFormat::DXT5;
   }

   IDirect3DTexture9* sysTex;
   HRESULT hr = m_rd->GetCoreDevice()->CreateTexture(
      texwidth, texheight, (texformat != colorFormat::DXT5 && m_rd->m_autogen_mipmap) ? 1 : 0, 0, (D3DFORMAT)texformat, (D3DPOOL)memoryPool::SYSTEM, &sysTex, nullptr);
   if (FAILED(hr))
   {
      ReportError("Fatal Error: unable to create texture!", hr, __FILE__, __LINE__);
   }

   // copy data into system memory texture
   if (basetexformat == BaseTexture::RGB_FP32 && texformat == colorFormat::RGBA32F)
   {
      D3DLOCKED_RECT locked;
      CHECKD3D(sysTex->LockRect(0, &locked, nullptr, 0));

      float* const __restrict pdest = (float*)locked.pBits;
      const float* const __restrict psrc = (float*)(surf->data());
      for (size_t i = 0; i < (size_t)texwidth * texheight; ++i)
      {
         pdest[i * 4 + 0] = psrc[i * 3 + 0];
         pdest[i * 4 + 1] = psrc[i * 3 + 1];
         pdest[i * 4 + 2] = psrc[i * 3 + 2];
         pdest[i * 4 + 3] = 1.f;
      }

      CHECKD3D(sysTex->UnlockRect(0));
   }
   else if (basetexformat == BaseTexture::RGB_FP16 && texformat == colorFormat::RGBA16F)
   {
      D3DLOCKED_RECT locked;
      CHECKD3D(sysTex->LockRect(0, &locked, nullptr, 0));

      unsigned short* const __restrict pdest = (unsigned short*)locked.pBits;
      const unsigned short* const __restrict psrc = (unsigned short*)(surf->data());
      const unsigned short one16 = float2half(1.f);
      for (size_t i = 0; i < (size_t)texwidth * texheight; ++i)
      {
         pdest[i * 4 + 0] = psrc[i * 3 + 0];
         pdest[i * 4 + 1] = psrc[i * 3 + 1];
         pdest[i * 4 + 2] = psrc[i * 3 + 2];
         pdest[i * 4 + 3] = one16;
      }

      CHECKD3D(sysTex->UnlockRect(0));
   }
   else if ((basetexformat == BaseTexture::BW) && texformat == colorFormat::RGBA8)
   {
      D3DLOCKED_RECT locked;
      CHECKD3D(sysTex->LockRect(0, &locked, nullptr, 0));

      BYTE* const __restrict pdest = (BYTE*)locked.pBits;
      const BYTE* const __restrict psrc = (BYTE*)(surf->data());
      for (size_t i = 0; i < (size_t)texwidth * texheight; ++i)
      {
         pdest[i * 4 + 0] =
         pdest[i * 4 + 1] =
         pdest[i * 4 + 2] = psrc[i];
         pdest[i * 4 + 3] = 255u;
      }

      CHECKD3D(sysTex->UnlockRect(0));
   }
   else if ((basetexformat == BaseTexture::RGB || basetexformat == BaseTexture::SRGB) && texformat == colorFormat::RGBA8)
   {
      D3DLOCKED_RECT locked;
      CHECKD3D(sysTex->LockRect(0, &locked, nullptr, 0));

      BYTE* const __restrict pdest = (BYTE*)locked.pBits;
      const BYTE* const __restrict psrc = (BYTE*)(surf->data());
      for (size_t i = 0; i < (size_t)texwidth * texheight; ++i)
      {
         pdest[i * 4 + 0] = psrc[i * 3 + 2];
         pdest[i * 4 + 1] = psrc[i * 3 + 1];
         pdest[i * 4 + 2] = psrc[i * 3 + 0];
         pdest[i * 4 + 3] = 255u;
      }

      CHECKD3D(sysTex->UnlockRect(0));
   }
   else if ((basetexformat == BaseTexture::RGBA || basetexformat == BaseTexture::SRGBA) && texformat == colorFormat::RGBA8)
   {
      D3DLOCKED_RECT locked;
      CHECKD3D(sysTex->LockRect(0, &locked, nullptr, 0));

      BYTE* const __restrict pdest = (BYTE*)locked.pBits;
      const BYTE* const __restrict psrc = (BYTE*)(surf->data());
      for (size_t i = 0; i < (size_t)texwidth * texheight; ++i)
      {
         pdest[i * 4 + 0] = psrc[i * 4 + 2];
         pdest[i * 4 + 1] = psrc[i * 4 + 1];
         pdest[i * 4 + 2] = psrc[i * 4 + 0];
         pdest[i * 4 + 3] = psrc[i * 4 + 3];
      }

      CHECKD3D(sysTex->UnlockRect(0));
      /* IDirect3DSurface9* sysSurf;
      CHECKD3D(sysTex->GetSurfaceLevel(0, &sysSurf));
      RECT sysRect;
      sysRect.top = 0;
      sysRect.left = 0;
      sysRect.right = texwidth;
      sysRect.bottom = texheight;
      CHECKD3D(D3DXLoadSurfaceFromMemory(sysSurf, nullptr, nullptr, surf->data(), (D3DFORMAT)colorFormat::RGBA8, surf->pitch(), nullptr, &sysRect, D3DX_FILTER_NONE, 0));
      SAFE_RELEASE_NO_RCC(sysSurf);*/
   }

   if (!(texformat != colorFormat::DXT5 && m_rd->m_autogen_mipmap))
      // normal maps or float textures are already in linear space!
      CHECKD3D(D3DXFilterTexture(sysTex, nullptr, D3DX_DEFAULT,
         (texformat == colorFormat::RGBA16F || texformat == colorFormat::RGBA32F || force_linear_rgb) ? D3DX_FILTER_TRIANGLE : (D3DX_FILTER_TRIANGLE | D3DX_FILTER_SRGB)));

   return sysTex;
}
#endif
