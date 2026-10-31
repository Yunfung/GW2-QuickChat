///----------------------------------------------------------------------------------------------------
/// GW2 快捷喊话 — Nexus 插件
/// 在游戏内显示预设句子面板，鼠标点击或快捷键直接发送到多个频道。
///----------------------------------------------------------------------------------------------------

#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <Windows.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nexus/Nexus.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#pragma comment(lib, "user32.lib")

/* proto */
void AddonLoad(AddonAPI_t* aApi);
void AddonUnload();
void RenderOptions();
void OnInputBind(const char* aIdentifier, bool aIsRelease);
static void MoveSlotTo(int from, int to);
static void SortSlotsByPage(int* follow);
static void MoveSlotToPage(int index, int page);

/* globals */
static AddonDefinition_t AddonDef = {};
static AddonAPI_t* APIDefs = nullptr;
static NexusLinkData_t* NexusLink = nullptr;
static HMODULE hSelf = nullptr;

struct ChannelDef
{
    const char* code;
    const char* name;
};

static const ChannelDef kChannels[] =
{
    { "g",  "当前公会" },
    { "g1", "公会1" },
    { "g2", "公会2" },
    { "g3", "公会3" },
    { "g4", "公会4" },
    { "g5", "公会5" },
    { "g6", "公会6" },
    { "m",  "地图" },
    { "p",  "队伍" },
    { "t",  "战队" },
    { "d",  "团队" },
    { "s",  "说" }
};
static const int kChannelCount = (int)(sizeof(kChannels) / sizeof(kChannels[0]));

static const char* kDefaultText[10] =
{
    "你好！", "谢谢！", "辛苦了~", "集合！", "快跑！",
    "有人组队吗？", "稍等，马上到", "注意脚下！", "打这个目标", "GG"
};

static const int kDefaultSlotCount = 10;
static const int kMaxSlots = 100000; // 实际上不限制，仅防解析异常数据

static std::string BindId(int index)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "QUICKCHAT_SLOT_%03d", index + 1);
    return buf;
}

static std::string ChannelLabel(const std::vector<std::string>& list)
{
    if (list.empty()) return "默认";
    std::string out = "/" + list[0];
    if (list.size() > 1)
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "+%d", (int)list.size() - 1);
        out += buf;
    }
    return out;
}

struct SlotCfg
{
    bool enabled = true;
    int page = 1;
    std::string text;
    std::vector<std::string> channels;
};

struct AddonCfg
{
    std::string prefix;
    int openDelay = 180;
    int charGap = 20;
    int multiGap = 400;
    bool useClipboard = true;
    bool useMessages = true;
    int textMode = 0;
    bool showPanel = true;
    int alphaPct = 0;
    int btnAlphaPct = 0;
    int listAlphaPct = 100;
    int layoutMode = 0;
    int gridCols = 3;
    int pageCount = 1;
    int pageLayout = 0;
    float pageBtnW = 0.0f;
    float pageBtnH = 0.0f;
    float pageSelColor[3] = { 1.0f, 0.62f, 0.12f };
    bool gridByColumn = false;
    float btnW = 0.0f;
    float btnH = 0.0f;
    float fontScale = 1.0f;
    bool btnColorSet = false;
    float btnColor[3] = { 0.26f, 0.34f, 0.46f };
    bool textColorSet = false;
    float textColor[3] = { 1.0f, 1.0f, 1.0f };
    bool lockPos = false;
    bool hideChrome = false;
    std::vector<SlotCfg> slots;
};

static AddonCfg gCfg;
static std::string gCfgPath;
static int gSaveHint = 0;
static std::string gStatus;
static DWORD gStatusUntil = 0;
static bool gWantKb = false;
static bool gReleaseFocus = false;
static int gEditSlot = -1;
static int gPanelPage = 1;
static int gPanelDropPage = 0;
static int gChannelMenuSlot = -1;
static ImVec2 gChannelMenuPos(0, 0);
static float gChannelMenuPivotY = 0.0f;
static ImVec4 gChannelMenuRect(0, 0, 0, 0);
static ImVec4 gPanelRectPrev(0, 0, 0, 0);
static bool gLBtnPrev = false;
static bool gRBtnPrev = false;
static bool gChannelMenuHovered = false;
static bool gChannelMenuHoveredPrev = false;
static std::vector<ImVec4> gHitRects;
static std::vector<ImVec4> gHitRectsPrev;

static void RecordHitRect()
{
    ImVec2 a = ImGui::GetItemRectMin();
    ImVec2 b = ImGui::GetItemRectMax();
    gHitRects.push_back(ImVec4(a.x, a.y, b.x, b.y));
}
static ImFont* gBigFont = nullptr;
static float gBigRatio = 1.0f;
static bool gUseMessages = true;

/* sending queue */
static std::mutex gQueueMx;
static std::condition_variable gQueueCv;
static std::deque<std::vector<std::string> > gQueue;
static std::atomic<bool> gWorkerStop(false);
static std::thread gWorker;

///----------------------------------------------------------------------------------------------------
/// 工具函数
///----------------------------------------------------------------------------------------------------
// 缺中文字形的字体就直接不用，避免出现 ??? 方块
static bool FontHasCjk(void* aFont)
{
    if (!aFont) return false;
    ImFont* font = (ImFont*)aFont;
    const ImFontGlyph* glyph = font->FindGlyph((ImWchar)0x6D4B); // 测
    return glyph && glyph->Codepoint == (ImWchar)0x6D4B;
}

static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static bool CopyFileUtf8(const std::string& src, const std::string& dst)
{
    return CopyFileW(Utf8ToWide(src).c_str(), Utf8ToWide(dst).c_str(), FALSE) != 0;
}

static bool PathExistsUtf8(const std::string& p)
{
    return GetFileAttributesW(Utf8ToWide(p).c_str()) != INVALID_FILE_ATTRIBUTES;
}

static bool PathIsDirUtf8(const std::string& p)
{
    DWORD a = GetFileAttributesW(Utf8ToWide(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static void MakeDirUtf8(const std::string& p)
{
    CreateDirectoryW(Utf8ToWide(p).c_str(), nullptr);
}

static bool DeleteFileUtf8(const std::string& p)
{
    return DeleteFileW(Utf8ToWide(p).c_str()) != 0;
}


static std::string TrimRight(const std::string& s)
{
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == '\r' || s[e - 1] == '\n' || s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
    return s.substr(0, e);
}

static std::string EscapeText(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++)
    {
        char c = s[i];
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

static std::string UnescapeText(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++)
    {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size())
        {
            char n = s[++i];
            if (n == 'n') out += '\n';
            else if (n == 'r') out += '\r';
            else if (n == '\\') out += '\\';
            else { out += '\\'; out += n; }
        }
        else out += c;
    }
    return out;
}

static bool HasChannel(const std::vector<std::string>& list, const char* code)
{
    for (size_t i = 0; i < list.size(); i++)
        if (list[i] == code) return true;
    return false;
}

static bool IsKnownChannel(const std::string& code)
{
    for (int i = 0; i < kChannelCount; i++)
        if (code == kChannels[i].code) return true;
    return false;
}

static std::string ChannelSummary(const std::vector<std::string>& list)
{
    std::string out;
    for (size_t i = 0; i < list.size(); i++)
    {
        if (!out.empty()) out += " ";
        out += "/";
        out += list[i];
    }
    return out;
}

static void SetChannel(std::vector<std::string>& list, const char* code, bool on)
{
    std::vector<std::string> next;
    for (int i = 0; i < kChannelCount; i++)
    {
        std::string c = kChannels[i].code;
        bool keep = (c == code) ? on : HasChannel(list, c.c_str());
        if (keep) next.push_back(c);
    }
    list = next;
}

///----------------------------------------------------------------------------------------------------
/// 键鼠模拟
///----------------------------------------------------------------------------------------------------
static void SendKeyDown(WORD vk)
{
    INPUT in = {};
    in.type = INPUT_KEYBOARD;

    WORD sc = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (sc)
    {
        in.ki.wScan = sc;
        in.ki.dwFlags = KEYEVENTF_SCANCODE;
    }
    else
    {
        in.ki.wVk = vk;
    }
    SendInput(1, &in, sizeof(in));
}

static void SendKeyUp(WORD vk)
{
    INPUT in = {};
    in.type = INPUT_KEYBOARD;

    WORD sc = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (sc)
    {
        in.ki.wScan = sc;
        in.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    }
    else
    {
        in.ki.wVk = vk;
        in.ki.dwFlags = KEYEVENTF_KEYUP;
    }
    SendInput(1, &in, sizeof(in));
}

static void PressKey(WORD vk)
{
    SendKeyDown(vk);
    Sleep(12);
    SendKeyUp(vk);
}

static void SendCtrlV()
{
    SendKeyDown(VK_CONTROL);
    Sleep(10);
    SendKeyDown('V');
    Sleep(10);
    SendKeyUp('V');
    Sleep(10);
    SendKeyUp(VK_CONTROL);
}

static void TypeText(const std::wstring& text, int gapMs)
{
    for (size_t i = 0; i < text.size(); i++)
    {
        INPUT down = {};
        down.type = INPUT_KEYBOARD;
        down.ki.wScan = (WORD)text[i];
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        SendInput(1, &down, sizeof(down));

        INPUT up = down;
        up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(1, &up, sizeof(up));

        Sleep(std::max(0, gapMs));
    }
}

///----------------------------------------------------------------------------------------------------
/// 剪贴板
///----------------------------------------------------------------------------------------------------
static bool ClipboardSet(const std::wstring& text)
{
    for (int attempt = 0; attempt < 6; attempt++)
    {
        if (!OpenClipboard(nullptr))
        {
            Sleep(40);
            continue;
        }

        bool ok = false;
        if (EmptyClipboard())
        {
            size_t bytes = (text.size() + 1) * sizeof(wchar_t);
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (h)
            {
                void* p = GlobalLock(h);
                if (p)
                {
                    memcpy(p, text.c_str(), bytes);
                    GlobalUnlock(h);
                    if (SetClipboardData(CF_UNICODETEXT, h) != nullptr)
                    {
                        ok = true;
                        h = nullptr;
                    }
                }
                if (h) GlobalFree(h);
            }
        }
        CloseClipboard();
        if (ok) return true;
        Sleep(40);
    }
    return false;
}

static bool ClipboardGet(std::wstring* out)
{
    for (int attempt = 0; attempt < 6; attempt++)
    {
        if (!OpenClipboard(nullptr))
        {
            Sleep(40);
            continue;
        }

        bool ok = false;
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h)
        {
            const wchar_t* p = (const wchar_t*)GlobalLock(h);
            if (p)
            {
                *out = p;
                ok = true;
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
        if (ok) return true;
        Sleep(40);
    }
    return false;
}

///----------------------------------------------------------------------------------------------------
/// 发送逻辑
///----------------------------------------------------------------------------------------------------
static void LogInfo(const std::string& msg)
{
    if (APIDefs && APIDefs->Log) APIDefs->Log(LOGL_INFO, "QuickChat", msg.c_str());
}

static void PostKeyToGame(HWND h, UINT msg, WPARAM vk, bool up)
{
    LONG scan = (LONG)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC) << 16;
    LONG lp = 1 | scan | (up ? (LONG)0xC0000000 : 0);

    if (APIDefs && APIDefs->WndProc_SendToGameOnly)
        APIDefs->WndProc_SendToGameOnly(h, msg, vk, lp);
    else
        PostMessageW(h, msg, vk, lp);
}

static void PostCharToGame(HWND h, wchar_t ch)
{
    if (APIDefs && APIDefs->WndProc_SendToGameOnly)
        APIDefs->WndProc_SendToGameOnly(h, WM_CHAR, (WPARAM)ch, 1);
    else
        PostMessageW(h, WM_CHAR, (WPARAM)ch, 1);
}

static void PostImeChar(HWND h, wchar_t ch)
{
    if (APIDefs && APIDefs->WndProc_SendToGameOnly)
        APIDefs->WndProc_SendToGameOnly(h, WM_IME_CHAR, (WPARAM)ch, 1);
    else
        PostMessageW(h, WM_IME_CHAR, (WPARAM)ch, 1);
}

static void PostTextToGame(HWND h, const std::string& utf8)
{
    std::wstring w = Utf8ToWide(utf8);

    if (gCfg.textMode == 1)
    {
        for (size_t i = 0; i < w.size(); i++)
        {
            PostCharToGame(h, w[i]);
            Sleep(2);
        }
        return;
    }

    if (gCfg.textMode == 2)
    {
        for (size_t i = 0; i < w.size(); i++)
        {
            PostImeChar(h, w[i]);
            Sleep(2);
        }
        return;
    }

    int n = WideCharToMultiByte(936, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return;
    std::string bytes((size_t)n, char(0));
    WideCharToMultiByte(936, 0, w.c_str(), (int)w.size(), &bytes[0], n, nullptr, nullptr);
    for (size_t i = 0; i < bytes.size(); i++)
    {
        PostCharToGame(h, (wchar_t)(unsigned char)bytes[i]);
        Sleep(2);
    }
}

static void SendLinesViaMessages(const std::vector<std::string>& lines)
{
    HWND h = GetForegroundWindow();
    if (!h) return;

    for (size_t i = 0; i < lines.size(); i++)
    {
        if (i > 0) Sleep(std::max(150, gCfg.multiGap));

        PostKeyToGame(h, WM_KEYDOWN, VK_RETURN, false);
        PostKeyToGame(h, WM_KEYUP, VK_RETURN, true);
        Sleep(std::max(60, gCfg.openDelay));

        PostTextToGame(h, lines[i]);
        Sleep(120);

        PostKeyToGame(h, WM_KEYDOWN, VK_RETURN, false);
        PostKeyToGame(h, WM_KEYUP, VK_RETURN, true);
        Sleep(120);

        char buf[256];
        snprintf(buf, sizeof(buf), "消息投递第 %d 行：%s", (int)i + 1, lines[i].c_str());
        LogInfo(buf);
    }
}

static void SendLinesViaInput(const std::vector<std::string>& lines)
{
    std::wstring oldText;
    bool hadOld = ClipboardGet(&oldText);

    {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        DWORD tid = fg ? GetWindowThreadProcessId(fg, &pid) : 0;

        GUITHREADINFO gi = {};
        gi.cbSize = sizeof(gi);
        if (tid) GetGUIThreadInfo(tid, &gi);

        char fgCls[128] = {};
        char focCls[128] = {};
        if (fg) GetClassNameA(fg, fgCls, 127);
        if (gi.hwndFocus) GetClassNameA(gi.hwndFocus, focCls, 127);

        char buf[512];
        snprintf(buf, sizeof(buf),
            "发送开始：行数=%d ImGui捕获键盘=%d 前台=0x%p[%s] 进程=%lu 焦点=0x%p[%s] 本进程=%lu",
            (int)lines.size(), gWantKb ? 1 : 0, fg, fgCls, pid, gi.hwndFocus, focCls, GetCurrentProcessId());
        LogInfo(buf);
    }

    Sleep(150);

    for (size_t i = 0; i < lines.size(); i++)
    {
        if (i > 0) Sleep(std::max(150, gCfg.multiGap));

        PressKey(VK_RETURN);
        Sleep(std::max(60, gCfg.openDelay));

        bool pasted = false;
        if (gCfg.useClipboard) pasted = ClipboardSet(Utf8ToWide(lines[i]));

        if (pasted)
        {
            SendCtrlV();
            Sleep(150);
        }
        else
        {
            TypeText(Utf8ToWide(lines[i]), gCfg.charGap);
        }

        SendKeyDown(VK_RETURN);
        Sleep(12);
        SendKeyUp(VK_RETURN);
        Sleep(80);

        char buf[512];
        snprintf(buf, sizeof(buf), "第 %d 行已注入：%s（剪贴板=%s）", (int)i + 1, lines[i].c_str(), pasted ? "是" : "否");
        LogInfo(buf);
    }

    if (hadOld) ClipboardSet(oldText);
}

static void SendLines(const std::vector<std::string>& lines)
{
    if (gCfg.useMessages && APIDefs && APIDefs->WndProc_SendToGameOnly)
        SendLinesViaMessages(lines);
    else
        SendLinesViaInput(lines);
}

static void WorkerLoop()
{
    for (;;)
    {
        std::vector<std::string> job;
        {
            std::unique_lock<std::mutex> lock(gQueueMx);
            gQueueCv.wait(lock, [] { return gWorkerStop.load() || !gQueue.empty(); });
            if (gWorkerStop.load()) return;
            job = gQueue.front();
            gQueue.pop_front();
        }
        SendLines(job);
    }
}

static std::vector<std::string> BuildLines(int index)
{
    std::vector<std::string> out;
    if (index < 0 || index >= (int)gCfg.slots.size()) return out;

    const SlotCfg& slot = gCfg.slots[index];
    std::string body = slot.text;
    if (body.empty()) return out;

    size_t first = body.find_first_not_of(" \t");
    bool isCommand = first != std::string::npos && body[first] == '/';

    if (isCommand || slot.channels.empty())
    {
        out.push_back(isCommand || gCfg.prefix.empty() ? body : gCfg.prefix + " " + body);
        return out;
    }

    std::string full = gCfg.prefix.empty() ? body : gCfg.prefix + " " + body;
    for (size_t i = 0; i < slot.channels.size(); i++)
        out.push_back("/" + slot.channels[i] + " " + full);
    return out;
}

static void QueueSlot(int index)
{
    if (NexusLink && !NexusLink->IsGameplay) return;

    if (index < 0 || index >= (int)gCfg.slots.size()) return;

    const SlotCfg& slot = gCfg.slots[index];
    std::vector<std::string> lines = BuildLines(index);
    if (lines.empty()) return;

    {
        std::lock_guard<std::mutex> lock(gQueueMx);
        gQueue.push_back(lines);
    }
    gQueueCv.notify_one();

    gStatus = "已发送：" + slot.text;
    std::string summary = ChannelSummary(slot.channels);
    if (!summary.empty()) gStatus += "  →  " + summary;
    gStatusUntil = GetTickCount() + 3000;
    gReleaseFocus = true;

    char buf[256];
    snprintf(buf, sizeof(buf), "排队第 %d 条：%d 行", index + 1, (int)lines.size());
    LogInfo(buf);
}

///----------------------------------------------------------------------------------------------------
/// 配置
///----------------------------------------------------------------------------------------------------
static void InitPaths()
{
    const char* dir = nullptr;
    if (APIDefs && APIDefs->Paths_GetAddonDirectory)
        dir = APIDefs->Paths_GetAddonDirectory("QuickChat");

    if (dir && dir[0])
    {
        gCfgPath = std::string(dir) + "\\QuickChatAddon.ini";
    }
    else
    {
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(hSelf, buf, MAX_PATH);
        std::string p(buf);
        size_t pos = p.find_last_of("\\/");
        gCfgPath = (pos == std::string::npos ? std::string(".") : p.substr(0, pos)) + "\\QuickChatAddon.ini";
    }

    size_t sep = gCfgPath.find_last_of('\\');
    if (sep != std::string::npos) MakeDirUtf8(gCfgPath.substr(0, sep));
}

static void SetDefaults()
{
    gCfg.prefix.clear();
    gCfg.openDelay = 180;
    gCfg.charGap = 20;
    gCfg.multiGap = 400;
    gCfg.useClipboard = true;
    gCfg.useMessages = true;
    gCfg.textMode = 0;
    gCfg.showPanel = true;
    gCfg.alphaPct = 100;
    gCfg.btnAlphaPct = 100;
    gCfg.listAlphaPct = 100;
    gCfg.layoutMode = 0;
    gCfg.gridCols = 3;
    gCfg.pageCount = 1;
    gCfg.pageLayout = 0;
    gCfg.pageBtnW = 0.0f;
    gCfg.pageBtnH = 0.0f;
    gCfg.pageSelColor[0] = 1.0f;
    gCfg.pageSelColor[1] = 0.62f;
    gCfg.pageSelColor[2] = 0.12f;
    gCfg.gridByColumn = false;
    gCfg.btnW = 0.0f;
    gCfg.btnH = 0.0f;
    gCfg.fontScale = 1.0f;
    gCfg.btnColorSet = false;
    gCfg.btnColor[0] = 0.26f;
    gCfg.btnColor[1] = 0.34f;
    gCfg.btnColor[2] = 0.46f;
    gCfg.textColorSet = false;
    gCfg.textColor[0] = 1.0f;
    gCfg.textColor[1] = 1.0f;
    gCfg.textColor[2] = 1.0f;
    gCfg.lockPos = false;
    gCfg.hideChrome = false;
    gCfg.slots.clear();
    for (int i = 0; i < kDefaultSlotCount; i++)
    {
        SlotCfg slot;
        slot.enabled = true;
        slot.text = kDefaultText[i];
        gCfg.slots.push_back(slot);
    }
}

static void SaveCfg()
{
    if (gCfgPath.empty()) return;

    std::ofstream out(gCfgPath.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) return;

    out << "prefix=" << EscapeText(gCfg.prefix) << "\n";
    out << "openDelay=" << gCfg.openDelay << "\n";
    out << "charGap=" << gCfg.charGap << "\n";
    out << "multiGap=" << gCfg.multiGap << "\n";
    out << "useMessages=" << (gCfg.useMessages ? 1 : 0) << "\n";
    out << "useClipboard=" << (gCfg.useClipboard ? 1 : 0) << "\n";
    out << "textMode=" << gCfg.textMode << "\n";
    out << "alphaPct=" << gCfg.alphaPct << "\n";
    out << "btnAlphaPct=" << gCfg.btnAlphaPct << "\n";
    out << "listAlphaPct=" << gCfg.listAlphaPct << "\n";
    out << "layoutMode=" << gCfg.layoutMode << "\n";
    out << "gridCols=" << gCfg.gridCols << "\n";
    out << "pageCount=" << gCfg.pageCount << "\n";
    out << "pageLayout=" << gCfg.pageLayout << "\n";
    out << "pageBtnW=" << gCfg.pageBtnW << "\n";
    out << "pageBtnH=" << gCfg.pageBtnH << "\n";
    {
        char selBuf[64];
        snprintf(selBuf, sizeof(selBuf), "%.3f,%.3f,%.3f", gCfg.pageSelColor[0], gCfg.pageSelColor[1], gCfg.pageSelColor[2]);
        out << "pageSelColor=" << selBuf << "\n";
    }
    out << "gridByColumn=" << (gCfg.gridByColumn ? 1 : 0) << "\n";
    out << "btnW=" << gCfg.btnW << "\n";
    out << "btnH=" << gCfg.btnH << "\n";
    out << "fontScale=" << gCfg.fontScale << "\n";
    out << "btnColorSet=" << (gCfg.btnColorSet ? 1 : 0) << "\n";
    char colorBuf[64];
    snprintf(colorBuf, sizeof(colorBuf), "%.3f,%.3f,%.3f", gCfg.btnColor[0], gCfg.btnColor[1], gCfg.btnColor[2]);
    out << "btnColor=" << colorBuf << "\n";
    out << "textColorSet=" << (gCfg.textColorSet ? 1 : 0) << "\n";
    snprintf(colorBuf, sizeof(colorBuf), "%.3f,%.3f,%.3f", gCfg.textColor[0], gCfg.textColor[1], gCfg.textColor[2]);
    out << "textColor=" << colorBuf << "\n";
    out << "lockPos=" << (gCfg.lockPos ? 1 : 0) << "\n";
    out << "hideChrome=" << (gCfg.hideChrome ? 1 : 0) << "\n";
    out << "count=" << gCfg.slots.size() << "\n";

    for (size_t i = 0; i < gCfg.slots.size(); i++)
    {
        const SlotCfg& slot = gCfg.slots[i];
        out << "slot" << (i + 1) << ".enabled=" << (slot.enabled ? 1 : 0) << "\n";
        out << "slot" << (i + 1) << ".page=" << slot.page << "\n";
        out << "slot" << (i + 1) << ".text=" << EscapeText(slot.text) << "\n";
        out << "slot" << (i + 1) << ".channels=" << ChannelSummary(slot.channels) << "\n";
    }
}

static void LoadCfg()
{
    SetDefaults();

    std::ifstream in(gCfgPath.c_str(), std::ios::binary);
    if (!in) return;

    int wantCount = -1;
    int maxIdx = 0;
    std::string line;
    while (std::getline(in, line))
    {
        line = TrimRight(line);
        if (line.empty() || line[0] == '#') continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if (key == "prefix") gCfg.prefix = UnescapeText(val);
        else if (key == "openDelay") gCfg.openDelay = std::max(60, atoi(val.c_str()));
        else if (key == "charGap") gCfg.charGap = std::max(0, atoi(val.c_str()));
        else if (key == "multiGap") gCfg.multiGap = std::max(150, atoi(val.c_str()));
        else if (key == "useClipboard") gCfg.useClipboard = atoi(val.c_str()) != 0;
        else if (key == "useMessages") gCfg.useMessages = atoi(val.c_str()) != 0;
        else if (key == "textMode") gCfg.textMode = std::min(2, std::max(0, atoi(val.c_str())));
        else if (key == "count") wantCount = std::min(kMaxSlots, std::max(0, atoi(val.c_str())));
        else if (key == "alphaPct") gCfg.alphaPct = std::min(100, std::max(0, atoi(val.c_str())));
        else if (key == "btnAlphaPct") gCfg.btnAlphaPct = std::min(100, std::max(0, atoi(val.c_str())));
        else if (key == "listAlphaPct") gCfg.listAlphaPct = std::min(100, std::max(0, atoi(val.c_str())));
        else if (key == "layoutMode") gCfg.layoutMode = std::min(2, std::max(0, atoi(val.c_str())));
        else if (key == "gridCols") gCfg.gridCols = std::min(10, std::max(1, atoi(val.c_str())));
        else if (key == "pageCount") gCfg.pageCount = std::min(9, std::max(1, atoi(val.c_str())));
        else if (key == "pageLayout") gCfg.pageLayout = std::min(1, std::max(0, atoi(val.c_str())));
        else if (key == "pageBtnW") gCfg.pageBtnW = std::min(400.0f, std::max(0.0f, (float)atof(val.c_str())));
        else if (key == "pageBtnH") gCfg.pageBtnH = std::min(120.0f, std::max(0.0f, (float)atof(val.c_str())));
        else if (key == "pageSelColor")
        {
            float r = 1.0f, g = 0.62f, b = 0.12f;
            if (sscanf(val.c_str(), "%f,%f,%f", &r, &g, &b) == 3)
            {
                gCfg.pageSelColor[0] = std::min(1.0f, std::max(0.0f, r));
                gCfg.pageSelColor[1] = std::min(1.0f, std::max(0.0f, g));
                gCfg.pageSelColor[2] = std::min(1.0f, std::max(0.0f, b));
            }
        }
        else if (key == "gridByColumn") gCfg.gridByColumn = atoi(val.c_str()) != 0;
        else if (key == "btnW") gCfg.btnW = std::min(400.0f, std::max(0.0f, (float)atof(val.c_str())));
        else if (key == "btnH") gCfg.btnH = std::min(120.0f, std::max(0.0f, (float)atof(val.c_str())));
        else if (key == "fontScale") gCfg.fontScale = std::min(2.0f, std::max(0.6f, (float)atof(val.c_str())));
        else if (key == "btnColorSet") gCfg.btnColorSet = atoi(val.c_str()) != 0;
        else if (key == "textColorSet") gCfg.textColorSet = atoi(val.c_str()) != 0;
        else if (key == "textColor")
        {
            float r = 0.0f, g = 0.0f, b = 0.0f;
            if (sscanf(val.c_str(), "%f,%f,%f", &r, &g, &b) == 3)
            {
                gCfg.textColor[0] = std::min(1.0f, std::max(0.0f, r));
                gCfg.textColor[1] = std::min(1.0f, std::max(0.0f, g));
                gCfg.textColor[2] = std::min(1.0f, std::max(0.0f, b));
            }
        }
        else if (key == "btnColor")
        {
            float r = 0.0f, g = 0.0f, b = 0.0f;
            if (sscanf(val.c_str(), "%f,%f,%f", &r, &g, &b) == 3)
            {
                gCfg.btnColor[0] = std::min(1.0f, std::max(0.0f, r));
                gCfg.btnColor[1] = std::min(1.0f, std::max(0.0f, g));
                gCfg.btnColor[2] = std::min(1.0f, std::max(0.0f, b));
            }
        }
        else if (key == "lockPos") gCfg.lockPos = atoi(val.c_str()) != 0;
        else if (key == "hideChrome") gCfg.hideChrome = atoi(val.c_str()) != 0;
        else
        {
            int idx = 0;
            char field[32] = {};
            if (sscanf(key.c_str(), "slot%d.%31s", &idx, field) == 2 && idx >= 1 && idx <= kMaxSlots)
            {
                if (idx > (int)gCfg.slots.size()) gCfg.slots.resize(idx);
                if (idx > maxIdx) maxIdx = idx;
                SlotCfg& slot = gCfg.slots[idx - 1];
                if (strcmp(field, "enabled") == 0)
                {
                    slot.enabled = atoi(val.c_str()) != 0;
                }
                else if (strcmp(field, "page") == 0)
                {
                    slot.page = std::min(9, std::max(1, atoi(val.c_str())));
                }
                else if (strcmp(field, "text") == 0)
                {
                    slot.text = UnescapeText(val);
                }
                else if (strcmp(field, "channels") == 0)
                {
                    slot.channels.clear();
                    size_t start = 0;
                    while (start < val.size())
                    {
                        size_t end = val.find('/', start);
                        if (end == std::string::npos) end = val.size();
                        std::string code = val.substr(start, end - start);
                        size_t b = code.find_first_not_of(" \t");
                        size_t e = code.find_last_not_of(" \t");
                        if (b != std::string::npos) code = code.substr(b, e - b + 1);
                        else code.clear();
                        if (IsKnownChannel(code)) slot.channels.push_back(code);
                        start = end + 1;
                    }
                }
            }
        }
    }

    if (wantCount >= 0) gCfg.slots.resize((size_t)wantCount);
    else if (maxIdx > 0) gCfg.slots.resize((size_t)maxIdx);

    for (size_t i = 0; i < gCfg.slots.size(); i++)
    {
        if (gCfg.slots[i].page < 1) gCfg.slots[i].page = 1;
        if (gCfg.slots[i].page > gCfg.pageCount) gCfg.slots[i].page = gCfg.pageCount;
    }
    SortSlotsByPage(nullptr);
}

///----------------------------------------------------------------------------------------------------
/// UI
///----------------------------------------------------------------------------------------------------
static void KeepPanelOnScreen()
{
    ImVec2 disp = ImGui::GetIO().DisplaySize;
    if (disp.x <= 100.0f || disp.y <= 100.0f) return;

    ImVec2 size = ImGui::GetWindowSize();
    ImVec2 pos = ImGui::GetWindowPos();

    ImVec2 limit(disp.x - 40.0f, disp.y - 80.0f);
    if (size.x > limit.x) { size.x = limit.x; ImGui::SetWindowSize(size); }
    if (size.y > limit.y) { size.y = limit.y; ImGui::SetWindowSize(size); }

    float nx = pos.x;
    float ny = pos.y;
    if (nx + size.x > disp.x - 10.0f) nx = disp.x - size.x - 10.0f;
    if (nx < 10.0f) nx = 10.0f;
    if (ny + size.y > disp.y - 10.0f) ny = disp.y - size.y - 10.0f;
    if (ny < 10.0f) ny = 10.0f;
    if (nx != pos.x || ny != pos.y) ImGui::SetWindowPos(ImVec2(nx, ny));
}

static void DrawChannelItems(std::vector<std::string>& channels)
{
    if (ImGui::Selectable("只用当前频道", channels.empty(), ImGuiSelectableFlags_DontClosePopups))
        channels.clear();

    ImGui::Separator();
    for (int c = 0; c < kChannelCount; c++)
    {
        bool on = HasChannel(channels, kChannels[c].code);
        std::string item = std::string(on ? "[x] " : "[ ] ") + kChannels[c].name + "  /" + kChannels[c].code;
        if (ImGui::Selectable(item.c_str(), on, ImGuiSelectableFlags_DontClosePopups))
            SetChannel(channels, kChannels[c].code, !on);
    }
}

static int gPanelDragFrom = -1;
static int gPanelDragTo = -1;

static void DrawSlotButton(int index, ImVec2 size)
{
    if (index < 0 || index >= (int)gCfg.slots.size()) return;
    SlotCfg& slot = gCfg.slots[index];

    ImGui::PushID(index);
    if (ImGui::Button(slot.text.c_str(), size))
    {
        gChannelMenuSlot = -1;
        QueueSlot(index);
    }
    RecordHitRect();

    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip))
    {
        ImGui::SetDragDropPayload("QC_PANEL_SLOT", &index, sizeof(int));
        ImGui::SetTooltip("%s", slot.text.c_str());
        ImGui::EndDragDropSource();
    }

    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("QC_PANEL_SLOT"))
        {
            gPanelDragFrom = *(const int*)payload->Data;
            gPanelDragTo = index;
        }
        ImGui::EndDragDropTarget();
    }

    if (ImGui::IsItemHovered())
    {
        if (ImGui::IsMouseClicked(1))
        {
            gChannelMenuSlot = index;
            ImVec2 rmin = ImGui::GetItemRectMin();
            ImVec2 rmax = ImGui::GetItemRectMax();
            const float estH = 340.0f;
            bool goAbove = (rmax.y + estH > ImGui::GetIO().DisplaySize.y);
            gChannelMenuPivotY = goAbove ? 1.0f : 0.0f;
            gChannelMenuPos = ImVec2(rmin.x, goAbove ? rmin.y - 2.0f : rmax.y + 2.0f);
        }

        std::string ch = ChannelSummary(slot.channels);
        if (ch.empty()) ch = "当前频道";
        ImGui::SetTooltip("%s\n%s", ch.c_str(), slot.text.c_str());
    }

    ImGui::PopID();
}

static void DrawPageButtons(bool vertical, float w, float h, const ImVec4& activeColor)
{
    for (int p = 1; p <= gCfg.pageCount; p++)
    {
        if (p > 1 && !vertical) ImGui::SameLine();
        bool cur = (p == gPanelPage);
        ImVec4 sel(activeColor.x, activeColor.y, activeColor.z, 1.0f);
        sel.x = gCfg.pageSelColor[0];
        sel.y = gCfg.pageSelColor[1];
        sel.z = gCfg.pageSelColor[2];
        if (cur)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, sel);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, sel);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, sel);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        }
        char lbl[8];
        snprintf(lbl, sizeof(lbl), "%d", p);
        if (ImGui::Button(lbl, ImVec2(w, h))) gPanelPage = p;
        RecordHitRect();

        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("QC_PANEL_SLOT"))
            {
                gPanelDragFrom = *(const int*)payload->Data;
                gPanelDropPage = p;
            }
            ImGui::EndDragDropTarget();
        }

        if (cur) ImGui::PopStyleColor(4);
    }
}

void RenderMain()
{
    gWantKb = ImGui::GetIO().WantCaptureKeyboard;
    if (gReleaseFocus)
    {
        ImGui::SetWindowFocus(nullptr);
        gReleaseFocus = false;
    }
    gHitRects.clear();
    gChannelMenuHovered = false;
    if (!gCfg.showPanel)
    {
        gHitRectsPrev.clear();
        return;
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    if (gCfg.lockPos) flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
    if (gCfg.hideChrome) flags |= ImGuiWindowFlags_NoTitleBar;

    ImVec2 mpNow = ImGui::GetIO().MousePos;
    bool gOverButton = false;
    for (size_t i = 0; i < gHitRectsPrev.size(); i++)
    {
        const ImVec4& r = gHitRectsPrev[i];
        if (mpNow.x >= r.x && mpNow.x <= r.z && mpNow.y >= r.y && mpNow.y <= r.w) { gOverButton = true; break; }
    }
    bool gMouseInPanel = false;

    // 用原始 Win32 状态检测左键按下：菜单开着时，即使这一下被放行给游戏，也能立刻关闭菜单
    const bool lbtnDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool lbtnPressed = (lbtnDown && !gLBtnPrev);
    gLBtnPrev = lbtnDown;

    const bool rbtnDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    const bool rbtnPressed = (rbtnDown && !gRBtnPrev);
    gRBtnPrev = rbtnDown;

    // 频道菜单打开时：点菜单内正常操作；点菜单外则关闭菜单，并让这一下直接穿透到游戏
    const bool menuOpen = (gChannelMenuSlot >= 0 && gChannelMenuSlot < (int)gCfg.slots.size());
    const bool mouseInMenu = (mpNow.x >= gChannelMenuRect.x && mpNow.x <= gChannelMenuRect.z &&
                              mpNow.y >= gChannelMenuRect.y && mpNow.y <= gChannelMenuRect.w);
    const bool mouseInPanelPrev = (mpNow.x >= gPanelRectPrev.x && mpNow.x <= gPanelRectPrev.z &&
                                   mpNow.y >= gPanelRectPrev.y && mpNow.y <= gPanelRectPrev.w);
    const bool clickOutsideMenu = menuOpen && (lbtnPressed || rbtnPressed) && !mouseInMenu && !gOverButton;
    const bool normalPass = (gCfg.lockPos && !gOverButton);
    const bool passThrough = (normalPass || clickOutsideMenu);

    // 锁定位置时：鼠标不在任何按钮上，就把整个窗口设为不可交互，让点击穿透到游戏
    if (passThrough) flags |= ImGuiWindowFlags_NoInputs;

    ImGui::SetNextWindowSize(ImVec2(300, 400), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(gCfg.alphaPct / 100.0f);

    ImVec2 wp(0, 0), ws(0, 0);

    float panelAlpha = gCfg.alphaPct / 100.0f;
    ImVec4 titleBg = ImGui::GetStyleColorVec4(ImGuiCol_TitleBg);
    ImVec4 titleBgActive = ImGui::GetStyleColorVec4(ImGuiCol_TitleBgActive);
    ImVec4 titleBgCollapsed = ImGui::GetStyleColorVec4(ImGuiCol_TitleBgCollapsed);
    titleBg.w = panelAlpha;
    titleBgActive.w = panelAlpha;
    titleBgCollapsed.w = panelAlpha;
    ImGui::PushStyleColor(ImGuiCol_TitleBg, titleBg);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, titleBgActive);
    ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, titleBgCollapsed);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("快捷喊话##quickchat_main", gCfg.hideChrome ? nullptr : &gCfg.showPanel, flags))
    {
        KeepPanelOnScreen();

        wp = ImGui::GetWindowPos();
        ws = ImGui::GetWindowSize();
        gMouseInPanel = (mpNow.x >= wp.x && mpNow.x <= wp.x + ws.x && mpNow.y >= wp.y && mpNow.y <= wp.y + ws.y);

        if (gCfg.hideChrome)
        {
            // 补回标题栏高度，减去 Dummy 之后自动附加的 ItemSpacing，保证按钮位置不变
            float pad = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2.0f - ImGui::GetStyle().ItemSpacing.y;
            if (pad > 0.0f) ImGui::Dummy(ImVec2(0, pad));
        }

        const float bottomH = gCfg.hideChrome
            ? 0.0f
            : ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f + 6.0f;

        const bool pagesVert = (gCfg.pageCount > 1 && gCfg.pageLayout == 1);
        const float pageColW = (gCfg.pageBtnW > 0.0f ? gCfg.pageBtnW : 44.0f) + ImGui::GetStyle().ItemSpacing.x;

        ImVec4 btnBase = gCfg.btnColorSet
            ? ImVec4(gCfg.btnColor[0], gCfg.btnColor[1], gCfg.btnColor[2], 1.0f)
            : ImGui::GetStyleColorVec4(ImGuiCol_Button);
        float btnAlpha = gCfg.btnAlphaPct / 100.0f;
        btnBase.w = btnAlpha;

        ImVec4 btnHover = btnBase;
        btnHover.x = std::min(1.0f, btnBase.x + 0.08f);
        btnHover.y = std::min(1.0f, btnBase.y + 0.08f);
        btnHover.z = std::min(1.0f, btnBase.z + 0.08f);
        btnHover.w = btnAlpha;

        ImVec4 btnActive = btnBase;
        btnActive.x = btnBase.x * 0.8f;
        btnActive.y = btnBase.y * 0.8f;
        btnActive.z = btnBase.z * 0.8f;
        btnActive.w = btnAlpha;
        ImGui::PushStyleColor(ImGuiCol_Button, btnBase);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, btnHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, btnActive);
        if (gCfg.textColorSet)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(gCfg.textColor[0], gCfg.textColor[1], gCfg.textColor[2], 1.0f));

        ImVec4 childBg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        childBg.w = gCfg.listAlphaPct / 100.0f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, childBg);

        ImGuiWindowFlags childFlags = (gCfg.layoutMode == 1) ? ImGuiWindowFlags_HorizontalScrollbar : 0;

        if (gCfg.pageCount > 1 && !pagesVert)
        {
            float spacing = ImGui::GetStyle().ItemSpacing.x;
            float rowW = ImGui::GetContentRegionAvail().x;
            float pw = gCfg.pageBtnW > 0.0f
                ? gCfg.pageBtnW
                : (rowW - spacing * (gCfg.pageCount - 1)) / gCfg.pageCount;
            if (pw < 16.0f) pw = 16.0f;
            DrawPageButtons(false, pw, gCfg.pageBtnH, btnActive);
        }

        if (pagesVert)
        {
            ImGui::BeginChild("##pages", ImVec2(pageColW, -bottomH), false);
            DrawPageButtons(true, gCfg.pageBtnW > 0.0f ? gCfg.pageBtnW : -1.0f, gCfg.pageBtnH, btnActive);
            ImGui::EndChild();
            ImGui::SameLine();
        }

        ImGui::BeginChild("##slots", ImVec2(0, -bottomH), false, childFlags);

        bool useBigFont = (gBigFont != nullptr) && gCfg.fontScale > 1.001f;
        if (useBigFont)
        {
            ImGui::PushFont(gBigFont);
            float extra = gCfg.fontScale / gBigRatio;
            if (extra > 1.0f) extra = 1.0f;
            ImGui::SetWindowFontScale(extra);
        }
        else
        {
            ImGui::SetWindowFontScale(gCfg.fontScale);
        }


        std::vector<int> visible;
        for (int i = 0; i < (int)gCfg.slots.size(); i++)
        {
            if (!gCfg.slots[i].enabled || gCfg.slots[i].text.empty()) continue;
            int page = gCfg.slots[i].page;
            if (page < 1) page = 1;
            if (page > gCfg.pageCount) page = gCfg.pageCount;
            if (page != gPanelPage) continue;
            visible.push_back(i);
        }

        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const int cols = std::max(1, gCfg.gridCols);
        const float availW = ImGui::GetContentRegionAvail().x;
        const float cellW = gCfg.btnW > 0.0f ? gCfg.btnW : (availW - spacing * (cols - 1)) / cols;

        std::vector<int> cells;
        if (gCfg.layoutMode == 2 && gCfg.gridByColumn && cols > 1 && !visible.empty())
        {
            int rows = ((int)visible.size() + cols - 1) / cols;
            cells.assign((size_t)rows * cols, -1);
            for (size_t p = 0; p < visible.size(); p++)
            {
                int r = (int)p % rows;
                int c = (int)p / rows;
                size_t cell = (size_t)(r * cols + c);
                if (cell < cells.size()) cells[cell] = visible[p];
            }
        }
        else
        {
            cells = visible;
        }

        for (size_t p = 0; p < cells.size(); p++)
        {
            ImVec2 size;
            if (gCfg.layoutMode == 1)
            {
                size = ImVec2(gCfg.btnW, gCfg.btnH);
                if (p > 0) ImGui::SameLine();
            }
            else if (gCfg.layoutMode == 2)
            {
                size = ImVec2(cellW, gCfg.btnH);
                if (p > 0 && (p % (size_t)cols) != 0) ImGui::SameLine();
                if (cells[p] < 0)
                {
                    ImGui::Dummy(size);
                    continue;
                }
            }
            else
            {
                size = ImVec2(gCfg.btnW > 0.0f ? gCfg.btnW : -1.0f, gCfg.btnH);
            }

            DrawSlotButton(cells[p], size);
        }

        const int shown = (int)visible.size();

        if (gPanelDragFrom >= 0 && gPanelDragTo >= 0)
        {
            MoveSlotTo(gPanelDragFrom, gPanelDragTo);
            gPanelDragFrom = -1;
            gPanelDragTo = -1;
        }
        if (shown == 0)
        {
            if (gCfg.pageCount > 1) ImGui::TextDisabled("本页没有句子，去 Nexus 设置里调整页码。");
            else ImGui::TextDisabled("没有启用的句子，去 Nexus 设置里添加。");
        }
        ImGui::EndChild();
        if (useBigFont) ImGui::PopFont();
        else ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();

        if (gPanelDropPage > 0)
        {
            int from = gPanelDragFrom;
            if (from >= 0 && from < (int)gCfg.slots.size())
            {
                int pg = gPanelDropPage;
                if (pg < 1) pg = 1;
                if (pg > gCfg.pageCount) pg = gCfg.pageCount;
                MoveSlotToPage(from, pg);
                gPanelPage = pg;
            }
            gPanelDragFrom = -1;
            gPanelDropPage = 0;
        }

        if (gCfg.textColorSet) ImGui::PopStyleColor();
        ImGui::PopStyleColor(3);

        if (gChannelMenuSlot >= 0 && gChannelMenuSlot < (int)gCfg.slots.size())
        {
            ImGui::SetNextWindowPos(gChannelMenuPos, ImGuiCond_Always, ImVec2(0.0f, gChannelMenuPivotY));
            ImGui::SetNextWindowSizeConstraints(ImVec2(150.0f, 0.0f), ImVec2(420.0f, 900.0f));
            ImGui::Begin("##qc_chmenu", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
            {
                ImVec2 p0 = ImGui::GetWindowPos();
                ImVec2 s0 = ImGui::GetWindowSize();
                gChannelMenuRect = ImVec4(p0.x, p0.y, p0.x + s0.x, p0.y + s0.y);
            }
            ImGui::TextDisabled("发送频道");
            ImGui::Separator();
            DrawChannelItems(gCfg.slots[gChannelMenuSlot].channels);
            gChannelMenuHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
            ImGui::End();

            if (clickOutsideMenu) gChannelMenuSlot = -1;
        }

        if (!gCfg.hideChrome)
        {
            if (gStatusUntil > GetTickCount()) ImGui::TextUnformatted(gStatus.c_str());
            else ImGui::TextDisabled("点击发送，右键选频道");
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    bool overOtherWindow = false;
    {
        ImGuiContext* ctx = ImGui::GetCurrentContext();
        ImGuiWindow* hw = ctx ? ctx->HoveredWindow : nullptr;
        if (hw && !gMouseInPanel && !gChannelMenuHoveredPrev)
        {
            ImVec2 a = hw->Pos;
            ImVec2 b(hw->Pos.x + hw->Size.x, hw->Pos.y + hw->Size.y);
            overOtherWindow = (mpNow.x >= a.x && mpNow.x <= b.x && mpNow.y >= a.y && mpNow.y <= b.y);
        }
    }

    const bool overOurMenu = (mouseInMenu || gChannelMenuHoveredPrev);
    if (passThrough && !overOtherWindow && !overOurMenu)
    {
        ImGui::GetIO().WantCaptureMouse = false;
        ImGui::GetIO().MouseDrawCursor = false;
    }

    gPanelRectPrev = ImVec4(wp.x, wp.y, wp.x + ws.x, wp.y + ws.y);
    gChannelMenuHoveredPrev = gChannelMenuHovered;
    gHitRectsPrev.swap(gHitRects);
    gHitRects.clear();
}

static void RegisterSlotBind(int index)
{
    if (!APIDefs || !APIDefs->InputBinds_RegisterWithString) return;
    std::string id = BindId(index);
    APIDefs->InputBinds_RegisterWithString(id.c_str(), OnInputBind, "");
}

static void DeregisterSlotBind(int index)
{
    if (!APIDefs || !APIDefs->InputBinds_Deregister) return;
    std::string id = BindId(index);
    APIDefs->InputBinds_Deregister(id.c_str());
}

static void AddSlot()
{
    SlotCfg slot;
    slot.enabled = true;
    gCfg.slots.push_back(slot);
    RegisterSlotBind((int)gCfg.slots.size() - 1);
}

static void RemoveSlot(int index)
{
    if (index < 0 || index >= (int)gCfg.slots.size()) return;
    DeregisterSlotBind((int)gCfg.slots.size() - 1);
    gCfg.slots.erase(gCfg.slots.begin() + index);
}

static void MoveSlot(int index, int dir)
{
    int target = index + dir;
    if (index < 0 || index >= (int)gCfg.slots.size()) return;
    if (target < 0 || target >= (int)gCfg.slots.size()) return;
    std::swap(gCfg.slots[(size_t)index], gCfg.slots[(size_t)target]);
}

static void MoveSlotTo(int from, int to)
{
    int n = (int)gCfg.slots.size();
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;

    SlotCfg moved = gCfg.slots[(size_t)from];
    gCfg.slots.erase(gCfg.slots.begin() + from);
    gCfg.slots.insert(gCfg.slots.begin() + to, moved);

    if (gEditSlot == from) gEditSlot = to;
    else if (from < gEditSlot && to >= gEditSlot) gEditSlot--;
    else if (from > gEditSlot && to <= gEditSlot) gEditSlot++;
}

static void SortSlotsByPage(int* follow)
{
    int n = (int)gCfg.slots.size();
    if (n <= 1) return;

    std::vector<int> order((size_t)n);
    for (int i = 0; i < n; i++) order[(size_t)i] = i;

    std::stable_sort(order.begin(), order.end(), [](int a, int b) {
        return gCfg.slots[(size_t)a].page < gCfg.slots[(size_t)b].page;
    });

    bool changed = false;
    for (int i = 0; i < n; i++) if (order[(size_t)i] != i) { changed = true; break; }
    if (!changed) return;

    std::vector<SlotCfg> sorted;
    sorted.reserve((size_t)n);
    int newFollow = -1;
    for (int i = 0; i < n; i++)
    {
        sorted.push_back(gCfg.slots[(size_t)order[(size_t)i]]);
        if (follow && *follow == order[(size_t)i]) newFollow = i;
    }
    gCfg.slots.swap(sorted);
    if (follow && newFollow >= 0) *follow = newFollow;
}

// 把某条移动到指定页，并放在该页最后
static void MoveSlotToPage(int index, int page)
{
    int n = (int)gCfg.slots.size();
    if (index < 0 || index >= n) return;
    if (page < 1) page = 1;
    if (page > gCfg.pageCount) page = gCfg.pageCount;
    if (gCfg.slots[(size_t)index].page == page && index == n - 1) { gCfg.slots[(size_t)index].page = page; return; }

    SlotCfg moved = gCfg.slots[(size_t)index];
    moved.page = page;
    gCfg.slots.erase(gCfg.slots.begin() + index);

    int insertAt = 0;
    while (insertAt < (int)gCfg.slots.size() && gCfg.slots[(size_t)insertAt].page <= page) insertAt++;
    gCfg.slots.insert(gCfg.slots.begin() + insertAt, moved);

    if (gEditSlot == index) gEditSlot = insertAt;
    else if (index < gEditSlot && insertAt > gEditSlot) gEditSlot--;
    else if (index > gEditSlot && insertAt <= gEditSlot) gEditSlot++;
}

static void RefreshSlotBinds(int oldCount, int newCount)
{
    for (int i = 0; i < oldCount; i++) DeregisterSlotBind(i);
    for (int i = 0; i < newCount; i++) RegisterSlotBind(i);
}

static std::string ConfigDir()
{
    size_t sep = gCfgPath.find_last_of('\\');
    return (sep == std::string::npos) ? std::string(".") : gCfgPath.substr(0, sep);
}

static std::string ProfilePath(const std::string& name)
{
    return ConfigDir() + "\\profiles\\" + name + ".ini";
}

static std::string TimestampSuffix()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[40];
    snprintf(buf, sizeof(buf), "-%04u%02u%02u-%02u%02u%02u",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return std::string(buf);
}

static std::string CleanPath(const char* in)
{
    std::string p = TrimRight(in);
    size_t b = p.find_first_not_of(" 	");
    if (b == std::string::npos) return std::string();
    p = p.substr(b);
    if (p.size() >= 2 && p.front() == '"' && p.back() == '"') p = p.substr(1, p.size() - 2);
    return p;
}

// 补全导出/导入路径：目录自动补文件名，缺扩展名补 .ini，相对路径放到插件目录
static std::string NormalizeConfigPath(const std::string& raw, bool forExport)
{
    std::string p = raw;
    if (p.empty()) return p;

    bool isDir = PathIsDirUtf8(p);
    if (isDir || (!p.empty() && (p.back() == '\\' || p.back() == '/')))
    {
        if (!p.empty() && (p.back() == '\\' || p.back() == '/')) {}
        else p += "\\";
        p += "QuickChat-config" + TimestampSuffix() + ".ini";
        return p;
    }

    size_t sl = p.find_last_of("\/");
    if (sl == std::string::npos)
    {
        p = ConfigDir() + "\\" + p;
        sl = p.find_last_of("\/");
    }

    std::string file = p.substr(sl + 1);
    if (file.size() < 4 || _stricmp(file.c_str() + file.size() - 4, ".ini") != 0)
        p += ".ini";

    return p;
}

static std::string DesktopDir()
{
    char buf[MAX_PATH] = {};
    wchar_t wbuf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, wbuf))) return WideToUtf8(wbuf);
    return std::string();
}

static std::string FindNewestIniInDir(const std::string& dir)
{
    std::string pattern = dir + "\*.ini";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Utf8ToWide(pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return std::string();

    std::string best;
    ULARGE_INTEGER bestTime = { 0, 0 };
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULARGE_INTEGER t;
        t.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        if (best.empty() || t.QuadPart > bestTime.QuadPart)
        {
            bestTime = t;
            best = dir + "\\" + WideToUtf8(fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best;
}

static void ListProfiles(std::vector<std::string>& out)
{
    out.clear();
    std::string pattern = ConfigDir() + "\\profiles\\*.ini";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Utf8ToWide(pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string n = WideToUtf8(fd.cFileName);
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".ini") == 0)
            out.push_back(n.substr(0, n.size() - 4));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
}

// 用一份配置文件替换当前配置并重新加载（快捷键绑定同步增删）
static bool ApplyConfigFile(const std::string& path)
{
    if (!CopyFileUtf8(path, gCfgPath)) return false;

    int oldCount = (int)gCfg.slots.size();
    gEditSlot = -1;
    gPanelPage = 1;
    gChannelMenuSlot = -1;
    LoadCfg();
    RefreshSlotBinds(oldCount, (int)gCfg.slots.size());
    gStatus = "已切换配置";
    gStatusUntil = GetTickCount() + 3000;
    return true;
}

static void DrawSlotEditor(int index)
{
    if (index < 0 || index >= (int)gCfg.slots.size()) return;
    SlotCfg& slot = gCfg.slots[index];

    ImGui::Indent(16.0f);
    ImGui::Checkbox("启用", &slot.enabled);

    static char editBuf[512] = {};
    static int editBufFor = -1;
    if (editBufFor != index)
    {
        snprintf(editBuf, sizeof(editBuf), "%s", slot.text.c_str());
        editBufFor = index;
    }
    if (ImGui::InputText("消息", editBuf, sizeof(editBuf))) slot.text = editBuf;

    if (ImGui::Button("从剪贴板粘贴"))
    {
        std::wstring w;
        if (ClipboardGet(&w) && !w.empty())
        {
            slot.text = WideToUtf8(w);
            editBufFor = -1;
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Nexus 内打不了中文，先在别处复制再点这里");

    ImGui::Text("发送频道（不勾选则为当前频道）：");
    for (int c = 0; c < kChannelCount; c++)
    {
        bool on = HasChannel(slot.channels, kChannels[c].code);
        if (ImGui::Checkbox(kChannels[c].name, &on))
            SetChannel(slot.channels, kChannels[c].code, on);
        if ((c % 4) != 3 && c != kChannelCount - 1) ImGui::SameLine();
    }
    ImGui::Unindent(16.0f);
}

void RenderOptions()
{
    ImGui::Separator();
    ImGui::Text("快捷喊话");
    ImGui::TextDisabled("在游戏内显示预设句子面板，点击或按快捷键发送。");

    ImGui::Checkbox("显示悬浮面板", &gCfg.showPanel);
    if (!gCfg.showPanel)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("（已隐藏，按 CTRL+ALT+M 可呼出）");
    }
    ImGui::SliderInt("面板背景透明度", &gCfg.alphaPct, 0, 100, "%d%%");
    ImGui::SliderInt("按钮透明度", &gCfg.btnAlphaPct, 0, 100, "%d%%");
    ImGui::SliderInt("列表背景透明度", &gCfg.listAlphaPct, 0, 100, "%d%%");
    ImGui::TextDisabled("透明度：0 = 完全透明，100 = 不透明");

    const char* layouts[] = { "纵向单排", "横向单排", "表格" };
    ImGui::Combo("按钮排列", &gCfg.layoutMode, layouts, 3);
    if (gCfg.layoutMode == 2)
    {
        ImGui::SliderInt("每行条数", &gCfg.gridCols, 1, 10);
        ImGui::Checkbox("按列填充", &gCfg.gridByColumn);
    }
    if (ImGui::SliderInt("分页数", &gCfg.pageCount, 1, 9))
    {
        for (size_t i = 0; i < gCfg.slots.size(); i++)
        {
            if (gCfg.slots[i].page < 1) gCfg.slots[i].page = 1;
            if (gCfg.slots[i].page > gCfg.pageCount) gCfg.slots[i].page = gCfg.pageCount;
        }
        if (gPanelPage > gCfg.pageCount) gPanelPage = gCfg.pageCount;
    }
    ImGui::TextDisabled("分页数 > 1 时，面板底部会出现 1/2/3… 翻页按钮；列表里每条右侧可设页码");
    {
        const char* pageLayouts[] = { "横向一排", "纵向一列" };
        ImGui::Combo("分页按钮排列", &gCfg.pageLayout, pageLayouts, 2);
        ImGui::SliderFloat("分页按钮宽度", &gCfg.pageBtnW, 0.0f, 400.0f, "%.0f");
        ImGui::SliderFloat("分页按钮高度", &gCfg.pageBtnH, 0.0f, 120.0f, "%.0f");
        ImGui::ColorEdit3("当前页高亮颜色", gCfg.pageSelColor);
        ImGui::TextDisabled("分页按钮：宽度 0 = 横向时平分整行 / 纵向时铺满，高度 0 = 自动");
    }

    ImGui::SliderFloat("按钮宽度", &gCfg.btnW, 0.0f, 400.0f, "%.0f");
    ImGui::SliderFloat("按钮高度", &gCfg.btnH, 0.0f, 120.0f, "%.0f");
    ImGui::TextDisabled("宽度 0 = 纵向铺满 / 横向按文字自适应，高度 0 = 自动");

    if (ImGui::ColorEdit3("按钮颜色", gCfg.btnColor))
        gCfg.btnColorSet = true;
    ImGui::SameLine();
    if (ImGui::Button("跟随主题")) gCfg.btnColorSet = false;

    if (ImGui::ColorEdit3("文字颜色", gCfg.textColor))
        gCfg.textColorSet = true;
    ImGui::SameLine();
    if (ImGui::Button("文字跟随主题")) gCfg.textColorSet = false;

    ImGui::SliderFloat("文字大小", &gCfg.fontScale, 0.6f, 2.0f, "%.2f 倍");
    ImGui::SameLine();
    ImGui::TextDisabled(gBigFont ? "（>1 用 Nexus 大号字体，清晰）" : "（未取到大字体，全部用拉伸）");

    ImGui::Checkbox("锁定面板位置", &gCfg.lockPos);
    ImGui::Checkbox("隐藏标题栏和底部提示", &gCfg.hideChrome);
    ImGui::Checkbox("直接向游戏投递消息（推荐）", &gCfg.useMessages);
    const char* textModes[] = { "GBK 字节（默认）", "UTF-16 宽字符", "IME 宽字符" };
    ImGui::Combo("文字投递方式", &gCfg.textMode, textModes, 3);
    if (!gCfg.useMessages) ImGui::Checkbox("使用剪贴板粘贴（模拟按键方式）", &gCfg.useClipboard);

    char prefixBuf[256];
    snprintf(prefixBuf, sizeof(prefixBuf), "%s", gCfg.prefix.c_str());
    if (ImGui::InputText("频道前缀", prefixBuf, sizeof(prefixBuf))) gCfg.prefix = prefixBuf;

    ImGui::SliderInt("打开聊天延迟 (ms)", &gCfg.openDelay, 60, 1500);
    ImGui::SliderInt("多频道间隔 (ms)", &gCfg.multiGap, 150, 3000);
    ImGui::SliderInt("逐字间隔 (ms)", &gCfg.charGap, 0, 200);

    int removeIndex = -1;
    int moveIndex = -1;
    int moveDir = 0;
    bool sortNeeded = false;
    int dragFrom = -1;
    int dragTo = -1;

    char secHeader[64];
    snprintf(secHeader, sizeof(secHeader), "句子编辑（%d 条）###slots_section", (int)gCfg.slots.size());
    if (ImGui::CollapsingHeader(secHeader))
    {
        ImGui::TextDisabled("按住某一行拖动，可以调整顺序");
        ImGui::Spacing();

        for (int i = 0; i < (int)gCfg.slots.size(); i++)
        {
            ImGui::PushID(i);
            SlotCfg& slot = gCfg.slots[i];

            if (ImGui::Button("删除")) removeIndex = i;
            ImGui::SameLine();

            if (gCfg.pageCount > 1)
            {
                ImGui::SetNextItemWidth(110.0f);
                if (ImGui::SliderInt("##page", &slot.page, 1, gCfg.pageCount, "第 %d 页"))
                {
                    if (slot.page < 1) slot.page = 1;
                    if (slot.page > gCfg.pageCount) slot.page = gCfg.pageCount;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) sortNeeded = true;
                ImGui::SameLine();
            }

            char label[192];
            snprintf(label, sizeof(label), "第 %d 条：%s", i + 1, slot.text.empty() ? "（空）" : slot.text.c_str());
            if (ImGui::Selectable(label, gEditSlot == i)) gEditSlot = (gEditSlot == i) ? -1 : i;

            if (ImGui::BeginDragDropSource())
            {
                ImGui::SetDragDropPayload("QC_SLOT", &i, sizeof(int));
                ImGui::Text("第 %d 条：%s", i + 1, slot.text.c_str());
                ImGui::EndDragDropSource();
            }

            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("QC_SLOT"))
                {
                    dragFrom = *(const int*)payload->Data;
                    dragTo = i;
                }
                ImGui::EndDragDropTarget();
            }

            if (gEditSlot == i) DrawSlotEditor(i);
            ImGui::PopID();
        }

        if (ImGui::Button("添加一条"))
        {
            AddSlot();
            gEditSlot = (int)gCfg.slots.size() - 1;
        }
    }

    if (dragFrom >= 0 && dragTo >= 0) MoveSlotTo(dragFrom, dragTo);

    if (sortNeeded) SortSlotsByPage(&gEditSlot);

    if (moveIndex >= 0)
    {
        MoveSlot(moveIndex, moveDir);
        if (gEditSlot == moveIndex) gEditSlot = moveIndex + moveDir;
    }
    if (removeIndex >= 0)
    {
        RemoveSlot(removeIndex);
        if (gEditSlot == removeIndex) gEditSlot = -1;
        if (gEditSlot > removeIndex) gEditSlot--;
    }
    if (gEditSlot >= (int)gCfg.slots.size()) gEditSlot = -1;

    if (ImGui::CollapsingHeader("配置管理###cfg_section"))
    {
        static std::vector<std::string> profiles;
        static bool profilesLoaded = false;
        static int sel = -1;
        static char nameBuf[64] = "";
        static char pathBuf[260] = "";
        static std::string cfgMsg;

        if (!profilesLoaded)
        {
            MakeDirUtf8(ConfigDir() + "\\profiles");
            ListProfiles(profiles);
            profilesLoaded = true;
        }

        ImGui::TextDisabled("当前配置：%s", gCfgPath.c_str());

        ImGui::SetNextItemWidth(200.0f);
        const char* preview = (sel >= 0 && sel < (int)profiles.size()) ? profiles[(size_t)sel].c_str() : "（未选择）";
        if (ImGui::BeginCombo("配置列表", preview))
        {
            for (int i = 0; i < (int)profiles.size(); i++)
            {
                if (ImGui::Selectable(profiles[(size_t)i].c_str(), sel == i)) sel = i;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("刷新"))
        {
            ListProfiles(profiles);
            if (sel >= (int)profiles.size()) sel = -1;
        }

        bool hasSel = (sel >= 0 && sel < (int)profiles.size());

        if (ImGui::Button("保存到选中")) 
        {
            if (hasSel)
            {
                SaveCfg();
                MakeDirUtf8(ConfigDir() + "\\profiles");
                if (CopyFileUtf8(gCfgPath, ProfilePath(profiles[(size_t)sel])))
                    cfgMsg = "已保存到 " + profiles[(size_t)sel];
                else cfgMsg = "保存失败";
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("加载选中"))
        {
            if (hasSel)
                cfgMsg = ApplyConfigFile(ProfilePath(profiles[(size_t)sel])) ? ("已加载 " + profiles[(size_t)sel]) : "加载失败";
        }
        ImGui::SameLine();
        if (ImGui::Button("删除选中"))
        {
            if (hasSel)
            {
                DeleteFileUtf8(ProfilePath(profiles[(size_t)sel]));
                cfgMsg = "已删除 " + profiles[(size_t)sel];
                ListProfiles(profiles);
                sel = -1;
            }
        }

        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("新配置名", nameBuf, sizeof(nameBuf));
        ImGui::SameLine();
        if (ImGui::Button("另存为新配置"))
        {
            std::string n = TrimRight(nameBuf);
            if (!n.empty())
            {
                MakeDirUtf8(ConfigDir() + "\\profiles");
                SaveCfg();
                if (CopyFileUtf8(gCfgPath, ProfilePath(n)))
                {
                    ListProfiles(profiles);
                    cfgMsg = "已保存为新配置 " + n;
                }
                else cfgMsg = "保存失败";
            }
            else cfgMsg = "请先填写配置名";
        }

        ImGui::SetNextItemWidth(320.0f);
        ImGui::InputText("文件路径", pathBuf, sizeof(pathBuf));
        ImGui::SameLine();
        if (ImGui::Button("导出到该路径"))
        {
            std::string p2 = NormalizeConfigPath(CleanPath(pathBuf), true);
            if (p2.empty()) cfgMsg = "请先填写导出路径";
            else
            {
                size_t sl = p2.find_last_of("\\/");
                if (sl != std::string::npos) MakeDirUtf8(p2.substr(0, sl));
                SaveCfg();
                SetLastError(0);
                if (CopyFileUtf8(gCfgPath, p2))
                    cfgMsg = "已导出：" + p2;
                else
                    cfgMsg = "导出失败，错误码 " + std::to_string((int)GetLastError()) + "：" + p2;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("导出到桌面"))
        {
            std::string desk = DesktopDir();
            if (desk.empty()) cfgMsg = "无法定位桌面目录";
            else
            {
                SYSTEMTIME st;
                GetLocalTime(&st);
                char name[128];
                snprintf(name, sizeof(name), "\\QuickChat-%04u%02u%02u-%02u%02u%02u.ini",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
                std::string dest = desk + name;
                SaveCfg();
                SetLastError(0);
                if (CopyFileUtf8(gCfgPath, dest)) cfgMsg = "已导出到桌面：" + dest;
                else cfgMsg = "导出失败，错误码 " + std::to_string((int)GetLastError());
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("从该路径导入"))
        {
            std::string p2 = CleanPath(pathBuf);
            bool ok = false;
            if (p2.empty())
            {
                cfgMsg = "请先填写导入路径";
            }
            else
            {
                if (PathIsDirUtf8(p2))
                {
                    std::string newest = FindNewestIniInDir(p2);
                    if (newest.empty())
                        cfgMsg = "该目录下没有 .ini 文件：" + p2;
                    else
                    {
                        p2 = newest;
                        ok = true;
                    }
                }
                else
                {
                    ok = true;
                }

                if (ok && !PathExistsUtf8(p2))
                {
                    cfgMsg = "文件不存在：" + p2;
                    ok = false;
                }

                if (ok)
                {
                    std::string base = p2;
                    size_t sl = base.find_last_of("\\/");
                    if (sl != std::string::npos) base = base.substr(sl + 1);
                    size_t dot = base.find_last_of('.');
                    if (dot != std::string::npos) base = base.substr(0, dot);

                    MakeDirUtf8(ConfigDir() + "\\profiles");
                    std::string dest = ProfilePath(base);
                    if (CopyFileUtf8(p2, dest))
                    {
                        ListProfiles(profiles);
                        cfgMsg = ApplyConfigFile(dest) ? ("已导入并加载 " + base) : "已导入，加载失败";
                    }
                    else cfgMsg = "导入失败，错误码 " + std::to_string((int)GetLastError());
                }
            }
        }

        if (!cfgMsg.empty()) ImGui::TextDisabled("%s", cfgMsg.c_str());
    }

    if (ImGui::CollapsingHeader("快捷键###hotkeys_section"))
    {
        ImGui::TextDisabled("每条句子的快捷键在 Nexus → 输入绑定 里单独设置，默认都不绑定。");
        ImGui::TextDisabled("显示/隐藏悬浮面板：绑定名 QUICKCHAT_PANEL，默认 F9。");
        ImGui::TextDisabled("注意：快捷键按条目序号对应，拖动或删除条目后要重新确认。");
    }

    if (ImGui::Button("保存配置")) { SaveCfg(); gSaveHint = 180; }
    ImGui::SameLine();
    if (ImGui::Button("立即发送第 1 条")) QueueSlot(0);

    if (gSaveHint > 0)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("已保存");
        gSaveHint--;
    }
}

///----------------------------------------------------------------------------------------------------
/// 快捷按键
///----------------------------------------------------------------------------------------------------
void OnInputBind(const char* aIdentifier, bool aIsRelease)
{
    if (aIsRelease || !aIdentifier) return;

    if (strcmp(aIdentifier, "QUICKCHAT_PANEL") == 0 || strcmp(aIdentifier, "QUICKCHAT_TOGGLE") == 0)
    {
        gCfg.showPanel = !gCfg.showPanel;
        return;
    }

    const char* prefix = "QUICKCHAT_SLOT_";
    size_t len = strlen(prefix);
    if (strncmp(aIdentifier, prefix, len) == 0)
    {
        int index = atoi(aIdentifier + len) - 1;
        if (index >= 0 && index < (int)gCfg.slots.size()) QueueSlot(index);
    }
}

///----------------------------------------------------------------------------------------------------
/// 生命周期
///----------------------------------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH: hSelf = hModule; break;
    default: break;
    }
    return TRUE;
}

extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef()
{
    AddonDef.Signature = (uint32_t)-97531;
    AddonDef.APIVersion = NEXUS_API_VERSION;
    AddonDef.Name = "快捷喊话";
    AddonDef.Version.Major = 1;
    AddonDef.Version.Minor = 0;
    AddonDef.Version.Build = 0;
    AddonDef.Version.Revision = 2;
    AddonDef.Author = "摸鱼哥";
    AddonDef.Description = "游戏内显示预设句子面板，点击或按快捷键发送到多个频道。";
    AddonDef.Load = AddonLoad;
    AddonDef.Unload = AddonUnload;
    AddonDef.Flags = AF_None;
    return &AddonDef;
}

void AddonLoad(AddonAPI_t* aApi)
{
    APIDefs = aApi;

    ImGui::SetCurrentContext((ImGuiContext*)APIDefs->ImguiContext);
    ImGui::SetAllocatorFunctions(
        (void* (*)(size_t, void*))APIDefs->ImguiMalloc,
        (void (*)(void*, void*))APIDefs->ImguiFree);

    if (APIDefs->DataLink_Get)
        NexusLink = (NexusLinkData_t*)APIDefs->DataLink_Get("DL_NEXUS_LINK");

    InitPaths();
    LoadCfg();

    gBigFont = nullptr;
    gBigRatio = 1.0f;
    if (NexusLink && NexusLink->Font && NexusLink->FontBig)
    {
        ImFont* base = (ImFont*)NexusLink->Font;
        ImFont* big = (ImFont*)NexusLink->FontBig;
        if (base && big && base->FontSize > 1.0f && big->FontSize > base->FontSize)
        {
            const ImFontGlyph* glyph = big->FindGlyph((ImWchar)0x6D4B);
            if (glyph && glyph->Codepoint == (ImWchar)0x6D4B)
            {
                gBigFont = big;
                gBigRatio = big->FontSize / base->FontSize;
            }
        }
    }

    APIDefs->GUI_Register(RT_Render, RenderMain);
    APIDefs->GUI_Register(RT_OptionsRender, RenderOptions);

    for (int i = 0; i < (int)gCfg.slots.size(); i++) RegisterSlotBind(i);
    APIDefs->InputBinds_Deregister("QUICKCHAT_TOGGLE");
    APIDefs->InputBinds_RegisterWithString("QUICKCHAT_PANEL", OnInputBind, "F9");

    gWorkerStop = false;
    gWorker = std::thread(WorkerLoop);

    if (APIDefs->Log)
        APIDefs->Log(LOGL_INFO, "QuickChat", "快捷喊话插件已加载。");
}

void AddonUnload()
{
    SaveCfg();

    gWorkerStop = true;
    gQueueCv.notify_all();
    if (gWorker.joinable()) gWorker.join();

    for (int i = 0; i < (int)gCfg.slots.size(); i++) DeregisterSlotBind(i);
    APIDefs->InputBinds_Deregister("QUICKCHAT_PANEL");
    APIDefs->InputBinds_Deregister("QUICKCHAT_TOGGLE");

    APIDefs->GUI_Deregister(RenderMain);
    APIDefs->GUI_Deregister(RenderOptions);

    if (APIDefs->Log)
        APIDefs->Log(LOGL_INFO, "QuickChat", "快捷喊话插件已卸载。");
}
