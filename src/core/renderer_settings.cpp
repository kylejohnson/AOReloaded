// Renderer settings tab (see renderer_settings.h).

#include "core/renderer_settings.h"

#include "ao/game_api.h"
#include "core/logging.h"
#include "core/settings.h"
#include "core/fps_cap.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace aor {

namespace {

// randy-vk's exported interface (proxy/ddraw/rvk_settings.h in randy-vk), versions 1 and 2. Version 2 adds the
// choice type and the fields from `parent` on (a version 1 renderer leaves them null).
struct RvkSettingInfo {
    uint32_t size;
    const char* name;
    const char* label;
    const char* section;
    uint32_t type;              // 0 bool, 1 int, 2 float, 3 choice (an int from `choices`)
    float min, max, step;
    float value, defaultValue;
    const char* parent;         // the bool switching the feature this setting belongs to (null: none)
    const char* choices;        // choice: the allowed values separated by spaces
};
enum : uint32_t { kBool = 0, kInt = 1, kFloat = 2, kChoice = 3 };
using FnVersion = uint32_t (*)();
using FnCount = uint32_t (*)();
using FnGet = int (*)(uint32_t, RvkSettingInfo*);
using FnSet = int (*)(const char*, float);

using FnPresetCount = uint32_t (*)();
using FnPresetName = const char* (*)(uint32_t);
using FnPresetIndex = int (*)(uint32_t);
using FnPresetCurrent = int (*)();

FnCount g_count;
FnGet g_get;
FnSet g_set;
// Presets (RvkPresets_*, newer renderers): 0 Original, 1 Classic+, 2 Modern, 3 Ultra.
FnPresetCount g_presetCount;
FnPresetName g_presetName;
FnPresetIndex g_presetApply;
FnPresetCurrent g_presetCurrent;
bool g_syncing;                 // pushing the renderer's values into the DValues: not changes from the panel
constexpr const char* kPresetVar = "RVK_Preset";       // the preset buttons (kCustom: none matches)
constexpr int kCustom = 99;
uint32_t g_version;
bool g_available;
char g_xml[64 * 1024];

bool Resolve()
{
    HMODULE randy = GetModuleHandleA("randy31.dll");
    if (!randy) return false;
    auto version = reinterpret_cast<FnVersion>(GetProcAddress(randy, "RvkSettings_Version"));
    g_count = reinterpret_cast<FnCount>(GetProcAddress(randy, "RvkSettings_Count"));
    g_get = reinterpret_cast<FnGet>(GetProcAddress(randy, "RvkSettings_Get"));
    g_set = reinterpret_cast<FnSet>(GetProcAddress(randy, "RvkSettings_Set"));
    if (!version || !g_count || !g_get || !g_set) return false;
    g_presetCount = reinterpret_cast<FnPresetCount>(GetProcAddress(randy, "RvkPresets_Count"));
    g_presetName = reinterpret_cast<FnPresetName>(GetProcAddress(randy, "RvkPresets_Name"));
    g_presetApply = reinterpret_cast<FnPresetIndex>(GetProcAddress(randy, "RvkPresets_Apply"));
    g_presetCurrent = reinterpret_cast<FnPresetCurrent>(GetProcAddress(randy, "RvkPresets_Current"));
    if (!g_presetCount || !g_presetName || !g_presetApply || !g_presetCurrent)
        g_presetCount = nullptr;                // an older renderer: no presets
    g_version = version();
    if (g_version != 1 && g_version != 2) {
        Log("[renderer] unknown settings interface version %u", g_version);
        return false;
    }
    return true;
}

bool GetInfo(uint32_t i, RvkSettingInfo& info)
{
    std::memset(&info, 0, sizeof(info));
    // A version 1 renderer only accepts its own (smaller) size.
    info.size = g_version >= 2 ? sizeof(info) : static_cast<uint32_t>(offsetof(RvkSettingInfo, parent));
    return g_get(i, &info) != 0;
}

// Float settings live in int DValues counting steps.
int ToSteps(const RvkSettingInfo& s, float v) { return static_cast<int>(std::lround(v / s.step)); }

// XML attribute text: escape the characters XML needs escaped.
void AppendEscaped(char*& out, char* end, const char* text)
{
    for (; *text && out < end - 8; ++text) {
        const char* rep = nullptr;
        switch (*text) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        }
        if (rep) { size_t n = std::strlen(rep); std::memcpy(out, rep, n); out += n; }
        else *out++ = *text;
    }
}

void Append(char*& out, char* end, const char* text)
{
    size_t n = std::strlen(text);
    if (out + n >= end) n = static_cast<size_t>(end - out - 1);
    std::memcpy(out, text, n);
    out += n;
}

bool Is(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }

// A setting in the "Experimental" section: shown in its own block under the switches, off by default (the
// defaults and the section are set in randy-vk's settings table).
bool IsExperimental(const RvkSettingInfo& s) { return Is(s.section, "Experimental"); }

// The settings whose `parent` is `p`, of the requested kind (bool switches or values).
std::vector<const RvkSettingInfo*> ChildrenOf(const std::vector<RvkSettingInfo>& all, const RvkSettingInfo& p, bool bools)
{
    std::vector<const RvkSettingInfo*> list;
    for (const RvkSettingInfo& c : all)
        if (Is(c.parent, p.name) && (c.type == kBool) == bools) list.push_back(&c);
    return list;
}

void AppendCheckBox(char*& out, char* end, const RvkSettingInfo& s, int indent)
{
    char attrs[128];
    std::snprintf(attrs, sizeof(attrs), "\" font=\"LARGE\" layout_borders=\"Rect(%d,0,0,0)\" opt_type=\"variant\" opt_variable=\"", indent);
    Append(out, end, "        <OptionCheckBox label=\"");
    AppendEscaped(out, end, s.label);
    Append(out, end, attrs);
    Append(out, end, s.name);
    Append(out, end, "\"/>\n");
}

// A slider (int: the value; float: steps, shown scaled back) or, for a choice, radio buttons of its values.
void AppendValue(char*& out, char* end, const RvkSettingInfo& s)
{
    if (s.type == kChoice && s.choices) {
        Append(out, end, "        <OptionRadioButtonGroup label=\"");
        AppendEscaped(out, end, s.label);
        Append(out, end, ":\" font=\"LARGE\" layout_borders=\"Rect(10,0,0,3)\" opt_type=\"variant\" opt_variable=\"");
        Append(out, end, s.name);
        Append(out, end, "\">\n");
        for (const char* c = s.choices; *c;) {
            while (*c == ' ') ++c;
            const char* e = c;
            while (*e && *e != ' ') ++e;
            if (e == c) break;
            char value[32];
            std::snprintf(value, sizeof(value), "%.*s", static_cast<int>(e - c), c);
            Append(out, end, "          <RadioButton label=\"");
            Append(out, end, value);
            Append(out, end, "\" value=\"");
            Append(out, end, value);
            Append(out, end, "\"/>\n");
            c = e;
        }
        Append(out, end, "        </OptionRadioButtonGroup>\n");
        return;
    }
    char attrs[192];
    if (s.type == kFloat)
        std::snprintf(attrs, sizeof(attrs),
                      "\" font=\"LARGE\" value_fmt=\"&lt;font color=#70C4D0&gt;%%.2f&lt;/font&gt;\" value_scale=\"%g\"/>\n", s.step);
    else
        std::snprintf(attrs, sizeof(attrs),
                      "\" font=\"LARGE\" value_fmt=\"&lt;font color=#70C4D0&gt;%%.0f&lt;/font&gt;\" value_scale=\"1\"/>\n");
    Append(out, end, "        <OptionSlider label=\"");
    AppendEscaped(out, end, s.label);
    Append(out, end, ":\" layout_borders=\"Rect(10,0,0,3)\" opt_type=\"variant\" opt_variable=\"");
    Append(out, end, s.name);
    Append(out, end, attrs);
}

void AppendHeading(char*& out, char* end, const char* text, int top, const char* font = "HUGE")
{
    char attrs[96];
    std::snprintf(attrs, sizeof(attrs), "\" font=\"%s\" layout_borders=\"Rect(0,%d,0,3)\" />\n", font, top);
    Append(out, end, "\n        <TextView value=\"");
    AppendEscaped(out, end, text);
    Append(out, end, attrs);
}

// The experimental features (settings in section "Experimental"): their own block under the switches, off by
// default. The switches come first - so they sit right under the other enhancements - then their values, grouped
// under each feature.
void AppendExperimental(char*& out, char* end, const std::vector<RvkSettingInfo>& all)
{
    bool any = false;
    for (const RvkSettingInfo& s : all)
        if (!s.parent && IsExperimental(s)) { any = true; break; }
    if (!any) return;

    AppendHeading(out, end, "Experimental", 6);
    Append(out, end, "        <TextView value=\"New and unfinished features. Off by default; they may be unstable or"
                     " change.\" font=\"LARGE\" layout_borders=\"Rect(10,0,0,3)\" />\n");
    // The switches, one block per feature (its own sub-switches indented).
    for (const RvkSettingInfo& p : all) {
        if (p.parent || !IsExperimental(p)) continue;
        AppendCheckBox(out, end, p, 10);
        for (const RvkSettingInfo* c : ChildrenOf(all, p, true)) AppendCheckBox(out, end, *c, 30);
    }
}

// The experimental features' values (sliders, choices), each under its feature's label - in the Advanced tab.
void AppendExperimentalValues(char*& out, char* end, const std::vector<RvkSettingInfo>& all)
{
    bool heading = false;
    for (const RvkSettingInfo& p : all) {
        if (p.parent || !IsExperimental(p)) continue;
        std::vector<const RvkSettingInfo*> values = ChildrenOf(all, p, false);
        if (values.empty()) continue;
        if (!heading) AppendHeading(out, end, "Experimental", 16);
        heading = true;
        AppendHeading(out, end, p.label, 8);
        for (const RvkSettingInfo* v : values) AppendValue(out, end, *v);
    }
}

// The frame rate cap (AOReloaded's own AOR_FpsCap, fps_cap.cpp) first: it belongs with the renderer's settings,
// not in the AOReloaded tab, which leaves it out when this tab is there.
void AppendFrameRate(char*& out, char* end)
{
    AppendHeading(out, end, "Frame rate", 0);
    Append(out, end, kFpsCapSliderXml);
}

int CurrentPreset()
{
    int p = g_presetCount ? g_presetCurrent() : -1;
    return p < 0 ? kCustom : p;
}

// The preset buttons, at the top of the tab.
void AppendPresets(char*& out, char* end)
{
    if (!g_presetCount) return;
    AppendHeading(out, end, "Preset", 0);
    Append(out, end, "        <OptionRadioButtonGroup label=\"Look:\" font=\"LARGE\" layout_borders=\"Rect(10,0,0,3)\""
                     " opt_type=\"variant\" opt_variable=\"");
    Append(out, end, kPresetVar);
    Append(out, end, "\">\n");
    uint32_t n = g_presetCount();
    char line[160];
    for (uint32_t i = 0; i < n; ++i) {
        Append(out, end, "          <RadioButton label=\"");
        AppendEscaped(out, end, g_presetName(i));
        std::snprintf(line, sizeof(line), "\" value=\"%u\"/>\n", i);
        Append(out, end, line);
    }
    std::snprintf(line, sizeof(line), "          <RadioButton label=\"Custom\" value=\"%d\"/>\n", kCustom);
    Append(out, end, line);
    Append(out, end, "        </OptionRadioButtonGroup>\n");
    Append(out, end, "        <TextView value=\"To keep a setup of your own, back up randy-vk.ini.\""
                     " font=\"LARGE\" layout_borders=\"Rect(10,0,0,3)\" />\n");
    AppendHeading(out, end, "Frame rate", 12);
    Append(out, end, kFpsCapSliderXml);
}

// Every renderer setting's DValue (and the preset buttons) set to the renderer's values - after a preset changed many
// of them at once.
void SyncAll()
{
    g_syncing = true;
    uint32_t count = g_count();
    for (uint32_t i = 0; i < count; ++i) {
        RvkSettingInfo s;
        if (!GetInfo(i, s) || std::strlen(s.name) > 15) continue;
        if (s.type == kBool) GameAPI::SetBool(s.name, s.value != 0.0f);
        else if (s.type == kFloat) GameAPI::SetInt(s.name, ToSteps(s, s.value));
        else GameAPI::SetInt(s.name, static_cast<int>(s.value));
    }
    if (g_presetCount) GameAPI::SetInt(kPresetVar, CurrentPreset());
    g_syncing = false;
}

// Version 2 layout: every on / off option at the top (by section, a feature's own options indented under it), then
// the values (sliders, choices) in blocks headed by their feature.
void BuildXml()
{
    char* out = g_xml;
    char* end = g_xml + sizeof(g_xml) - 256;   // room for the closing tags
    Append(out, end,
           "\n"
           "  <ScrollView h_alignment=\"LEFT\" label=\"Renderer\" v_scrollbar_mode=\"auto\""
           " scroll_client=\"rvk_scroll\" max_size=\"Point(16000,-1)\">\n"
           "    <ScrollViewChild name=\"rvk_scroll\">\n"
           "      <View h_alignment=\"LEFT\" view_layout=\"vertical\""
           " max_size=\"Point(16000,-1)\" layout_borders=\"Rect(0,0,10,0)\">\n"
           "        <TextView value=\"randy-vk renderer\" font=\"HUGE\" layout_borders=\"Rect(0,0,0,5)\" />\n"
           "        <TextView value=\"Changes apply at once. Settings are saved to randy-vk.ini.\""
           " font=\"LARGE\" layout_borders=\"Rect(0,0,0,3)\" />\n"
           "        <TextView value=\"The game's ground render mode stays at Shadows: the grass and the terrain's"
           " lighting need it.\" font=\"LARGE\" layout_borders=\"Rect(0,0,0,10)\" />\n");
    if (g_presetCount) AppendPresets(out, end);
    else AppendFrameRate(out, end);
    uint32_t count = g_count();
    std::vector<RvkSettingInfo> all;
    for (uint32_t i = 0; i < count; ++i) {
        RvkSettingInfo s;
        if (GetInfo(i, s)) all.push_back(s);
    }
    // Options: the switches (experimental features are pulled out into their own block just below).
    AppendHeading(out, end, "Options", 16);
    const char* section = "";
    for (const RvkSettingInfo& s : all) {
        if (s.type != kBool || s.parent || IsExperimental(s)) continue;
        if (!Is(section, s.section)) {
            section = s.section;
            AppendHeading(out, end, section, 6);
        }
        AppendCheckBox(out, end, s, 10);
        for (const RvkSettingInfo* c : ChildrenOf(all, s, true)) AppendCheckBox(out, end, *c, 30);
    }
    // The experimental features, right under the switches: new and unfinished, off by default.
    AppendExperimental(out, end, all);

    Append(out, end,
           "\n"
           "        <VLayoutSpacer/>\n"
           "      </View>\n"
           "    </ScrollViewChild>\n"
           "  </ScrollView>\n");

    // The Advanced tab, right after: every value (sliders, choices) - a block per feature (its label) or, for values
    // of no feature, one per section; then the experimental features' values. (A tab of its own: the Renderer tab
    // stays short, and the game builds both itself - no hooks.)
    Append(out, end,
           "\n"
           "  <ScrollView h_alignment=\"LEFT\" label=\"Advanced\" v_scrollbar_mode=\"auto\""
           " scroll_client=\"rvk_adv_scroll\" max_size=\"Point(16000,-1)\">\n"
           "    <ScrollViewChild name=\"rvk_adv_scroll\">\n"
           "      <View h_alignment=\"LEFT\" view_layout=\"vertical\""
           " max_size=\"Point(16000,-1)\" layout_borders=\"Rect(0,0,10,0)\">\n"
           "        <TextView value=\"randy-vk renderer: advanced\" font=\"HUGE\" layout_borders=\"Rect(0,0,0,5)\" />\n"
           "        <TextView value=\"Every slider and choice. Changes apply at once. Settings are saved to"
           " randy-vk.ini.\" font=\"LARGE\" layout_borders=\"Rect(0,0,0,10)\" />\n");
    const char* heading = "";
    for (const RvkSettingInfo& s : all) {
        if (!s.parent && IsExperimental(s)) continue;
        if (s.type == kBool && !s.parent) {
            std::vector<const RvkSettingInfo*> values = ChildrenOf(all, s, false);
            if (values.empty()) continue;
            AppendHeading(out, end, s.label, 8);
            heading = s.label;
            for (const RvkSettingInfo* v : values) AppendValue(out, end, *v);
        } else if (s.type != kBool && !s.parent) {
            if (!Is(heading, s.section)) AppendHeading(out, end, s.section, 8);
            heading = s.section;
            AppendValue(out, end, s);
        }
    }
    AppendExperimentalValues(out, end, all);
    end = g_xml + sizeof(g_xml) - 1;
    Append(out, end,
           "\n"
           "        <VLayoutSpacer/>\n"
           "      </View>\n"
           "    </ScrollViewChild>\n"
           "  </ScrollView>\n");
    *out = '\0';
    if (out >= g_xml + sizeof(g_xml) - 300)
        Log("[renderer] options XML truncated (%u bytes)", static_cast<unsigned>(out - g_xml));
}

// Version 1 renderers: in table order, sections as headings.
void BuildXmlV1()
{
    char* out = g_xml;
    char* end = g_xml + sizeof(g_xml) - 256;
    Append(out, end,
           "\n"
           "  <ScrollView h_alignment=\"LEFT\" label=\"Renderer\" v_scrollbar_mode=\"auto\""
           " scroll_client=\"rvk_scroll\" max_size=\"Point(16000,-1)\">\n"
           "    <ScrollViewChild name=\"rvk_scroll\">\n"
           "      <View h_alignment=\"LEFT\" view_layout=\"vertical\""
           " max_size=\"Point(16000,-1)\" layout_borders=\"Rect(0,0,10,0)\">\n"
           "        <TextView value=\"randy-vk renderer\" font=\"HUGE\" layout_borders=\"Rect(0,0,0,5)\" />\n"
           "        <TextView value=\"Changes apply at once. Settings are saved to randy-vk.ini.\""
           " font=\"LARGE\" layout_borders=\"Rect(0,0,0,10)\" />\n");
    AppendFrameRate(out, end);
    const char* section = "";
    uint32_t count = g_count();
    for (uint32_t i = 0; i < count; ++i) {
        RvkSettingInfo s;
        if (!GetInfo(i, s)) continue;
        if (!Is(section, s.section)) {
            section = s.section;
            AppendHeading(out, end, section, 10);
        }
        if (s.type == kBool) AppendCheckBox(out, end, s, 10);
        else AppendValue(out, end, s);
    }
    end = g_xml + sizeof(g_xml) - 1;
    Append(out, end,
           "\n"
           "        <VLayoutSpacer/>\n"
           "      </View>\n"
           "    </ScrollViewChild>\n"
           "  </ScrollView>\n");
    *out = '\0';
}

// The game's ground render mode (video options: Simple, Shadows, Optimized, Autodetect): randy-vk reads the ground
// as Shadows draws it - a base pass and a lightmap pass - for its grass, valley mist and terrain lighting, so with the
// renderer loaded it stays at Shadows.
constexpr const char* kGroundModeVar = "GroundRendering";
constexpr int kGroundShadows = 1;

void KeepGroundShadows()
{
    if (!GameAPI::Exists(kGroundModeVar)) return;
    AOVariant v{};
    if (!GameAPI::GetVariant(kGroundModeVar, v)) return;
    int mode;
    switch (static_cast<VariantType>(v.type)) {
    case VariantType::Int: mode = v.as_int; break;
    case VariantType::Float: mode = static_cast<int>(v.as_float); break;
    case VariantType::Double: mode = static_cast<int>(v.as_double); break;
    default: return;
    }
    if (mode == kGroundShadows) return;
    GameAPI::SetInt(kGroundModeVar, kGroundShadows);
    Log("[renderer] ground render mode %d -> Shadows (randy-vk's grass and terrain lighting need it)", mode);
}

}  // namespace

void RendererSettingsRegisterAll()
{
    g_available = Resolve();
    if (!g_available) {
        Log("[renderer] no randy-vk settings interface in randy31.dll - no Renderer tab");
        return;
    }
    uint32_t count = g_count();
    for (uint32_t i = 0; i < count; ++i) {
        RvkSettingInfo s;
        if (!GetInfo(i, s) || std::strlen(s.name) > 15) continue;
        if (s.type == kBool) {
            GameAPI::RegisterBool(s.name, s.value != 0.0f);
        } else if (s.type == kInt || s.type == kChoice) {
            GameAPI::RegisterInt(s.name, static_cast<int>(s.value));
            SetDValueMinMax(s.name, static_cast<int>(s.min), static_cast<int>(s.max));
        } else {
            GameAPI::RegisterInt(s.name, ToSteps(s, s.value));
            SetDValueMinMax(s.name, ToSteps(s, s.min), ToSteps(s, s.max));
        }
    }
    if (g_presetCount) {
        GameAPI::RegisterInt(kPresetVar, CurrentPreset());
    }
    if (g_version >= 2) BuildXml();
    else BuildXmlV1();
    Log("[renderer] %u renderer settings registered (interface %u, options XML %u bytes)", count, g_version,
        static_cast<unsigned>(std::strlen(g_xml)));
}

const char* RendererXmlBlock() { return g_available ? g_xml : nullptr; }

void RendererTick()
{
    static uint32_t frames;
    if (g_available && ++frames % 60 == 1)       // (about once a second; the hook below catches changes at once)
        KeepGroundShadows();
}

bool RendererOnSetDValue(const char* name, const AOVariant& value)
{
    if (g_available && std::strcmp(name, kGroundModeVar) == 0) {
        KeepGroundShadows();                    // another mode chosen in the video options: back to Shadows
        return true;
    }
    if (!g_available || std::strncmp(name, "RVK_", 4) != 0)
        return false;
    if (g_syncing)
        return true;                            // our own update of the panel
    auto asInt = [&]() -> int {
        switch (value.type) {
        case static_cast<uint32_t>(VariantType::Bool): return value.as_bool ? 1 : 0;
        case static_cast<uint32_t>(VariantType::Int): return value.as_int;
        case static_cast<uint32_t>(VariantType::Float): return static_cast<int>(value.as_float);
        case static_cast<uint32_t>(VariantType::Double): return static_cast<int>(value.as_double);
        default: return -1;
        }
    };
    if (g_presetCount && std::strcmp(name, kPresetVar) == 0) {
        int p = asInt();
        if (p >= 0 && p != kCustom && g_presetApply(static_cast<uint32_t>(p)))
            Log("[renderer] preset %s", g_presetName(static_cast<uint32_t>(p)));
        SyncAll();                              // the panel shows the preset's values
        return true;
    }
    uint32_t count = g_count();
    for (uint32_t i = 0; i < count; ++i) {
        RvkSettingInfo s;
        if (!GetInfo(i, s) || std::strcmp(s.name, name) != 0) continue;
        double v = 0.0;
        switch (value.type) {
        case static_cast<uint32_t>(VariantType::Bool): v = value.as_bool ? 1.0 : 0.0; break;
        case static_cast<uint32_t>(VariantType::Int): v = value.as_int; break;
        case static_cast<uint32_t>(VariantType::Float): v = value.as_float; break;
        case static_cast<uint32_t>(VariantType::Double): v = value.as_double; break;
        default: return true;
        }
        if (!std::isfinite(v)) return true;      // (a slider without a size sends NaN - never to the renderer)
        float setting = s.type == kFloat ? static_cast<float>(std::round(v) * s.step) : static_cast<float>(v);
        if (s.type == kBool) setting = setting != 0.0f ? 1.0f : 0.0f;
        setting = std::clamp(setting, s.min, s.max);
        if (setting == s.value) return true;    // (sliders send their value every frame while dragged)
        g_set(name, setting);
        Log("[renderer] %s = %g", name, setting);
        if (g_presetCount) {                    // a change by hand: the preset buttons follow (Custom)
            g_syncing = true;
            GameAPI::SetInt(kPresetVar, CurrentPreset());
            g_syncing = false;
        }
        return true;
    }
    return false;
}

}  // namespace aor
