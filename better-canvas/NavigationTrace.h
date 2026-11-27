#pragma once
/* Private events emitted only by the navigation benchmark build. */
#ifdef STYLOS_NAV_TRACE
void CanvasTrace(unsigned event, int a, int b);
#define NAV_TRACE(event, a, b) CanvasTrace(event, a, b)
#else
#define NAV_TRACE(event, a, b) ((void)0)
#endif
enum {
    NavTile = 1, NavNativeDone, NavAllocate, NavReadBlock, NavReadDone,
    NavScanDone, NavTileDone, NavPaint, NavPaintDone, NavZoomDone,
    NavScrollDone, NavInput, NavInputDone, NavRefine, NavAnchorBefore,
    NavAnchorAfter, NavFirstPoint, NavRefined
};
