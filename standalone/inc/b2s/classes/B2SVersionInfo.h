#define B2S_VERSION_MAJOR       2
#define B2S_VERSION_MINOR       3
#define B2S_VERSION_REVISION    1
#define B2S_VERSION_BUILD       999
#define B2S_VERSION_HASH        "nonset"

#define _STR(x)    #x
#define STR(x)     _STR(x)

#define B2S_VERSION_STRING      STR(B2S_VERSION_MAJOR) "." STR(B2S_VERSION_MINOR) "." STR(B2S_VERSION_REVISION)
#define B2S_BUILD_STRING        B2S_VERSION_STRING "." STR(B2S_VERSION_BUILD)
#define B2S_BUILD_STRING_HASH   B2S_BUILD_STRING "-" B2S_VERSION_HASH
