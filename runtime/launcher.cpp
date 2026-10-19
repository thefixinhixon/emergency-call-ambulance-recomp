// Emergency Call Ambulance launcher.
//
// Supermodel's frontend (Main.cpp) calls RunGUI() when the program is started without
// arguments. The eca binary links this file instead of Supermodel's Gui.cpp, so running
// ./eca opens a settings window for this one game. Everything is saved to the [ eca ]
// section of Config/Supermodel.ini, which Supermodel applies over the global settings;
// other sections are written back unchanged. Launch returns the ROM set path and
// Main.cpp starts the game with the saved settings.
#include "SDLIncludes.h"
#include <GL/glew.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Game.h"
#include "GameLoader.h"
#include "Gui.h"
#include "Main.h"
#include "OSD/FileSystemPath.h"
#include "OSD/SDL/SDLInputSystem.h"
#include "Inputs/Inputs.h"
#include "Pkgs/imgui/imgui.h"
#include "Pkgs/imgui/imgui_impl_opengl3.h"
#include "Pkgs/imgui/imgui_impl_sdl2.h"
#include "Util/ConfigBuilders.h"
#include "Util/NewConfig.h"

namespace
{
  const char *kGame = "eca";
  const char *kTitle = "Emergency Call Ambulance";

  // Xbox-layout pad defaults, matching config/eca_gamepad.ini
  const std::map<std::string, std::string> kPadDefaults = {
    {"Start1", "KEY_1,JOY1_BUTTON8"},
    {"Coin1", "KEY_3,JOY1_BUTTON7"},
    {"Steering", "JOY1_XAXIS"},
    {"SteeringLeft", "KEY_LEFT"},
    {"SteeringRight", "KEY_RIGHT"},
    {"Accelerator", "KEY_UP,JOY1_RZAXIS_POS"},
    {"Brake", "KEY_DOWN,JOY1_ZAXIS_POS"},
    {"GearShiftUp", "KEY_Y,JOY1_BUTTON6"},
    {"GearShiftDown", "KEY_H,JOY1_BUTTON5"},
    {"ViewChange", "KEY_A,JOY1_BUTTON4"},
  };

  struct Resolution
  {
    int w, h;
    const char *label;
  };
  const Resolution kResolutions[] = {
    {496, 384, "496 x 384 (native Model 3)"},
    {640, 480, "640 x 480"},
    {800, 600, "800 x 600"},
    {1024, 768, "1024 x 768"},
    {1280, 720, "1280 x 720 (720p)"},
    {1280, 1024, "1280 x 1024"},
    {1366, 768, "1366 x 768"},
    {1600, 900, "1600 x 900"},
    {1920, 1080, "1920 x 1080 (1080p)"},
    {2560, 1440, "2560 x 1440 (1440p)"},
    {3840, 2160, "3840 x 2160 (4K)"},
  };

  const char *kDisplayModes[] = {"Windowed", "Borderless window", "Fullscreen"};
  const char *kCrtColors[] = {"Off (sRGB)", "ARI/D93 (Japanese arcade monitor)", "PVM 20M2U/D93", "BT.601 525/D93",
                              "BT.601 525/D65 (US)", "BT.601 625/D65 (Europe/Australia)"};
  const char *kUpscale[] = {"None (sharp pixels)", "Biquintic", "Bilinear", "Bicubic (smoothest)"};
  const char *kChannels[] = {"Mono", "Stereo", "Quad (front/rear)"};
  const int kChannelValues[] = {1, 2, 4};

  // All the settings the launcher edits, read from the effective configuration
  // (defaults < [ Global ] < [ eca ]) and written back to [ eca ]
  struct Settings
  {
    int displayMode = 0;
    int resX = 496, resY = 384;
    bool wideScreen = false, wideBackground = false, stretch = false;
    int supersampling = 1;
    bool new3d = true, quadRendering = true;
    bool vsync = true, trueHz = false, showFps = false, noWhiteFlash = false;
    int crtColors = 0, upscale = 2;
    bool hdTextures = false;
    bool dumpTextures = false;

    bool sound = true;
    int soundVolume = 100;
    float balance = 0;
    bool flipStereo = false;
    int channels = 2;     // index into kChannelValues
    bool legacyDsp = false;

    bool forceFeedback = false;
    bool triggersRestAtMin = true;
    int steerSaturation = 100, steerDeadZone = 2;

    bool multiThreaded = true, gpuThreaded = true;
    int cpuCore = 0;      // 0 = recompiled, 1 = interpreter (not saved: debugging aid)
  };

  bool SameSaved(const Settings &a, const Settings &b)
  {
    // Every saved field; cpuCore is deliberately left out
    return a.displayMode == b.displayMode && a.resX == b.resX && a.resY == b.resY && a.wideScreen == b.wideScreen &&
           a.wideBackground == b.wideBackground && a.stretch == b.stretch && a.supersampling == b.supersampling &&
           a.new3d == b.new3d && a.quadRendering == b.quadRendering && a.vsync == b.vsync && a.trueHz == b.trueHz &&
           a.showFps == b.showFps && a.noWhiteFlash == b.noWhiteFlash && a.crtColors == b.crtColors &&
           a.upscale == b.upscale && a.hdTextures == b.hdTextures && a.dumpTextures == b.dumpTextures && a.sound == b.sound && a.soundVolume == b.soundVolume && a.balance == b.balance &&
           a.flipStereo == b.flipStereo && a.channels == b.channels && a.legacyDsp == b.legacyDsp &&
           a.forceFeedback == b.forceFeedback && a.triggersRestAtMin == b.triggersRestAtMin &&
           a.steerSaturation == b.steerSaturation && a.steerDeadZone == b.steerDeadZone &&
           a.multiThreaded == b.multiThreaded && a.gpuThreaded == b.gpuThreaded;
  }

  template <typename T>
  T Get(const Util::Config::Node &cfg, const std::string &key, T fallback)
  {
    const Util::Config::Node *n = cfg.TryGet(key);
    if (!n || !n->Exists())
      return fallback;
    try
    {
      return n->ValueAs<T>();
    }
    catch (...)
    {
      return fallback;
    }
  }

  Settings Load(const Util::Config::Node &eff)
  {
    Settings s;
    bool full = Get(eff, "FullScreen", false), borderless = Get(eff, "BorderlessWindow", false);
    s.displayMode = full ? 2 : borderless ? 1 : 0;
    s.resX = Get(eff, "XResolution", 496);
    s.resY = Get(eff, "YResolution", 384);
    s.wideScreen = Get(eff, "WideScreen", false);
    s.wideBackground = Get(eff, "WideBackground", false);
    s.stretch = Get(eff, "Stretch", false);
    s.supersampling = std::clamp(Get(eff, "Supersampling", 1), 1, 8);
    s.new3d = Get(eff, "New3DEngine", true);
    s.quadRendering = Get(eff, "QuadRendering", true);
    s.vsync = Get(eff, "VSync", true);
    s.trueHz = Get(eff, "RefreshRate", 60.0f) < 59.0f;
    s.showFps = Get(eff, "ShowFrameRate", false);
    s.noWhiteFlash = Get(eff, "NoWhiteFlash", false);
    s.crtColors = std::clamp(Get(eff, "CRTcolors", 0), 0, 5);
    s.upscale = std::clamp(Get(eff, "UpscaleMode", 2), 0, 3);
    s.hdTextures = Get(eff, "HDTextures", false);
    s.dumpTextures = Get(eff, "DumpTextures", false);

    s.sound = Get(eff, "EmulateSound", true);
    s.soundVolume = std::clamp(Get(eff, "SoundVolume", 100), 0, 200);
    s.balance = Get(eff, "BalanceLeftRight", 0.0f);
    s.flipStereo = Get(eff, "FlipStereo", false);
    int ch = Get(eff, "NbSoundChannels", 4);
    s.channels = ch == 1 ? 0 : ch == 2 ? 1 : 2;
    s.legacyDsp = Get(eff, "LegacySoundDSP", false);

    s.forceFeedback = Get(eff, "ForceFeedback", false);
    s.triggersRestAtMin = Get(eff, "InputJoy1RZOffVal", 0) == -32768;
    s.steerSaturation = std::clamp(Get(eff, "InputJoy1XSaturation", 100), 50, 200);
    s.steerDeadZone = std::clamp(Get(eff, "InputJoy1XDeadZone", 2), 0, 50);

    s.multiThreaded = Get(eff, "MultiThreaded", true);
    s.gpuThreaded = Get(eff, "GPUMultiThreaded", true);
    return s;
  }

  void Store(const Settings &s, Util::Config::Node &sec)
  {
    auto b = [&](const char *k, bool v) { sec.Set<std::string>(k, v ? "true" : "false"); };
    auto i = [&](const char *k, int v) { sec.Set<std::string>(k, std::to_string(v)); };
    b("FullScreen", s.displayMode == 2);
    b("BorderlessWindow", s.displayMode == 1);
    i("XResolution", s.resX);
    i("YResolution", s.resY);
    b("WideScreen", s.wideScreen);
    b("WideBackground", s.wideBackground);
    b("Stretch", s.stretch);
    i("Supersampling", s.supersampling);
    b("New3DEngine", s.new3d);
    b("QuadRendering", s.quadRendering);
    b("VSync", s.vsync);
    sec.Set<std::string>("RefreshRate", s.trueHz ? "57.524" : "60.0");
    b("ShowFrameRate", s.showFps);
    b("NoWhiteFlash", s.noWhiteFlash);
    i("CRTcolors", s.crtColors);
    i("UpscaleMode", s.upscale);
    b("HDTextures", s.hdTextures);
    b("DumpTextures", s.dumpTextures);

    b("EmulateSound", s.sound);
    i("SoundVolume", s.soundVolume);
    sec.Set<std::string>("BalanceLeftRight", std::to_string((int)s.balance));
    b("FlipStereo", s.flipStereo);
    i("NbSoundChannels", kChannelValues[s.channels]);
    b("LegacySoundDSP", s.legacyDsp);

    // The control mappings use SDL's joystick numbering
    sec.Set<std::string>("InputSystem", "sdl");
    b("ForceFeedback", s.forceFeedback);
    i("InputJoy1ZOffVal", s.triggersRestAtMin ? -32768 : 0);
    i("InputJoy1RZOffVal", s.triggersRestAtMin ? -32768 : 0);
    i("InputJoy1XSaturation", s.steerSaturation);
    i("InputJoy1XDeadZone", s.steerDeadZone);

    b("MultiThreaded", s.multiThreaded);
    b("GPUMultiThreaded", s.gpuThreaded);
  }

  void SetCpuEnv(int core)
  {
#ifdef _WIN32
    _putenv_s("ECA_PPC_MODE", core == 1 ? "interp" : "");
#else
    if (core == 1)
      setenv("ECA_PPC_MODE", "interp", 1);
    else
      unsetenv("ECA_PPC_MODE");
#endif
  }

  float DpiScale()
  {
#ifdef _WIN32
    float ddpi = 96.0f;
    if (SDL_GetDisplayDPI(0, &ddpi, nullptr, nullptr) == 0)
      return std::max(1.0f, ddpi / 96.0f);
#endif
    return 1.0f;
  }

  void ApplyStyle(float scale)
  {
    ImGui::StyleColorsDark();
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 0;
    st.FrameRounding = 4;
    st.GrabRounding = 4;
    st.TabRounding = 4;
    st.FramePadding = ImVec2(8, 5);
    st.ItemSpacing = ImVec2(10, 8);
    // Ambulance red accents on a neutral dark grey
    const ImVec4 red(0.78f, 0.13f, 0.15f, 1.0f), redHover(0.88f, 0.22f, 0.24f, 1.0f), redActive(0.65f, 0.08f, 0.10f, 1.0f);
    ImVec4 *c = st.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.11f, 0.11f, 0.12f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.18f, 0.20f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.24f, 0.27f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.28f, 0.28f, 0.31f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.22f, 0.22f, 0.25f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.30f, 0.34f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.35f, 0.35f, 0.40f, 1.0f);
    c[ImGuiCol_Tab] = ImVec4(0.16f, 0.16f, 0.18f, 1.0f);
    c[ImGuiCol_TabHovered] = ImVec4(0.42f, 0.20f, 0.21f, 1.0f);
    c[ImGuiCol_TabSelected] = red;
    c[ImGuiCol_CheckMark] = redHover;
    c[ImGuiCol_SliderGrab] = red;
    c[ImGuiCol_SliderGrabActive] = redHover;
    c[ImGuiCol_Header] = ImVec4(0.22f, 0.22f, 0.25f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0.30f, 0.34f, 1.0f);
    c[ImGuiCol_HeaderActive] = redActive;
    c[ImGuiCol_NavCursor] = redHover;
    st.ScaleAllSizes(scale);
    st.FontScaleDpi = scale;
  }

  void Help(const char *text)
  {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip())
    {
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
      ImGui::TextUnformatted(text);
      ImGui::PopTextWrapPos();
      ImGui::EndTooltip();
    }
  }

#ifdef _WIN32
  int strcasecmp(const char *a, const char *b) { return _stricmp(a, b); }
#endif

  bool Combo(const char *label, int *value, const char *const items[], int count)
  {
    return ImGui::Combo(label, value, items, count);
  }

  // What the HD texture pack in Textures/eca holds (see runtime/hdtex.h)
  struct PackInfo
  {
    size_t textures = 0, blocks = 0;
  };

  PackInfo ScanPack(const std::string &dir)
  {
    PackInfo info;
    std::error_code ec;
    for (auto &e : std::filesystem::directory_iterator(dir, ec))
      info.textures += e.path().extension() == ".png";
    for (auto &e : std::filesystem::directory_iterator(std::filesystem::path(dir) / "blocks", ec))
      info.blocks += e.path().extension() == ".png";
    return info;
  }

  class Launcher
  {
  public:
    Launcher(const std::string &configPath, const Util::Config::Node &fileConfig)
      : m_configPath(configPath), m_file(fileConfig)
    {
      RebuildEffective();
      m_s = Load(m_effective);
      m_saved = m_s;
      m_pack = ScanPack(Get<std::string>(m_effective, "TexturePackDirectory", std::string("Textures/") + kGame));
    }

    std::vector<std::string> Run();

  private:
    std::string m_configPath;
    Util::Config::Node m_file;                 // complete config file contents
    Util::Config::Node m_effective{"Global"};  // defaults < Global < [ eca ]
    Settings m_s, m_saved;
    bool m_inputsDirty = false;

    SDL_Window *m_window = nullptr;
    float m_scale = 1.0f;
    Game m_game;
    bool m_haveGame = false;
    std::shared_ptr<CInputs> m_inputs;
    std::shared_ptr<CInput> m_binding;         // input waiting for a key/button press
    bool m_bindAppend = false;                 // add to the existing mapping instead of replacing it
    int m_bindFrames = 0;
    std::string m_status;

    bool m_launch = false, m_quit = false, m_firstFrame = true;
    PackInfo m_pack;

    Util::Config::Node &Section()
    {
      if (Util::Config::Node *sec = m_file.TryGet(kGame))
        return *sec;
      return m_file.Add(kGame);
    }

    void RebuildEffective()
    {
      Util::Config::Node withGlobal("Global");
      Util::Config::MergeINISections(&withGlobal, DefaultConfig(), m_file);
      if (const Util::Config::Node *sec = m_file.TryGet(kGame))
        Util::Config::MergeINISections(&m_effective, withGlobal, *sec);
      else
        m_effective = withGlobal;
    }

    bool Dirty() const
    {
      return m_inputsDirty || !SameSaved(m_s, m_saved);
    }

    void CreateInputs()
    {
      m_inputs = nullptr;
      auto system = std::shared_ptr<CInputSystem>(new CSDLInputSystem(m_effective, false));
      auto inputs = std::make_shared<CInputs>(system);
      if (!inputs->Initialize())
      {
        m_status = "Could not initialise the input system.";
        return;
      }
      inputs->LoadFromConfig(m_effective);
      int x, y, w, h;
      SDL_GetWindowPosition(m_window, &x, &y);
      SDL_GL_GetDrawableSize(m_window, &w, &h);
      system->SetDisplayGeom(x, y, w, h);
      m_inputs = inputs;
    }

    std::vector<std::shared_ptr<CInput>> GameInputs()
    {
      std::vector<std::shared_ptr<CInput>> list;
      if (!m_inputs || !m_haveGame)
        return list;
      for (auto &in : m_inputs->GetGameInputs(m_game))
        if (!in->IsUIInput())
          list.push_back(in);
      return list;
    }

    void Save()
    {
      Util::Config::Node &sec = Section();
      Store(m_s, sec);
      for (auto &in : GameInputs())
        in->StoreToConfig(&sec);
      Util::Config::WriteINIFile(m_configPath, m_file,
                                 ";;\n;; Supermodel Configuration File\n;; [ eca ] is maintained by the Emergency Call Ambulance launcher.\n;;");
      m_saved = m_s;
      m_inputsDirty = false;
      RebuildEffective();
      m_status = "Settings saved.";
    }

    std::string RomPath() const
    {
      namespace fs = std::filesystem;
      std::string romDir = Get<std::string>(m_effective, "ROMDirectory", "ROMs/");
      fs::path inDir = fs::path(romDir) / (std::string(kGame) + ".zip");
      if (fs::exists(inDir))
        return inDir.string();
      if (fs::exists(std::string(kGame) + ".zip"))
        return std::string(kGame) + ".zip";
      return {};
    }

    void DrawVideo();
    void DrawAudio();
    void DrawControls();
    void DrawSystem();
    void DrawFrame();
  };

  void Launcher::DrawVideo()
  {
    Settings &s = m_s;
    ImGui::SeparatorText("Display");
    Combo("Display mode", &s.displayMode, kDisplayModes, 3);

    int current = -1;
    for (int i = 0; i < (int)(sizeof(kResolutions) / sizeof(kResolutions[0])); i++)
      if (kResolutions[i].w == s.resX && kResolutions[i].h == s.resY)
        current = i;
    char preview[64];
    if (current >= 0)
      snprintf(preview, sizeof(preview), "%s", kResolutions[current].label);
    else
      snprintf(preview, sizeof(preview), "%d x %d (custom)", s.resX, s.resY);
    if (ImGui::BeginCombo("Resolution", preview))
    {
      SDL_DisplayMode desktop;
      if (SDL_GetDesktopDisplayMode(0, &desktop) == 0)
      {
        char label[64];
        snprintf(label, sizeof(label), "%d x %d (this display)", desktop.w, desktop.h);
        if (ImGui::Selectable(label, s.resX == desktop.w && s.resY == desktop.h))
        {
          s.resX = desktop.w;
          s.resY = desktop.h;
        }
        ImGui::Separator();
      }
      for (int i = 0; i < (int)(sizeof(kResolutions) / sizeof(kResolutions[0])); i++)
        if (ImGui::Selectable(kResolutions[i].label, i == current))
        {
          s.resX = kResolutions[i].w;
          s.resY = kResolutions[i].h;
        }
      ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    int wh[2] = {s.resX, s.resY};
    if (ImGui::InputInt2("Custom size", wh))
    {
      s.resX = std::clamp(wh[0], 320, 16384);
      s.resY = std::clamp(wh[1], 240, 16384);
    }

    ImGui::Checkbox("Widescreen", &s.wideScreen);
    Help("Expands the 3D field of view to the full window width. The 2D layers stay 4:3 unless "
         "'Widen 2D background' is also on.");
    ImGui::BeginDisabled(!s.wideScreen);
    ImGui::Checkbox("Widen 2D background", &s.wideBackground);
    ImGui::EndDisabled();
    ImGui::Checkbox("Stretch to window", &s.stretch);
    Help("Fill the window and ignore the aspect ratio.");

    ImGui::SeparatorText("Rendering");
    int engine = s.new3d ? 0 : 1;
    const char *engines[] = {"New3D (accurate, default)", "Legacy3D (faster, less accurate)"};
    if (Combo("3D engine", &engine, engines, 2))
      s.new3d = engine == 0;
    ImGui::BeginDisabled(!s.new3d);
    ImGui::Checkbox("Quad rendering", &s.quadRendering);
    Help("Render quads natively rather than as two triangles. Fixes texture seams; costs a little speed.");
    ImGui::EndDisabled();
    ImGui::SliderInt("Supersampling", &s.supersampling, 1, 8, "%dx");
    Help("Renders at a multiple of the resolution and scales down: smoother edges, much more GPU work.");
    Combo("2D upscaling", &s.upscale, kUpscale, 4);
    Combo("CRT colours", &s.crtColors, kCrtColors, 6);
    Help("Emulates the colour response of period arcade monitors. ARI/D93 is recommended for Model 3 games.");

    ImGui::SeparatorText("HD textures");
    bool havePack = m_pack.textures + m_pack.blocks > 0;
    ImGui::BeginDisabled(!havePack || !s.new3d);
    bool hd = s.hdTextures && havePack && s.new3d;
    if (ImGui::Checkbox("AI-upscaled textures", &hd))
      s.hdTextures = hd;
    ImGui::EndDisabled();
    Help("Replaces the game's textures with 4x Real-ESRGAN upscales from Textures/eca. Build the pack with "
         "tools/build_texture_pack.py. Needs the New3D engine.");
    if (!havePack)
      ImGui::TextDisabled("No texture pack found in Textures/eca.");
    else
      ImGui::TextDisabled("Pack: %zu individual textures, %zu ROM blocks%s", m_pack.textures, m_pack.blocks,
                          s.new3d ? "" : "  (needs the New3D engine)");

    ImGui::SeparatorText("Timing");
    ImGui::Checkbox("Vertical sync", &s.vsync);
    ImGui::Checkbox("True Model 3 refresh rate (57.524 Hz)", &s.trueHz);
    Help("Runs at the arcade board's exact rate instead of 60 Hz. Best with a variable refresh display.");
    ImGui::Checkbox("Show frame rate", &s.showFps);
    ImGui::Checkbox("Disable white flash", &s.noWhiteFlash);
    Help("Some games flash the screen white when 3D rendering is disabled; this hides it.");
  }

  void Launcher::DrawAudio()
  {
    Settings &s = m_s;
    ImGui::SeparatorText("Sound");
    ImGui::Checkbox("Enable sound", &s.sound);
    ImGui::BeginDisabled(!s.sound);
    ImGui::SliderInt("Volume", &s.soundVolume, 0, 200, "%d%%");
    ImGui::SliderFloat("Balance (left / right)", &s.balance, -100.0f, 100.0f, "%.0f");
    Combo("Output channels", &s.channels, kChannels, 3);
    ImGui::Checkbox("Swap left and right", &s.flipStereo);
    ImGui::Checkbox("Legacy SCSP DSP", &s.legacyDsp);
    Help("Use Supermodel's older sound DSP. Only needed if effects sound wrong with the default one.");
    ImGui::EndDisabled();
  }

  void Launcher::DrawControls()
  {
    Settings &s = m_s;
    ImGui::SeparatorText("Controllers");
    int n = SDL_NumJoysticks();
    if (n <= 0)
      ImGui::TextDisabled("No controllers detected. The keyboard still works.");
    for (int i = 0; i < n; i++)
      ImGui::BulletText("Joystick %d: %s", i + 1, SDL_JoystickNameForIndex(i));

    ImGui::SeparatorText("Bindings");
    ImGui::TextDisabled("Select Set, then press a key or controller button (move a stick for steering).");
    if (!m_inputs)
      ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "Input system unavailable.");
    else if (ImGui::BeginTable("bindings", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
    {
      ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableSetupColumn("Mapped to", ImGuiTableColumnFlags_WidthStretch, 1.6f);
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
      for (auto &in : GameInputs())
      {
        ImGui::PushID(in->id);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(in->label);
        ImGui::TableNextColumn();
        const char *mapping = in->GetMapping();
        ImGui::TextWrapped("%s", (mapping && *mapping && strcmp(mapping, "NONE")) ? mapping : "(none)");
        ImGui::TableNextColumn();
        if (ImGui::SmallButton("Set"))
        {
          m_binding = in;
          m_bindAppend = false;
          m_bindFrames = 0;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Add"))
        {
          m_binding = in;
          m_bindAppend = true;
          m_bindFrames = 0;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear"))
        {
          in->ClearMapping();
          m_inputsDirty = true;
        }
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    if (ImGui::Button("Reset to Xbox-style pad defaults"))
    {
      Util::Config::Node defaults = DefaultConfig();
      for (auto &in : GameInputs())
      {
        auto it = kPadDefaults.find(in->id);
        std::string m = it != kPadDefaults.end() ? it->second
                                                 : Get<std::string>(defaults, std::string("Input") + in->id, "NONE");
        in->SetMapping(m.c_str());
      }
      s.triggersRestAtMin = true;
      m_inputsDirty = true;
    }

    ImGui::SeparatorText("Analog");
    ImGui::Checkbox("Pedals on analog triggers (rest at minimum)", &s.triggersRestAtMin);
    Help("Xbox and PlayStation pads report released triggers as the bottom of the axis rather than "
         "the centre. Leave this on for those pads; turn it off for wheels with centred pedal axes. "
         "Applies to joystick 1's Z and RZ axes.");
    ImGui::SliderInt("Steering range", &s.steerSaturation, 50, 200, "%d%%");
    Help("How far the stick must move for full lock. Above 100% makes thumbstick steering less twitchy.");
    ImGui::SliderInt("Steering dead zone", &s.steerDeadZone, 0, 50, "%d%%");
    ImGui::Checkbox("Force feedback", &s.forceFeedback);
    Help("Sends the cabinet's steering wheel force feedback to a supported wheel or rumble pad.");
  }

  void Launcher::DrawSystem()
  {
    Settings &s = m_s;
    ImGui::SeparatorText("Performance");
    ImGui::Checkbox("Multi-threaded emulation", &s.multiThreaded);
    ImGui::BeginDisabled(!s.multiThreaded);
    ImGui::Checkbox("Render graphics on a separate thread", &s.gpuThreaded);
    ImGui::EndDisabled();
    const char *cores[] = {"Recompiled (native code)", "Interpreter (debugging)"};
    Combo("CPU core", &s.cpuCore, cores, 2);
    Help("The interpreter is Supermodel's original PowerPC emulator. It runs about 3x slower and is "
         "only useful for comparing behaviour. This choice is not saved.");

    ImGui::SeparatorText("Texture pack tools");
    ImGui::Checkbox("Dump textures while playing", &s.dumpTextures);
    Help("Writes every texture the game draws to Textures/eca_dump. tools/build_texture_pack.py upscales these "
         "individually, with seamless tiling, ahead of the whole-block versions extracted from the ROM.");

    ImGui::SeparatorText("Game data");
    ImGui::TextWrapped("The game keeps its operator settings, bookkeeping and records in NVRAM.");
    if (ImGui::Button("Reset NVRAM..."))
      ImGui::OpenPopup("Reset NVRAM?");
    if (ImGui::BeginPopupModal("Reset NVRAM?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
      ImGui::TextUnformatted("Delete the saved NVRAM? The game will start from factory settings.");
      if (ImGui::Button("Delete"))
      {
        std::string nv = FileSystemPath::GetPath(FileSystemPath::NVRAM) + kGame + ".nv";
        std::error_code ec;
        m_status = std::filesystem::remove(nv, ec) ? "NVRAM reset." : "No NVRAM file to reset.";
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel"))
        ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }

    ImGui::SeparatorText("Files");
    ImGui::TextDisabled("Settings: %s [ %s ]", m_configPath.c_str(), kGame);
    std::string rom = RomPath();
    ImGui::TextDisabled("ROM set: %s", rom.empty() ? "not found (put eca.zip in ROMs/)" : rom.c_str());
  }

  void Launcher::DrawFrame()
  {
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Header
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.7f);
    ImGui::TextUnformatted(kTitle);
    ImGui::PopFont();
    ImGui::TextDisabled("Sega Model 3 (1999)  |  static recompilation for x86-64");
    ImGui::Spacing();

    // Settings tabs fill the space above the footer
    float footer = ImGui::GetFrameHeightWithSpacing() * 1.6f + ImGui::GetStyle().ItemSpacing.y * 2;
    ImGui::BeginChild("tabs", ImVec2(0, -footer));
    if (ImGui::BeginTabBar("sections"))
    {
      struct Tab { const char *name; void (Launcher::*draw)(); };
      const Tab tabs[] = {{"Video", &Launcher::DrawVideo}, {"Audio", &Launcher::DrawAudio},
                          {"Controls", &Launcher::DrawControls}, {"System", &Launcher::DrawSystem}};
      // ECA_LAUNCHER_TAB=<name> opens on that tab (screenshots, testing)
      const char *startTab = m_firstFrame ? getenv("ECA_LAUNCHER_TAB") : nullptr;
      for (const Tab &t : tabs)
        if (ImGui::BeginTabItem(t.name, nullptr,
                                startTab && !strcasecmp(startTab, t.name) ? ImGuiTabItemFlags_SetSelected : 0))
        {
          ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
          ImGui::PushItemWidth(ImGui::GetFontSize() * 18);
          (this->*t.draw)();
          ImGui::PopItemWidth();
          ImGui::EndChild();
          ImGui::EndTabItem();
        }
      ImGui::EndTabBar();
    }
    ImGui::EndChild();

    // Footer: status on the left, actions on the right
    ImGui::Separator();
    const float footerRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    if (!m_status.empty())
      ImGui::TextDisabled("%s", m_status.c_str());
    else if (Dirty())
      ImGui::TextDisabled("Unsaved changes");
    else
      ImGui::TextDisabled("Controller: D-pad / stick to move, A to select");

    const ImGuiStyle &st = ImGui::GetStyle();
    float bh = ImGui::GetFrameHeight() * 1.4f;
    float wLaunch = ImGui::GetFontSize() * 9, wOther = ImGui::GetFontSize() * 6;
    ImGui::SameLine(footerRight - wLaunch - 2 * wOther - 2 * st.ItemSpacing.x);
    if (ImGui::Button("Quit", ImVec2(wOther, bh)))
      m_quit = true;
    ImGui::SameLine();
    ImGui::BeginDisabled(!Dirty());
    if (ImGui::Button("Save", ImVec2(wOther, bh)))
      Save();
    ImGui::EndDisabled();
    ImGui::SameLine();
    bool haveRom = !RomPath().empty();
    ImGui::BeginDisabled(!haveRom);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.13f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.88f, 0.22f, 0.24f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.65f, 0.08f, 0.10f, 1.0f));
    if (m_firstFrame)
      ImGui::SetKeyboardFocusHere();   // A on the pad launches straight away
    if (ImGui::Button("Launch", ImVec2(wLaunch, bh)))
      m_launch = true;
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    m_firstFrame = false;

    // Binding prompt
    if (m_binding)
    {
      ImGui::OpenPopup("Bind");
      if (ImGui::BeginPopupModal("Bind", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
      {
        ImGui::Text("%s: press a key or controller button", m_binding->label);
        ImGui::TextDisabled("Esc cancels");
        ImGui::EndPopup();
      }
    }
    ImGui::End();
  }

  std::vector<std::string> Launcher::Run()
  {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0)
    {
      fprintf(stderr, "launcher: SDL_Init failed: %s\n", SDL_GetError());
      return {};
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    m_scale = DpiScale();
    m_window = SDL_CreateWindow(kTitle, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, int(820 * m_scale), int(680 * m_scale),
                                SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!m_window)
    {
      fprintf(stderr, "launcher: cannot create window: %s\n", SDL_GetError());
      SDL_Quit();
      return {};
    }
    SDL_GLContext gl = SDL_GL_CreateContext(m_window);
    SDL_GL_MakeCurrent(m_window, gl);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;
    io.Fonts->AddFontDefaultVector();
    ApplyStyle(m_scale);
    ImGui_ImplSDL2_InitForOpenGL(m_window, gl);
    ImGui_ImplOpenGL3_Init("#version 410");

    GameLoader loader(Get<std::string>(m_effective, "GameXMLFile", "Config/Games.xml"));
    auto it = loader.GetGames().find(kGame);
    if (it != loader.GetGames().end())
    {
      m_game = it->second;
      m_haveGame = true;
    }
    CreateInputs();

    // ECA_LAUNCHER_AUTO=launch presses Launch after a few frames; =save saves and quits
    // (automated tests)
    const char *autoAction = getenv("ECA_LAUNCHER_AUTO");
    int frame = 0;
    while (!m_launch && !m_quit)
    {
      if (autoAction && ++frame == 10)
      {
        if (!strcmp(autoAction, "launch"))
          m_launch = true;
        else if (!strcmp(autoAction, "save"))
        {
          Save();
          m_quit = true;
        }
      }
      SDL_Event e;
      while (SDL_PollEvent(&e))
      {
        ImGui_ImplSDL2_ProcessEvent(&e);
        if (e.type == SDL_QUIT)
          m_quit = true;
      }

      ImGui_ImplOpenGL3_NewFrame();
      ImGui_ImplSDL2_NewFrame();
      ImGui::NewFrame();
      DrawFrame();
      ImGui::Render();
      glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
      glClearColor(0.11f, 0.11f, 0.12f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
      SDL_GL_SwapWindow(m_window);

      // CInput::Configure() blocks until it sees an input, so let the prompt render first
      if (m_binding && ++m_bindFrames == 3)
      {
        bool append = m_bindAppend;
        auto system = m_binding->GetInputSystem();
        system->UngrabMouse();
        if (m_binding->Configure(append))
          m_inputsDirty = true;
        system->GrabMouse();
        m_binding = nullptr;
        m_status.clear();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::vector<std::string> roms;
    if (m_launch)
    {
      if (Dirty())
        Save();
      SetCpuEnv(m_s.cpuCore);
      roms.push_back(RomPath());
    }

    m_binding = nullptr;
    m_inputs = nullptr;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(m_window);
    SDL_Quit();
    return roms;
  }
}

std::vector<std::string> RunGUI(const std::string &configPath, const Util::Config::Node &fileConfig)
{
  Launcher launcher(configPath, fileConfig);
  return launcher.Run();
}
