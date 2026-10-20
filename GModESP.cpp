#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdarg>
#include <cstddef>
#include <atomic>
#include <algorithm>
#include "Config.hpp"
#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

static_assert(sizeof(void*) == 8, "Windows x64 is required");

namespace {

struct Vec3 { float x, y, z; };
struct Mat4 { float m[4][4]; };
struct ModelRenderInfo {
    Vec3 origin, angles;
    void* renderable;
    const void* model;
    const void* modelToWorld;
    const void* lightingOffset;
    const Vec3* lightingOrigin;
    int flags, entityIndex, skin, body, hitboxset;
    unsigned short instance;
};

// Source 2013 player_info_t layout; tested only at compile-time, not against a running game.
struct PlayerInfo {
    char name[32];
    int userId;
    char guid[33];
    unsigned friendsId;
    char friendsName[32];
    bool fake;
    bool hltv;
    unsigned customFiles[4];
    unsigned char filesDownloaded;
};

struct RecvTable;
struct RecvProp {
    const char* name;
    int type, flags, stringBufferSize;
    bool insideArray;
    const void* extraData;
    RecvProp* arrayProp;
    void* arrayLengthProxy;
    void* proxyFn;
    void* dataTableProxyFn;
    RecvTable* dataTable;
    int offset, elementStride, elements;
    const char* parentArrayPropName;
};
struct RecvTable {
    RecvProp* props;
    int numProps;
    void* decoder;
    const char* tableName;
    bool initialized, inMainList;
};
struct ClientClass {
    void* createFn;
    void* createEventFn;
    const char* networkName;
    RecvTable* recvTable;
    ClientClass* next;
    int classId;
};

static_assert(offsetof(ModelRenderInfo, entityIndex) == 68, "ModelRenderInfo x64 layout changed");
static_assert(offsetof(RecvProp, offset) == 72, "RecvProp x64 layout changed");

using Factory = void* (__cdecl*)(const char*, int*);
using Present = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using PresentEx = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
using Reset = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using ResetEx = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
using DrawModel = void (*)(void*, const void*, const ModelRenderInfo&, const void*);

void* g_engine = nullptr;
void* g_client = nullptr;
void* g_entityList = nullptr;
void* g_modelRender = nullptr;
void* g_materialSystem = nullptr;
void* g_material = nullptr;
Present g_present = nullptr;
PresentEx g_presentEx = nullptr;
Reset g_reset = nullptr;
ResetEx g_resetEx = nullptr;
DrawModel g_drawModel = nullptr;

std::atomic<bool> g_showMenu{false};
std::atomic<bool> g_esp{true};
std::atomic<bool> g_chams{true};
std::atomic<bool> g_enemyOnly{false};
std::atomic<bool> g_names{true};
std::atomic<bool> g_health{true};
std::atomic<bool> g_boxes{true};
std::atomic<uint32_t> g_visibleColor{IM_COL32(45, 210, 240, 255)};
std::atomic<uint32_t> g_hiddenColor{IM_COL32(230, 65, 100, 255)};
std::atomic<int> g_healthOffset{-1};
std::atomic<int> g_maxHealthOffset{-1};
std::atomic<int> g_teamOffset{-1};
std::atomic<int> g_originOffset{-1};

HWND g_hwnd = nullptr;
WNDPROC g_wndProc = nullptr;
IDirect3DDevice9* g_guiDevice = nullptr;
bool g_guiReady = false;

void Log(const char* format, ...) {
    wchar_t path[MAX_PATH]{};
    DWORD n = GetTempPathW(MAX_PATH, path);
    if (!n || n >= MAX_PATH) return;
    constexpr wchar_t name[] = L"GModESP_x64_F11.log";
    if (n + (sizeof(name) / sizeof(wchar_t)) > MAX_PATH) return;
    wcscat_s(path, MAX_PATH, name);
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char line[512]{};
    va_list args;
    va_start(args, format);
    int len = std::vsnprintf(line, sizeof(line) - 2, format, args);
    va_end(args);
    if (len > 0) {
        unsigned count = static_cast<unsigned>(std::min(len, static_cast<int>(sizeof(line) - 3)));
        line[count++] = '\r';
        line[count++] = '\n';
        DWORD written = 0;
        WriteFile(file, line, count, &written, nullptr);
    }
    CloseHandle(file);
}

// GetInterface probes version names instead of assuming a 32-bit client's version.
template<size_t N>
void* GetInterface(const wchar_t* module, const char* const (&names)[N]) {
    HMODULE handle = GetModuleHandleW(module);
    if (!handle) return nullptr;
    auto create = reinterpret_cast<Factory>(GetProcAddress(handle, "CreateInterface"));
    if (!create) return nullptr;
    for (const char* name : names) {
        if (void* result = create(name, nullptr)) return result;
    }
    return nullptr;
}

template<typename Fn>
Fn VFunc(void* obj, size_t index) {
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(obj))[index]);
}

bool EdgeKey(int key) {
    static bool down[256]{};
    const bool current = (GetAsyncKeyState(key) & 0x8000) != 0;
    const bool edge = current && !down[key];
    down[key] = current;
    return edge;
}

void PollKeys() {
    if (EdgeKey(GModConfig::MenuKey)) g_showMenu.store(!g_showMenu.load());
    if (EdgeKey(GModConfig::EspKey)) g_esp.store(!g_esp.load());
    if (EdgeKey(GModConfig::ChamsKey)) g_chams.store(!g_chams.load());
    if (EdgeKey(GModConfig::EnemyKey)) g_enemyOnly.store(!g_enemyOnly.load());
}

bool InGame() {
    return g_engine && VFunc<bool (*)(void*)>(g_engine, GModConfig::VTable::IsInGame)(g_engine);
}
int LocalIndex() {
    return g_engine ? VFunc<int (*)(void*)>(g_engine, GModConfig::VTable::GetLocalPlayer)(g_engine) : -1;
}
int MaxClients() {
    if (!g_engine) return 0;
    int n = VFunc<int (*)(void*)>(g_engine, GModConfig::VTable::GetMaxClients)(g_engine);
    return n >= 1 && n <= 128 ? n : 0;
}
void* Entity(int index) {
    if (!g_entityList || index < 1) return nullptr;
    return VFunc<void* (*)(void*, int)>(g_entityList, GModConfig::VTable::GetClientEntity)(g_entityList, index);
}
bool Player(int index, PlayerInfo& info) {
    if (!g_engine) return false;
    std::memset(&info, 0, sizeof(info));
    return VFunc<bool (*)(void*, int, PlayerInfo*)>(g_engine, GModConfig::VTable::GetPlayerInfo)(g_engine, index, &info);
}

// RecvTable pointers and offsets are discovered from the running x64 game's client classes.
RecvTable* FindTable(RecvTable* root, const char* name, int depth = 0) {
    if (!root || depth > 12 || root->numProps < 0 || root->numProps > 8192 || !root->props) return nullptr;
    if (root->tableName && std::strcmp(root->tableName, name) == 0) return root;
    for (int i = 0; i < root->numProps; ++i) {
        const RecvProp& p = root->props[i];
        if (p.dataTable && p.dataTable != root)
            if (RecvTable* t = FindTable(p.dataTable, name, depth + 1)) return t;
    }
    return nullptr;
}

int FindField(RecvTable* root, const char* name, int depth = 0) {
    if (!root || !root->props || depth > 12 || root->numProps < 0 || root->numProps > 8192) return -1;
    for (int i = 0; i < root->numProps; ++i) {
        const RecvProp& p = root->props[i];
        if (p.name && std::strcmp(p.name, name) == 0 && p.offset >= 0) return p.offset;
        if (p.dataTable && p.dataTable != root) {
            int nested = FindField(p.dataTable, name, depth + 1);
            if (nested >= 0 && p.offset >= 0) return p.offset + nested;
        }
    }
    return -1;
}

bool ResolveNetvars() {
    if (!g_client) return false;
    auto* head = VFunc<ClientClass* (*)(void*)>(g_client, GModConfig::VTable::GetAllClasses)(g_client);
    RecvTable* player = nullptr;
    RecvTable* base = nullptr;
    for (ClientClass* c = head; c; c = c->next) {
        if (!player) player = FindTable(c->recvTable, "DT_BasePlayer");
        if (!base) base = FindTable(c->recvTable, "DT_BaseEntity");
        if (player && base) break;
    }
    if (!player || !base) return false;
    int health = FindField(player, "m_iHealth");
    if (health < 0) health = FindField(base, "m_iHealth");
    g_healthOffset.store(health);
    g_maxHealthOffset.store(FindField(player, "m_iMaxHealth"));
    if (g_maxHealthOffset.load() < 0) g_maxHealthOffset.store(FindField(base, "m_iMaxHealth"));
    int team = FindField(base, "m_iTeamNum");
    if (team < 0) team = FindField(player, "m_iTeamNum");
    g_teamOffset.store(team);
    int origin = FindField(base, "m_vecOrigin");
    if (origin < 0) origin = FindField(base, "m_vecOrigin[0]");
    g_originOffset.store(origin);
    return g_healthOffset.load() >= 0 && g_originOffset.load() >= 0;
}

template<typename T>
T Read(const void* obj, int offset) {
    return *reinterpret_cast<const T*>(reinterpret_cast<const unsigned char*>(obj) + offset);
}

bool TeamFilter(const void* candidate) {
    if (!g_enemyOnly.load()) return true;
    if (g_teamOffset.load() < 0) return false;
    void* local = Entity(LocalIndex());
    if (!local) return false;
    return Read<int>(candidate, g_teamOffset.load()) != Read<int>(local, g_teamOffset.load());
}

bool ValidPlayer(int index, void* obj) {
    if (!obj || index == LocalIndex() || g_healthOffset.load() < 0) return false;
    const int hp = Read<int>(obj, g_healthOffset.load());
    if (hp <= 0 || hp > 50000) return false;
    if (!TeamFilter(obj)) return false;
    PlayerInfo info{};
    return Player(index, info);
}

bool Project(const Mat4& m, Vec3 point, ImVec2 display, ImVec2& out) {
    const float w = m.m[3][0]*point.x + m.m[3][1]*point.y + m.m[3][2]*point.z + m.m[3][3];
    if (!std::isfinite(w) || w < 0.02f) return false;
    const float x = (m.m[0][0]*point.x + m.m[0][1]*point.y + m.m[0][2]*point.z + m.m[0][3]) / w;
    const float y = (m.m[1][0]*point.x + m.m[1][1]*point.y + m.m[1][2]*point.z + m.m[1][3]) / w;
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    out = ImVec2((1.f + x) * display.x * 0.5f, (1.f - y) * display.y * 0.5f);
    return true;
}

void DrawEsp() {
    if (!g_esp.load() || !g_engine || !g_entityList || !InGame() ||
        g_healthOffset.load() < 0 || g_originOffset.load() < 0) return;
    const Mat4& m = VFunc<const Mat4& (*)(void*)>(g_engine, GModConfig::VTable::WorldToScreenMatrix)(g_engine);
    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    for (int index = 1; index <= MaxClients(); ++index) {
        void* obj = Entity(index);
        if (!ValidPlayer(index, obj)) continue;
        const Vec3 feet3d = Read<Vec3>(obj, g_originOffset.load());
        Vec3 head3d = feet3d;
        head3d.z += GModConfig::FallbackPlayerHeight;
        ImVec2 feet{}, top{};
        if (!Project(m, feet3d, display, feet) || !Project(m, head3d, display, top)) continue;
        const float height = feet.y - top.y;
        if (height < 4 || height > display.y * 2) continue;
        const float width = height * 0.43f;
        const ImVec2 a(top.x - width * .5f, top.y);
        const ImVec2 b(top.x + width * .5f, feet.y);
        const ImU32 col = g_teamOffset.load() >= 0 && Entity(LocalIndex()) &&
            Read<int>(obj, g_teamOffset.load()) == Read<int>(Entity(LocalIndex()), g_teamOffset.load())
            ? IM_COL32(80, 170, 255, 255) : IM_COL32(255, 80, 104, 255);
        if (g_boxes.load()) {
            draw->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), IM_COL32(9, 12, 18, 255), 0, 0, 3.f);
            draw->AddRect(a, b, col, 0, 0, 1.3f);
        }
        if (g_health.load()) {
            const int hp = Read<int>(obj, g_healthOffset.load());
            int maxHp = g_maxHealthOffset.load() >= 0 ? Read<int>(obj, g_maxHealthOffset.load()) : 100;
            if (maxHp < 1 || maxHp > 50000) maxHp = 100;
            float fraction = std::clamp(static_cast<float>(hp) / maxHp, 0.f, 1.f);
            draw->AddRectFilled(ImVec2(a.x - 7, a.y), ImVec2(a.x - 3, b.y), IM_COL32(10, 10, 16, 220));
            draw->AddRectFilled(ImVec2(a.x - 6, b.y - height*fraction), ImVec2(a.x - 4, b.y), IM_COL32(64, 215, 116, 255));
            char label[24];
            std::snprintf(label, sizeof(label), "%d HP", hp);
            draw->AddText(ImVec2(b.x + 6, a.y), IM_COL32(85, 220, 132, 255), label);
        }
        if (g_names.load()) {
            PlayerInfo pi{};
            if (Player(index, pi)) {
                pi.name[31] = 0;
                ImVec2 size = ImGui::CalcTextSize(pi.name);
                draw->AddText(ImVec2(top.x - size.x/2, a.y - size.y - 3), IM_COL32(240, 242, 255, 255), pi.name);
            }
        }
    }
}

void Toggle(const char* name, std::atomic<bool>& state) {
    bool value = state.load();
    if (ImGui::Checkbox(name, &value)) state.store(value);
}

void DrawMenu() {
    if (!g_showMenu.load()) return;
    ImGui::SetNextWindowSize(ImVec2(480, 340), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.96f);
    bool show = true;
    if (ImGui::Begin("GMOD  |  ESP + CHAMS  [x64]", &show, ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::BeginTabBar("Sections")) {
            if (ImGui::BeginTabItem("ESP")) {
                Toggle("Enable player ESP  [F1]", g_esp);
                Toggle("Player boxes", g_boxes);
                Toggle("Health bars and HP", g_health);
                Toggle("Player names", g_names);
                Toggle("Enemies only  [F3]", g_enemyOnly);
                ImGui::Separator();
                ImGui::TextDisabled("ESP only runs when in a map with valid netvars.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("CHAMS")) {
                Toggle("Enable colored models  [F2]", g_chams);
                ImVec4 visible = ImGui::ColorConvertU32ToFloat4(g_visibleColor.load());
                ImVec4 hidden = ImGui::ColorConvertU32ToFloat4(g_hiddenColor.load());
                float front[3] = {visible.x, visible.y, visible.z};
                float back[3] = {hidden.x, hidden.y, hidden.z};
                if (ImGui::ColorEdit3("Visible models", front))
                    g_visibleColor.store(ImGui::ColorConvertFloat4ToU32(ImVec4(front[0], front[1], front[2], 1.f)));
                if (ImGui::ColorEdit3("Behind walls", back))
                    g_hiddenColor.store(ImGui::ColorConvertFloat4ToU32(ImVec4(back[0], back[1], back[2], 1.f)));
                ImGui::TextDisabled("Uses a two-pass Source model render hook.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("CONFIG")) {
                ImGui::Text("F11 - menu   F1 - ESP   F2 - CHAMS   F3 - enemy filter");
                ImGui::Separator();
                ImGui::Text("Engine: %s", g_engine ? "connected" : "not found");
                ImGui::TextDisabled("Log: %%TEMP%%\\GModESP_x64_F11.log");
                ImGui::Text("Entities: %s", g_entityList ? "connected" : "not found");
                ImGui::Text("Model renderer: %s", g_modelRender ? "connected" : "not found");
                ImGui::Text("Chams material: %s", g_material ? "found" : "not found");
                ImGui::Text("m_iHealth: 0x%X", g_healthOffset.load());
                ImGui::Text("m_vecOrigin: 0x%X", g_originOffset.load());
                ImGui::Text("m_iTeamNum: 0x%X", g_teamOffset.load());
                if (ImGui::Button("Rescan netvars")) ResolveNetvars();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::Separator();
        ImGui::TextDisabled("F11 hides the menu. x64 / DirectX 9.");
    }
    ImGui::End();
    if (!show) g_showMenu.store(false);
}

LRESULT CALLBACK WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_showMenu.load() && g_guiReady && ImGui::GetCurrentContext()) {
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
        const ImGuiIO& io = ImGui::GetIO();
        if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST && io.WantCaptureMouse) ||
            ((msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR ||
              msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) && io.WantCaptureKeyboard)) {
            return 1;
        }
    }
    return CallWindowProcW(g_wndProc, hWnd, msg, wParam, lParam);
}

bool InitGui(IDirect3DDevice9* device) {
    if (g_guiReady) return true;
    D3DDEVICE_CREATION_PARAMETERS params{};
    if (FAILED(device->GetCreationParameters(&params)) || !IsWindow(params.hFocusWindow)) return false;
    g_hwnd = params.hFocusWindow;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 7.f;
    style.FrameRounding = 4.f;
    style.GrabRounding = 3.f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(.08f, .105f, .14f, 1.f);
    if (!ImGui_ImplWin32_Init(g_hwnd)) { ImGui::DestroyContext(); return false; }
    if (!ImGui_ImplDX9_Init(device)) {
        ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); return false;
    }
    g_guiDevice = device;
    SetLastError(0);
    LONG_PTR old = SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WindowProc));
    if (!old) {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    g_wndProc = reinterpret_cast<WNDPROC>(old);
    g_guiReady = true;
    return true;
}

void Render(IDirect3DDevice9* dev) {
    static thread_local bool drawing = false;
    if (drawing) return;
    drawing = true;
    PollKeys();
    if (!InitGui(dev)) { drawing = false; return; }
    if (dev != g_guiDevice) { drawing = false; return; }
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::GetIO().MouseDrawCursor = g_showMenu.load();
    if (g_showMenu.load()) ClipCursor(nullptr);
    ImGui::NewFrame();
    DrawEsp();
    DrawMenu();
    ImGui::EndFrame();
    ImGui::Render();
    IDirect3DStateBlock9* backup = nullptr;
    if (SUCCEEDED(dev->CreateStateBlock(D3DSBT_ALL, &backup)) && backup) {
        backup->Capture();
        if (SUCCEEDED(dev->BeginScene())) {
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            dev->EndScene();
        }
        backup->Apply();
        backup->Release();
    }
    drawing = false;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND c, const RGNDATA* d) {
    Render(dev);
    return g_present(dev, a, b, c, d);
}
HRESULT STDMETHODCALLTYPE HookPresentEx(IDirect3DDevice9Ex* dev, const RECT* a, const RECT* b,
                                       HWND c, const RGNDATA* d, DWORD flags) {
    Render(dev);
    return g_presentEx(dev, a, b, c, d, flags);
}
HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* params) {
    if (g_guiReady && g_guiDevice == dev) ImGui_ImplDX9_InvalidateDeviceObjects();
    HRESULT result = g_reset(dev, params);
    if (SUCCEEDED(result) && g_guiReady && g_guiDevice == dev) ImGui_ImplDX9_CreateDeviceObjects();
    return result;
}
HRESULT STDMETHODCALLTYPE HookResetEx(IDirect3DDevice9Ex* dev, D3DPRESENT_PARAMETERS* params, D3DDISPLAYMODEEX* mode) {
    if (g_guiReady && g_guiDevice == dev) ImGui_ImplDX9_InvalidateDeviceObjects();
    HRESULT result = g_resetEx(dev, params, mode);
    if (SUCCEEDED(result) && g_guiReady && g_guiDevice == dev) ImGui_ImplDX9_CreateDeviceObjects();
    return result;
}

void OverrideMaterial(void* mat) {
    if (!g_modelRender) return;
    VFunc<void (*)(void*, void*, int)>(g_modelRender, GModConfig::VTable::ForcedMaterialOverride)(g_modelRender, mat, 0);
}
void Modulate(const ImVec4& color, bool ignoreZ) {
    if (!g_material) return;
    VFunc<void (*)(void*, int, bool)>(g_material, GModConfig::VTable::SetMaterialVarFlag)
        (g_material, GModConfig::MaterialIgnoreZ, ignoreZ);
    VFunc<void (*)(void*, float, float, float)>(g_material, GModConfig::VTable::ColorModulate)
        (g_material, color.x, color.y, color.z);
}
void HookDrawModel(void* self, const void* state, const ModelRenderInfo& info, const void* bones) {
    if (!g_drawModel) return;
    if (g_chams.load() && g_material && InGame() && info.entityIndex > 0 &&
        info.entityIndex <= MaxClients() && ValidPlayer(info.entityIndex, Entity(info.entityIndex))) {
        Modulate(ImGui::ColorConvertU32ToFloat4(g_hiddenColor.load()), true);
        OverrideMaterial(g_material);
        g_drawModel(self, state, info, bones);
        Modulate(ImGui::ColorConvertU32ToFloat4(g_visibleColor.load()), false);
        OverrideMaterial(g_material);
        g_drawModel(self, state, info, bones);
        OverrideMaterial(nullptr);
        Modulate(ImVec4(1,1,1,1), false);
        return;
    }
    g_drawModel(self, state, info, bones);
}

void InstallHook(void* target, void* hook, void** original) {
    if (!target || !hook || !original) return;
    if (MH_CreateHook(target, hook, original) == MH_OK) MH_EnableHook(target);
}

bool InstallD3D9Hooks() {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    constexpr wchar_t name[] = L"GModEsp_D3D9_x64_probe";
    WNDCLASSW klass{};
    klass.lpfnWndProc = DefWindowProcW;
    klass.hInstance = instance;
    klass.lpszClassName = name;
    RegisterClassW(&klass);
    HWND window = CreateWindowExW(0, name, L"", WS_OVERLAPPED, 0, 0, 32, 32,
                                   nullptr, nullptr, instance, nullptr);
    if (!window) return false;
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = window;
    bool hooked = false;
    if (IDirect3D9* api = Direct3DCreate9(D3D_SDK_VERSION)) {
        IDirect3DDevice9* dev = nullptr;
        if (SUCCEEDED(api->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                    D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev)) && dev) {
            auto** vt = *reinterpret_cast<void***>(dev);
            InstallHook(vt[GModConfig::VTable::Present], reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void**>(&g_present));
            InstallHook(vt[GModConfig::VTable::Reset], reinterpret_cast<void*>(&HookReset), reinterpret_cast<void**>(&g_reset));
            hooked = g_present != nullptr;
            dev->Release();
        }
        api->Release();
    }
    IDirect3D9Ex* apiEx = nullptr;
    if (SUCCEEDED(Direct3DCreate9Ex(D3D_SDK_VERSION, &apiEx)) && apiEx) {
        IDirect3DDevice9Ex* deviceEx = nullptr;
        if (SUCCEEDED(apiEx->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                    D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, nullptr, &deviceEx)) && deviceEx) {
            auto** vt = *reinterpret_cast<void***>(deviceEx);
            InstallHook(vt[GModConfig::VTable::PresentEx], reinterpret_cast<void*>(&HookPresentEx), reinterpret_cast<void**>(&g_presentEx));
            InstallHook(vt[GModConfig::VTable::ResetEx], reinterpret_cast<void*>(&HookResetEx), reinterpret_cast<void**>(&g_resetEx));
            hooked = hooked || g_presentEx != nullptr;
            deviceEx->Release();
        }
        apiEx->Release();
    }
    DestroyWindow(window);
    UnregisterClassW(name, instance);
    return hooked;
}

DWORD WINAPI Worker(void*) {
    Log("Injected x64 DLL initialized; expecting engine VEngineClient015, client VClient017");
    for (int i = 0; i < 240; ++i) {
        if (GetModuleHandleW(L"client.dll") && GetModuleHandleW(L"engine.dll") &&
            GetModuleHandleW(L"materialsystem.dll") && GetModuleHandleW(L"d3d9.dll")) break;
        Sleep(250);
    }
    g_engine = GetInterface(L"engine.dll", GModConfig::EngineVersions);
    g_client = GetInterface(L"client.dll", GModConfig::ClientVersions);
    g_entityList = GetInterface(L"client.dll", GModConfig::EntityVersions);
    g_modelRender = GetInterface(L"engine.dll", GModConfig::ModelVersions);
    g_materialSystem = GetInterface(L"materialsystem.dll", GModConfig::MaterialVersions);
    Log("Interfaces: engine=%d client=%d entities=%d models=%d materialSystem=%d",
        g_engine != nullptr, g_client != nullptr, g_entityList != nullptr,
        g_modelRender != nullptr, g_materialSystem != nullptr);
    if (g_client) {
        bool found = ResolveNetvars();
        Log("Netvars: found=%d health=0x%X maxHealth=0x%X team=0x%X origin=0x%X",
            found, g_healthOffset.load(), g_maxHealthOffset.load(),
            g_teamOffset.load(), g_originOffset.load());
    }
    if (g_materialSystem) {
        g_material = VFunc<void* (*)(void*, const char*, const char*, bool, const char*)>
            (g_materialSystem, GModConfig::VTable::FindMaterial)
            (g_materialSystem, "models/debug/debugwhite", "Model textures", false, nullptr);
    }
    if (MH_Initialize() != MH_OK) { Log("MinHook initialization failed"); return 1; }
    if (g_modelRender && g_material) {
        auto** vt = *reinterpret_cast<void***>(g_modelRender);
        InstallHook(vt[GModConfig::VTable::DrawModelExecute], reinterpret_cast<void*>(&HookDrawModel),
                    reinterpret_cast<void**>(&g_drawModel));
    }
    Log("Material: %d; DrawModelExecute original: %d", g_material != nullptr, g_drawModel != nullptr);
    bool d3dHooked = false;
    for (int attempt = 0; attempt < 60; ++attempt) {
        if (InstallD3D9Hooks()) { d3dHooked = true; break; }
        Sleep(500);
    }
    Log("D3D9 hooks: %s", d3dHooked ? "installed" : "not installed");
    return 0;
}

}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        HANDLE handle = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (handle) CloseHandle(handle);
    }
    return TRUE;
}
