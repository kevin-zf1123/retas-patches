#pragma once
#include <windows.h>
/* Stable same-desktop API. SendMessageTimeoutW(..., SMTO_ABORTIFHUNG, 1000, &result).
   Both arguments are integers. Returned window handles belong to Stylos. */
#define RETAS_MESSAGE_V1 L"RETAS.Stylos.DevKit.v1"
enum RetasEntry {
    RETAS_VERSION = 1, RETAS_CAPABILITIES, RETAS_ACTIVE_CANVAS, RETAS_ZOOM_BASIS_POINTS,
    RETAS_ROTATION_DEGREES, RETAS_OPEN_PREFERENCES, RETAS_OPEN_MANAGER
};
enum RetasCapability { RETAS_VIEW=1, RETAS_PREFERENCES=2, RETAS_MANAGER=4 };
enum RetasPage { RETAS_GENERAL=0, RETAS_CANVAS=1, RETAS_MIDPOINT=2 };
#define RETAS_PAGE_PROPERTY L"RETAS.Stylos.DevKit.RequestedPage"
