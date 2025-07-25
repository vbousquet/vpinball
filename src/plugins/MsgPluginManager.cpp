// license:GPLv3+

#include "MsgPluginManager.h"

#include <cassert>
#include <iostream>
#include <filesystem>
#include <chrono>

#define MINI_CASE_SENSITIVE
#include "mINI/ini.h"
#include <plog/Log.h>

using namespace std::string_literals;

#if defined(ENABLE_SDL_VIDEO) || defined(ENABLE_SDL_INPUT) 
   #include <SDL3/SDL_loadso.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
   #ifndef WIN32_LEAN_AND_MEAN
      #define WIN32_LEAN_AND_MEAN
   #endif
   #include <windows.h>
#endif

#if !defined(PATH_SEPARATOR_CHAR)
  #ifdef _MSC_VER
    #define PATH_SEPARATOR_CHAR '\\'
  #else
    #define PATH_SEPARATOR_CHAR '/'
  #endif
#endif

#if !defined(GET_PLATFORM_BITS_ENUM)
   #if (INTPTR_MAX == INT32_MAX)
      #define GET_PLATFORM_BITS_ENUM 0
   #else
      #define GET_PLATFORM_BITS_ENUM 1
   #endif
#endif

#if defined(__APPLE__) || (defined(__linux) || defined(__linux__)) || defined(__ANDROID__)
   #define _stricmp strcasecmp
#endif


MsgPluginManager& MsgPluginManager::GetInstance()
{
   static MsgPluginManager instance;
   return instance;
}

MsgPluginManager::MsgPluginManager()
{
   m_api.GetPluginEndpoint = GetPluginEndpoint;
   m_api.GetEndpointInfo = GetEndpointInfo;
   m_api.GetMsgID = GetMsgID;
   m_api.SubscribeMsg = SubscribeMsg;
   m_api.UnsubscribeMsg = UnsubscribeMsg;
   m_api.BroadcastMsg = BroadcastMsg;
   m_api.SendMsg = SendMsg;
   m_api.ReleaseMsgID = ReleaseMsgID;
   m_api.GetSetting = GetSetting; 
   m_api.RunOnMainThread = RunOnMainThread;
   m_apiThread = std::this_thread::get_id();
}

MsgPluginManager::~MsgPluginManager()
{
   assert(std::this_thread::get_id() == m_apiThread);
   m_plugins.clear();
}


///////////////////////////////////////////////////////////////////////////////
// Message API

unsigned int MsgPluginManager::GetPluginEndpoint(const char* id)
{
   MsgPluginManager& pm = GetInstance();
   auto item = std::ranges::find_if(pm.m_plugins, [id](std::shared_ptr<MsgPlugin>& plg) { return plg->IsLoaded() && strcmp(plg->m_id.c_str(), id) == 0; });
   if (item == pm.m_plugins.end())
      return 0;
   return item->get()->m_endpointId;
}

void MsgPluginManager::GetEndpointInfo(const uint32_t endpointId, MsgEndpointInfo* info)
{
   MsgPluginManager& pm = GetInstance();
   assert(std::this_thread::get_id() == pm.m_apiThread);
   auto item = std::ranges::find_if(pm.m_plugins, [endpointId](std::shared_ptr<MsgPlugin>& plg) { return plg->IsLoaded() && plg->m_endpointId == endpointId; });
   if (item == pm.m_plugins.end())
      return;
   info->id = (*item)->m_id.c_str();
   info->name = (*item)->m_name.c_str();
   info->description = (*item)->m_description.c_str();
   info->author = (*item)->m_author.c_str();
   info->version = (*item)->m_version.c_str();
   info->link = (*item)->m_link.c_str();
}

unsigned int MsgPluginManager::GetMsgID(const char* name_space, const char* name)
{
   MsgPluginManager& pm = GetInstance();
   assert(std::this_thread::get_id() == pm.m_apiThread);
   MsgEntry* freeMsg = nullptr;
   for (MsgEntry& msg : pm.m_msgs)
      if (freeMsg == nullptr && msg.refCount == 0)
         freeMsg = &msg;
      else if ((_stricmp(msg.name_space.c_str(), name_space) == 0) && (_stricmp(msg.name.c_str(), name) == 0))
      {
         msg.refCount++;
         return msg.id;
      }
   if (freeMsg == nullptr)
   {
      pm.m_msgs.push_back(MsgEntry());
      freeMsg = &pm.m_msgs.back();
      freeMsg->id = static_cast<unsigned int>(pm.m_msgs.size()) - 1;
   }
   freeMsg->refCount = 1;
   freeMsg->name_space = name_space;
   freeMsg->name = name;
   freeMsg->callbacks.clear();
   return freeMsg->id;
}

void MsgPluginManager::SubscribeMsg(const uint32_t endpointId, const unsigned int msgId, const msgpi_msg_callback callback, void* userData)
{
   MsgPluginManager& pm = GetInstance();
   assert(std::this_thread::get_id() == pm.m_apiThread);
   assert(callback != nullptr);
   assert(msgId < pm.m_msgs.size());
   assert(pm.m_msgs[msgId].refCount > 0);
   assert(1 <= endpointId && endpointId < pm.m_nextEndpointId);
   #ifdef DEBUG
      // Callback are only allowed to be registered once per message
      for (const CallbackEntry entry : pm.m_msgs[msgId].callbacks)
         assert(entry.callback != callback);
   #endif
   if (pm.m_broadcastInProgress)
      pm.m_deferredAfterBroadCastRunnables.push_back([endpointId, msgId, callback, userData]() { SubscribeMsg(endpointId, msgId, callback, userData); });
   else
      pm.m_msgs[msgId].callbacks.push_back(CallbackEntry { endpointId, callback, userData });
}

void MsgPluginManager::UnsubscribeMsg(const unsigned int msgId, const msgpi_msg_callback callback)
{
   MsgPluginManager& pm = GetInstance();
   assert(std::this_thread::get_id() == pm.m_apiThread);
   assert(callback != nullptr);
   assert(msgId < pm.m_msgs.size());
   assert(pm.m_msgs[msgId].refCount > 0);
   if (pm.m_broadcastInProgress)
   {
      pm.m_deferredAfterBroadCastRunnables.push_back([msgId, callback]() { UnsubscribeMsg(msgId, callback); });
      return;
   }
   for (std::vector<CallbackEntry>::iterator it = pm.m_msgs[msgId].callbacks.begin(); it != pm.m_msgs[msgId].callbacks.end(); ++it)
   {
      if (it->callback == callback)
      {
         pm.m_msgs[msgId].callbacks.erase(it);
         return;
      }
   }
   // Detect invalid subscribe/unsubscribe pairs
   assert(false);
}

void MsgPluginManager::BroadcastMsg(const uint32_t endpointId, const unsigned int msgId, void* data)
{
   MsgPluginManager& pm = GetInstance();
   assert(std::this_thread::get_id() == pm.m_apiThread);
   assert(msgId < pm.m_msgs.size());
   assert(pm.m_msgs[msgId].refCount > 0);
   assert(1 <= endpointId && endpointId < pm.m_nextEndpointId);
   pm.m_broadcastInProgress++;
   for (const CallbackEntry entry : pm.m_msgs[msgId].callbacks)
      entry.callback(msgId, entry.context, data);
   pm.m_broadcastInProgress--;
   if (pm.m_broadcastInProgress == 0 && !pm.m_deferredAfterBroadCastRunnables.empty())
   {
      for (auto fn : pm.m_deferredAfterBroadCastRunnables)
         fn();
      pm.m_deferredAfterBroadCastRunnables.clear();
   }
}

void MsgPluginManager::SendMsg(const uint32_t endpointId, const unsigned int msgId, const uint32_t targetEndpointId, void* data)
{
   MsgPluginManager& pm = GetInstance();
   assert(std::this_thread::get_id() == pm.m_apiThread);
   assert(msgId < pm.m_msgs.size());
   assert(pm.m_msgs[msgId].refCount > 0);
   assert(1 <= endpointId && endpointId < pm.m_nextEndpointId);
   for (const CallbackEntry entry : pm.m_msgs[msgId].callbacks)
      if (entry.endpointId == targetEndpointId)
      {
         entry.callback(msgId, entry.context, data);
         break;
      }
}

void MsgPluginManager::ReleaseMsgID(const unsigned int msgId)
{
   MsgPluginManager& pm = GetInstance();
   assert(std::this_thread::get_id() == pm.m_apiThread);
   assert(msgId < pm.m_msgs.size());
   assert(pm.m_msgs[msgId].refCount > 0);
   pm.m_msgs[msgId].refCount--;
   while (!pm.m_msgs.empty() && pm.m_msgs.back().refCount == 0)
      pm.m_msgs.pop_back();
}

void MsgPluginManager::GetSetting(const char* name_space, const char* name, char* valueBuf, unsigned int valueBufSize)
{
   MsgPluginManager& pm = GetInstance();
   assert(name_space != nullptr);
   assert(name != nullptr);
   assert(valueBuf != nullptr);
   assert(valueBufSize > 0);
   pm.m_settingHandler(name_space, name, valueBuf, valueBufSize);
}

void MsgPluginManager::RunOnMainThread(const double delayInS, const msgpi_timer_callback callback, void* userData)
{
   MsgPluginManager& pm = GetInstance();
   if (delayInS <= 0. && std::this_thread::get_id() == pm.m_apiThread)
   {
      callback(userData);
      return;
   }
   std::unique_lock<std::mutex> lock(pm.m_timerListMutex);
   if (delayInS < 0.)
   {
      pm.m_timers.insert(pm.m_timers.begin(), TimerEntry { callback, userData, std::chrono::high_resolution_clock::now() });
      #ifdef _MSC_VER
         // Wake up message loop
         PostThreadMessage(GetCurrentThreadId(), WM_USER + 12345, 0, 0);
      #endif
      // FIXME block cleanly until processed
      lock.unlock();
      while (!pm.m_timers.empty())
         std::this_thread::sleep_for(std::chrono::nanoseconds(100));
   }
   else
   {
      auto timer = TimerEntry { callback, userData, std::chrono::high_resolution_clock::now() + std::chrono::microseconds(static_cast<int64_t>(delayInS * 1000000)) };
      pm.m_timers.insert(std::ranges::upper_bound(pm.m_timers.begin(), pm.m_timers.end(), timer, [](const TimerEntry &a, const TimerEntry &b) { return a.time < b.time; }), timer);
      #ifdef _MSC_VER
         // Wake up message loop
         PostThreadMessage(GetCurrentThreadId(), WM_USER + 12345, 0, 0);
      #endif
   }
}

void MsgPluginManager::ProcessAsyncCallbacks()
{
   assert(std::this_thread::get_id() == m_apiThread);
   if (m_timers.empty())
      return;
   std::vector<TimerEntry> timers;
   {
      const std::lock_guard<std::mutex> lock(m_timerListMutex);
      std::chrono::high_resolution_clock::time_point now = std::chrono::high_resolution_clock::now();
      for (std::vector<TimerEntry>::iterator it = m_timers.begin(); it < m_timers.end();)
      {
         if (it->time > now)
            break;
         timers.push_back(*it);
         it = m_timers.erase(it);
      }
   }
   // Release lock before calling callbacks to avoid deadlock
   for (const auto& it : timers)
      it.callback(it.userData);
}


///////////////////////////////////////////////////////////////////////////////
// Plugin management

static std::string unquote(const std::string& str)
{
   if (str.front() == '"' && str.back() == '"')
      return str.substr(1, str.size() - 2);
   return str;
}

std::shared_ptr<MsgPlugin> MsgPluginManager::RegisterPlugin(const std::string& id, const std::string& name, const std::string& description, const std::string& author, const std::string& version, const std::string& link, msgpi_load_plugin loadPlugin, msgpi_unload_plugin unloadPlugin)
{
   assert(loadPlugin != nullptr);
   assert(unloadPlugin != nullptr);
   std::shared_ptr<MsgPlugin> plugin = std::make_shared<MsgPlugin>(id, name, description, author, version, link, loadPlugin, unloadPlugin, m_nextEndpointId++);
   m_plugins.push_back(plugin);
   return plugin;
}

void MsgPluginManager::ScanPluginFolder(const std::string& pluginDir, const std::function<void(MsgPlugin&)>& callback)
{
   assert(std::this_thread::get_id() == m_apiThread);
   if (!std::filesystem::exists(pluginDir))
   {
      PLOGE << "Missing plugin directory: " << pluginDir;
      return;
   }
   std::string libraryKey;
   #ifdef _MSC_VER
      #if (INTPTR_MAX == INT32_MAX)
         libraryKey = "windows.x86"s;
      #else
         libraryKey = "windows.x64"s;
      #endif
   #elif defined(__ANDROID__) // leave here, as it also defines linux
      #if defined(_M_IX86) || defined(_M_X64) || defined(_M_AMD64) || defined(__i386__) || defined(__i386) || defined(__i486__) || defined(__i486) || defined(i386) || defined(__ia64__) || defined(__x86_64__)
         #if (INTPTR_MAX == INT32_MAX)
            libraryKey = "android.x86_32"s;
         #else
            libraryKey = "android.x86_64"s;
         #endif
      #elif (INTPTR_MAX == INT32_MAX)
         libraryKey = "android.x86_32"s;
      #endif
   #elif (defined(__linux) || defined(__linux__))
      #if defined(__aarch64__)
         libraryKey = "linux.aarch64"s;
      #else
         libraryKey = "linux.x64"s;
      #endif
   #elif defined(__APPLE__)
      #if defined(TARGET_OS_IOS) && TARGET_OS_IOS
         // Not yet implemented
      #elif defined(TARGET_OS_TV) && TARGET_OS_TV
         // Not yet implemented
      #elif defined(__aarch64__)
         libraryKey = "macos.arm64"s;
      #else
         libraryKey = "macos.x64"s;
      #endif
   #endif
   if (libraryKey.empty())
   {
      // Unsupported platform
      return;
   }

   for (const auto& entry : std::filesystem::directory_iterator(pluginDir))
   {
      if (entry.is_directory())
      {
         mINI::INIStructure ini;
         mINI::INIFile file(entry.path().string() + PATH_SEPARATOR_CHAR + "plugin.cfg");
         if (file.read(ini) && ini.has("configuration"s) && ini["configuration"s].has("id"s) && ini.has("libraries"s) && ini["libraries"s].has(libraryKey))
         {
            std::string id = unquote(ini["configuration"s]["id"s]);
            for (auto it = m_plugins.begin(); it != m_plugins.end(); ++it)
               if ((*it)->m_id == id)
                  it = m_plugins.erase(it);
            const std::string libraryFile = unquote(ini["libraries"s][libraryKey]);
            const std::string libraryPath = entry.path().string() + PATH_SEPARATOR_CHAR + libraryFile;
            if (!std::filesystem::exists(libraryPath))
            {
               PLOGE << "Plugin " << id << " has an invalid library reference to a missing file for " << libraryKey << ": " << libraryFile;
               continue;
            }
            std::shared_ptr<MsgPlugin> plugin = std::make_shared<MsgPlugin>(id, 
               unquote(ini["configuration"s].get("name"s)),
               unquote(ini["configuration"s].get("description"s)),
               unquote(ini["configuration"s].get("author"s)),
               unquote(ini["configuration"s].get("version"s)),
               unquote(ini["configuration"s].get("link"s)),
               entry.path().string(),
               libraryPath,
               m_nextEndpointId++);
            m_plugins.push_back(plugin);
            callback(*plugin);
         }
      }
   }
}

void MsgPluginManager::UnloadPlugins()
{
   for (auto plugin : m_plugins)
      if (!plugin->m_library.empty() && plugin->IsLoaded())
         plugin->Unload();
}

std::shared_ptr<MsgPlugin> MsgPluginManager::GetPlugin(std::string_view pluginId) const
{
   for (auto plugin : m_plugins)
      if(std::equal(plugin->m_id.begin(), plugin->m_id.end(), pluginId.begin(), pluginId.end(), [](char a, char b) { return std::tolower(a) == std::tolower(b); }))
         return plugin;
   return nullptr;
}

#if !(defined(ENABLE_SDL_VIDEO) || defined(ENABLE_SDL_INPUT)) && defined(_MSC_VER)
static std::string GetLastErrorAsString()
{
   DWORD errorMessageID = ::GetLastError();
   if (errorMessageID == 0)
      return std::string();
   LPSTR messageBuffer = nullptr;
   size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, errorMessageID, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      (LPSTR)&messageBuffer, 0, NULL);
   std::string message(messageBuffer, size);
   LocalFree(messageBuffer);
   return message;
}
#endif

MsgPlugin::~MsgPlugin()
{
   if (!m_library.empty() && IsLoaded())
      Unload();
}

void MsgPlugin::Load(const MsgPluginAPI* msgAPI)
{
   if (m_isLoaded)
   {
      PLOGE << "Requested to load plugin '" << m_name << "' which is already loaded";
      return;
   }
   if (m_isDynamicallyLinked)
   {
      if (m_module == nullptr)
      {
         const std::string load = std::string(m_id) + "PluginLoad";
         const std::string unload = std::string(m_id) + "PluginUnload";
         #if defined(_WIN32) || defined(_WIN64)
            SetDllDirectory(m_directory.c_str());
         #endif
         #if defined(ENABLE_SDL_VIDEO) || defined(ENABLE_SDL_INPUT)
            m_module = SDL_LoadObject(m_library.c_str());
            if (m_module == nullptr)
            {
               PLOGE << "Plugin " << m_id << " failed to load library " << m_library << ": " << SDL_GetError();
               return;
            }
            m_loadPlugin = (msgpi_load_plugin)SDL_LoadFunction(static_cast<SDL_SharedObject*>(m_module), load.c_str());
            m_unloadPlugin = (msgpi_unload_plugin)SDL_LoadFunction(static_cast<SDL_SharedObject*>(m_module), unload.c_str());
            if (m_loadPlugin == nullptr || m_unloadPlugin == nullptr)
            {
               SDL_UnloadObject(static_cast<SDL_SharedObject*>(m_module));
               m_loadPlugin = nullptr;
               m_unloadPlugin = nullptr;
               m_module = nullptr;
               PLOGE << "Plugin " << m_id << " invalid library " << m_library << ": required " << load << "/" << unload << " functions are not correct.";
               return;
            }
         #elif defined(_WIN32) || defined(_WIN64)
            constexpr DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
            m_module = LoadLibraryEx(m_library.c_str(), NULL, flags);
            if (m_module == nullptr)
            {
               PLOGE << "Plugin " << m_id << " failed to load library " << m_library;
               PLOGE << "Last error was: " << GetLastErrorAsString();
               return;
            }
            m_loadPlugin = (msgpi_load_plugin)GetProcAddress(static_cast<HMODULE>(m_module), load.c_str());
            m_unloadPlugin = (msgpi_unload_plugin)GetProcAddress(static_cast<HMODULE>(m_module), unload.c_str());
            if (m_loadPlugin == nullptr || m_unloadPlugin == nullptr)
            {
               FreeLibrary(static_cast<HMODULE>(m_module));
               m_loadPlugin = nullptr;
               m_unloadPlugin = nullptr;
               m_module = nullptr;
               PLOGE << "Plugin " << m_id << " invalid library " << m_library << ": required " << load << '/' << unload << " functions are not correct.";
               return;
            }
         #else
            assert(false);
         #endif
         #if defined(_WIN32) || defined(_WIN64)
            SetDllDirectory(NULL);
         #endif
      }
   }
   m_isLoaded = true;
   m_loadPlugin(m_endpointId, msgAPI);
   if (m_isDynamicallyLinked)
   {
      PLOGI << "Plugin " << m_id << " loaded (library: " << m_library << ')';
   }
   else
   {
      PLOGI << "Plugin " << m_id << " loaded (statically linked plugin)";
   }
}

void MsgPlugin::Unload()
{
   if (!m_isLoaded)
   {
      PLOGE << "Requested to unload plugin '" << m_name << "' which is not loaded";
      return;
   }
   m_isLoaded = false;
   m_unloadPlugin();
   if (m_isDynamicallyLinked)
   {
      // We use module unload instead of explicit unloading to avoid crashes due to forced unloading of modules with thread that are not yet joined
      // The only drawback is that the application keep the module (dll file) locked
      /*
      #if defined(ENABLE_SDL_VIDEO) || defined(ENABLE_SDL_INPUT)
         SDL_UnloadObject(static_cast<SDL_SharedObject*>(m_module));
      #elif defined(_WIN32) || defined(_WIN64)
         FreeLibrary(static_cast<HMODULE>(m_module));
      #endif
      */
      m_module = nullptr;
      m_loadPlugin = nullptr;
      m_unloadPlugin = nullptr;
   }
}
