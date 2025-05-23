#pragma once

#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>

#include <SDL3_ttf/SDL_ttf.h>

#define PUP_SCREEN_TOPPER             0
#define PUP_SETTINGS_TOPPERX          320
#define PUP_SETTINGS_TOPPERY          30
#define PUP_SETTINGS_TOPPERWIDTH      290
#define PUP_SETTINGS_TOPPERHEIGHT     75
#define PUP_ZORDER_TOPPER             300

#define PUP_SCREEN_BACKGLASS          2
#define PUP_SETTINGS_BACKGLASSX       PUP_SETTINGS_TOPPERX
#define PUP_SETTINGS_BACKGLASSY       (PUP_SETTINGS_TOPPERY + PUP_SETTINGS_TOPPERHEIGHT + 5)
#define PUP_SETTINGS_BACKGLASSWIDTH   290
#define PUP_SETTINGS_BACKGLASSHEIGHT  218
#define PUP_ZORDER_BACKGLASS          150

#define PUP_SCREEN_DMD                1
#define PUP_SETTINGS_DMDX             PUP_SETTINGS_TOPPERX
#define PUP_SETTINGS_DMDY             (PUP_SETTINGS_BACKGLASSY + PUP_SETTINGS_BACKGLASSHEIGHT + 5)
#define PUP_SETTINGS_DMDWIDTH         290
#define PUP_SETTINGS_DMDHEIGHT        75
#define PUP_ZORDER_DMD                200

#define PUP_SCREEN_PLAYFIELD          3
#define PUP_SETTINGS_PLAYFIELDX       (PUP_SETTINGS_TOPPERX + PUP_SETTINGS_TOPPERWIDTH + 5)
#define PUP_SETTINGS_PLAYFIELDY       PUP_SETTINGS_TOPPERY
#define PUP_SETTINGS_PLAYFIELDWIDTH   216
#define PUP_SETTINGS_PLAYFIELDHEIGHT  384
#define PUP_ZORDER_PLAYFIELD          150

#define PUP_SCREEN_FULLDMD            5
#define PUP_SETTINGS_FULLDMDX         PUP_SETTINGS_TOPPERX
#define PUP_SETTINGS_FULLDMDY         (PUP_SETTINGS_DMDY + 5)
#define PUP_SETTINGS_FULLDMDWIDTH     290
#define PUP_SETTINGS_FULLDMDHEIGHT    150
#define PUP_ZORDER_FULLDMD            200

typedef struct {
   char type;
   int number;
   int value;
} PUPTriggerData;

class PUPScreen;
class PUPPlaylist;
class PUPTrigger;
class PUPWindow;

class PUPManager final
{
public:
   PUPManager();
   ~PUPManager();

   const string& GetRootPath() const { return m_szRootPath; }

   bool IsInit() const { return m_init; }
   void LoadConfig(const string& szRomName);
   void Unload();
   const string& GetPath() const { return m_szPath; }
   bool AddScreen(PUPScreen* pScreen);
   bool AddScreen(LONG lScreenNum);
   bool HasScreen(int screenNum);
   PUPScreen* GetScreen(int screenNum) const;
   bool AddFont(TTF_Font* pFont, const string& szFilename);
   TTF_Font* GetFont(const string& szFamily);
   void QueueTriggerData(const PUPTriggerData& data);
   int GetTriggerValue(const string& triggerId);
   void Start();
   void Stop();

private:
   void LoadPlaylists();
   void ProcessQueue();
   void AddWindow(const string& szWindowName, int defaultScreen, int defaultX, int defaultY, int defaultWidth, int defaultHeight, int zOrder);

   bool m_init;
   string m_szRootPath;
   string m_szPath;
   ankerl::unordered_dense::map<int, PUPScreen*> m_screenMap;
   vector<TTF_Font*> m_fonts;
   ankerl::unordered_dense::map<string, TTF_Font*> m_fontMap;
   ankerl::unordered_dense::map<string, TTF_Font*> m_fontFilenameMap;
   vector<PUPWindow*> m_windows;
   std::queue<PUPTriggerData> m_triggerDataQueue;
   std::mutex m_queueMutex;
   std::condition_variable m_queueCondVar;
   bool m_isRunning;
   std::thread m_thread;
   vector<PUPPlaylist*> m_playlists;
   ankerl::unordered_dense::map<string, int> m_triggerMap;
};
