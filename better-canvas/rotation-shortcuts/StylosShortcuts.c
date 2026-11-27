#include <windows.h>
#include <string.h>

typedef LRESULT (CALLBACK *Subclass)(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
typedef BOOL (WINAPI *SetSubclass)(HWND, Subclass, UINT_PTR, DWORD_PTR);
typedef BOOL (WINAPI *RemoveSubclass)(HWND, Subclass, UINT_PTR);
typedef LRESULT (WINAPI *DefSubclass)(HWND, UINT, WPARAM, LPARAM);

void RegisterStylosShortcut(void *entry, void *link, int command, const char *name);
int CallStylosCommand(void *entry, void *object, int command);
void RefreshStylosShortcuts(void *entry, void *settings);
BOOL RotateStylosCanvas(HWND canvas, int degrees, const MSG *key);
#ifdef STYLOS_MANAGER_INPUT
void FlushQueuedCanvasInput(void);
#endif

enum {
    Application = 0x7095c8,
    ShortcutLink = 0x70836c,
    LinkVtable = 0x5d0c44,
    LinkCommand = 0xde60,
    AppSettings = 0x80,
    AppTools = 0x98,
    ShortcutVtable = 0x5d350c,
    ShortcutArray = 0x5c,
    VectorItems = 8,
    VectorLength = 0x20,
    ResizeVector = 0x25450,
    NewRecord = 0x35ab0a,
    RefreshKeys = 0x19d50,
    VirtualKeyForBinding = 0x18370,
    PreviewPaletteVtable = 0x5d2ecc,
    PreviewPaletteControl = 0xdc,
    PreviewControlVtable = 0x5da25c,
    PreviewDispatch = 0x98cf0,
    PreviewRegisterSelection = 0x2b13,
    NativeShortcutSettings = 40049,
    NativeRotate90 = 40139,
    NativeRegisterFromDisk = 40223,
    RotateLeft = 0x6e50,
    RotateRight = 0x6e51,
    RegisterPreview = 0x6e52
};

typedef struct {
    unsigned short key;
    unsigned short modifiers;
} Key;

typedef struct {
    int command;
    Key binding;
} Shortcut;

static const struct {
    int command;
    const char *name;
    const WCHAR *label;
    Key defaults;
} commands[] = {
    {RotateLeft, "VIEW_ROTATE_LEFT_5", L"\x5411\x5de6\x65cb\x8f6c 5\x00b0", {'-', 0}},
    {RotateRight, "VIEW_ROTATE_RIGHT_5", L"\x5411\x53f3\x65cb\x8f6c 5\x00b0", {'=', 0}},
    {RegisterPreview, "FILEPREVIEW_REGISTER_LIGHTTABLE",
        L"\x6587\x4ef6\x9884\x89c8\x8c03\x677f\xff1a\x6dfb\x52a0\x5230\x900f\x5149\x53f0", {'L', 3}}
};

static unsigned char *image;
static HWND frame;
static HMENU installed_menu;
static RemoveSubclass remove_subclass;
static DefSubclass def_subclass;
static BOOL registered;
static BOOL bindings_ready;

static unsigned short short_at(const BYTE *bytes)
{
    unsigned short value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static DWORD dword_at(const BYTE *bytes)
{
    DWORD value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static BOOL preference_file(WCHAR *path, const WCHAR *leaf)
{
    static const WCHAR directory[] = L"\\CELSYS\\RETAS STUDIO\\Stylos\\2.0\\Preference\\";
    DWORD start = GetEnvironmentVariableW(L"APPDATA", path, MAX_PATH);
    if (!start || start + lstrlenW(directory) + lstrlenW(leaf) >= MAX_PATH)
        return FALSE;
    lstrcatW(path, directory);
    lstrcatW(path, leaf);
    return TRUE;
}

static void read_saved_keys(Key *keys, BOOL *found)
{
    WCHAR path[MAX_PATH];
    if (!preference_file(path, L"shortcut.rac"))
        return;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD length = GetFileSize(file, NULL);
    if (length >= 22 && length <= 1024 * 1024) {
        BYTE *bytes = HeapAlloc(GetProcessHeap(), 0, length);
        DWORD read;
        if (bytes && ReadFile(file, bytes, length, &read, NULL) && read == length
            && !memcmp(bytes, "CARC", 4) && !memcmp(bytes + 10, "SLTS", 4)
            && dword_at(bytes + 14) <= length - 18) {
            DWORD end = 18 + dword_at(bytes + 14);
            for (DWORD offset = 22; offset + 10 <= end;) {
                DWORD size = dword_at(bytes + offset);
                unsigned int characters = short_at(bytes + offset + 4);
                if (size > end - offset - 4 || size != characters + 6)
                    break;
                for (unsigned int i = 0; i < ARRAYSIZE(commands); ++i) {
                    if (strlen(commands[i].name) == characters
                        && !memcmp(bytes + offset + 6, commands[i].name, characters)) {
                        keys[i].key = short_at(bytes + offset + 6 + characters);
                        keys[i].modifiers = short_at(bytes + offset + 8 + characters);
                        found[i] = TRUE;
                        break;
                    }
                }
                offset += 4 + size;
            }
        }
        if (bytes)
            HeapFree(GetProcessHeap(), 0, bytes);
    }
    CloseHandle(file);
}

static BOOL add_binding(unsigned char *settings, int command, Key key)
{
    unsigned char *array = settings + ShortcutArray;
    int count = *(int *)(array + VectorLength);
    Shortcut **items = *(Shortcut ***)(array + VectorItems);
    if (count < 0 || count > 4096 || (count && !items))
        return FALSE;
    for (int i = 0; i < count; ++i) {
        if (items[i]->command == command)
            return FALSE;
        if (items[i]->binding.key == key.key && items[i]->binding.modifiers == key.modifiers)
            return FALSE;
    }
    Shortcut *record = ((void *(__cdecl *)(size_t))(image + NewRecord))(sizeof(Shortcut));
    if (!record)
        return FALSE;
    record->command = command;
    record->binding = key;
    CallStylosCommand(image + ResizeVector, array, count + 1);
    (*(Shortcut ***)(array + VectorItems))[count] = record;
    return TRUE;
}

static BOOL install_bindings(void)
{
    if (bindings_ready)
        return TRUE;
    unsigned char *app = *(unsigned char **)(image + Application);
    if (!app || !*(void **)(app + AppTools))
        return FALSE;
    unsigned char *settings = *(unsigned char **)(app + AppSettings);
    if (!settings || *(void **)settings != image + ShortcutVtable)
        return FALSE;

    // Stylos may have loaded its preferences before it loaded the WinTab module.
    Key saved[ARRAYSIZE(commands)] = {{0}};
    BOOL found[ARRAYSIZE(commands)] = {FALSE};
    read_saved_keys(saved, found);
    WCHAR marker[MAX_PATH];
    BOOL marker_path = preference_file(marker, L"StylosShortcutDefaults");
    BOOL first_run = marker_path && GetFileAttributesW(marker) == INVALID_FILE_ATTRIBUTES;
    BOOL changed = FALSE;
    for (unsigned int i = 0; i < ARRAYSIZE(commands); ++i) {
        Key binding = found[i] ? saved[i] : commands[i].defaults;
        if ((found[i] || first_run) && binding.key && add_binding(settings, commands[i].command, binding))
            changed = TRUE;
    }
    if (changed)
        RefreshStylosShortcuts(image + RefreshKeys, settings);
    if (first_run) {
        HANDLE stamp = CreateFileW(marker, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, NULL);
        if (stamp != INVALID_HANDLE_VALUE)
            CloseHandle(stamp);
    }
    bindings_ready = TRUE;
    return TRUE;
}

static HMENU menu_containing(HMENU menu, UINT command)
{
    int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        if (GetMenuItemID(menu, i) == command)
            return menu;
        HMENU child = GetSubMenu(menu, i);
        if (child) {
            HMENU found = menu_containing(child, command);
            if (found)
                return found;
        }
    }
    return NULL;
}

static void add_menu_item(HMENU menu, unsigned int index, int position)
{
    if (!menu || GetMenuState(menu, commands[index].command, MF_BYCOMMAND) != (UINT)-1)
        return;
    InsertMenuW(menu, position, MF_BYPOSITION | MF_STRING, commands[index].command, commands[index].label);
}

static void refresh_menus(HWND window)
{
    HMENU menu = GetMenu(window);
    if (!menu || menu == installed_menu)
        return;
    HMENU rotation = menu_containing(menu, NativeRotate90);
    HMENU light_table = menu_containing(menu, NativeRegisterFromDisk);
    if (!rotation || !light_table)
        return;
    add_menu_item(rotation, 0, GetMenuItemCount(rotation));
    add_menu_item(rotation, 1, GetMenuItemCount(rotation));
    for (int i = 0; i < GetMenuItemCount(light_table); ++i) {
        if (GetMenuItemID(light_table, i) == NativeRegisterFromDisk) {
            add_menu_item(light_table, 2, i + 1);
            break;
        }
    }
    installed_menu = menu;
}

static BOOL CALLBACK find_preview(HWND window, LPARAM data)
{
    char name[32];
    if (!IsWindowVisible(window) || GetAncestor(window, GA_ROOTOWNER) != frame
        || !GetClassNameA(window, name, sizeof(name)) || strcmp(name, "CFilePreviewerPalette"))
        return TRUE;
    unsigned char *palette = (unsigned char *)GetWindowLongA(window, GWL_USERDATA);
    if (!palette || *(void **)palette != image + PreviewPaletteVtable || *(HWND *)(palette + 4) != window)
        return TRUE;
    unsigned char *control = *(unsigned char **)(palette + PreviewPaletteControl);
    if (!control || *(void **)control != image + PreviewControlVtable)
        return TRUE;
    *(void **)data = control;
    return FALSE;
}

static BOOL run_command(int command, const MSG *key)
{
    HWND mdi = FindWindowExA(frame, NULL, "MDIClient", NULL);
    HWND canvas = mdi ? (HWND)SendMessageA(mdi, WM_MDIGETACTIVE, 0, 0) : NULL;
    if (!canvas || !IsWindowEnabled(frame) || GetCapture())
        return FALSE;
    if (command == RegisterPreview) {
#ifdef STYLOS_MANAGER_INPUT
        FlushQueuedCanvasInput();
#endif
        void *preview = NULL;
        EnumThreadWindows(GetCurrentThreadId(), find_preview, (LPARAM)&preview);
        if (!preview)
            return FALSE;
        CallStylosCommand(image + PreviewDispatch, preview, PreviewRegisterSelection);
        return TRUE;
    }
    return RotateStylosCanvas(canvas, command == RotateLeft ? -5 : 5, key);
}

BOOL DispatchStylosShortcut(const MSG *message)
{
    if (!frame || !message->hwnd || !IsWindowEnabled(frame) || GetCapture()
        || GetAncestor(message->hwnd, GA_ROOTOWNER) != frame)
        return FALSE;
    char name[64];
    HWND root = GetAncestor(message->hwnd, GA_ROOT);
    if (root != frame && (!GetClassNameA(root, name, sizeof(name)) || !strstr(name, "Palette")))
        return FALSE;
    for (HWND w = message->hwnd; w && w != root; w = GetParent(w)) {
        GetClassNameA(w, name, sizeof(name));
        if (!lstrcmpiA(name, "Edit") || !lstrcmpiA(name, "ComboBox")
            || !lstrcmpiA(name, "ComboLBox") || !lstrcmpiA(name, "#32770"))
            return FALSE;
    }
    unsigned char *app = *(unsigned char **)(image + Application);
    unsigned char *settings = app ? *(unsigned char **)(app + AppSettings) : NULL;
    if (!settings || *(void **)settings != image + ShortcutVtable)
        return FALSE;
    unsigned char *array = settings + ShortcutArray;
    int count = *(int *)(array + VectorLength);
    Shortcut **items = *(Shortcut ***)(array + VectorItems);
    if (!items || count < 0 || count > 4096)
        return FALSE;
    unsigned int modifiers = ((GetKeyState(VK_CONTROL) < 0) ? 1 : 0)
        | ((GetKeyState(VK_SHIFT) < 0) ? 2 : 0) | ((GetKeyState(VK_MENU) < 0) ? 4 : 0);
    for (int i = 0; i < count; ++i) {
        const Shortcut *item = items[i];
        if (!item || item->command < RotateLeft || item->command > RegisterPreview
            || !item->binding.key || item->binding.modifiers != modifiers)
            continue;
        unsigned int key = item->binding.key;
        unsigned int vk = ((unsigned short (__cdecl *)(unsigned int))(image + VirtualKeyForBinding))(key);
        if (message->wParam == vk || (key >= '0' && key <= '9'
                && message->wParam == VK_NUMPAD0 + key - '0'))
            return run_command(item->command, message);
    }
    return FALSE;
}

static LRESULT CALLBACK frame_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR id, DWORD_PTR data)
{
    if (message == WM_COMMAND && !lparam) {
        int command = LOWORD(wparam);
        if (command >= RotateLeft && command <= RegisterPreview) {
            run_command(command, NULL);
            return 0;
        }
        if (command == NativeShortcutSettings)
            refresh_menus(window);
    }
    if (message == WM_INITMENU)
        refresh_menus(window);
    if (message == WM_NCDESTROY) {
        remove_subclass(window, frame_message, id);
        frame = NULL;
        installed_menu = NULL;
    }
    return def_subclass(window, message, wparam, lparam);
}

BOOL InstallStylosShortcuts(unsigned char *base, HWND message_window)
{
    if (frame)
        return install_bindings();
    HWND candidate = GetAncestor(message_window, GA_ROOTOWNER);
    char name[32];
    if (!candidate || !GetClassNameA(candidate, name, sizeof(name))
        || strcmp(name, "DrawFrameWnd") || !GetMenu(candidate))
        return FALSE;

    const BYTE link_prolog[] = {0x6a, 0xff, 0x68, 0x58, 0xa4, 0x76, 0, 0x64};
    const BYTE resize_prolog[] = {0x8b, 0x54, 0x24, 0x04, 0x57, 0x8b, 0xf9};
    const BYTE refresh_prolog[] = {0x53, 0x56, 0x57, 0x8b, 0xf1, 0xe8};
    const BYTE preview_prolog[] = {0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68, 0xcf, 0x37};
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if (nt->OptionalHeader.SizeOfImage < Application + sizeof(void *)
        || *(void **)(base + ShortcutLink) != base + LinkVtable
        || memcmp(base + LinkCommand, link_prolog, sizeof(link_prolog))
        || memcmp(base + ResizeVector, resize_prolog, sizeof(resize_prolog))
        || memcmp(base + RefreshKeys, refresh_prolog, sizeof(refresh_prolog))
        || memcmp(base + PreviewDispatch, preview_prolog, sizeof(preview_prolog)))
        return FALSE;
    HMODULE common = GetModuleHandleA("comctl32.dll");
    if (!common)
        return FALSE;
    SetSubclass set_subclass = (void *)GetProcAddress(common, "SetWindowSubclass");
    remove_subclass = (void *)GetProcAddress(common, "RemoveWindowSubclass");
    def_subclass = (void *)GetProcAddress(common, "DefSubclassProc");
    if (!set_subclass || !remove_subclass || !def_subclass
        || !set_subclass(candidate, frame_message, RotateLeft, 0))
        return FALSE;

    image = base;
    frame = candidate;
    if (!registered) {
        for (unsigned int i = 0; i < ARRAYSIZE(commands); ++i)
            RegisterStylosShortcut(image + LinkCommand, image + ShortcutLink,
                commands[i].command, commands[i].name);
        registered = TRUE;
    }
    refresh_menus(frame);
    return install_bindings();
}
