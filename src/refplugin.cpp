// <product>_Reflex.dll (RD_PRODUCT from version.h): a small REFramework plugin (reframework\plugins) that tells the add-on when the
// game's own frame starts on its game thread: right before the engine's input update (the
// via.ModuleEntry "UpdateHID"), and right after it. The add-on does Reflex's wait there, so it
// happens before the frame's input is read whatever the input device (keyboard and mouse,
// DualSense, Xbox controllers). Latency plan step 2, 2026-11-25.
//
// Only what's needed from REFramework's plugin API (include/reframework/API.h, plugin version
// 1.x) is declared here; the structures' first members are laid out as there.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "version.h"  // RD_PRODUCT

namespace {

struct PluginVersion {
    int major, minor, patch;
    const char* game_name;
};

using EntryCb = void (*)();
using OnEntryFn = bool (*)(const char*, EntryCb);
using LogFn = void (*)(const char*, ...);

struct PluginFunctions {
    void* on_lua_state_created;
    void* on_lua_state_destroyed;
    void* on_present;
    OnEntryFn on_pre_application_entry;
    OnEntryFn on_post_application_entry;
    void* lock_lua;
    void* unlock_lua;
    void* on_device_reset;
    void* on_message;
    LogFn log_error;
    LogFn log_warn;
    LogFn log_info;
};

struct InitializeParam {
    void* reframework_module;
    const PluginVersion* version;
    const PluginFunctions* functions;
};

using Fn = void (*)();
Fn g_before = nullptr, g_after = nullptr;
bool g_looked = false;

// The add-on's entry points (the add-on loads with ReShade, possibly after this plugin).
bool Resolve() {
    if (g_before) return true;
    static DWORD lastTry = 0;
    const DWORD now = GetTickCount();
    if (g_looked && now - lastTry < 1000) return false;  // look again about once a second
    g_looked = true, lastTry = now;
    HMODULE addon = GetModuleHandleA(RD_PRODUCT ".addon64");
    if (!addon) return false;
    g_before = reinterpret_cast<Fn>(GetProcAddress(addon, RD_PRODUCT "_GameFrameStart"));
    g_after = reinterpret_cast<Fn>(GetProcAddress(addon, RD_PRODUCT "_GameInputRead"));
    return g_before != nullptr;
}

void BeforeUpdateHid() {
    if (Resolve()) g_before();
}

void AfterUpdateHid() {
    if (g_after) g_after();
}

}  // namespace

extern "C" __declspec(dllexport) void reframework_plugin_required_version(PluginVersion* version) {
    version->major = 1, version->minor = 0, version->patch = 0;
}

extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const InitializeParam* param) {
    if (!param || !param->functions || !param->functions->on_pre_application_entry) return false;
    const bool pre = param->functions->on_pre_application_entry("UpdateHID", &BeforeUpdateHid);
    const bool post = param->functions->on_post_application_entry &&
                      param->functions->on_post_application_entry("UpdateHID", &AfterUpdateHid);
    if (param->functions->log_info)
        param->functions->log_info("[" RD_PRODUCT "_Reflex] input update callbacks: before %s, after %s", pre ? "ok" : "failed",
                                   post ? "ok" : "failed");
    return pre;
}
