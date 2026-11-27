#pragma once
#include "../../retas-devkit/plugin.h"
typedef struct LoadedPlugin {
    struct LoadedPlugin *next;
    HMODULE library;
    WCHAR id[96], title[160];
    UINT page, legacyPage;
    RetasPlugin callbacks;
} LoadedPlugin;
extern LoadedPlugin *modules;
void LoadModules(void);
