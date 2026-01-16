#include "pch.h"
#include "ControllerOverlayPro.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Xinput.h>
#pragma comment(lib, "xinput9_1_0.lib")

#include <cmath>
#include <cctype>
#include <string>
#include <memory>

#include "imgui/imgui.h"

// Set your release version here:
static const char* COP_VERSION = "1.0.0";

// -------------------- Helpers --------------------
static float Clamp(float v, float lo, float hi) { return (v < lo) ? lo : (v > hi ? hi : v); }
static float Clamp01(float v) { return Clamp(v, 0.0f, 1.0f); }

static std::string ToLowerCopy(std::string s)
{
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static std::string ToUpperCopy(std::string s)
{
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

static float ApplyDeadzone(float v, float dz)
{
    dz = Clamp(dz, 0.0f, 0.95f);
    if (std::fabs(v) < dz) return 0.0f;

    float sign = (v < 0.0f) ? -1.0f : 1.0f;
    float t = (std::fabs(v) - dz) / (1.0f - dz);
    t = Clamp(t, 0.0f, 1.0f);
    return sign * t;
}

static std::string SkinPath(const std::string& dataFolder, const std::string& skin, const std::string& file)
{
    return dataFolder + "/ControllerOverlay/skins/" + skin + "/" + file;
}

static std::string BaseFilenameForSkin(const std::string& skinLower)
{
    if (skinLower == "ps4") return "PS4 Base.png";
    if (skinLower == "ps5") return "PS5 Base.png";
    return "Xbox Base.png";
}

static bool IsWorkshopMap(std::shared_ptr<GameWrapper> gw)
{
    if (!gw) return false;
    std::string m = gw->GetCurrentMap();
    if (m.empty()) return false;
    m = ToLowerCopy(m);
    return (m.find("workshop") != std::string::npos);
}

// -------------------- Canvas compatibility --------------------
template <typename CANVAS>
static auto TrySetColor(CANVAS& c, int r, int g, int b, int a, int)
-> decltype(c.SetColor(r, g, b, a), void()) {
    c.SetColor(r, g, b, a);
}

template <typename CANVAS>
static auto TrySetColor(CANVAS& c, int r, int g, int b, int a, long)
-> decltype(c.SetDrawColor(r, g, b, a), void()) {
    c.SetDrawColor(r, g, b, a);
}

template <typename CANVAS>
static void TrySetColor(CANVAS&, int, int, int, int, char) {}

static void ForceFullBright(CanvasWrapper& canvas)
{
    TrySetColor(canvas, 255, 255, 255, 255, 0);
}

template <typename CANVAS>
static auto TrySetPos(CANVAS& c, float x, float y, int)
-> decltype(c.SetPosition(Vector2{ (int)x, (int)y }), void()) {
    c.SetPosition(Vector2{ (int)x, (int)y });
}

template <typename CANVAS>
static auto TrySetPos(CANVAS& c, float x, float y, long)
-> decltype(c.SetPosition(Vector2F{ x, y }), void()) {
    c.SetPosition(Vector2F{ x, y });
}

template <typename CANVAS>
static void TrySetPos(CANVAS&, float, float, char) {}

static void DrawImage(CanvasWrapper& canvas, ImageWrapper* img, float x, float y, float scale)
{
    if (!img) return;
    ForceFullBright(canvas);
    TrySetPos(canvas, x, y, 0);
    canvas.DrawTexture(img, scale);
}

static std::shared_ptr<ImageWrapper> MakeImageCanvas(const std::string& path)
{
    auto img = std::make_shared<ImageWrapper>(path, true, false);
    img->LoadForCanvas();
    return img;
}

static std::string OffKeyX(const std::string& k) { return "cop_off_" + k + "_x"; }
static std::string OffKeyY(const std::string& k) { return "cop_off_" + k + "_y"; }

// START state for controller-menu latch
static bool g_copStartDown = false;

// -------------------- Plugin --------------------
BAKKESMOD_PLUGIN(ControllerOverlayPro, "Controller Overlay Pro", COP_VERSION, PLUGINTYPE_FREEPLAY)

float ControllerOverlayPro::GetOffX(const std::string& key)
{
    return cvarManager->getCvar(OffKeyX(key)).getFloatValue();
}

float ControllerOverlayPro::GetOffY(const std::string& key)
{
    return cvarManager->getCvar(OffKeyY(key)).getFloatValue();
}

void ControllerOverlayPro::onLoad()
{
    // Version cvar
    cvarManager->registerCvar("cop_version", COP_VERSION, "Controller Overlay Pro version");

    // Core
    cvarManager->registerCvar("cop_enabled", "1", "Enable overlay (0/1)");

    // Default bind key (F9)
    cvarManager->registerCvar("cop_bind_key", "F9", "Key to bind to cop_toggle (example: F9, F10, K, MOUSE1)");

    // Allowed modes
    cvarManager->registerCvar("cop_online_only", "1", "Require allowed mode (0/1).");
    cvarManager->registerCvar("cop_show_freeplay_workshop", "1", "Also allow Freeplay + Workshop (0/1)");

    // Hide in menus/settings/pause
    cvarManager->registerCvar("cop_hide_in_menus", "1", "Hide overlay when any menu is open (0/1)");
    cvarManager->registerCvar("cop_hide_controller_menu", "1", "Hide when controller pause/menu is opened (0/1)");

    // IMPORTANT: allow overlay while BakkesMod menu (cursor) is open
    cvarManager->registerCvar("cop_show_when_menu_open", "1",
        "Show overlay when BakkesMod menu is open (cursor visible) (0/1)");

    // Position defaults
    cvarManager->registerCvar("cop_x", "20", "Overlay X");
    cvarManager->registerCvar("cop_y", "980", "Overlay Y (1440p start)");
    cvarManager->registerCvar("cop_scale", "1.0", "Overlay scale");

    cvarManager->registerCvar("cop_layer_shift_x", "0", "Global shift X for layers");
    cvarManager->registerCvar("cop_layer_shift_y", "0", "Global shift Y for layers");

    // Input feel
    cvarManager->registerCvar("cop_sticks_always", "1", "Always draw sticks (0/1)");
    cvarManager->registerCvar("cop_stick_range", "18", "Stick movement range (pixels)");
    cvarManager->registerCvar("cop_deadzone", "0.12", "Stick deadzone");
    cvarManager->registerCvar("cop_stick_smooth", "0.90", "Stick smoothing (0..0.98)");
    cvarManager->registerCvar("cop_trigger_thresh", "0.10", "Trigger threshold");

    cvarManager->registerCvar("cop_pad", "-1", "XInput pad: -1 auto, 0-3 fixed");

    // Skin
    auto skinCvar = cvarManager->registerCvar("cop_skin", "xbox", "Skin folder: xbox / ps4 / ps5");
    skinCvar.addOnValueChanged([this](std::string, CVarWrapper cvar) {
        std::string v = ToLowerCopy(cvar.getStringValue());
        if (v.empty()) return;
        skinName = v;
        FreeTextures();
        cvarManager->log("[cop] skin changed to: " + skinName);
        });

    // Per-layer offsets
    const char* keys[] = { "A","B","X","Y","DU","DD","DL","DR","LB","RB","LT","RT","L3","R3" };
    for (auto k : keys)
    {
        cvarManager->registerCvar(OffKeyX(k), "0", std::string("Offset X for ") + k);
        cvarManager->registerCvar(OffKeyY(k), "0", std::string("Offset Y for ") + k);
    }

    // ---- Console commands ----
    cvarManager->registerNotifier("cop_toggle", [this](std::vector<std::string>) {
        auto cv = cvarManager->getCvar("cop_enabled");
        cv.setValue(cv.getIntValue() ? 0 : 1);
        cvarManager->log(std::string("[cop] overlay: ") + (cv.getIntValue() ? "ON" : "OFF"));
        }, "Toggle overlay. Bind a key: bind F9 \"cop_toggle\"", 0);

    cvarManager->registerNotifier("cop_resetpos", [this](std::vector<std::string>) {
        cvarManager->getCvar("cop_x").setValue(20.f);
        cvarManager->getCvar("cop_y").setValue(980.f);
        cvarManager->getCvar("cop_scale").setValue(1.0f);
        cvarManager->getCvar("cop_layer_shift_x").setValue(0.f);
        cvarManager->getCvar("cop_layer_shift_y").setValue(0.f);
        cvarManager->log("[cop] position reset");
        }, "Reset overlay position/scale to defaults", 0);

    cvarManager->registerNotifier("cop_bind_apply", [this](std::vector<std::string>) {
        std::string key = cvarManager->getCvar("cop_bind_key").getStringValue();
        if (key.empty()) key = "F9";
        key = ToUpperCopy(key);

        cvarManager->executeCommand("bind " + key + " \"cop_toggle\"");
        cvarManager->log("[cop] bound " + key + " to cop_toggle");
        }, "Bind cop_toggle to cop_bind_key. Example: cop_bind_key F9; cop_bind_apply", 0);

    gameWrapper->RegisterDrawable([this](CanvasWrapper canvas) { Render(canvas); });

    cvarManager->log("[cop] loaded. F2 -> Plugins -> Controller Overlay Pro");
}

void ControllerOverlayPro::onUnload()
{
    FreeTextures();
}

void ControllerOverlayPro::FreeTextures()
{
    texturesLoaded = false;
    baseImg.reset();
    layers.clear();
}

void ControllerOverlayPro::EnsureTexturesLoaded()
{
    if (texturesLoaded) return;

    const std::string dataFolder = gameWrapper->GetDataFolder().string();
    const std::string skinLower = ToLowerCopy(skinName);
    const std::string baseFile = BaseFilenameForSkin(skinLower);

    baseImg = MakeImageCanvas(SkinPath(dataFolder, skinName, baseFile));

    layers["A"] = MakeImageCanvas(SkinPath(dataFolder, skinName, "A Button.png"));
    layers["B"] = MakeImageCanvas(SkinPath(dataFolder, skinName, "B Button.png"));
    layers["X"] = MakeImageCanvas(SkinPath(dataFolder, skinName, "X Button.png"));
    layers["Y"] = MakeImageCanvas(SkinPath(dataFolder, skinName, "Y Button.png"));

    layers["DU"] = MakeImageCanvas(SkinPath(dataFolder, skinName, "Dpad Up.png"));
    layers["DD"] = MakeImageCanvas(SkinPath(dataFolder, skinName, "Dpad Down.png"));
    layers["DL"] = MakeImageCanvas(SkinPath(dataFolder, skinName, "Dpad Left.png"));
    layers["DR"] = MakeImageCanvas(SkinPath(dataFolder, skinName, "Dpad Right.png"));

    layers["LB"] = MakeImageCanvas(SkinPath(dataFolder, skinName, "Left Bumper.png"));
    layers["RB"] = MakeImageCanvas(SkinPath(dataFolder, skinName, "Right Bumper.png"));

    layers["LT"] = MakeImageCanvas(SkinPath(dataFolder, skinName, "Left Trigger.png"));
    layers["RT"] = MakeImageCanvas(SkinPath(dataFolder, skinName, "Right Trigger.png"));

    layers["L3"] = MakeImageCanvas(SkinPath(dataFolder, skinName, "L3.png"));
    layers["R3"] = MakeImageCanvas(SkinPath(dataFolder, skinName, "R3.png"));

    texturesLoaded = true;
}

void ControllerOverlayPro::PollController()
{
    int pad = cvarManager->getCvar("cop_pad").getIntValue();

    XINPUT_STATE s{};
    bool found = false;

    auto tryPad = [&](int i) -> bool {
        ZeroMemory(&s, sizeof(s));
        return XInputGetState(i, &s) == ERROR_SUCCESS;
        };

    if (pad >= 0 && pad <= 3)
        found = tryPad(pad);
    else
        for (int i = 0; i < 4; ++i)
            if (tryPad(i)) { found = true; break; }

    if (!found)
    {
        st = State{};
        g_copStartDown = false;
        return;
    }

    WORD b = s.Gamepad.wButtons;
    g_copStartDown = ((b & XINPUT_GAMEPAD_START) != 0);

    st.a = (b & XINPUT_GAMEPAD_A) != 0;
    st.b = (b & XINPUT_GAMEPAD_B) != 0;
    st.x = (b & XINPUT_GAMEPAD_X) != 0;
    st.y = (b & XINPUT_GAMEPAD_Y) != 0;

    st.du = (b & XINPUT_GAMEPAD_DPAD_UP) != 0;
    st.dd = (b & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
    st.dl = (b & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
    st.dr = (b & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;

    st.lb = (b & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
    st.rb = (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;

    st.l3 = (b & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
    st.r3 = (b & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;

    st.lt = s.Gamepad.bLeftTrigger / 255.0f;
    st.rt = s.Gamepad.bRightTrigger / 255.0f;

    auto norm = [](SHORT v)
        {
            if (v >= 0) return (float)v / 32767.0f;
            return (float)v / 32768.0f;
        };

    st.lx_raw = norm(s.Gamepad.sThumbLX);
    st.ly_raw = -norm(s.Gamepad.sThumbLY);
    st.rx_raw = norm(s.Gamepad.sThumbRX);
    st.ry_raw = -norm(s.Gamepad.sThumbRY);

    float smooth = Clamp(cvarManager->getCvar("cop_stick_smooth").getFloatValue(), 0.0f, 0.98f);
    float k = 1.0f - smooth;

    st.lx += (st.lx_raw - st.lx) * k;
    st.ly += (st.ly_raw - st.ly) * k;
    st.rx += (st.rx_raw - st.rx) * k;
    st.ry += (st.ry_raw - st.ry) * k;
}

void ControllerOverlayPro::Render(CanvasWrapper canvas)
{
    if (cvarManager->getCvar("cop_enabled").getIntValue() == 0)
        return;

    PollController();

    bool allow = false;
    if (gameWrapper->IsInOnlineGame())
        allow = true;
    else if (cvarManager->getCvar("cop_show_freeplay_workshop").getIntValue() != 0)
        allow = gameWrapper->IsInFreeplay() || IsWorkshopMap(gameWrapper);

    if (cvarManager->getCvar("cop_online_only").getIntValue() != 0 && !allow)
        return;

    // Controller menu latch (START toggles, B closes)
    static bool menuLatched = false;
    static bool prevStart = false;
    static bool prevB = false;

    bool startNow = g_copStartDown;
    bool startEdge = (startNow && !prevStart);
    prevStart = startNow;

    bool bNow = st.b;
    bool bEdge = (bNow && !prevB);
    prevB = bNow;

    if (cvarManager->getCvar("cop_hide_controller_menu").getIntValue() != 0)
    {
        if (startEdge) menuLatched = !menuLatched;
        if (menuLatched && bEdge) menuLatched = false;
    }
    if (!allow) menuLatched = false;

    // Hide in menus - BUT allow overlay while F2 menu open if cop_show_when_menu_open=1
    if (cvarManager->getCvar("cop_hide_in_menus").getIntValue() != 0)
    {
        if (gameWrapper->IsPaused())
            return;

        const bool showWhenMenuOpen = cvarManager->getCvar("cop_show_when_menu_open").getIntValue() != 0;
        if (!showWhenMenuOpen)
        {
            if (gameWrapper->IsCursorVisible() != 0)
                return;
        }

        if (menuLatched)
            return;
    }

    EnsureTexturesLoaded();
    if (!texturesLoaded || !baseImg) return;

    float x = cvarManager->getCvar("cop_x").getFloatValue();
    float y = cvarManager->getCvar("cop_y").getFloatValue();
    float scale = Clamp(cvarManager->getCvar("cop_scale").getFloatValue(), 0.05f, 5.0f);

    float layerShiftX = cvarManager->getCvar("cop_layer_shift_x").getFloatValue();
    float layerShiftY = cvarManager->getCvar("cop_layer_shift_y").getFloatValue();

    float stickRangePx = cvarManager->getCvar("cop_stick_range").getFloatValue();
    float deadzone = Clamp01(cvarManager->getCvar("cop_deadzone").getFloatValue());
    float trigThresh = Clamp01(cvarManager->getCvar("cop_trigger_thresh").getFloatValue());
    bool sticksAlways = cvarManager->getCvar("cop_sticks_always").getIntValue() != 0;

    float bx = x + layerShiftX;
    float by = y + layerShiftY;

    // Base
    DrawImage(canvas, baseImg.get(), bx, by, scale);

    auto drawIf = [&](const char* key, bool on)
        {
            if (!on) return;
            auto it = layers.find(key);
            if (it == layers.end()) return;

            float ox = GetOffX(key);
            float oy = GetOffY(key);
            DrawImage(canvas, it->second.get(), bx + ox, by + oy, scale);
        };

    // Buttons
    drawIf("A", st.a);
    drawIf("B", st.b);
    drawIf("X", st.x);
    drawIf("Y", st.y);

    // D-pad
    drawIf("DU", st.du);
    drawIf("DD", st.dd);
    drawIf("DL", st.dl);
    drawIf("DR", st.dr);

    // Bumpers
    drawIf("LB", st.lb);
    drawIf("RB", st.rb);

    // Triggers
    drawIf("LT", st.lt > trigThresh);
    drawIf("RT", st.rt > trigThresh);

    // Sticks (L3/R3 images are your “stick layer”)
    auto drawStick = [&](const char* key, float ax, float ay, bool pressed)
        {
            auto it = layers.find(key);
            if (it == layers.end()) return;

            float dx = ApplyDeadzone(ax, deadzone);
            float dy = ApplyDeadzone(ay, deadzone);

            if (!sticksAlways && !pressed && dx == 0.0f && dy == 0.0f)
                return;

            float ox = GetOffX(key);
            float oy = GetOffY(key);

            DrawImage(canvas, it->second.get(),
                bx + ox + dx * stickRangePx,
                by + oy + dy * stickRangePx,
                scale);
        };

    drawStick("L3", st.lx, st.ly, st.l3);
    drawStick("R3", st.rx, st.ry, st.r3);
}

// -------------------- Settings window (CRASH-SAFE) --------------------
std::string ControllerOverlayPro::GetPluginName()
{
    return "Controller Overlay Pro";
}

void ControllerOverlayPro::SetImGuiContext(uintptr_t ctx)
{
    if (ctx == 0)
    {
        imguiReady = false;
        return;
    }
    ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(ctx));
    imguiReady = true;
}

void ControllerOverlayPro::RenderSettings()
{
    if (!imguiReady || ImGui::GetCurrentContext() == nullptr)
        return;

    // Credit line
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.20f, 0.95f, 1.0f));
    ImGui::TextUnformatted("Designed by ABW ASSASSIN");
    ImGui::PopStyleColor();
    ImGui::Separator();

    ImGui::Text("Controller Overlay Pro");
    ImGui::SameLine();
    ImGui::TextDisabled("(v%s)", COP_VERSION);

    bool enabled = cvarManager->getCvar("cop_enabled").getIntValue() != 0;
    if (ImGui::Checkbox("Enabled", &enabled))
        cvarManager->getCvar("cop_enabled").setValue(enabled ? 1 : 0);

    bool showMenuOpen = cvarManager->getCvar("cop_show_when_menu_open").getIntValue() != 0;
    if (ImGui::Checkbox("Show overlay while menu open (F2)", &showMenuOpen))
        cvarManager->getCvar("cop_show_when_menu_open").setValue(showMenuOpen ? 1 : 0);

    ImGui::Separator();

    // Hotkey bind
    ImGui::Text("Toggle Hotkey");
    std::string curKey = cvarManager->getCvar("cop_bind_key").getStringValue();

    static char keyBuf[32]{};
    std::snprintf(keyBuf, sizeof(keyBuf), "%s", curKey.c_str());
    ImGui::InputText("Key", keyBuf, IM_ARRAYSIZE(keyBuf));
    ImGui::SameLine();
    if (ImGui::Button("Apply Bind"))
    {
        cvarManager->getCvar("cop_bind_key").setValue(std::string(keyBuf));
        cvarManager->executeCommand("cop_bind_apply");
    }
    ImGui::TextDisabled("Examples: F9, F10, F12, K, L, MOUSE1, MOUSE2");

    ImGui::Separator();

    // Skin selection
    const char* skins[] = { "xbox", "ps4", "ps5" };
    std::string cur = ToLowerCopy(cvarManager->getCvar("cop_skin").getStringValue());

    int idx = 0;
    if (cur == "ps4") idx = 1;
    else if (cur == "ps5") idx = 2;

    if (ImGui::Combo("Skin", &idx, skins, IM_ARRAYSIZE(skins)))
        cvarManager->getCvar("cop_skin").setValue(std::string(skins[idx]));

    ImGui::TextDisabled("Base filenames: Xbox Base.png / PS4 Base.png / PS5 Base.png");

    ImGui::Separator();

    // Position controls
    float x = cvarManager->getCvar("cop_x").getFloatValue();
    float y = cvarManager->getCvar("cop_y").getFloatValue();
    float s = cvarManager->getCvar("cop_scale").getFloatValue();

    if (ImGui::SliderFloat("X", &x, 0.f, 4000.f))
        cvarManager->getCvar("cop_x").setValue(x);

    if (ImGui::SliderFloat("Y", &y, 0.f, 4000.f))
        cvarManager->getCvar("cop_y").setValue(y);

    s = Clamp(s, 0.1f, 3.0f);
    if (ImGui::SliderFloat("Scale", &s, 0.1f, 3.0f))
        cvarManager->getCvar("cop_scale").setValue(s);

    if (ImGui::Button("Reset Position"))
        cvarManager->executeCommand("cop_resetpos");
}
