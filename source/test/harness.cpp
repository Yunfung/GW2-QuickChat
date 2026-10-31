// 离线验证：真实 ImGui 上下文 + 模拟 Nexus API，拦截 SendInput 校验实际要发出的文本序列。
#define NOMINMAX
#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

/* ---- 记录器：替代真实 SendInput，只记录会发生的操作 ---- */
static std::vector<std::string> gRecLines;
static std::string gRecCur;
static bool gRecCtrl = false;
static int gRecPastes = 0;

static std::string RecClipboard()
{
    std::string out;
    for (int i = 0; i < 5; i++)
    {
        if (!OpenClipboard(nullptr)) { Sleep(20); continue; }
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h)
        {
            const wchar_t* p = (const wchar_t*)GlobalLock(h);
            if (p)
            {
                int n = WideCharToMultiByte(CP_UTF8, 0, p, -1, nullptr, 0, nullptr, nullptr);
                if (n > 1)
                {
                    std::vector<char> buf(n);
                    WideCharToMultiByte(CP_UTF8, 0, p, -1, &buf[0], n, nullptr, nullptr);
                    out.assign(&buf[0], n - 1);
                }
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
        break;
    }
    return out;
}

static UINT RecordingSendInput(UINT count, LPINPUT inputs, int size)
{
    for (UINT i = 0; i < count && inputs; i++)
    {
        const INPUT& in = inputs[i];
        if (in.type != INPUT_KEYBOARD) continue;

        bool up = (in.ki.dwFlags & KEYEVENTF_KEYUP) != 0;
        bool unicode = (in.ki.dwFlags & KEYEVENTF_UNICODE) != 0;
        WORD vk = in.ki.wVk;
        if (!unicode && vk == 0 && in.ki.wScan != 0)
            vk = (WORD)MapVirtualKeyW(in.ki.wScan, MAPVK_VSC_TO_VK);

        if (unicode)
        {
            if (!up)
            {
                wchar_t wc = (wchar_t)in.ki.wScan;
                char buf[8] = {};
                int n = WideCharToMultiByte(CP_UTF8, 0, &wc, 1, buf, 7, nullptr, nullptr);
                if (n > 0) gRecCur.append(buf, n);
            }
            continue;
        }

        if (vk == VK_CONTROL)
        {
            gRecCtrl = !up;
        }
        else if (!up && vk == 'V' && gRecCtrl)
        {
            gRecCur = RecClipboard();
            gRecPastes++;
        }
        else if (!up && vk == VK_RETURN)
        {
            if (!gRecCur.empty())
            {
                gRecLines.push_back(gRecCur);
                gRecCur.clear();
            }
        }
    }
    return count;
}

#define SendInput RecordingSendInput
#include "../src/entry.cpp"

static ImGuiContext* gCtx = nullptr;
static std::map<std::string, INPUTBINDS_PROCESS> gBinds;
static NexusLinkData_t gNexusLink = {};
static std::string gAddonDir;

static void* MockMalloc(size_t sz, void*) { return malloc(sz); }
static void MockFree(void* p, void*) { free(p); }
static const char* MockAddonDir(const char*) { return gAddonDir.c_str(); }
static void MockLog(ELogLevel, const char* aChannel, const char* aStr)
{
    printf("[log:%s] %s\n", aChannel, aStr);
}
static void MockGuiAdd(ERenderType, GUI_RENDER) {}
static void MockGuiRemove(GUI_RENDER) {}
static void MockBindAdd(const char* aIdentifier, INPUTBINDS_PROCESS aHandler, const char*)
{
    gBinds[aIdentifier] = aHandler;
}
static void MockBindRemove(const char* aIdentifier) { gBinds.erase(aIdentifier); }
static void* MockDataLinkGet(const char* aIdentifier)
{
    if (strcmp(aIdentifier, "DL_NEXUS_LINK") == 0) return &gNexusLink;
    return nullptr;
}

static void PumpFrames(int count)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;

    for (int i = 0; i < count; i++)
    {
        ImGui::NewFrame();
        RenderMain();
        RenderOptions();
        ImGui::Render();
    }
}

int main()
{
    setvbuf(stdout, NULL, _IONBF, 0);

    char tmp[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tmp);
    gAddonDir = std::string(tmp) + "qc_nexus_test";
    CreateDirectoryA(gAddonDir.c_str(), nullptr);

    std::string iniPath = gAddonDir + "\\QuickChatAddon.ini";
    DeleteFileA(iniPath.c_str());

    FILE* seed = fopen(iniPath.c_str(), "wb");
    if (!seed) { printf("FAIL: cannot seed config\n"); return 1; }
    fputs("openDelay=80\nmultiGap=200\nuseClipboard=1\nuseMessages=0\ncount=12\nlayoutMode=2\ngridCols=2\ngridByColumn=1\nbtnH=28\n"
      "slot1.enabled=1\nslot1.text=测试中文\nslot1.channels=/g1 /m\n"
      "slot11.enabled=1\nslot11.text=第十一条\n", seed);
    fclose(seed);

    gCtx = ImGui::CreateContext();
    ImGui::SetCurrentContext(gCtx);
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels = nullptr;
    int fw = 0, fh = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
    io.Fonts->SetTexID((ImTextureID)(intptr_t)1);

    static AddonAPI_t api = {};
    api.ImguiContext = gCtx;
    api.ImguiMalloc = (void*)&MockMalloc;
    api.ImguiFree = (void*)&MockFree;
    api.DataLink_Get = MockDataLinkGet;
    api.Paths_GetAddonDirectory = MockAddonDir;
    api.GUI_Register = MockGuiAdd;
    api.GUI_Deregister = MockGuiRemove;
    api.InputBinds_RegisterWithString = MockBindAdd;
    api.InputBinds_Deregister = MockBindRemove;
    api.Log = MockLog;

    gNexusLink.Width = 1280;
    gNexusLink.Height = 720;
    gNexusLink.Scaling = 1.0f;
    gNexusLink.IsGameplay = true;

    AddonLoad(&api);
    printf("cfg: openDelay=%d multiGap=%d slot1.text='%s' channels=%s\n",
        gCfg.openDelay, gCfg.multiGap, gCfg.slots[0].text.c_str(),
        ChannelSummary(gCfg.slots[0].channels).c_str());

    PumpFrames(3);
    printf("render: ok, bind count=%d\n", (int)gBinds.size());

    printf("slots: %d\n", (int)gCfg.slots.size());
    std::vector<std::string> extra = BuildLines(10);
    printf("slot11 lines=%d text=%s\n", (int)extra.size(), extra.empty() ? "" : extra[0].c_str());

    std::vector<std::string> expected = BuildLines(0);
    printf("expected lines: %d\n", (int)expected.size());
    for (size_t i = 0; i < expected.size(); i++) printf("  [%d] %s\n", (int)i, expected[i].c_str());

    INPUTBINDS_PROCESS handler = nullptr;
    std::map<std::string, INPUTBINDS_PROCESS>::iterator it = gBinds.find("快捷喊话-第001条");
    if (it != gBinds.end()) handler = it->second;
    if (!handler) { printf("FAIL: bind not registered\n"); return 1; }

    handler("快捷喊话-第001条", false);
    DWORD start = GetTickCount();
    while (GetTickCount() - start < 8000 && gRecLines.size() < expected.size())
    {
        PumpFrames(1);
        Sleep(30);
    }
    Sleep(200);
    PumpFrames(1);

    printf("captured lines: %d (pastes=%d)\n", (int)gRecLines.size(), gRecPastes);
    for (size_t i = 0; i < gRecLines.size(); i++) printf("  [%d] <%s>\n", (int)i, gRecLines[i].c_str());

    bool pass = gRecLines.size() == expected.size();
    if (pass)
    {
        for (size_t i = 0; i < expected.size(); i++)
            if (gRecLines[i] != expected[i]) pass = false;
    }
    printf("result: %s\n", pass ? "PASS" : "FAIL");

    AddonUnload();

    FILE* saved = fopen(iniPath.c_str(), "rb");
    if (saved)
    {
        char line[512];
        printf("--- saved ini (slot1) ---\n");
        while (fgets(line, sizeof(line), saved))
            if (strncmp(line, "slot1", 5) == 0 || strncmp(line, "slot12", 6) == 0 || strncmp(line, "multiGap", 8) == 0 || strncmp(line, "count", 5) == 0 || strncmp(line, "bgAlpha", 7) == 0 || strncmp(line, "lockPos", 7) == 0 || strncmp(line, "textMode", 8) == 0) printf("%s", line);
        fclose(saved);
    }

    ImGui::DestroyContext(gCtx);
    return pass ? 0 : 2;
}
