/* Library vector offset names (Kickstart 1.3), for tracing and errors. */
#include <stddef.h>

#include "hle.h"

static const char *const exec_names[] = {
    "Supervisor", "ExitIntr", "Schedule", "Reschedule", "Switch", "Dispatch",
    "Exception", "InitCode", "InitStruct", "MakeLibrary", "MakeFunctions",
    "FindResident", "InitResident", "Alert", "Debug", "Disable", "Enable",
    "Forbid", "Permit", "SetSR", "SuperState", "UserState", "SetIntVector",
    "AddIntServer", "RemIntServer", "Cause", "Allocate", "Deallocate",
    "AllocMem", "AllocAbs", "FreeMem", "AvailMem", "AllocEntry", "FreeEntry",
    "Insert", "AddHead", "AddTail", "Remove", "RemHead", "RemTail", "Enqueue",
    "FindName", "AddTask", "RemTask", "FindTask", "SetTaskPri", "SetSignal",
    "SetExcept", "Wait", "Signal", "AllocSignal", "FreeSignal", "AllocTrap",
    "FreeTrap", "AddPort", "RemPort", "PutMsg", "GetMsg", "ReplyMsg",
    "WaitPort", "FindPort", "AddLibrary", "RemLibrary", "OldOpenLibrary",
    "CloseLibrary", "SetFunction", "SumLibrary", "AddDevice", "RemDevice",
    "OpenDevice", "CloseDevice", "DoIO", "SendIO", "CheckIO", "WaitIO",
    "AbortIO", "AddResource", "RemResource", "OpenResource", "RawIOInit",
    "RawMayGetChar", "RawPutChar", "RawDoFmt", "GetCC", "TypeOfMem",
    "Procure", "Vacate", "OpenLibrary",
};

static const char *const dos_names[] = {
    "Open", "Close", "Read", "Write", "Input", "Output", "Seek", "DeleteFile",
    "Rename", "Lock", "UnLock", "DupLock", "Examine", "ExNext", "Info",
    "CreateDir", "CurrentDir", "IoErr", "CreateProc", "Exit", "LoadSeg",
    "UnLoadSeg", "GetPacket", "QueuePacket", "DeviceProc", "SetComment",
    "SetProtection", "DateStamp", "Delay", "WaitForChar", "ParentDir",
    "IsInteractive", "Execute",
};

static const char *const graphics_names[] = {
    "BltBitMap", "BltTemplate", "ClearEOL", "ClearScreen", "TextLength",
    "Text", "SetFont", "OpenFont", "CloseFont", "AskSoftStyle",
    "SetSoftStyle", "AddBob", "AddVSprite", "DoCollision", "DrawGList",
    "InitGels", "InitMasks", "RemIBob", "RemVSprite", "SetCollision",
    "SortGList", "AddAnimOb", "Animate", "GetGBuffers", "InitGMasks",
    "DrawEllipse", "AreaEllipse", "LoadRGB4", "InitRastPort", "InitVPort",
    "MrgCop", "MakeVPort", "LoadView", "WaitBlit", "SetRast", "Move", "Draw",
    "AreaMove", "AreaDraw", "AreaEnd", "WaitTOF", "QBlit", "InitArea",
    "SetRGB4", "QBSBlit", "BltClear", "RectFill", "BltPattern", "ReadPixel",
    "WritePixel", "Flood", "PolyDraw", "SetAPen", "SetBPen", "SetDrMd",
    "InitView", "CBump", "CMove", "CWait", "VBeamPos", "InitBitMap",
    "ScrollRaster", "WaitBOVP", "GetSprite", "FreeSprite", "ChangeSprite",
    "MoveSprite", "LockLayerRom", "UnlockLayerRom", "SyncSBitMap",
    "CopySBitMap", "OwnBlitter", "DisownBlitter", "InitTmpRas", "AskFont",
    "AddFont", "RemFont", "AllocRaster", "FreeRaster", "AndRectRegion",
    "OrRectRegion", "NewRegion", "ClearRectRegion", "ClearRegion",
    "DisposeRegion", "FreeVPortCopLists", "FreeCopList", "ClipBlit",
    "XorRectRegion", "FreeCprList", "GetColorMap", "FreeColorMap", "GetRGB4",
    "ScrollVPort", "UCopperListInit", "FreeGBuffers", "BltBitMapRastPort",
    "OrRegionRegion", "XorRegionRegion", "AndRegionRegion", "SetRGB4CM",
    "BltMaskBitMapRastPort",
};

static const char *const intuition_names[] = {
    "OpenIntuition", "Intuition", "AddGadget", "ClearDMRequest",
    "ClearMenuStrip", "ClearPointer", "CloseScreen", "CloseWindow",
    "CloseWorkBench", "CurrentTime", "DisplayAlert", "DisplayBeep",
    "DoubleClick", "DrawBorder", "DrawImage", "EndRequest", "GetDefPrefs",
    "GetPrefs", "InitRequester", "ItemAddress", "ModifyIDCMP", "ModifyProp",
    "MoveScreen", "MoveWindow", "OffGadget", "OffMenu", "OnGadget", "OnMenu",
    "OpenScreen", "OpenWindow", "OpenWorkBench", "PrintIText",
    "RefreshGadgets", "RemoveGadget", "ReportMouse", "Request",
    "ScreenToBack", "ScreenToFront", "SetDMRequest", "SetMenuStrip",
    "SetPointer", "SetWindowTitles", "ShowTitle", "SizeWindow", "ViewAddress",
    "ViewPortAddress", "WindowToBack", "WindowToFront", "WindowLimits",
    "SetPrefs", "IntuiTextLength", "WBenchToBack", "WBenchToFront",
    "AutoRequest", "BeginRefresh", "BuildSysRequest", "EndRefresh",
    "FreeSysRequest", "MakeScreen", "RemakeDisplay", "RethinkDisplay",
    "AllocRemember", "AlohaWorkbench", "FreeRemember", "LockIBase",
    "UnlockIBase",
};

static const char *const layers_names[] = {
    "InitLayers", "CreateUpfrontLayer", "CreateBehindLayer", "UpfrontLayer",
    "BehindLayer", "MoveLayer", "SizeLayer", "ScrollLayer", "BeginUpdate",
    "EndUpdate", "DeleteLayer", "LockLayer", "UnlockLayer", "LockLayers",
    "UnlockLayers", "LockLayerInfo", "SwapBitsRastPortClipRect", "WhichLayer",
    "UnlockLayerInfo", "NewLayerInfo", "DisposeLayerInfo", "FattenLayerInfo",
    "ThinLayerInfo", "MoveLayerInFrontOf", "InstallClipRegion",
};

static const char *const icon_names[] = {
    "GetWBObject", "PutWBObject", "GetIcon", "PutIcon", "FreeFreeList",
    "FreeWBObject", "AddFreeList", "GetDiskObject", "PutDiskObject",
    "FreeDiskObject", "FindToolType", "MatchToolValue", "BumpRevision",
};

static const char *const mathffp_names[] = {
    "SPFix", "SPFlt", "SPCmp", "SPTst", "SPAbs", "SPNeg", "SPAdd", "SPSub",
    "SPMul", "SPDiv",
};

static const char *const device_names[] = {
    "Open", "Close", "Expunge", "ExtFunc", "BeginIO", "AbortIO",
};

#define LOOKUP(tbl) \
    (index < (int)(sizeof(tbl) / sizeof(tbl[0])) ? tbl[index] : NULL)

const char *hle_lvo_name(LibId id, int offset)
{
    int index = -offset / 6 - 5;   /* first public vector is at -30 */
    if (offset > -30 || offset % 6) return NULL;
    switch (id) {
    case LIB_EXEC: return LOOKUP(exec_names);
    case LIB_DOS: return LOOKUP(dos_names);
    case LIB_GRAPHICS: return LOOKUP(graphics_names);
    case LIB_INTUITION: return LOOKUP(intuition_names);
    case LIB_LAYERS: return LOOKUP(layers_names);
    case LIB_ICON: return LOOKUP(icon_names);
    case LIB_MATHFFP: return LOOKUP(mathffp_names);
    default:
        index = -offset / 6 - 1;
        return LOOKUP(device_names);
    }
}
