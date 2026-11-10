/* FlipKey - fixes text typed in the wrong keyboard language (any two languages).
 *
 *   Hotkey    flips the highlighted text, or the current line from its start to the cursor
 *   Word key  flips only the last word (Ctrl+hotkey, or Shift added if it has Ctrl)
 *   Hotkey again, soon after, restores the previous text
 *
 * Nothing watches the keyboard: Windows only signals FlipKey when the hotkey is pressed.
 *
 * With different alphabets each word is flipped by its own alphabet, so a line can be
 * wrong in both directions. With a shared alphabet the current keyboard decides.
 * A single word written entirely in capital English letters, on its own, is left alone.
 * The character tables are read from the keyboards installed in Windows.
 */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef NIN_BALLOONUSERCLICK
#define NIN_BALLOONUSERCLICK (WM_USER + 5)
#endif

#define APP_VER     "1.9"
#define WIDEN_(x)   L##x
#define WIDEN(x)    WIDEN_(x)
#define APP_VERSION WIDEN(APP_VER)
#define GITHUB_OWNER L"BueProject"
#define RELEASE_PATH L"/repos/" GITHUB_OWNER L"/flipkey/releases/latest"
#define MAX_DOWNLOAD (16u * 1024 * 1024)
#define CHECK_EVERY_MIN (24 * 60)
#define APP_TITLE   L"FlipKey v" APP_VERSION
#define RUN_KEY     L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE   L"FlipKey"

#define WM_TRAY     (WM_APP + 1)
#define ID_PAUSE    1001
#define ID_STARTUP  1002
#define ID_EXIT     1003
#define ID_HOTKEY   1004
#define ID_UPDATE   1005
#define WM_UPDATE_FOUND (WM_APP + 3)
#define WM_CAPTURED (WM_APP + 2)
#define CFG_KEY     L"Software\\FlipKey"
#define HOTKEY_ALL  1
#define HOTKEY_WORD 2

#define BUF_MAX     4096
#define UNDO_MS     30000
#define SIDE_A   0
#define SIDE_B   1
#define MODE_ALL    0
#define MODE_WORD   1

/* ---------- shared state ---------- */
static WCHAR g_toA[65536];            /* characters of side B, as side A */
static WCHAR g_toB[65536];
static BYTE  g_side[65536];           /* for letters: side + 1 */
static int   g_sameAlpha;              /* the two languages share letters */
static DWORD g_langA, g_langB;        /* saved choice; 0 means the default pair */
static HKL   g_hkl[2];

static WCHAR g_undoBefore[BUF_MAX + 1];
static WCHAR g_undoAfter[BUF_MAX + 1];
static HWND  g_undoWnd;
static DWORD g_undoTick;              /* when the last flip happened */

static HHOOK g_captureHook;           /* exists only while "Set key..." is listening */
static HANDLE g_flipEvent;
static volatile LONG g_mode;
static volatile LONG g_paused;
static DWORD g_hkVk = VK_F10;
static DWORD g_hkMods = 0;            /* MOD_CONTROL | MOD_SHIFT | MOD_ALT | MOD_WIN */
static volatile BYTE g_phys[256];     /* keys physically held right now */
static volatile LONG g_capturing;
static DWORD g_capVk, g_capMods;
static HWND  g_settings;
static volatile LONG g_autoUpdate;    /* off unless the user ticked the box */
static int   g_wordTaken;              /* another program holds the one-word shortcut */
static WCHAR g_updateVer[32];
static WCHAR g_updateUrl[512];
static char  g_updateHash[80];         /* "sha256:..." published by GitHub, if any */
static volatile LONG g_installing;

static HWND  g_wnd;
static NOTIFYICONDATAW g_nid;
static UINT  g_taskbarCreated;

/* ---------- layout tables ---------- */
static HKL FindLanguage(const HKL *list, int n, LANGID lang)
{
    int i;
    for (i = 0; i < n; i++)
        if (LOWORD((UINT_PTR)list[i]) == lang) return list[i];
    return NULL;
}

static void FindLayouts(void)
{
    HKL list[64];
    int i, n = GetKeyboardLayoutList(64, list);
    LANGID he = MAKELANGID(LANG_HEBREW, SUBLANG_DEFAULT);

    g_hkl[SIDE_A] = g_langA ? FindLanguage(list, n, (LANGID)g_langA) : NULL;
    g_hkl[SIDE_B] = g_langB ? FindLanguage(list, n, (LANGID)g_langB) : NULL;
    if (!g_hkl[SIDE_A]) {
        for (i = 0; i < n; i++)
            if (PRIMARYLANGID(LOWORD((UINT_PTR)list[i])) == LANG_ENGLISH) g_hkl[SIDE_A] = list[i];
        if (!g_hkl[SIDE_A]) g_hkl[SIDE_A] = LoadKeyboardLayoutW(L"00000409", KLF_NOTELLSHELL);
    }
    if (!g_hkl[SIDE_B]) {
        g_hkl[SIDE_B] = FindLanguage(list, n, he);
        if (!g_hkl[SIDE_B])
            for (i = 0; i < n; i++)
                if (list[i] != g_hkl[SIDE_A]) g_hkl[SIDE_B] = list[i];
        if (!g_hkl[SIDE_B]) g_hkl[SIDE_B] = LoadKeyboardLayoutW(L"0000040D", KLF_NOTELLSHELL);
    }
}

static WCHAR KeyChar(HKL h, int vk, int shift)
{
    BYTE ks[256] = {0};
    WCHAR out[8];
    UINT sc = MapVirtualKeyExW(vk, 0 /* VK_TO_VSC */, h);
    if (shift) ks[VK_SHIFT] = 0x80;
    /* flag 4: do not disturb the kernel's dead-key state */
    return ToUnicodeEx(vk, sc, ks, out, 8, 4, h) == 1 ? out[0] : 0;
}

/* Two languages can only be told apart when no letter belongs to both. */
static int ShareAlphabet(HKL a, HKL b)
{
    static BYTE seen[65536];
    int vk;
    memset(seen, 0, sizeof seen);
    for (vk = 1; vk < 256; vk++) {
        WCHAR c = KeyChar(a, vk, 0);
        if (c && IsCharAlphaW(c)) seen[c] = 1;
    }
    for (vk = 1; vk < 256; vk++) {
        WCHAR c = KeyChar(b, vk, 0);
        if (c && IsCharAlphaW(c) && seen[c]) return 1;
    }
    return 0;
}

static void BuildTables(void)
{
    WCHAR t[2][256][2];   /* side, vk, shift */
    int l, vk, sh;
    memset(g_toA, 0, sizeof g_toA);
    memset(g_toB, 0, sizeof g_toB);
    memset(g_side, 0, sizeof g_side);
    g_sameAlpha = ShareAlphabet(g_hkl[SIDE_A], g_hkl[SIDE_B]);

    for (l = 0; l < 2; l++)
        for (vk = 1; vk < 256; vk++)
            for (sh = 0; sh < 2; sh++)
                t[l][vk][sh] = KeyChar(g_hkl[l], vk, sh);

    for (l = 0; l < 2; l++)
        for (vk = 1; vk < 256; vk++) {
            WCHAR c = t[l][vk][0];
            if (c && IsCharAlphaW(c) && !g_side[c]) g_side[c] = (BYTE)(l + 1);
        }
    /* Shift on a Hebrew key may give a Latin capital, which belongs with its small letter. */
    for (l = 0; l < 2; l++)
        for (vk = 1; vk < 256; vk++) {
            WCHAR c = t[l][vk][1];
            if (c && IsCharAlphaW(c) && !g_side[c]) {
                WCHAR low = (WCHAR)(UINT_PTR)CharLowerW((LPWSTR)(UINT_PTR)c);
                g_side[c] = g_side[low] ? g_side[low] : (BYTE)(l + 1);
            }
        }

    /* Unshifted entries go first, so they win when two keys give one character. */
    for (sh = 0; sh < 2; sh++)
        for (vk = 1; vk < 256; vk++)
            for (l = 0; l < 2; l++) {
                int other = 1 - l;
                WCHAR from = t[l][vk][sh], to = t[other][vk][sh];
                WCHAR *map = l == SIDE_A ? g_toB : g_toA;
                if (from < 0x20) continue;
                /* A letter must land on a letter of the other language. */
                if (!g_sameAlpha && g_side[from] == l + 1 && (!to || g_side[to] != other + 1))
                    to = t[other][vk][0];
                if (to >= 0x20 && !map[from]) map[from] = to;
            }
}

#define WORD_KEEP (-2)
#define WORD_UNKNOWN (-1)

/* Decides one word: the side to convert it to, keep it, or unknown (no letters). */
static int WordTarget(const WCHAR *w, size_t n, int *letters, int keepCaps)
{
    int a = 0, b = 0, capitals = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        if (g_side[w[i]] == SIDE_A + 1) a++;
        else if (g_side[w[i]] == SIDE_B + 1) b++;
        if (w[i] >= L'A' && w[i] <= L'Z') capitals++;
    }
    *letters = a + b;
    if (a + b == 0) return WORD_UNKNOWN;
    if (keepCaps && capitals == a + b) return WORD_KEEP;
    if (a == b) return WORD_UNKNOWN;
    return a > b ? SIDE_B : SIDE_A;
}

static int IsGap(WCHAR c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; }

/* Capitals are kept only for a word on its own, like USB; in a sentence everything flips. */
static int IsSingleWord(const WCHAR *in, size_t n)
{
    size_t i = 0, words = 0;
    while (i < n) {
        while (i < n && IsGap(in[i])) i++;
        if (i < n) words++;
        while (i < n && !IsGap(in[i])) i++;
    }
    return words == 1;
}

/* The letters can't show the language, so all of it is treated as typed on the current keyboard. */
static int FlipByKeyboard(const WCHAR *in, size_t n, WCHAR *out, int currentSide)
{
    int target = currentSide == SIDE_B ? SIDE_A : SIDE_B;
    const WCHAR *map = target == SIDE_A ? g_toA : g_toB;
    size_t i = 0, e, k;
    int single = IsSingleWord(in, n);

    memcpy(out, in, n * sizeof(WCHAR));
    out[n] = 0;
    for (; i < n; i = e) {
        int letters = 0, capitals = 0;
        while (i < n && IsGap(in[i])) i++;
        e = i;
        while (e < n && !IsGap(in[e])) e++;
        for (k = i; k < e; k++) {
            if (IsCharAlphaW(in[k])) letters++;
            if (in[k] >= L'A' && in[k] <= L'Z') capitals++;
        }
        if (single && letters && capitals == letters) continue;
        for (k = i; k < e; k++)
            if (map[in[k]]) out[k] = map[in[k]];
    }
    return target;
}

/* Flips word by word and returns the side most of the text ended up on. */
static int FlipSpan(const WCHAR *in, size_t n, WCHAR *out, int currentSide)
{
    int count[2] = {0, 0}, prev = WORD_UNKNOWN, last, letters, t, single;
    size_t i = 0, e, k;

    if (g_sameAlpha) return FlipByKeyboard(in, n, out, currentSide);
    single = IsSingleWord(in, n);

    /* A word with no letters, like a lone comma, follows the words around it. */
    while (i < n && prev == WORD_UNKNOWN) {
        while (i < n && IsGap(in[i])) i++;
        e = i;
        while (e < n && !IsGap(in[e])) e++;
        t = WordTarget(in + i, e - i, &letters, single);
        if (t >= 0) prev = t;
        i = e;
    }
    if (prev == WORD_UNKNOWN) prev = currentSide == SIDE_B ? SIDE_A : SIDE_B;
    last = prev;

    memcpy(out, in, n * sizeof(WCHAR));
    out[n] = 0;
    for (i = 0; i < n; i = e) {
        const WCHAR *map;
        while (i < n && IsGap(in[i])) i++;
        e = i;
        while (e < n && !IsGap(in[e])) e++;
        if (e == i) break;
        t = WordTarget(in + i, e - i, &letters, single);
        if (t == WORD_KEEP) continue;
        if (t == WORD_UNKNOWN) t = prev;
        prev = t;
        last = t;
        count[t] += letters;
        map = t == SIDE_A ? g_toA : g_toB;
        for (k = i; k < e; k++) {
            WCHAR m = map[in[k]];
            if (m) out[k] = m;
        }
    }
    if (count[SIDE_A] == count[SIDE_B]) return last;
    return count[SIDE_A] > count[SIDE_B] ? SIDE_A : SIDE_B;
}

/* ---------- hotkey ---------- */
static int IsModifierOrIgnored(DWORD vk)
{
    switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN:
    case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL:
        return 1;
    }
    return vk >= VK_BROWSER_BACK && vk <= VK_LAUNCH_APP2; /* media keys */
}

static int IsRealModifier(DWORD vk)
{
    return vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL ||
           vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN ||
           vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU;
}

static DWORD PhysMods(void)
{
    DWORD m = 0;
    if (g_phys[VK_LCONTROL] || g_phys[VK_RCONTROL]) m |= MOD_CONTROL;
    if (g_phys[VK_LSHIFT] || g_phys[VK_RSHIFT]) m |= MOD_SHIFT;
    if (g_phys[VK_LMENU] || g_phys[VK_RMENU]) m |= MOD_ALT;
    if (g_phys[VK_LWIN] || g_phys[VK_RWIN]) m |= MOD_WIN;
    return m;
}

/* The combination that flips one word; 0 when no free modifier is left. */
static DWORD WordMods(DWORD mods)
{
    if (!(mods & MOD_CONTROL)) return mods | MOD_CONTROL;
    if (!(mods & MOD_SHIFT)) return mods | MOD_SHIFT;
    return 0;
}

static int Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static void UnregisterHotkeys(void)
{
    UnregisterHotKey(g_wnd, HOTKEY_ALL);
    UnregisterHotKey(g_wnd, HOTKEY_WORD);
}

static int RegisterHotkeys(void)
{
    DWORD word = WordMods(g_hkMods);
    UnregisterHotkeys();
    g_wordTaken = word && !RegisterHotKey(g_wnd, HOTKEY_WORD, word | MOD_NOREPEAT, g_hkVk);
    return RegisterHotKey(g_wnd, HOTKEY_ALL, g_hkMods | MOD_NOREPEAT, g_hkVk);
}

/* Runs only while the settings window is waiting for a key combination. */
static LRESULT CALLBACK CaptureProc(int code, WPARAM w, LPARAM l)
{
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)l;
        int down = (w == WM_KEYDOWN || w == WM_SYSKEYDOWN);
        if (!(k->flags & LLKHF_INJECTED)) {
            if (k->vkCode < 256) g_phys[k->vkCode] = down ? 1 : 0;
            if (g_capturing && !IsRealModifier(k->vkCode)) {
                if (down) {
                    g_capVk = k->vkCode;
                    g_capMods = PhysMods();
                    PostMessageW(g_settings, WM_CAPTURED, 0, 0);
                }
                return 1;
            }
        }
    }
    return CallNextHookEx(g_captureHook, code, w, l);
}

static void RemoveCaptureHook(void)
{
    InterlockedExchange(&g_capturing, 0);
    if (g_captureHook) UnhookWindowsHookEx(g_captureHook);
    g_captureHook = NULL;
}

/* ---------- injecting input ---------- */
static void SendKey(WORD vk, int up)
{
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &in, sizeof in);
}

/* Injected keys must not combine with the hotkey's modifiers, so give the user
   a moment to let go, and only then release whatever is still held. */
static void WaitForKeysUp(void)
{
    static const WORD keys[] = { VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL,
                                 VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN };
    int i, t, any = 0;
    for (t = 0; t < 150; t++) {
        any = 0;
        for (i = 0; i < 8; i++) if (Down(keys[i])) any = 1;
        if (!any) break;
        Sleep(10);
    }
    if (!any) return;
    /* An unused key stops Alt opening menus and Win opening Start on release. */
    SendKey(0xE8, 0);
    SendKey(0xE8, 1);
    for (i = 0; i < 8; i++) if (Down(keys[i])) SendKey(keys[i], 1);
}

static void SendShiftHome(void)
{
    INPUT in[2];
    SendKey(VK_SHIFT, 0);
    memset(in, 0, sizeof in);
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = VK_HOME;
    in[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    in[1].ki.dwFlags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
    SendKey(VK_SHIFT, 1);
}

static void SendCtrlCombo(WORD vk)
{
    SendKey(VK_CONTROL, 0);
    SendKey(vk, 0);
    SendKey(vk, 1);
    SendKey(VK_CONTROL, 1);
}

static void SendText(const WCHAR *s, int n)
{
    INPUT *in;
    int i;
    if (n <= 0) return;
    in = (INPUT *)calloc((size_t)n * 2, sizeof(INPUT));
    if (!in) return;
    for (i = 0; i < n; i++) {
        in[2 * i].type = in[2 * i + 1].type = INPUT_KEYBOARD;
        in[2 * i].ki.wScan = in[2 * i + 1].ki.wScan = s[i];
        in[2 * i].ki.dwFlags = KEYEVENTF_UNICODE;
        in[2 * i + 1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    }
    SendInput((UINT)n * 2, in, sizeof(INPUT));
    free(in);
}

static void SwitchLayout(HWND fg, int layout)
{
    GUITHREADINFO gi;
    HWND target = fg;
    memset(&gi, 0, sizeof gi);
    gi.cbSize = sizeof gi;
    if (GetGUIThreadInfo(GetWindowThreadProcessId(fg, NULL), &gi) && gi.hwndFocus)
        target = gi.hwndFocus;
    PostMessageW(target, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)g_hkl[layout]);
}

/* ---------- clipboard ---------- */
typedef struct { UINT fmt; HGLOBAL mem; } SavedFmt;
typedef struct { SavedFmt *items; int n; } SavedClip;

static int OpenClip(void)
{
    int i;
    for (i = 0; i < 20; i++) {
        if (OpenClipboard(NULL)) return 1;
        Sleep(10);
    }
    return 0;
}

static void SaveClipboard(SavedClip *c)
{
    UINT f = 0;
    c->items = NULL;
    c->n = 0;
    if (!OpenClip()) return;
    while ((f = EnumClipboardFormats(f)) != 0) {
        HANDLE h;
        SIZE_T sz;
        void *src, *dst;
        HGLOBAL copy;
        SavedFmt *grown;
        /* These handles are not global memory and Windows regenerates them. */
        if (f == CF_BITMAP || f == CF_PALETTE || f == CF_METAFILEPICT ||
            f == CF_ENHMETAFILE || f == CF_DSPBITMAP || f == CF_DSPENHMETAFILE ||
            f == CF_DSPMETAFILEPICT || f == CF_OWNERDISPLAY)
            continue;
        h = GetClipboardData(f);
        if (!h) continue;
        sz = GlobalSize(h);
        if (!sz || sz > 64 * 1024 * 1024) continue;
        src = GlobalLock(h);
        if (!src) continue;
        copy = GlobalAlloc(GMEM_MOVEABLE, sz);
        if (copy && (dst = GlobalLock(copy)) != NULL) {
            memcpy(dst, src, sz);
            GlobalUnlock(copy);
            grown = (SavedFmt *)realloc(c->items, sizeof(SavedFmt) * (c->n + 1));
            if (grown) {
                c->items = grown;
                c->items[c->n].fmt = f;
                c->items[c->n].mem = copy;
                c->n++;
            } else {
                GlobalFree(copy);
            }
        } else if (copy) {
            GlobalFree(copy);
        }
        GlobalUnlock(h);
    }
    CloseClipboard();
}

static void RestoreClipboard(SavedClip *c)
{
    int i;
    if (OpenClip()) {
        EmptyClipboard();
        for (i = 0; i < c->n; i++)
            if (!SetClipboardData(c->items[i].fmt, c->items[i].mem))
                GlobalFree(c->items[i].mem);
        CloseClipboard();
    } else {
        for (i = 0; i < c->n; i++) GlobalFree(c->items[i].mem);
    }
    free(c->items);
    c->items = NULL;
    c->n = 0;
}

static WCHAR *ReadClipboardText(void)
{
    WCHAR *r = NULL;
    if (OpenClip()) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            const WCHAR *p = (const WCHAR *)GlobalLock(h);
            if (p) {
                r = _wcsdup(p);
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
    }
    return r;
}

static void WriteClipboardText(const WCHAR *s)
{
    size_t bytes = (wcslen(s) + 1) * sizeof(WCHAR);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    void *p;
    if (!g) return;
    p = GlobalLock(g);
    if (!p) { GlobalFree(g); return; }
    memcpy(p, s, bytes);
    GlobalUnlock(g);
    if (OpenClip()) {
        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
        CloseClipboard();
    } else {
        GlobalFree(g);
    }
}

static int NeedsPaste(const WCHAR *s, size_t n)
{
    return n > 1000 || wcspbrk(s, L"\r\n\t") != NULL;
}

/* Copies what is highlighted. Returns NULL when the application copied nothing.
   Editors such as VS Code copy the whole line when nothing is highlighted, which
   is recognised by its single line break at the end and treated as nothing. In other
   programs that same shape is a real selection, such as a triple-clicked paragraph. */
static int NameInList(const WCHAR *name, const WCHAR *const *list)
{
    for (; *list; list++)
        if (!lstrcmpiW(name, *list)) return 1;
    return 0;
}

/* Programs whose Ctrl+C copies the whole line when nothing is highlighted. */
static int IsLineCopier(const WCHAR *app)
{
    static const WCHAR *const apps[] = {
        L"Code.exe", L"Code - Insiders.exe", L"Cursor.exe", L"Windsurf.exe", L"VSCodium.exe",
        L"sublime_text.exe", L"devenv.exe", L"EXCEL.EXE", L"idea64.exe", L"pycharm64.exe",
        L"webstorm64.exe", L"clion64.exe", L"rider64.exe", L"goland64.exe", L"phpstorm64.exe",
        L"rubymine64.exe", L"datagrip64.exe", L"studio64.exe", NULL };
    return NameInList(app, apps);
}

static int IsTerminalApp(const WCHAR *app)
{
    static const WCHAR *const apps[] = {
        L"WindowsTerminal.exe", L"mintty.exe", L"ConEmu.exe", L"ConEmu64.exe", L"putty.exe",
        L"kitty.exe", L"Tabby.exe", L"Hyper.exe", L"wezterm-gui.exe", L"alacritty.exe", NULL };
    return NameInList(app, apps);
}

static int IsTerminalClass(const WCHAR *cls)
{
    static const WCHAR *const classes[] = {
        L"ConsoleWindowClass", L"CASCADIA_HOSTING_WINDOW_CLASS", L"mintty",
        L"VirtualConsoleClass", L"PuTTY", NULL };
    return NameInList(cls, classes);
}

static void WindowApp(HWND w, WCHAR *out, DWORD size)
{
    DWORD pid = 0, n = MAX_PATH;
    WCHAR path[MAX_PATH];
    HANDLE p;
    out[0] = 0;
    GetWindowThreadProcessId(w, &pid);
    p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return;
    if (QueryFullProcessImageNameW(p, 0, path, &n)) {
        const WCHAR *b = wcsrchr(path, L'\\');
        lstrcpynW(out, b ? b + 1 : path, (int)size);
    }
    CloseHandle(p);
}

static int IsOneLineWithBreak(const WCHAR *text, size_t n)
{
    size_t body = n;
    while (body > 0 && (text[body - 1] == L'\n' || text[body - 1] == L'\r')) body--;
    return body < n && wcscspn(text, L"\r\n") >= body;
}

static WCHAR *CopySelected(int waitMs, int lineCopier, int *copiedLine)
{
    DWORD seq = GetClipboardSequenceNumber();
    WCHAR *text;
    size_t n;
    int i, changed = 0;
    SendCtrlCombo('C');
    for (i = 0; i < waitMs / 10; i++) {
        Sleep(10);
        if (GetClipboardSequenceNumber() != seq) { changed = 1; break; }
    }
    if (!changed) return NULL;
    Sleep(20); /* let the application finish writing the clipboard */
    text = ReadClipboardText();
    n = text ? wcslen(text) : 0;
    if (n == 0 || wcsspn(text, L"\r\n") == n || (lineCopier && IsOneLineWithBreak(text, n))) {
        if (n && copiedLine) *copiedLine = 1;
        free(text);
        return NULL;
    }
    return text;
}

/* Fills out (same length as text) and returns the layout to switch to. */
static int ComputeFlip(const WCHAR *text, size_t n, int mode, int layout, HWND fg, WCHAR *out)
{
    size_t start = 0, len, alen = wcslen(g_undoAfter);
    int target;

    memcpy(out, text, (n + 1) * sizeof(WCHAR));

    /* Pressing again: the text still ends with our last result, so put back the original. */
    if (g_undoWnd == fg && alen && alen <= n && GetTickCount() - g_undoTick < UNDO_MS &&
        !memcmp(text + n - alen, g_undoAfter, alen * sizeof(WCHAR))) {
        static WCHAR tmp[BUF_MAX + 1];
        memcpy(out + n - alen, g_undoBefore, alen * sizeof(WCHAR));
        wcscpy(tmp, g_undoBefore);
        wcscpy(g_undoBefore, g_undoAfter);
        wcscpy(g_undoAfter, tmp);
        g_undoTick = GetTickCount();
        return layout == SIDE_B ? SIDE_A : SIDE_B;
    }

    if (mode == MODE_WORD) {
        start = n;
        while (start > 0 && IsGap(text[start - 1])) start--;
        while (start > 0 && !IsGap(text[start - 1])) start--;
    }
    len = n - start;
    target = FlipSpan(text + start, len, out + start, layout);

    if (len <= BUF_MAX) {
        wcscpy(g_undoBefore, text + start);
        wcscpy(g_undoAfter, out + start);
        g_undoWnd = fg;
        g_undoTick = GetTickCount();
    } else {
        g_undoAfter[0] = 0;
    }
    return target;
}

/* In a terminal Ctrl+C would stop the running program, so FlipKey stays out. The terminal
   inside an editor such as VS Code can't be told apart from the editor itself. */
static int IsTerminal(HWND fg)
{
    WCHAR cls[64], app[64];
    if (GetClassNameW(fg, cls, 64) && IsTerminalClass(cls)) return 1;
    WindowApp(fg, app, 64);
    return IsTerminalApp(app);
}

static void FlipNow(HWND fg, int layout, int mode)
{
    SavedClip saved;
    WCHAR *text, *out;
    size_t n;
    int target, copiedLine = 0;
    int lineCopier;
    WCHAR cls[32] = L"", app[64];

    GetClassNameW(fg, cls, 32);
    WindowApp(fg, app, 64);
    lineCopier = IsLineCopier(app);
    SaveClipboard(&saved);
    text = CopySelected(150, lineCopier, &copiedLine);
    /* Excel copies the cell when it is not being edited; Shift+Home would extend the selection. */
    if (!text && !(copiedLine && !lstrcmpW(cls, L"XLMAIN"))) {
        SendShiftHome();
        Sleep(30);
        text = CopySelected(300, lineCopier, NULL);
    }
    if (!text) {
        RestoreClipboard(&saved);
        return;
    }
    n = wcslen(text);
    out = (WCHAR *)malloc((n + 1) * sizeof(WCHAR));
    if (!out) {
        free(text);
        RestoreClipboard(&saved);
        return;
    }
    target = ComputeFlip(text, n, mode, layout, fg, out);

    if (NeedsPaste(out, n)) {
        WriteClipboardText(out);
        Sleep(20);
        SendCtrlCombo('V');
        Sleep(400); /* the target application reads the clipboard asynchronously */
    } else {
        SendText(out, (int)n);
    }
    RestoreClipboard(&saved);
    SwitchLayout(fg, target);
    free(text);
    free(out);
}

static DWORD WINAPI FlipWorker(LPVOID unused)
{
    (void)unused;
    for (;;) {
        HWND fg;
        int layout;
        WaitForSingleObject(g_flipEvent, INFINITE);
        WaitForKeysUp();
        fg = GetForegroundWindow();
        if (!fg || IsTerminal(fg)) continue;
        layout = GetKeyboardLayout(GetWindowThreadProcessId(fg, NULL)) == g_hkl[SIDE_B]
                     ? SIDE_B : SIDE_A;
        FlipNow(fg, layout, (int)g_mode);
    }
    return 0;
}

/* ---------- hotkey settings ---------- */
static void HotkeyName(DWORD vk, DWORD mods, WCHAR *out)
{
    WCHAR key[32];
    out[0] = 0;
    if (mods & MOD_CONTROL) lstrcatW(out, L"Ctrl + ");
    if (mods & MOD_ALT) lstrcatW(out, L"Alt + ");
    if (mods & MOD_SHIFT) lstrcatW(out, L"Shift + ");
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        key[0] = (WCHAR)vk;
        key[1] = 0;
    } else if (vk >= VK_F1 && vk <= VK_F24) {
        wsprintfW(key, L"F%d", (int)(vk - VK_F1 + 1));
    } else {
        LONG scan = (LONG)MapVirtualKeyW(vk, 0) << 16;
        if ((vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT || vk == VK_DELETE)
            scan |= 1 << 24;
        if (!GetKeyNameTextW(scan, key, 32)) wsprintfW(key, L"Key %d", (int)vk);
    }
    lstrcatW(out, key);
}

/* Returns NULL when the combination is usable, otherwise the reason it is not. */
static const WCHAR *HotkeyProblem(DWORD vk, DWORD mods)
{
    int standalone = (vk >= VK_F1 && vk <= VK_F24) || vk == VK_PAUSE ||
                     vk == VK_INSERT;
    if (vk == VK_CAPITAL || vk == VK_NUMLOCK || IsModifierOrIgnored(vk))
        return L"That key can't be used. Try another.";
    if (mods & MOD_WIN)
        return L"Windows-key combinations are reserved by Windows.";
    if (!(mods & (MOD_CONTROL | MOD_ALT)) && !standalone)
        return L"That key is used for typing. Add Ctrl or Alt.";
    if (mods == MOD_CONTROL && (vk == 'C' || vk == 'V' || vk == 'X' || vk == 'A' || vk == 'Z'))
        return L"That shortcut is needed for copy, paste and undo.";
    if (mods == MOD_ALT && (vk == VK_F4 || vk == VK_TAB))
        return L"Windows needs that shortcut.";
    return NULL;
}

static int LoadConfig(void)
{
    HKEY k;
    DWORD vk = 0, mods = 0, done = 0, size;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, CFG_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    size = sizeof vk; RegQueryValueExW(k, L"HotkeyVk", NULL, NULL, (BYTE *)&vk, &size);
    size = sizeof mods; RegQueryValueExW(k, L"HotkeyMods", NULL, NULL, (BYTE *)&mods, &size);
    size = sizeof done; RegQueryValueExW(k, L"Configured", NULL, NULL, (BYTE *)&done, &size);
    size = sizeof g_langA; RegQueryValueExW(k, L"LangA", NULL, NULL, (BYTE *)&g_langA, &size);
    size = sizeof g_langB; RegQueryValueExW(k, L"LangB", NULL, NULL, (BYTE *)&g_langB, &size);
    {
        DWORD up = 0;
        size = sizeof up;
        RegQueryValueExW(k, L"AutoUpdate", NULL, NULL, (BYTE *)&up, &size);
        g_autoUpdate = up == 1;
    }
    RegCloseKey(k);
    if (done != 1 || vk == 0 || vk > 254) return 0;
    g_hkVk = vk;
    g_hkMods = mods;
    return 1;
}

static void SaveConfig(void)
{
    HKEY k;
    DWORD done = 1;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, CFG_KEY, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL)
        != ERROR_SUCCESS)
        return;
    RegSetValueExW(k, L"HotkeyVk", 0, REG_DWORD, (const BYTE *)&g_hkVk, sizeof g_hkVk);
    RegSetValueExW(k, L"HotkeyMods", 0, REG_DWORD, (const BYTE *)&g_hkMods, sizeof g_hkMods);
    RegSetValueExW(k, L"LangA", 0, REG_DWORD, (const BYTE *)&g_langA, sizeof g_langA);
    RegSetValueExW(k, L"LangB", 0, REG_DWORD, (const BYTE *)&g_langB, sizeof g_langB);
    {
        DWORD up = g_autoUpdate ? 1 : 0;
        RegSetValueExW(k, L"AutoUpdate", 0, REG_DWORD, (const BYTE *)&up, sizeof up);
    }
    RegSetValueExW(k, L"Configured", 0, REG_DWORD, (const BYTE *)&done, sizeof done);
    RegCloseKey(k);
}

/* ---------- tray ---------- */
static int StartupEnabled(void)
{
    HKEY k;
    int on = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        on = RegQueryValueExW(k, RUN_VALUE, NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
        RegCloseKey(k);
    }
    return on;
}

static void SetStartup(int on)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL)
        != ERROR_SUCCESS)
        return;
    if (on) {
        WCHAR path[MAX_PATH], val[MAX_PATH + 3];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        wsprintfW(val, L"\"%s\"", path);
        RegSetValueExW(k, RUN_VALUE, 0, REG_SZ, (const BYTE *)val,
                       (DWORD)((lstrlenW(val) + 1) * sizeof(WCHAR)));
    } else {
        RegDeleteValueW(k, RUN_VALUE);
    }
    RegCloseKey(k);
}

static HICON MakeIcon(int paused)
{
    int size = GetSystemMetrics(SM_CXSMICON);
    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, size, size);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, NULL);
    HGDIOBJ old = SelectObject(dc, color);
    RECT r = {0, 0, size, size};
    HBRUSH bg = CreateSolidBrush(paused ? RGB(120, 120, 120) : RGB(30, 90, 200));
    HFONT font = CreateFontW(-(size - 2), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                             CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HGDIOBJ oldFont;
    ICONINFO ii;
    HICON icon;

    FillRect(dc, &r, bg);
    oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, L"\x05D0", 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
    SelectObject(dc, old);

    memset(&ii, 0, sizeof ii);
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    icon = CreateIconIndirect(&ii);

    DeleteObject(font);
    DeleteObject(bg);
    DeleteObject(color);
    DeleteObject(mask);
    DeleteDC(dc);
    ReleaseDC(NULL, screen);
    return icon;
}

static void UpdateTray(DWORD action)
{
    HICON previous = g_nid.hIcon;
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_wnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = MakeIcon(g_paused);
    {
        WCHAR name[64];
        HotkeyName(g_hkVk, g_hkMods, name);
        wsprintfW(g_nid.szTip, L"%s - %s%s", APP_TITLE, name, g_paused ? L" (paused)" : L"");
    }
    Shell_NotifyIconW(action, &g_nid);
    if (previous) DestroyIcon(previous);
}

static void ShowMenu(void)
{
    POINT p;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, APP_TITLE);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    if (g_updateVer[0]) {
        WCHAR label[64];
        wsprintfW(label, L"Install update v%s", g_updateVer);
        AppendMenuW(m, MF_STRING | (g_installing ? MF_GRAYED : 0), ID_UPDATE, label);
    }
    {
        WCHAR name[64], label[96];
        HotkeyName(g_hkVk, g_hkMods, name);
        wsprintfW(label, L"Change hotkey (now %s)...", name);
        AppendMenuW(m, MF_STRING, ID_HOTKEY, label);
    }
    AppendMenuW(m, MF_STRING | (g_paused ? MF_CHECKED : 0), ID_PAUSE, L"Pause");
    AppendMenuW(m, MF_STRING | (StartupEnabled() ? MF_CHECKED : 0), ID_STARTUP, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    GetCursorPos(&p);
    SetForegroundWindow(g_wnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, g_wnd, NULL);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

/* ---------- updates (only when the user opted in) ---------- */
static int VersionNumber(const char *v)
{
    int part[3] = {0, 0, 0}, i = 0;
    if (*v == 'v' || *v == 'V') v++;
    for (; *v && i < 3; v++) {
        /* Each part is capped, so an absurd version number can't overflow. */
        if (*v >= '0' && *v <= '9') { if (part[i] < 100) part[i] = part[i] * 10 + (*v - '0'); }
        else if (*v == '.') i++;
        else break;
    }
    return part[0] * 1000000 + part[1] * 1000 + part[2];
}

/* Copies the string value that follows key, starting the search at from.
   Returns the position after the value, or NULL. */
static const char *JsonValue(const char *from, const char *key, char *out, size_t size)
{
    const char *p = strstr(from, key), *q;
    size_t n;
    if (!p) return NULL;
    p += strlen(key);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return NULL;
    q = strchr(++p, '"');
    if (!q) return NULL;
    n = (size_t)(q - p);
    if (n >= size) n = size - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return q;
}

/* Finds the version, the program's download link and its published fingerprint. */
static int ParseRelease(const char *json, char *tag, size_t tagSize, char *url, size_t urlSize,
                        char *digest, size_t digestSize)
{
    const char *assetStart = json, *p = json, *next;
    digest[0] = 0;
    if (!JsonValue(json, "\"tag_name\"", tag, tagSize)) return 0;
    while ((next = JsonValue(p, "\"browser_download_url\"", url, urlSize)) != NULL) {
        size_t n = strlen(url);
        if (n > 4 && !_stricmp(url + n - 4, ".exe")) {
            /* The fingerprint comes earlier in the same asset's entry. */
            const char *d = strstr(assetStart, "\"digest\"");
            if (d && d < next) JsonValue(d, "\"digest\"", digest, digestSize);
            return 1;
        }
        p = assetStart = next;
    }
    return 0;
}

/* Writes "sha256:" plus lowercase hex. Returns 0 on failure. */
static int Sha256Text(const char *data, DWORD len, char *out, size_t size)
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    UCHAR digest[32];
    int ok = 0, i;
    if (size < 72) return 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) return 0;
    if (BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) == 0) {
        if (BCryptHashData(hash, (PUCHAR)data, len, 0) == 0 &&
            BCryptFinishHash(hash, digest, 32, 0) == 0)
            ok = 1;
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return 0;
    memcpy(out, "sha256:", 7);
    for (i = 0; i < 32; i++) {
        static const char hex[] = "0123456789abcdef";
        out[7 + i * 2] = hex[digest[i] >> 4];
        out[8 + i * 2] = hex[digest[i] & 15];
    }
    out[71] = 0;
    return 1;
}

static char *HttpGet(const WCHAR *url, DWORD *lenOut)
{
    URL_COMPONENTS c;
    WCHAR host[256], path[1024];
    HINTERNET ses, con = NULL, req = NULL;
    char *buf = NULL;
    DWORD len = 0, status = 0, size = sizeof status, got;
    int ok = 0;

    memset(&c, 0, sizeof c);
    c.dwStructSize = sizeof c;
    c.lpszHostName = host; c.dwHostNameLength = 256;
    c.lpszUrlPath = path;  c.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(url, 0, 0, &c) || c.nScheme != INTERNET_SCHEME_HTTPS) return NULL;

    ses = WinHttpOpen(L"FlipKey/" APP_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) ses = WinHttpOpen(L"FlipKey/" APP_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return NULL;
    con = WinHttpConnect(ses, host, c.nPort, 0);
    if (con) req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) &&
        WinHttpReceiveResponse(req, NULL) &&
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, NULL) &&
        status == 200 && (buf = (char *)malloc(MAX_DOWNLOAD + 1)) != NULL) {
        ok = 1;
        for (;;) {
            if (!WinHttpReadData(req, buf + len, MAX_DOWNLOAD - len, &got)) { ok = 0; break; }
            if (got == 0) break;
            len += got;
            if (len >= MAX_DOWNLOAD) { ok = 0; break; }
        }
    }
    if (ok) {
        /* A connection cut halfway must never be mistaken for a complete file. */
        DWORD expected = 0;
        size = sizeof expected;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &expected, &size, NULL) &&
            expected != len)
            ok = 0;
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    if (!ok) { free(buf); return NULL; }
    buf[len] = 0;
    *lenOut = len;
    return buf;
}

static void CheckForUpdate(void)
{
    char tag[32], url[512];
    DWORD len;
    char *json = HttpGet(L"https://api.github.com" RELEASE_PATH, &len);
    if (!json) return;
    if (ParseRelease(json, tag, sizeof tag, url, sizeof url, g_updateHash, sizeof g_updateHash) &&
        g_updateHash[0] && VersionNumber(tag) > VersionNumber(APP_VER) &&
        MultiByteToWideChar(CP_UTF8, 0, url, -1, g_updateUrl, 512)) {
        const char *t = (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag;
        MultiByteToWideChar(CP_UTF8, 0, t, -1, g_updateVer, 32);
        PostMessageW(g_wnd, WM_UPDATE_FOUND, 0, 0);
    }
    free(json);
}

static DWORD WINAPI UpdateThread(LPVOID unused)
{
    int minutes = CHECK_EVERY_MIN;
    (void)unused;
    /* Nothing touches the network until the setting is on. */
    for (;;) {
        Sleep(60 * 1000);
        if (minutes < CHECK_EVERY_MIN) minutes++;
        if (g_autoUpdate && minutes >= CHECK_EVERY_MIN && !g_updateVer[0]) {
            minutes = 0;
            CheckForUpdate();
        }
    }
    return 0;
}

/* Swaps the running program for the new one; Windows allows renaming a running file. */
static int InstallBytes(const WCHAR *exe, const char *data, DWORD len)
{
    WCHAR fresh[MAX_PATH + 8], old[MAX_PATH + 8], cmd[MAX_PATH + 16];
    HANDLE f;
    DWORD written = 0;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    if (len < 1024 || data[0] != 'M' || data[1] != 'Z') return 0;
    wsprintfW(fresh, L"%s.new", exe);
    wsprintfW(old, L"%s.old", exe);
    f = CreateFileW(fresh, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    WriteFile(f, data, len, &written, NULL);
    CloseHandle(f);
    if (written != len) { DeleteFileW(fresh); return 0; }

    DeleteFileW(old);
    if (!MoveFileW(exe, old)) { DeleteFileW(fresh); return 0; }
    if (!MoveFileW(fresh, exe)) { MoveFileW(old, exe); DeleteFileW(fresh); return 0; }

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    wsprintfW(cmd, L"\"%s\" --updated", exe);
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        DeleteFileW(exe);
        MoveFileW(old, exe);
        return 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

static DWORD WINAPI InstallThread(LPVOID unused)
{
    WCHAR exe[MAX_PATH];
    DWORD len = 0;
    char *data;
    int ok = 0;
    (void)unused;
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    data = HttpGet(g_updateUrl, &len);
    if (data) {
        char actual[80];
        /* No fingerprint or a different one: the file is not installed. */
        if (g_updateHash[0] && Sha256Text(data, len, actual, sizeof actual) &&
            !_stricmp(actual, g_updateHash))
            ok = InstallBytes(exe, data, len);
        free(data);
    }
    if (ok) {
        PostMessageW(g_wnd, WM_COMMAND, ID_EXIT, 0);
    } else {
        InterlockedExchange(&g_installing, 0);
        MessageBoxW(NULL, L"The update could not be installed. FlipKey will keep working as it is.",
                    APP_TITLE, MB_OK | MB_ICONWARNING);
    }
    return 0;
}

static void OfferUpdate(void)
{
    WCHAR text[128];
    if (!g_updateVer[0] || g_installing) return;
    wsprintfW(text, L"Install FlipKey v%s now?\nFlipKey will restart by itself.", g_updateVer);
    if (MessageBoxW(NULL, text, APP_TITLE, MB_YESNO | MB_ICONQUESTION) == IDYES) {
        InterlockedExchange(&g_installing, 1);
        CreateThread(NULL, 0, InstallThread, NULL, 0, NULL);
    }
}

static void ShowUpdateBalloon(void)
{
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    wsprintfW(n.szInfoTitle, L"FlipKey v%s is available", g_updateVer);
    lstrcpyW(n.szInfo, L"Click here to install it.");
    n.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

/* ---------- settings window ---------- */
static void ApplyHotkey(void)
{
    if (!g_paused && !RegisterHotkeys()) {
        WCHAR name[64], text[160];
        HotkeyName(g_hkVk, g_hkMods, name);
        wsprintfW(text, L"Another program is already using %s.\n"
                        L"Close it, or click the tray icon and choose another key.", name);
        MessageBoxW(NULL, text, APP_TITLE, MB_OK | MB_ICONWARNING);
    }
}

enum { IDC_F10 = 2001, IDC_CTRLL, IDC_CUSTOM, IDC_KEYTEXT, IDC_SET, IDC_STARTUP, IDC_WORD,
       IDC_ERROR, IDC_NOTE, IDC_UPDATES, IDC_LANGA, IDC_LANGB };
static DWORD  g_tmpVk, g_tmpMods;
static int    g_dpi = 96;
static HFONT  g_font, g_fontBold;
static HBRUSH g_white;

static int Px(int v) { return MulDiv(v, g_dpi, 96); }

static HWND AddCtl(HWND parent, const WCHAR *cls, const WCHAR *text, DWORD style,
                   int x, int y, int w, int h, int id, HFONT font)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, Px(x), Px(y), Px(w),
                             Px(h), parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

static int SelectedHotkey(DWORD *vk, DWORD *mods)
{
    if (IsDlgButtonChecked(g_settings, IDC_F10) == BST_CHECKED) {
        *vk = VK_F10; *mods = 0;
    } else if (IsDlgButtonChecked(g_settings, IDC_CTRLL) == BST_CHECKED) {
        *vk = 'L'; *mods = MOD_CONTROL;
    } else {
        *vk = g_tmpVk; *mods = g_tmpMods;
    }
    return *vk != 0;
}

static void RefreshSettings(void)
{
    WCHAR name[64], text[96];
    DWORD vk, mods, word;
    if (!g_capturing) {
        if (g_tmpVk) HotkeyName(g_tmpVk, g_tmpMods, name);
        else lstrcpyW(name, L"Not set yet");
        SetDlgItemTextW(g_settings, IDC_KEYTEXT, name);
    }
    text[0] = 0;
    if (SelectedHotkey(&vk, &mods)) {
        word = WordMods(mods);
        if (word) {
            HotkeyName(vk, word, name);
            wsprintfW(text, L"To flip only the last word: %s", name);
        } else {
            lstrcpyW(text, L"This key has no one-word shortcut.");
        }
    }
    SetDlgItemTextW(g_settings, IDC_WORD, text);
}

static void StopCapture(void)
{
    RemoveCaptureHook();
    SetDlgItemTextW(g_settings, IDC_SET, L"Set key...");
}

static LRESULT CALLBACK SettingsProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkMode((HDC)w, TRANSPARENT);
        if (GetDlgCtrlID((HWND)l) == IDC_ERROR) SetTextColor((HDC)w, RGB(190, 30, 30));
        else if (GetDlgCtrlID((HWND)l) == IDC_NOTE || GetDlgCtrlID((HWND)l) == IDC_WORD)
            SetTextColor((HDC)w, RGB(100, 100, 100));
        return (LRESULT)g_white;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && g_capturing) {
            StopCapture();
            RefreshSettings();
        }
        break;
    case WM_CAPTURED: {
        const WCHAR *problem;
        if (!g_capturing) return 0;
        if (g_capVk == VK_ESCAPE && g_capMods == 0) {
            StopCapture();
            RefreshSettings();
            return 0;
        }
        problem = HotkeyProblem(g_capVk, g_capMods);
        if (problem) {
            SetDlgItemTextW(h, IDC_ERROR, problem);
            return 0; /* keep listening for another try */
        }
        g_tmpVk = g_capVk;
        g_tmpMods = g_capMods;
        SetDlgItemTextW(h, IDC_ERROR, L"");
        StopCapture();
        RefreshSettings();
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_F10:
        case IDC_CTRLL:
        case IDC_CUSTOM:
            if (HIWORD(w) == BN_CLICKED) {
                if (g_capturing) StopCapture();
                SetDlgItemTextW(h, IDC_ERROR, L"");
                RefreshSettings();
            }
            return 0;
        case IDC_SET:
            if (g_capturing) {
                StopCapture();
                RefreshSettings();
                return 0;
            }
            CheckRadioButton(h, IDC_F10, IDC_CUSTOM, IDC_CUSTOM);
            SetDlgItemTextW(h, IDC_ERROR, L"");
            SetDlgItemTextW(h, IDC_KEYTEXT, L"Press your keys now...");
            SetDlgItemTextW(h, IDC_SET, L"Cancel");
            RefreshSettings();
            memset((void *)g_phys, 0, sizeof g_phys);
            g_captureHook = SetWindowsHookExW(WH_KEYBOARD_LL, CaptureProc,
                                              GetModuleHandleW(NULL), 0);
            InterlockedExchange(&g_capturing, 1);
            return 0;
        case IDOK: {
            DWORD vk, mods;
            HKL a = (HKL)SendDlgItemMessageW(h, IDC_LANGA, CB_GETITEMDATA,
                        SendDlgItemMessageW(h, IDC_LANGA, CB_GETCURSEL, 0, 0), 0);
            HKL b = (HKL)SendDlgItemMessageW(h, IDC_LANGB, CB_GETITEMDATA,
                        SendDlgItemMessageW(h, IDC_LANGB, CB_GETCURSEL, 0, 0), 0);
            if (g_capturing) return 0;
            if (a == b || a == (HKL)(LONG_PTR)CB_ERR || b == (HKL)(LONG_PTR)CB_ERR) {
                SetDlgItemTextW(h, IDC_ERROR, L"Pick two different languages.");
                return 0;
            }
            if (!SelectedHotkey(&vk, &mods)) {
                SetDlgItemTextW(h, IDC_ERROR, L"Press \"Set key...\" and choose your keys first.");
                return 0;
            }
            g_hkVk = vk;
            g_hkMods = mods;
            g_hkl[SIDE_A] = a;
            g_hkl[SIDE_B] = b;
            g_langA = LOWORD((UINT_PTR)a);
            g_langB = LOWORD((UINT_PTR)b);
            BuildTables();
            g_undoAfter[0] = 0;
            InterlockedExchange(&g_autoUpdate,
                                IsDlgButtonChecked(h, IDC_UPDATES) == BST_CHECKED);
            SaveConfig();
            SetStartup(IsDlgButtonChecked(h, IDC_STARTUP) == BST_CHECKED);
            UpdateTray(NIM_MODIFY);
            DestroyWindow(h);
            return 0;
        }
        case IDCANCEL:
            if (g_capturing) { StopCapture(); RefreshSettings(); return 0; }
            DestroyWindow(h);
            return 0;
        }
        break;
    case WM_CLOSE:
        RemoveCaptureHook();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        RemoveCaptureHook();
        g_settings = NULL;
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

static void FillLanguages(int id, HKL current)
{
    HKL list[64];
    int i, n = GetKeyboardLayoutList(64, list);
    for (i = 0; i < n; i++) {
        WCHAR name[64];
        LRESULT at;
        if (!GetLocaleInfoW(MAKELCID(LOWORD((UINT_PTR)list[i]), SORT_DEFAULT),
                            LOCALE_SENGLANGUAGE, name, 64))
            lstrcpyW(name, L"Unknown");
        at = SendDlgItemMessageW(g_settings, id, CB_ADDSTRING, 0, (LPARAM)name);
        SendDlgItemMessageW(g_settings, id, CB_SETITEMDATA, at, (LPARAM)list[i]);
    }
    for (i = 0; i < n; i++)
        if ((HKL)SendDlgItemMessageW(g_settings, id, CB_GETITEMDATA, i, 0) == current)
            SendDlgItemMessageW(g_settings, id, CB_SETCURSEL, i, 0);
}

/* Blocks until the window closes; other messages keep being handled inside this loop. */
static void RunSettings(int firstRun)
{
    static int registered;
    WNDCLASSW wc;
    NONCLIENTMETRICSW ncm;
    LOGFONTW bold;
    HDC dc;
    RECT r = {0, 0, 0, 0};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    MSG m;
    HINSTANCE inst = GetModuleHandleW(NULL);
    int cw = 400, ch = 388, sel;

    if (g_settings) {
        SetForegroundWindow(g_settings);
        return;
    }
    dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);

    memset(&ncm, 0, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    if (!g_font) {
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
        bold = ncm.lfMessageFont;
        bold.lfWeight = FW_SEMIBOLD;
        bold.lfHeight = MulDiv(bold.lfHeight, 13, 9);
        g_fontBold = CreateFontIndirectW(&bold);
        g_white = CreateSolidBrush(RGB(255, 255, 255));
    }
    if (!registered) {
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = SettingsProc;
        wc.hInstance = inst;
        wc.hbrBackground = g_white;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.lpszClassName = L"FlipKeySettings";
        RegisterClassW(&wc);
        registered = 1;
    }

    r.right = Px(cw);
    r.bottom = Px(ch);
    AdjustWindowRect(&r, style, FALSE);
    g_settings = CreateWindowExW(0, L"FlipKeySettings", APP_TITLE, style,
        (GetSystemMetrics(SM_CXSCREEN) - (r.right - r.left)) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - (r.bottom - r.top)) / 2,
        r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);

    AddCtl(g_settings, L"STATIC", L"Choose your flip key", 0, 24, 20, 352, 28, 0, g_fontBold);
    AddCtl(g_settings, L"STATIC", L"Press it after typing in the wrong language.", 0,
           24, 50, 352, 20, IDC_NOTE, g_font);
    AddCtl(g_settings, L"STATIC", L"Flip between", SS_CENTERIMAGE, 24, 84, 84, 26, 0, g_font);
    AddCtl(g_settings, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_GROUP | WS_TABSTOP,
           112, 84, 124, 240, IDC_LANGA, g_font);
    AddCtl(g_settings, L"STATIC", L"and", SS_CENTER | SS_CENTERIMAGE, 236, 84, 32, 26, 0, g_font);
    AddCtl(g_settings, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
           268, 84, 108, 240, IDC_LANGB, g_font);
    AddCtl(g_settings, L"BUTTON", L"F10", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP,
                   24, 128, 340, 24, IDC_F10, g_font);
    AddCtl(g_settings, L"BUTTON", L"Ctrl + L", BS_AUTORADIOBUTTON | WS_TABSTOP,
           24, 156, 340, 24, IDC_CTRLL, g_font);
    AddCtl(g_settings, L"BUTTON", L"My own:", BS_AUTORADIOBUTTON | WS_TABSTOP,
           24, 184, 80, 24, IDC_CUSTOM, g_font);
    AddCtl(g_settings, L"STATIC", L"", SS_CENTERIMAGE, 108, 184, 172, 24, IDC_KEYTEXT, g_font);
    AddCtl(g_settings, L"BUTTON", L"Set key...", BS_PUSHBUTTON | WS_GROUP | WS_TABSTOP,
           288, 183, 88, 26, IDC_SET, g_font);
    AddCtl(g_settings, L"STATIC", L"", 0, 24, 216, 352, 20, IDC_ERROR, g_font);
    AddCtl(g_settings, L"STATIC", L"", 0, 24, 240, 352, 20, IDC_WORD, g_font);
    AddCtl(g_settings, L"BUTTON", L"Start FlipKey automatically with Windows",
           BS_AUTOCHECKBOX | WS_GROUP | WS_TABSTOP, 24, 276, 352, 24, IDC_STARTUP, g_font);
    AddCtl(g_settings, L"BUTTON", L"Check for updates at startup and daily",
           BS_AUTOCHECKBOX | WS_TABSTOP, 24, 302, 352, 24, IDC_UPDATES, g_font);
    AddCtl(g_settings, L"BUTTON", L"Save", BS_DEFPUSHBUTTON | WS_GROUP | WS_TABSTOP,
           192, 336, 88, 30, IDOK, g_font);
    AddCtl(g_settings, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP,
           288, 336, 88, 30, IDCANCEL, g_font);

    FillLanguages(IDC_LANGA, g_hkl[SIDE_A]);
    FillLanguages(IDC_LANGB, g_hkl[SIDE_B]);
    g_tmpVk = 0;
    g_tmpMods = 0;
    if (g_hkVk == VK_F10 && g_hkMods == 0) sel = IDC_F10;
    else if (g_hkVk == 'L' && g_hkMods == MOD_CONTROL) sel = IDC_CTRLL;
    else { sel = IDC_CUSTOM; g_tmpVk = g_hkVk; g_tmpMods = g_hkMods; }
    CheckRadioButton(g_settings, IDC_F10, IDC_CUSTOM, sel);
    CheckDlgButton(g_settings, IDC_STARTUP,
                   (firstRun || StartupEnabled()) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_settings, IDC_UPDATES, g_autoUpdate ? BST_CHECKED : BST_UNCHECKED);
    RefreshSettings();

    UnregisterHotkeys(); /* so the current key can be pressed while choosing */
    ShowWindow(g_settings, SW_SHOW);
    SetForegroundWindow(g_settings);

    while (g_settings) {
        int got = GetMessageW(&m, NULL, 0, 0);
        if (got <= 0) {
            if (got == 0) PostQuitMessage((int)m.wParam);
            break;
        }
        if (!IsDialogMessageW(g_settings, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    /* Closing the first-run window keeps F10 and does not ask again. */
    if (firstRun) SaveConfig();
    ApplyHotkey();
    if (g_wordTaken && !g_paused)
        MessageBoxW(NULL, L"Another program is using the one-word shortcut, so it won't work.\n"
                          L"Your main key works normally.", APP_TITLE, MB_OK | MB_ICONWARNING);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == g_taskbarCreated && msg != 0) {
        UpdateTray(NIM_ADD);
        return 0;
    }
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(l) == NIN_BALLOONUSERCLICK) {
            OfferUpdate();
            return 0;
        }
        if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_LBUTTONUP) {
            if (g_settings) SetForegroundWindow(g_settings);
            else ShowMenu();
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_UPDATE:
            OfferUpdate();
            break;
        case ID_HOTKEY:
            RunSettings(0);
            break;
        case ID_PAUSE:
            InterlockedExchange(&g_paused, !g_paused);
            if (g_paused) UnregisterHotkeys();
            else ApplyHotkey();
            UpdateTray(NIM_MODIFY);
            break;
        case ID_STARTUP:
            SetStartup(!StartupEnabled());
            break;
        case ID_EXIT:
            DestroyWindow(h);
            break;
        }
        return 0;
    case WM_UPDATE_FOUND:
        ShowUpdateBalloon();
        return 0;
    case WM_HOTKEY:
        if (w == HOTKEY_ALL || w == HOTKEY_WORD) {
            InterlockedExchange(&g_mode, w == HOTKEY_ALL ? MODE_ALL : MODE_WORD);
            SetEvent(g_flipEvent);
        }
        return 0;
    case WM_DESTROY:
        UnregisterHotkeys();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSW wc;
    MSG m;
    HANDLE single;
    int configured;
    (void)prev; (void)show;

    if (strstr(cmd, "--updated")) {
        /* The previous version is still closing; wait for it, then remove its file. */
        WCHAR exe[MAX_PATH], old[MAX_PATH + 8];
        int i;
        for (i = 0; i < 100; i++) {
            single = CreateMutexW(NULL, TRUE, L"FlipKey.SingleInstance");
            if (single && GetLastError() != ERROR_ALREADY_EXISTS) break;
            if (single) CloseHandle(single);
            Sleep(100);
        }
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        wsprintfW(old, L"%s.old", exe);
        DeleteFileW(old);
    }
    single = CreateMutexW(NULL, TRUE, L"FlipKey.SingleInstance");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS && !strstr(cmd, "--updated")) {
        MessageBoxW(NULL, L"FlipKey is already running in the system tray.", APP_TITLE,
                    MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    configured = LoadConfig();
    FindLayouts();
    if (!g_hkl[SIDE_A] || !g_hkl[SIDE_B]) {
        MessageBoxW(NULL, L"FlipKey needs two keyboard languages installed in Windows.",
                    APP_TITLE, MB_OK | MB_ICONERROR);
        return 1;
    }
    BuildTables();
    if (StartupEnabled()) SetStartup(1); /* keeps the path right if the file moved */

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"FlipKeyWindow";
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, APP_TITLE, 0, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    g_flipEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    CreateThread(NULL, 0, FlipWorker, NULL, 0, NULL);
    UpdateTray(NIM_ADD);
    CreateThread(NULL, 0, UpdateThread, NULL, 0, NULL);
    if (configured) ApplyHotkey();
    else RunSettings(1);

    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
