#pragma once

#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/plugin/pluginsettingswindow.h"
#include "bakkesmod/wrappers/canvaswrapper.h"
#include "bakkesmod/wrappers/ImageWrapper.h"

#include <map>
#include <memory>
#include <string>

class ControllerOverlayPro :
    public BakkesMod::Plugin::BakkesModPlugin,
    public BakkesMod::Plugin::PluginSettingsWindow
{
public:
    void onLoad() override;
    void onUnload() override;

    // PluginSettingsWindow (shows in F2 -> Plugins)
    void RenderSettings() override;
    std::string GetPluginName() override;
    void SetImGuiContext(uintptr_t ctx) override;

private:
    struct State
    {
        bool a = false, b = false, x = false, y = false;
        bool du = false, dd = false, dl = false, dr = false;
        bool lb = false, rb = false;
        bool l3 = false, r3 = false;
        float lt = 0.f, rt = 0.f;

        float lx_raw = 0.f, ly_raw = 0.f;
        float rx_raw = 0.f, ry_raw = 0.f;

        float lx = 0.f, ly = 0.f;
        float rx = 0.f, ry = 0.f;
    };

    void Render(CanvasWrapper canvas);
    void PollController();

    void EnsureTexturesLoaded();
    void FreeTextures();

    float GetOffX(const std::string& key);
    float GetOffY(const std::string& key);

private:
    bool texturesLoaded = false;
    std::string skinName = "xbox";

    std::shared_ptr<ImageWrapper> baseImg;
    std::map<std::string, std::shared_ptr<ImageWrapper>> layers;

    State st;

    bool imguiReady = false;
};
