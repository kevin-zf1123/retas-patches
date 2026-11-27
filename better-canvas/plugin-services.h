#pragma once
#include "../retas-devkit/plugin.h"
extern const RetasHost *canvasHost;
typedef struct FastCanvasAPI {
    BOOL (*enabled)(void);
    BOOL (*preview)(BYTE *);
    void (*changed)(void *);
} FastCanvasAPI;
static inline FastCanvasAPI *fast_api(void) {
    return (FastCanvasAPI *)canvasHost->service("retas.canvas.fast/1");
}
