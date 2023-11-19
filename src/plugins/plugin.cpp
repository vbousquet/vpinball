// license:GPLv3+

#include "stdafx.h"
#include "plugin.h"
#include <iostream>
#include <filesystem>
#include "inc/mINI/ini.h"

class VPXPlugin
{
public:
   VPXPluginInfo info;
   bool is_loaded = false;
   vpxpi_load_plugin   load_plugin = nullptr;
   vpxpi_unload_plugin unload_plugin = nullptr;
}


std::unordered_map<std::string, VPXPlugin*> g_plugins;

void UpdatePlugins()
{
    string pluginDir = g_pvp->m_szMyPath + "\plugins"; 
    for (const auto& entry : std::filesystem::directory_iterator(pluginDir))
    {
        if (entry.is_directory())
        {
           mINI::INIStructure ini;
           mINI::INIFile file(pluginDir + "\plugin.cfg");
           if (file.read(ini) && ini.has("informations") && ini["informations"].has("id") && ini.has("libraries"))
           {
              string id = ini["informations"]["id"];
              auto it = g_plugins.find(id);
              if (it == g_plugins.end())
              {
                 VPXPlugin* plugin = new VPXPlugin();
                 g_plugins[id] = plugin;
                 plugin->info.id = id;
                 plugin->info.name = ini["informations"].get("name");
                 plugin->info.description = ini["informations"].get("description");
                 plugin->info.author = ini["informations"].get("author");
                 plugin->info.version = ini["informations"].get("version");
                 plugin->info.link = ini["informations"].get("link");
              }
           }
        }
    }
}
