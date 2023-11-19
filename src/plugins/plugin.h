// license:GPLv3+

#pragma once

///////////////////////////////////////////////////////////////////////////////
// VPX plugins
//
// Plugins are a simple way to extend VPX core features. VPinballX application
// will scan for plugins in its 'plugin' folder, searching for subfolders
// containing a 'plugin.cfg' file. When found, the file will be read to find
// the needed metadata to present to the user as well as path for the native
// builds of the plugins for each supported platform. Then, the plugin will be
// available for the end user to enable it from the application settings.
//
// For the sake of simplicity and portability, the API only use C definitions.
//
// The plugin API is not thread safe. If a plugin uses multithreading, it must
// perform all needed synchronization and data copies.
//
// Plugins communicates between each others and with VPX using a basic event
// system. Events are identified by their name. They are registered by requesting 
// an id corresponding to this unique name. After registration, they may be 
// used by subscribing to them or broadcasting them. Events can be broadcasted 
// with an optional datablock. It is the task of the publisher & subscribers to 
// avoid any misunderstanding on the exchanged data.
//
// When loaded, plugins are provided a pointer table to the VPX API. This API
// takes for granted that at any time, only one game can be played. All calls
// related to a running game may only be called between the 'OnGameStart' & 
// 'OnGameEnd' events. They are marked '[InGame]' in the following comments.
//
// This header is a common header to be used both by VPX and its plugins.
//
// Core VPX events:
// - OnGameStart: broadcasted during player creation, before script initialization
// - OnFrameStart: broadcasted when a new frame is about to be prepared
// - OnGameEnd: broadcasted during player shutdown

///////////////////////////////////////////////////////////////////////////////
// VPX Plugin API definition

// Events
typedef unsigned int (*vpxpi_get_event_id)(const char* name);
typedef void (*vpxpi_event_callback)(const unsigned int eventId, void* data);
typedef void (*vpxpi_subscribe_event)(const unsigned int eventId, const vpxpi_event_callback callback);
typedef void (*vpxpi_unsubscribe_event)(const unsigned int eventId, const vpxpi_event_callback callback);
typedef void (*vpxpi_broadcast_event)(const unsigned int eventId, void* data);

// [InGame] View management
//typedef ViewSetupDef (*vpxpi_get_view_setup)(const ViewSetupId viewId);
//typedef void (*vpxpi_set_view_setup)(const ViewSetupId viewId, const ViewSetupDef& setup);


typedef struct
{
   // Events
   vpxpi_get_event_id              get_event_id;
   vpxpi_subscribe_event           subscribe_event;
   vpxpi_unsubscribe_event         unscribe_event;
   vpxpi_broadcast_event           broadcast_event;
   // [InGame] View management
   //vpxpi_get_view_setup          get_view_setup;
   //vpxpi_set_view_setup          set_view_setup;
} VPXPluginAPI;

///////////////////////////////////////////////////////////////////////////////
// Plugin lifecycle
//
// Plugins must implement and export the 2 following functions to be valid.
// For example for windows, use:
// extern "C" __declspec(dllexport) void plugin_load(const VPXPluginAPI* api);
// extern "C" __declspec(dllexport) void plugin_unload();

typedef void (*vpxpi_load_plugin)(const VPXPluginAPI* api);
typedef void (*vpxpi_unload_plugin)();

typedef struct
{
   std::string id;
   std::string name;
   std::string description;
   std::string author;
   std::string version;
   std::string link;
} VPXPluginInfo;
