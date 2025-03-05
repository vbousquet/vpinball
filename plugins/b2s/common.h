#pragma once

#include <string>
using std::string;

#include <vector>
using std::vector;

#include "plugins/MsgPlugin.h"
#include "plugins/CorePlugin.h"
#include "plugins/VPXPlugin.h"

// Shared logging
#include "plugins/LoggingPlugin.h"
LPI_USE();
#define LOGD LPI_LOGD
#define LOGI LPI_LOGI
#define LOGE LPI_LOGE

// Scriptable API
#include "plugins/ScriptablePlugin.h"
PSC_USE_ERROR();


typedef enum {
    ScaleMode_Manual = 0,
    ScaleMode_Stretch = 1,
    ScaleMode_Zoom = 2
} ScaleMode;

typedef enum {
    SegmentNumberType_SevenSegment = 7,
    SegmentNumberType_TenSegment = 10,
    SegmentNumberType_FourteenSegment = 14
} SegmentNumberType;

typedef enum {
    PluginStatusEnum_Active = 0,
    PluginStatusEnum_Disabled = 1,
    PluginStatusEnum_DisabledDueToException = 2
} PluginStatusEnum;

typedef enum {
    eType_Undefined = 0,
    eType_ImageCollectionAtForm = 1,
    eType_ImageCollectionAtPictureBox = 2,
    eType_PictureBoxCollection = 3
} eType;

typedef enum {
    eLightsStateAtAnimationStart_Undefined = 0,
    eLightsStateAtAnimationStart_InvolvedLightsOff = 1,
    eLightsStateAtAnimationStart_InvolvedLightsOn = 2,
    eLightsStateAtAnimationStart_LightsOff = 3,
    eLightsStateAtAnimationStart_NoChange = 4
} eLightsStateAtAnimationStart;

typedef enum {
    eLightsStateAtAnimationEnd_Undefined = 0,
    eLightsStateAtAnimationEnd_InvolvedLightsOff = 1,
    eLightsStateAtAnimationEnd_InvolvedLightsOn = 2,
    eLightsStateAtAnimationEnd_LightsReseted = 3,
    eLightsStateAtAnimationEnd_NoChange = 4
} eLightsStateAtAnimationEnd;

typedef enum {
    eAnimationStopBehaviour_Undefined = 0,
    eAnimationStopBehaviour_StopImmediatelly = 1,
    eAnimationStopBehaviour_RunAnimationTillEnd = 2,
    eAnimationStopBehaviour_RunAnimationToFirstStep = 3
} eAnimationStopBehaviour;

typedef enum {
    eCollectedDataType_TopImage = 1,
    eCollectedDataType_SecondImage = 2,
    eCollectedDataType_Standard = 4,
    eCollectedDataType_Animation = 8
} eCollectedDataType;

typedef enum {
    eDMDType_NotDefined = 0,
    eDMDType_NoB2SDMD = 1,
    eDMDType_B2SAlwaysOnSecondMonitor = 2,
    eDMDType_B2SAlwaysOnThirdMonitor = 3,
    eDMDType_B2SOnSecondOrThirdMonitor = 4
} eDMDType;

typedef enum {
    eDualMode_Both = 0,
    eDualMode_Authentic = 1,
    eDualMode_Fantasy = 2
} eDualMode;

typedef enum {
    eLEDType_Undefined = 0,
    eLEDType_LED8 = 1,
    eLEDType_LED10 = 2,
    eLEDType_LED14 = 3,
    eLEDType_LED16 = 4
} eLEDType;

typedef enum {
    eControlType_NotDefined = 0,
    eControlType_LEDBox = 1,
    eControlType_Dream7LEDDisplay = 2,
    eControlType_ReelBox = 3,
    eControlType_ReelDisplay = 4
} eControlType;

typedef enum {
    eDMDViewMode_NotDefined = 0,
    eDMDViewMode_NoDMD = 1,
    eDMDViewMode_ShowDMD = 2,
    eDMDViewMode_ShowDMDOnlyAtDefaultLocation = 3,
    eDMDViewMode_DoNotShowDMDAtDefaultLocation = 4
} eDMDViewMode;

typedef enum {
    eDefaultStartMode_Standard = 1,
    eDefaultStartMode_EXE = 2
} eDefaultStartMode;

typedef enum {
    eDMDTypes_Standard = 0,
    eDMDTypes_TwoMonitorSetup = 1,
    eDMDTypes_ThreeMonitorSetup = 2,
    eDMDTypes_Hidden = 3
} eDMDTypes;

typedef enum {
    eLEDTypes_Undefined = 0,
    eLEDTypes_Rendered = 1,
    eLEDTypes_Dream7 = 2
} eLEDTypes;

typedef enum {
    eImageFileType_PNG = 0,
    eImageFileType_JPG = 1,
    eImageFileType_GIF = 2,
    eImageFileType_BMP = 3
} eImageFileType;

typedef enum {
    eRomIDType_NotDefined = 0,
    eRomIDType_Lamp = 1,
    eRomIDType_Solenoid = 2,
    eRomIDType_GIString = 3,
    eRomIDType_Mech = 4
} eRomIDType;

typedef enum {
    ePictureBoxType_StandardImage = 0,
    ePictureBoxType_SelfRotatingImage = 1,
    ePictureBoxType_MechRotatingImage = 2
} ePictureBoxType;

typedef enum {
    eSnippitRotationDirection_Clockwise = 0,
    eSnippitRotationDirection_AntiClockwise = 1
} eSnippitRotationDirection;

typedef enum {
    eSnippitRotationStopBehaviour_SpinOff = 0,
    eSnippitRotationStopBehaviour_StopImmediatelly = 1,
    eSnippitRotationStopBehaviour_RunAnimationTillEnd = 2,
    eSnippitRotationStopBehaviour_RunAnimationToFirstStep = 3
} eSnippitRotationStopBehaviour;

typedef enum {
    eScoreType_NotUsed = 0,
    eScoreType_Scores = 1,
    eScoreType_Credits = 2
} eScoreType;

typedef enum {
    eDualMode_2_NotSet = 0,
    eDualMode_2_Authentic = 1,
    eDualMode_2_Fantasy = 2
} eDualMode_2;

typedef enum {
    eType_2_NotDefined = 0,
    eType_2_OnBackglass = 1,
    eType_2_OnDMD = 2
} eType_2;
