///----------------------------------------------------------------------------------------------------
/// GW2 快捷喊话 — Nexus 插件
/// 在游戏内显示预设句子面板，鼠标点击或快捷键直接发送到多个频道。
///----------------------------------------------------------------------------------------------------

#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <Windows.h>

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

#pragma comment(lib, "user32.lib")

/* proto */
void AddonLoad(AddonAPI_t* aApi);
void AddonUnload();
void RenderOptions();
void OnInputBind(const char* aIdentifier, bool aIsRelease);
static void MoveSlotTo(int from, int to);

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
static const int kMaxSlots = 200;

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
    if (sep != std::string::npos) CreateDirectoryA(gCfgPath.substr(0, sep).c_str(), nullptr);
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
    if (ImGui::Button(slot.text.c_str(), size)) QueueSlot(index);

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

    if (ImGui::BeginPopupContextItem("##chctx"))
    {
        ImGui::TextDisabled("发送频道");
        ImGui::Separator();
        DrawChannelItems(slot.channels);
        ImGui::EndPopup();
    }

    if (ImGui::IsItemHovered() && !slot.channels.empty())
        ImGui::SetTooltip("%s", ChannelSummary(slot.channels).c_str());

    ImGui::PopID();
}

void RenderMain()
{
    gWantKb = ImGui::GetIO().WantCaptureKeyboard;
    if (gReleaseFocus)
    {
        ImGui::SetWindowFocus(nullptr);
        gReleaseFocus = false;
    }
    if (!gCfg.showPanel) return;

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    if (gCfg.lockPos) flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
    if (gCfg.hideChrome) flags |= ImGuiWindowFlags_NoTitleBar;

    ImGui::SetNextWindowSize(ImVec2(300, 400), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(gCfg.alphaPct / 100.0f);

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

        if (gCfg.hideChrome)
        {
            // 补回标题栏高度，减去 Dummy 之后自动附加的 ItemSpacing，保证按钮位置不变
            float pad = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2.0f - ImGui::GetStyle().ItemSpacing.y;
            if (pad > 0.0f) ImGui::Dummy(ImVec2(0, pad));
        }

        const float pageRowH = (gCfg.pageCount > 1)
            ? ImGui::GetFrameHeightWithSpacing() + 4.0f
            : 0.0f;

        const float reserved = (gCfg.hideChrome
            ? 0.0f
            : ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f + 6.0f) + pageRowH;

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
        ImGui::BeginChild("##slots", ImVec2(0, -reserved), false, childFlags);

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

        if (gCfg.pageCount > 1)
        {
            if (gPanelPage < 1) gPanelPage = 1;
            if (gPanelPage > gCfg.pageCount) gPanelPage = gCfg.pageCount;

            float spacing = ImGui::GetStyle().ItemSpacing.x;
            float btnW2 = (ImGui::GetContentRegionAvail().x - spacing * (gCfg.pageCount - 1)) / gCfg.pageCount;
            if (btnW2 < 16.0f) btnW2 = 16.0f;

            for (int p = 1; p <= gCfg.pageCount; p++)
            {
                if (p > 1) ImGui::SameLine();
                bool cur = (p == gPanelPage);
                if (cur) ImGui::PushStyleColor(ImGuiCol_Button, btnActive);
                char lbl[8];
                snprintf(lbl, sizeof(lbl), "%d", p);
                if (ImGui::Button(lbl, ImVec2(gCfg.btnW > 0.0f ? gCfg.btnW : btnW2, gCfg.btnH))) gPanelPage = p;
                if (cur) ImGui::PopStyleColor();
            }
        }

        if (gCfg.textColorSet) ImGui::PopStyleColor();
        ImGui::PopStyleColor(3);

        if (!gCfg.hideChrome)
        {
            ImGui::Separator();
            if (gStatusUntil > GetTickCount()) ImGui::TextUnformatted(gStatus.c_str());
            else ImGui::TextDisabled("点击发送，右键选频道");
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
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
    if ((int)gCfg.slots.size() >= kMaxSlots) return;
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

static void DrawSlotEditor(int index)
{
    if (index < 0 || index >= (int)gCfg.slots.size()) return;
    SlotCfg& slot = gCfg.slots[index];

    ImGui::Indent(16.0f);
    ImGui::Checkbox("启用", &slot.enabled);
    if (gCfg.pageCount > 1)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderInt("页码", &slot.page, 1, gCfg.pageCount);
    }

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
    ImGui::TextDisabled("分页数 > 1 时，面板底部会出现 1/2/3… 翻页按钮；每条可单独设页码");

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

    if (ImGui::CollapsingHeader("快捷键###hotkeys_section"))
    {
        ImGui::TextDisabled("每条句子的快捷键在 Nexus → 输入绑定 里单独设置，默认都不绑定。");
        ImGui::TextDisabled("面板开关的绑定名是 QUICKCHAT_TOGGLE（默认 CTRL+ALT+M）。");
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

    if (strcmp(aIdentifier, "QUICKCHAT_TOGGLE") == 0)
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
    AddonDef.Version.Revision = 1;
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
    APIDefs->InputBinds_RegisterWithString("QUICKCHAT_TOGGLE", OnInputBind, "CTRL+ALT+M");

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
    APIDefs->InputBinds_Deregister("QUICKCHAT_TOGGLE");

    APIDefs->GUI_Deregister(RenderMain);
    APIDefs->GUI_Deregister(RenderOptions);

    if (APIDefs->Log)
        APIDefs->Log(LOGL_INFO, "QuickChat", "快捷喊话插件已卸载。");
}
