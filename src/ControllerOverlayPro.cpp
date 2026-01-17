#include "pch.h"
#include "ControllerOverlayPro.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Xinput.h>
#pragma comment(lib, "xinput.lib")

#include <cmath>
#include <cctype>
#include <string>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <memory>

// -------------------- Helpers --------------------
static float Clamp(float v, float lo, float hi) { return (v < lo) ? lo : (v > hi ? hi : v); }
static float Clamp01(float v) { return Clamp(v, 0.0f, 1.0f); }

static float ApplyDeadzone(float v, float dz)
{
    dz = Clamp(dz, 0.0f, 0.95f);
    if (std::fabs(v) < dz) return 0.0f;

    float sign = (v < 0.0f) ? -1.0f : 1.0f;
    float t = (std::fabs(v) - dz) / (1.0f - dz);
    t = Clamp(t, 0.0f, 1.0f);
    return sign * t;
}

static bool KeyDown(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static std::string SkinPath(const std::string& dataFolder, const std::string& skin, const std::string& file)
{
    // NOTE: We ONLY support underscore filenames now (no spaces).
    return dataFolder + "/ControllerOverlayPro/skins/" + skin + "/" + file;
}

// Workshop detection using current map name (Steam workshop usually contains "workshop")
static bool IsWorkshopMap(std::shared_ptr<GameWrapper> gw)
{
    if (!gw) return false;
    std::string m = gw->GetCurrentMap();
    if (m.empty()) return false;

    for (char& c : m) c = (char)std::tolower((unsigned char)c);
    return (m.find("workshop") != std::string::npos);
}

// Saved pos file
static std::filesystem::path GetSavePath(std::shared_ptr<GameWrapper> gw)
{
    std::filesystem::path base = gw->GetDataFolder();
    std::filesystem::path dir = base / "ControllerOverlayPro";
    std::filesystem::create_directories(dir);
    return dir / "xco_saved_pos.ini";
}

static void SaveOverlayPos(std::shared_ptr<GameWrapper> gw, float x, float y, float scale)
{
    try
    {
        auto path = GetSavePath(gw);
        std::ofstream f(path.string(), std::ios::trunc);
        if (!f.is_open()) return;

        f << "x=" << x << "\n";
        f << "y=" << y << "\n";
        f << "scale=" << scale << "\n";
        f.close();
    }
    catch (...) {}
}

static bool LoadOverlayPos(std::shared_ptr<GameWrapper> gw, float& x, float& y, float& scale)
{
    try
    {
        auto path = GetSavePath(gw);
        std::ifstream f(path.string());
        if (!f.is_open()) return false;

        std::string line;
        float lx = x, ly = y, ls = scale;
        while (std::getline(f, line))
        {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);

            float val = 0.f;
            try { val = std::stof(v); }
            catch (...) { continue; }

            if (k == "x") lx = val;
            else if (k == "y") ly = val;
            else if (k == "scale") ls = val;
        }
        x = lx; y = ly; scale = ls;
        return true;
    }
    catch (...) { return false; }
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

static std::shared_ptr<ImageWrapper> MakeImage(const std::string& path)
{
    auto img = std::make_shared<ImageWrapper>(path, true, false);
    img->LoadForCanvas();
    return img;
}

static std::string OffKeyX(const std::string& k) { return "xco_off_" + k + "_x"; }
static std::string OffKeyY(const std::string& k) { return "xco_off_" + k + "_y"; }

// -------------------- Plugin --------------------
BAKKESMOD_PLUGIN(ControllerOverlayPro, "Controller Overlay Pro", "1.0.4", PLUGINTYPE_FREEPLAY)

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
    // Version cvar (for support/debug)
    cvarManager->registerCvar("xco_version", "1.0.4", "ControllerOverlayPro version (read-only)");

    cvarManager->registerCvar("xco_enabled", "1", "Enable overlay (0/1)");

    // Allowed modes
    cvarManager->registerCvar("xco_online_only", "1", "Require allowed mode (0/1).");
    cvarManager->registerCvar("xco_show_freeplay_workshop", "1", "Also allow Freeplay + Workshop (0/1)");

    // Hide in pause/menus (IMPORTANT: we do NOT hide on IsCursorVisible so it stays visible while F2 is open)
    cvarManager->registerCvar("xco_hide_in_menus", "1", "Hide overlay when paused / controller-menu latch (0/1)");
    cvarManager->registerCvar("xco_hide_controller_menu", "1", "Hide when controller pause/menu is opened (0/1)");

    // Position / scale
    cvarManager->registerCvar("xco_x", "20", "Overlay X");
    cvarManager->registerCvar("xco_y", "980", "Overlay Y (1440p start)");
    cvarManager->registerCvar("xco_scale", "1.0", "Overlay scale (700px pack => 1.0)");

    cvarManager->registerCvar("xco_layer_shift_x", "0", "Global shift X for layers");
    cvarManager->registerCvar("xco_layer_shift_y", "0", "Global shift Y for layers");

    // Input visuals
    cvarManager->registerCvar("xco_sticks_always", "1", "Always draw sticks (0/1)");
    cvarManager->registerCvar("xco_stick_range", "18", "Stick movement range (pixels)");
    cvarManager->registerCvar("xco_deadzone", "0.12", "Stick deadzone");
    cvarManager->registerCvar("xco_stick_smooth", "0.90", "Stick smoothing (0..0.98)");
    cvarManager->registerCvar("xco_trigger_thresh", "0.10", "Trigger threshold");

    // Controller / hotkey
    cvarManager->registerCvar("xco_pad", "-1", "XInput pad: -1 auto, 0-3 fixed");

    // Hotkey VK code (default F9 = 120)
    cvarManager->registerCvar("xco_hotkey_vk", "120", "Hotkey virtual-key code (default F9=120)");

    // Skin selection (xbox/ps4/ps5)
    auto skinCvar = cvarManager->registerCvar("xco_skin", "xbox", "Skin folder: xbox / ps4 / ps5");
    skinCvar.addOnValueChanged([this](std::string, CVarWrapper cvar) {
        std::string v = cvar.getStringValue();
        if (v.empty()) return;
        for (char& ch : v) ch = (char)std::tolower((unsigned char)ch);
        skinName = v;
        FreeTextures();
        cvarManager->log("[xco] skin changed to: " + skinName);
        });

    // Autosave + load saved
    cvarManager->registerCvar("xco_autosave", "0", "Autosave last pos while moving (0/1).");
    cvarManager->registerCvar("xco_load_saved", "0", "Load saved pos at startup (0/1).");

    // Reset position button target
    cvarManager->registerNotifier("xco_reset_pos", [this](std::vector<std::string>) {
        cvarManager->getCvar("xco_x").setValue(20.f);
        cvarManager->getCvar("xco_y").setValue(980.f);
        cvarManager->getCvar("xco_scale").setValue(1.0f);
        cvarManager->log("[xco] reset position to defaults");
        }, "Reset overlay position/scale", 0);

    // Toggle overlay command (for binds)
    cvarManager->registerNotifier("xco_toggle", [this](std::vector<std::string>) {
        auto cv = cvarManager->getCvar("xco_enabled");
        cv.setValue(cv.getIntValue() ? 0 : 1);
        }, "Toggle controller overlay", 0);

    // Print position
    cvarManager->registerNotifier("xco_pos", [this](std::vector<std::string>) {
        float x = cvarManager->getCvar("xco_x").getFloatValue();
        float y = cvarManager->getCvar("xco_y").getFloatValue();
        float s = cvarManager->getCvar("xco_scale").getFloatValue();
        cvarManager->log("[xco] x=" + std::to_string(x) + " y=" + std::to_string(y) + " scale=" + std::to_string(s));
        }, "Print current overlay position + scale", 0);

    // Per-layer offsets
    const char* keys[] = { "A","B","X","Y","DU","DD","DL","DR","LB","RB","LT","RT","L3","R3" };
    for (auto k : keys)
    {
        cvarManager->registerCvar(OffKeyX(k), "0", std::string("Offset X for ") + k);
        cvarManager->registerCvar(OffKeyY(k), "0", std::string("Offset Y for ") + k);
    }

    // Optional: load saved pos at startup
    if (cvarManager->getCvar("xco_load_saved").getIntValue() != 0)
    {
        float x = cvarManager->getCvar("xco_x").getFloatValue();
        float y = cvarManager->getCvar("xco_y").getFloatValue();
        float s = cvarManager->getCvar("xco_scale").getFloatValue();
        if (LoadOverlayPos(gameWrapper, x, y, s))
        {
            cvarManager->getCvar("xco_x").setValue(x);
            cvarManager->getCvar("xco_y").setValue(y);
            cvarManager->getCvar("xco_scale").setValue(s);
            cvarManager->log("[xco] loaded saved overlay position");
        }
    }

    gameWrapper->RegisterDrawable([this](CanvasWrapper canvas) { Render(canvas); });
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

    auto load = [&](const std::string& key, const std::string& fileName) -> std::shared_ptr<ImageWrapper>
    {
        const std::string full = SkinPath(dataFolder, skinName, fileName);
        std::filesystem::path p = std::filesystem::u8path(full);

        if (!std::filesystem::exists(p))
        {
            cvarManager->log("[xco] MISSING PNG (underscore format required): " + full);
            return nullptr;
        }

        return MakeImage(full);
    };

    // Base name changes by skin (UNDERSCORE FORMAT ONLY)
    // xbox: "Xbox_Base.png"
    // ps4 : "PS4_Base.png"
    // ps5 : "PS5_Base.png"
    std::string baseFile = "Xbox_Base.png";
    if (skinName == "ps4") baseFile = "PS4_Base.png";
    else if (skinName == "ps5") baseFile = "PS5_Base.png";

    baseImg = load("__BASE__", baseFile);

    // UNDERSCORE FORMAT ONLY
    layers["A"]  = load("A",  "A_Button.png");
    layers["B"]  = load("B",  "B_Button.png");
    layers["X"]  = load("X",  "X_Button.png");
    layers["Y"]  = load("Y",  "Y_Button.png");

    layers["DU"] = load("DU", "Dpad_Up.png");
    layers["DD"] = load("DD", "Dpad_Down.png");
    layers["DL"] = load("DL", "Dpad_Left.png");
    layers["DR"] = load("DR", "Dpad_Right.png");

    layers["LB"] = load("LB", "Left_Bumper.png");
    layers["RB"] = load("RB", "Right_Bumper.png");

    layers["LT"] = load("LT", "Left_Trigger.png");
    layers["RT"] = load("RT", "Right_Trigger.png");

    layers["L3"] = load("L3", "L3.png");
    layers["R3"] = load("R3", "R3.png");

    texturesLoaded = true;
}

void ControllerOverlayPro::PollController()
{
    int pad = cvarManager->getCvar("xco_pad").getIntValue();

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
        return;
    }

    WORD b = s.Gamepad.wButtons;

    st.start = (b & XINPUT_GAMEPAD_START) != 0;

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

    float smooth = Clamp(cvarManager->getCvar("xco_stick_smooth").getFloatValue(), 0.0f, 0.98f);
    float k = 1.0f - smooth;

    st.lx += (st.lx_raw - st.lx) * k;
    st.ly += (st.ly_raw - st.ly) * k;

    st.rx += (st.rx_raw - st.rx) * k;
    st.ry += (st.ry_raw - st.ry) * k;
}

void ControllerOverlayPro::Render(CanvasWrapper canvas)
{
    // Hotkey toggle (default F9) - works anytime RL is focused
    int vk = cvarManager->getCvar("xco_hotkey_vk").getIntValue();
    vk = (int)Clamp((float)vk, 1.f, 255.f);

    bool hkNow = KeyDown(vk);
    bool hkEdge = hkNow && !prevHotkeyDown;
    prevHotkeyDown = hkNow;

    if (hkEdge)
    {
        auto cv = cvarManager->getCvar("xco_enabled");
        cv.setValue(cv.getIntValue() ? 0 : 1);
    }

    if (cvarManager->getCvar("xco_enabled").getIntValue() == 0)
        return;

    // Poll controller state
    PollController();

    // ----- Allowed modes -----
    bool allow = false;

    if (gameWrapper->IsInOnlineGame())
    {
        allow = true;
    }
    else if (cvarManager->getCvar("xco_show_freeplay_workshop").getIntValue() != 0)
    {
        if (gameWrapper->IsInFreeplay() || IsWorkshopMap(gameWrapper))
            allow = true;
    }

    if (cvarManager->getCvar("xco_online_only").getIntValue() != 0)
    {
        if (!allow)
            return;
    }

    // ----- Controller menu latch (START toggles open/close; B closes) -----
    static bool menuLatched = false;
    static bool prevStart = false;
    static bool prevB = false;

    bool startEdge = (st.start && !prevStart);
    prevStart = st.start;

    bool bEdge = (st.b && !prevB);
    prevB = st.b;

    if (cvarManager->getCvar("xco_hide_controller_menu").getIntValue() != 0)
    {
        if (startEdge) menuLatched = !menuLatched;
        if (menuLatched && bEdge) menuLatched = false;
    }

    if (!allow) menuLatched = false;

    // Hide logic (IMPORTANT: do NOT hide just because cursor is visible -> overlay stays visible while F2 is open)
    if (cvarManager->getCvar("xco_hide_in_menus").getIntValue() != 0)
    {
        if (gameWrapper->IsPaused())
            return;

        if (menuLatched)
            return;
    }

    EnsureTexturesLoaded();
    if (!texturesLoaded || !baseImg) return;

    float x = cvarManager->getCvar("xco_x").getFloatValue();
    float y = cvarManager->getCvar("xco_y").getFloatValue();
    float scale = Clamp(cvarManager->getCvar("xco_scale").getFloatValue(), 0.05f, 5.0f);

    float layerShiftX = cvarManager->getCvar("xco_layer_shift_x").getFloatValue();
    float layerShiftY = cvarManager->getCvar("xco_layer_shift_y").getFloatValue();

    float stickRangePx = cvarManager->getCvar("xco_stick_range").getFloatValue();
    float deadzone = Clamp01(cvarManager->getCvar("xco_deadzone").getFloatValue());
    float trigThresh = Clamp01(cvarManager->getCvar("xco_trigger_thresh").getFloatValue());
    bool sticksAlways = cvarManager->getCvar("xco_sticks_always").getIntValue() != 0;

    float bx = x + layerShiftX;
    float by = y + layerShiftY;

    // Draw base
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

    drawIf("A", st.a);
    drawIf("B", st.b);
    drawIf("X", st.x);
    drawIf("Y", st.y);

    drawIf("DU", st.du);
    drawIf("DD", st.dd);
    drawIf("DL", st.dl);
    drawIf("DR", st.dr);
    drawIf("DR", st.dr);

    drawIf("LB", st.lb);
    drawIf("RB", st.rb);

    drawIf("LT", st.lt > trigThresh);
    drawIf("RT", st.rt > trigThresh);

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
                scale
            );
        };

    drawStick("L3", st.lx, st.ly, st.l3);
    drawStick("R3", st.rx, st.ry, st.r3);

    // Autosave pos/scale (optional)
    if (cvarManager->getCvar("xco_autosave").getIntValue() != 0)
    {
        static float lastX = -99999.f, lastY = -99999.f, lastS = -99999.f;
        static auto lastWrite = std::chrono::steady_clock::now();

        float cx = cvarManager->getCvar("xco_x").getFloatValue();
        float cy = cvarManager->getCvar("xco_y").getFloatValue();
        float cs = cvarManager->getCvar("xco_scale").getFloatValue();

        bool changed =
            (std::fabs(cx - lastX) > 0.01f) ||
            (std::fabs(cy - lastY) > 0.01f) ||
            (std::fabs(cs - lastS) > 0.0005f);

        if (changed)
        {
            auto now = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastWrite).count();
            if (ms > 250)
            {
                SaveOverlayPos(gameWrapper, cx, cy, cs);
                lastWrite = now;
                lastX = cx; lastY = cy; lastS = cs;
            }
        }
    }
}
