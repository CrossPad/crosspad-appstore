/**
 * @file AppStoreApp.cpp
 * @brief App Store — browse registry, install/remove/update apps.
 *
 * Encoder navigates between app tiles (LVGL group). Press opens action
 * popup. Reads app-registry.json + apps.json via ArduinoJson. Actions
 * delegate to scripts/app_manager.py via popen().
 */

#if USE_LVGL

#include "AppStoreApp.hpp"
#include "pc_stubs/PcApp.hpp"
#include "pc_stubs/pc_platform.h"

#include <crosspad/app/AppRegistry.hpp>
#include "crosspad-gui/platform/IGuiPlatform.h"

#include "lvgl.h"
#include <ArduinoJson.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define popen  _popen
#define pclose _pclose
#else
#include <unistd.h>
#endif

/* ── Data ───────────────────────────────────────────────────────────── */

struct AppInfo {
    std::string id;
    std::string name;
    std::string version;
    std::string description;
    std::string category;
    std::string repo;
    std::string icon;
    std::vector<std::string> platforms;
    bool installed    = false;
    std::string installedCommit;
    std::string installedRef;
};

/* ── Shared state ───────────────────────────────────────────────────── */

static std::atomic<AppStoreState> s_state{AppStoreState::Idle};
static std::mutex                 s_dataMutex;
static std::vector<AppInfo>       s_apps;
static std::atomic<bool>          s_dataReady{false};
static std::mutex                 s_msgMutex;
static std::string                s_statusMsg;

// Persistent strings for event user_data (avoids strdup leak)
static std::vector<std::string>   s_appIds;

/* ── Widgets ────────────────────────────────────────────────────────── */

static lv_obj_t*   s_listContainer = nullptr;
static lv_obj_t*   s_statusLabel   = nullptr;
static lv_obj_t*   s_refreshBtn    = nullptr;
static lv_obj_t*   s_buildBar      = nullptr;
static lv_obj_t*   s_buildBtn      = nullptr;
static lv_obj_t*   s_restartBtn    = nullptr;
static lv_timer_t* s_pollTimer     = nullptr;
static std::atomic<bool> s_pendingChanges{false};
static std::atomic<bool> s_buildDone{false};

/* ── Helpers ─────────────────────────────────────────────────────────── */

static void set_status(const std::string& msg) {
    std::lock_guard<std::mutex> lock(s_msgMutex);
    s_statusMsg = msg;
}

static std::string get_status() {
    std::lock_guard<std::mutex> lock(s_msgMutex);
    return s_statusMsg;
}

static std::string get_project_root() {
    std::error_code ec;
    return std::filesystem::current_path(ec).string();
}

static bool platform_matches(const std::vector<std::string>& platforms) {
    if (platforms.empty()) return true;
    for (auto& p : platforms) {
        if (p == "pc") return true;
    }
    return false;
}

static std::string platforms_tag(const std::vector<std::string>& platforms) {
    if (platforms.empty()) return "all";
    if (platforms.size() == 1) {
        auto& p = platforms[0];
        if (p == "pc") return "pc";
        if (p == "arduino") return "ard";
        if (p == "esp-idf") return "idf";
        return p;
    }
    bool has_pc = false, has_ard = false, has_idf = false;
    for (auto& p : platforms) {
        if (p == "pc") has_pc = true;
        if (p == "arduino") has_ard = true;
        if (p == "esp-idf") has_idf = true;
    }
    if (has_pc && has_ard && has_idf) return "all";
    if (has_ard && has_idf) return "hw";
    std::string r;
    for (auto& p : platforms) {
        if (!r.empty()) r += "+";
        if (p == "arduino") r += "ard";
        else if (p == "esp-idf") r += "idf";
        else r += p;
    }
    return r;
}

static std::string resolve_icon_path(const std::string& iconName) {
    if (iconName.empty()) return "";
    std::string prefix = crosspad_gui::getGuiPlatform().assetPathPrefix();
    std::string full = prefix + iconName;
    // Check if file exists
    std::error_code ec;
    if (std::filesystem::exists(full, ec)) return full;
    return "";
}

/* ── Data loading ───────────────────────────────────────────────────── */

static void load_data() {
    set_status("Loading...");
    s_state = AppStoreState::Loading;

    std::string root = get_project_root();
    std::string registryPath = root + "/app-registry.json";
    std::string manifestPath = root + "/apps.json";

    std::vector<AppInfo> apps;

    // Parse registry
    {
        std::ifstream f(registryPath);
        if (!f.is_open()) {
            set_status("No registry — press Refresh");
            s_state = AppStoreState::Error;
            s_dataReady = true;
            return;
        }
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());

        JsonDocument doc;
        auto err = deserializeJson(doc, content);
        if (err) {
            set_status("Registry parse error");
            s_state = AppStoreState::Error;
            s_dataReady = true;
            return;
        }

        JsonObject appsObj = doc["apps"].as<JsonObject>();
        for (JsonPair kv : appsObj) {
            AppInfo info;
            info.id = kv.key().c_str();
            JsonObject v = kv.value().as<JsonObject>();
            info.name        = v["name"] | info.id.c_str();
            info.version     = v["version"] | "?";
            info.description = v["description"] | "";
            info.category    = v["category"] | "other";
            info.repo        = v["repo"] | "";
            info.icon        = v["icon"] | "";
            JsonArray plats = v["platforms"].as<JsonArray>();
            for (JsonVariant p : plats) {
                info.platforms.push_back(p.as<std::string>());
            }
            apps.push_back(std::move(info));
        }
    }

    // Parse manifest
    {
        std::ifstream f(manifestPath);
        if (f.is_open()) {
            std::string content((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
            JsonDocument doc;
            if (!deserializeJson(doc, content)) {
                JsonObject inst = doc["installed"].as<JsonObject>();
                for (auto& app : apps) {
                    if (inst[app.id].is<JsonObject>()) {
                        app.installed = true;
                        app.installedCommit = inst[app.id]["version"] | "?";
                        app.installedRef    = inst[app.id]["ref"] | "main";
                    }
                }
            }
        }
    }

    // Sort: compatible first, then by category, then by name
    std::sort(apps.begin(), apps.end(), [](const AppInfo& a, const AppInfo& b) {
        bool aCompat = platform_matches(a.platforms);
        bool bCompat = platform_matches(b.platforms);
        if (aCompat != bCompat) return aCompat > bCompat;
        if (a.category != b.category) return a.category < b.category;
        return a.name < b.name;
    });

    {
        std::lock_guard<std::mutex> lock(s_dataMutex);
        s_apps = std::move(apps);
        // Store IDs for event callbacks (stable pointers)
        s_appIds.clear();
        for (auto& a : s_apps) s_appIds.push_back(a.id);
    }

    int installed = 0, compatible = 0;
    for (auto& a : s_apps) {
        if (platform_matches(a.platforms)) compatible++;
        if (a.installed) installed++;
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "%d/%d installed  |  %d total",
             installed, compatible, (int)s_apps.size());
    set_status(buf);
    s_state = AppStoreState::Idle;
    s_dataReady = true;
}

/* ── Actions ────────────────────────────────────────────────────────── */

static bool run_app_cmd(const char* command, const char* appId) {
    std::string root = get_project_root();
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "cd \"%s\" && python3 scripts/app_manager.py %s %s 2>&1",
             root.c_str(), command, appId);

    FILE* pipe = popen(cmd, "r");
    if (!pipe) { set_status("Failed to run app manager"); return false; }

    char buf[256];
    while (fgets(buf, sizeof(buf), pipe)) {
        std::string line(buf);
        if (!line.empty() && line.back() == '\n') line.pop_back();
        if (!line.empty() && line.find_first_not_of(' ') != std::string::npos) {
            set_status(line);
        }
    }
    return (pclose(pipe) == 0);
}

static void action_install(const std::string& appId) {
    s_state = AppStoreState::Installing;
    set_status("Installing " + appId + "...");
    std::string id = appId;
    std::thread([id]() {
        bool ok = run_app_cmd("install", id.c_str());
        if (ok) {
            s_pendingChanges = true;
            set_status("Installed — rebuild needed");
        } else {
            set_status("Install failed");
            s_state = AppStoreState::Error;
        }
        load_data();
    }).detach();
}

static void action_remove(const std::string& appId) {
    s_state = AppStoreState::Removing;
    set_status("Removing " + appId + "...");
    std::string id = appId;
    std::thread([id]() {
        bool ok = run_app_cmd("remove", id.c_str());
        if (ok) {
            s_pendingChanges = true;
            set_status("Removed — rebuild needed");
        } else {
            set_status("Remove failed");
            s_state = AppStoreState::Error;
        }
        load_data();
    }).detach();
}

static void action_update(const std::string& appId) {
    s_state = AppStoreState::Updating;
    set_status("Updating " + appId + "...");
    std::string id = appId;
    std::thread([id]() {
        bool ok = run_app_cmd("update", id.c_str());
        if (ok) {
            s_pendingChanges = true;
            set_status("Updated — rebuild needed");
        } else {
            set_status("Update failed");
            s_state = AppStoreState::Error;
        }
        load_data();
    }).detach();
}

static void action_build() {
    s_state = AppStoreState::Installing; // reuse for "busy"
    set_status("Building...");
    std::thread([]() {
        std::string root = get_project_root();
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "cd \"%s\" && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug"
                 " && cmake --build build 2>&1",
                 root.c_str());

        FILE* pipe = popen(cmd, "r");
        if (!pipe) { set_status("Build failed — no cmake"); s_state = AppStoreState::Error; return; }

        char buf[256];
        int errors = 0;
        while (fgets(buf, sizeof(buf), pipe)) {
            std::string line(buf);
            if (!line.empty() && line.back() == '\n') line.pop_back();
            // Show ninja progress lines [N/M]
            if (!line.empty() && line[0] == '[') {
                set_status(line.substr(0, 40));
            }
            if (line.find("error:") != std::string::npos) errors++;
        }
        int rc = pclose(pipe);
        if (rc == 0 && errors == 0) {
            s_pendingChanges = false;
            s_buildDone = true;
            set_status("Build OK — restart to apply");
        } else {
            set_status("Build failed (" + std::to_string(errors) + " errors)");
            s_state = AppStoreState::Error;
            return;
        }
        s_state = AppStoreState::Idle;
    }).detach();
}

static void action_refresh() {
    s_state = AppStoreState::Loading;
    set_status("Refreshing registry...");
    std::thread([]() {
        std::string root = get_project_root();
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "cd \"%s\" && python3 scripts/app_manager.py list > /dev/null 2>&1",
                 root.c_str());
        system(cmd);
        load_data();
    }).detach();
}

/* ── Detail popup ───────────────────────────────────────────────────── */

static void show_detail(size_t idx) {
    bool busy = (s_state != AppStoreState::Idle && s_state != AppStoreState::Error);
    if (busy) return;

    std::lock_guard<std::mutex> lock(s_dataMutex);
    if (idx >= s_apps.size()) return;
    const auto& app = s_apps[idx];
    bool compat = platform_matches(app.platforms);

    std::string desc = app.description;
    desc += "\n\nVersion: " + app.version;
    desc += "\nPlatforms: " + platforms_tag(app.platforms);
    if (app.installed) {
        desc += "\nInstalled: " + app.installedRef + " @ " + app.installedCommit;
    }

    lv_obj_t* mbox = lv_msgbox_create(nullptr);
    lv_msgbox_add_title(mbox, app.name.c_str());
    lv_obj_t* txtObj = lv_msgbox_add_text(mbox, desc.c_str());
    lv_obj_set_style_text_color(txtObj, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(txtObj, &lv_font_montserrat_12, 0);
    lv_msgbox_add_close_button(mbox);

    lv_obj_set_width(mbox, 290);
    lv_obj_center(mbox);
    lv_obj_set_style_bg_color(mbox, lv_color_hex(0x16213E), 0);

    // Safe pointer to persistent ID string
    const char* idPtr = s_appIds[idx].c_str();

    if (app.installed) {
        lv_obj_t* btnUpdate = lv_msgbox_add_footer_button(mbox, "Update");
        lv_obj_t* btnRemove = lv_msgbox_add_footer_button(mbox, "Remove");

        lv_obj_add_event_cb(btnUpdate, [](lv_event_t* e) {
            auto* btn = lv_event_get_target_obj(e);
            auto* mb = lv_obj_get_parent(lv_obj_get_parent(btn));
            const char* id = (const char*)lv_event_get_user_data(e);
            lv_msgbox_close(mb);
            action_update(id);
        }, LV_EVENT_CLICKED, (void*)idPtr);

        lv_obj_add_event_cb(btnRemove, [](lv_event_t* e) {
            auto* btn = lv_event_get_target_obj(e);
            auto* mb = lv_obj_get_parent(lv_obj_get_parent(btn));
            const char* id = (const char*)lv_event_get_user_data(e);
            lv_msgbox_close(mb);
            action_remove(id);
        }, LV_EVENT_CLICKED, (void*)idPtr);
    } else if (compat) {
        lv_obj_t* btnInstall = lv_msgbox_add_footer_button(mbox, "Install");

        lv_obj_add_event_cb(btnInstall, [](lv_event_t* e) {
            auto* btn = lv_event_get_target_obj(e);
            auto* mb = lv_obj_get_parent(lv_obj_get_parent(btn));
            const char* id = (const char*)lv_event_get_user_data(e);
            lv_msgbox_close(mb);
            action_install(id);
        }, LV_EVENT_CLICKED, (void*)idPtr);
    }
}

/* ── UI building ────────────────────────────────────────────────────── */

static void rebuild_list() {
    if (!s_listContainer) return;

    lv_obj_clean(s_listContainer);

    std::lock_guard<std::mutex> lock(s_dataMutex);

    std::string lastCategory;
    for (size_t i = 0; i < s_apps.size(); i++) {
        const auto& app = s_apps[i];
        bool compat = platform_matches(app.platforms);

        // Category header
        if (app.category != lastCategory) {
            lastCategory = app.category;
            lv_obj_t* hdr = lv_label_create(s_listContainer);
            std::string catTitle = app.category;
            if (!catTitle.empty()) catTitle[0] = toupper(catTitle[0]);
            lv_label_set_text(hdr, catTitle.c_str());
            lv_obj_set_width(hdr, LV_PCT(100));
            lv_obj_set_style_text_color(hdr, lv_color_hex(0x4488CC), 0);
            lv_obj_set_style_text_font(hdr, &lv_font_montserrat_10, 0);
            lv_obj_set_style_pad_top(hdr, 6, 0);
            lv_obj_set_style_pad_bottom(hdr, 2, 0);
        }

        // App tile
        lv_obj_t* tile = lv_obj_create(s_listContainer);
        lv_obj_set_size(tile, LV_PCT(100), 32);
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(tile, 4, 0);
        lv_obj_set_style_pad_column(tile, 6, 0);
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(0x222244), 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(0x4488CC), LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(tile, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_radius(tile, 4, 0);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);

        if (compat) {
            lv_obj_set_style_bg_color(tile, lv_color_hex(0x16213E), 0);
            lv_obj_set_style_bg_color(tile, lv_color_hex(0x1F4068), LV_STATE_FOCUSED);
        } else {
            lv_obj_set_style_bg_color(tile, lv_color_hex(0x0D0D14), 0);
        }

        // Icon (image or symbol fallback)
        std::string iconPath = resolve_icon_path(app.icon);
        if (!iconPath.empty()) {
            lv_obj_t* img = lv_image_create(tile);
            lv_image_set_src(img, iconPath.c_str());
            lv_obj_set_size(img, 20, 20);
            lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
        } else {
            // Symbol fallback based on category
            lv_obj_t* sym = lv_label_create(tile);
            if (app.category == "music") lv_label_set_text(sym, LV_SYMBOL_AUDIO);
            else if (app.category == "audio") lv_label_set_text(sym, LV_SYMBOL_VOLUME_MAX);
            else lv_label_set_text(sym, LV_SYMBOL_SETTINGS);
            lv_obj_set_style_text_color(sym, lv_color_hex(0x4488CC), 0);
            lv_obj_set_width(sym, 20);
        }

        // Installed indicator
        lv_obj_t* dot = lv_label_create(tile);
        if (app.installed) {
            lv_label_set_text(dot, LV_SYMBOL_OK);
            lv_obj_set_style_text_color(dot, lv_color_hex(0x22DD22), 0);
        } else if (compat) {
            lv_label_set_text(dot, LV_SYMBOL_DOWNLOAD);
            lv_obj_set_style_text_color(dot, lv_color_hex(0x555555), 0);
        } else {
            lv_label_set_text(dot, LV_SYMBOL_CLOSE);
            lv_obj_set_style_text_color(dot, lv_color_hex(0x553333), 0);
        }
        lv_obj_set_style_text_font(dot, &lv_font_montserrat_12, 0);
        lv_obj_set_width(dot, 16);

        // App name
        lv_obj_t* nameLabel = lv_label_create(tile);
        lv_label_set_text(nameLabel, app.name.c_str());
        lv_obj_set_flex_grow(nameLabel, 1);
        lv_label_set_long_mode(nameLabel, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(nameLabel, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(nameLabel,
            compat ? lv_color_hex(0xEEEEEE) : lv_color_hex(0x555555), 0);

        // Version
        lv_obj_t* verLabel = lv_label_create(tile);
        char verBuf[16];
        snprintf(verBuf, sizeof(verBuf), "v%s", app.version.c_str());
        lv_label_set_text(verLabel, verBuf);
        lv_obj_set_style_text_font(verLabel, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(verLabel, lv_color_hex(0x888888), 0);

        // Platform tag
        lv_obj_t* platLabel = lv_label_create(tile);
        lv_label_set_text(platLabel, platforms_tag(app.platforms).c_str());
        lv_obj_set_style_text_font(platLabel, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(platLabel,
            compat ? lv_color_hex(0x4488CC) : lv_color_hex(0x443333), 0);

        // Click callback — open detail popup
        lv_obj_add_event_cb(tile, [](lv_event_t* e) {
            int idx = (int)(intptr_t)lv_event_get_user_data(e);
            show_detail((size_t)idx);
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
}

/* ── Timer ──────────────────────────────────────────────────────────── */

static void poll_timer_cb(lv_timer_t*) {
    if (s_statusLabel) {
        std::string msg = get_status();
        if (strcmp(lv_label_get_text(s_statusLabel), msg.c_str()) != 0) {
            lv_label_set_text(s_statusLabel, msg.c_str());
        }
    }

    if (s_dataReady.exchange(false)) {
        rebuild_list();
    }

    bool busy = (s_state == AppStoreState::Loading ||
                 s_state == AppStoreState::Installing ||
                 s_state == AppStoreState::Removing ||
                 s_state == AppStoreState::Updating);

    if (s_refreshBtn) {
        if (busy) lv_obj_add_state(s_refreshBtn, LV_STATE_DISABLED);
        else      lv_obj_remove_state(s_refreshBtn, LV_STATE_DISABLED);
    }

    // Show/hide build bar + restart button
    if (s_buildBar) {
        if (s_buildDone) {
            lv_obj_remove_flag(s_buildBar, LV_OBJ_FLAG_HIDDEN);
            if (s_buildBtn) lv_obj_add_flag(s_buildBtn, LV_OBJ_FLAG_HIDDEN);
            if (s_restartBtn) lv_obj_remove_flag(s_restartBtn, LV_OBJ_FLAG_HIDDEN);
        } else if (s_pendingChanges && !busy) {
            lv_obj_remove_flag(s_buildBar, LV_OBJ_FLAG_HIDDEN);
            if (s_buildBtn) lv_obj_remove_flag(s_buildBtn, LV_OBJ_FLAG_HIDDEN);
            if (s_restartBtn) lv_obj_add_flag(s_restartBtn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_buildBar, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ── App create / destroy ───────────────────────────────────────────── */

lv_obj_t* AppStore_create(lv_obj_t* parent, App*) {
    lv_obj_t* root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_obj_get_content_width(parent),
                    lv_obj_get_content_height(parent));
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(root);

    // ── Title bar ──
    lv_obj_t* titleBar = lv_obj_create(root);
    lv_obj_set_size(titleBar, LV_PCT(100), 26);
    lv_obj_set_style_bg_color(titleBar, lv_color_hex(0x0F3460), 0);
    lv_obj_set_style_border_width(titleBar, 0, 0);
    lv_obj_set_style_pad_all(titleBar, 3, 0);
    lv_obj_set_style_radius(titleBar, 0, 0);
    lv_obj_remove_flag(titleBar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(titleBar);
    lv_label_set_text(title, LV_SYMBOL_LIST "  App Store");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 4, 0);

    s_refreshBtn = lv_button_create(titleBar);
    lv_obj_set_size(s_refreshBtn, 70, 20);
    lv_obj_align(s_refreshBtn, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(s_refreshBtn, lv_color_hex(0x1A5276), 0);
    lv_obj_set_style_radius(s_refreshBtn, 4, 0);
    lv_obj_t* refreshLabel = lv_label_create(s_refreshBtn);
    lv_label_set_text(refreshLabel, LV_SYMBOL_REFRESH " Refresh");
    lv_obj_set_style_text_font(refreshLabel, &lv_font_montserrat_10, 0);
    lv_obj_center(refreshLabel);
    lv_obj_add_event_cb(s_refreshBtn, [](lv_event_t*) {
        action_refresh();
    }, LV_EVENT_CLICKED, nullptr);

    // ── App list ──
    s_listContainer = lv_obj_create(root);
    lv_obj_set_width(s_listContainer, LV_PCT(100));
    lv_obj_set_flex_grow(s_listContainer, 1);
    lv_obj_set_style_bg_color(s_listContainer, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_listContainer, 0, 0);
    lv_obj_set_style_pad_all(s_listContainer, 4, 0);
    lv_obj_set_style_pad_row(s_listContainer, 2, 0);
    lv_obj_set_style_radius(s_listContainer, 0, 0);
    lv_obj_set_flex_flow(s_listContainer, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_listContainer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_listContainer, LV_SCROLLBAR_MODE_AUTO);

    // ── Build bar (hidden until pending changes) ──
    s_buildBar = lv_obj_create(root);
    lv_obj_set_size(s_buildBar, LV_PCT(100), 28);
    lv_obj_set_style_bg_color(s_buildBar, lv_color_hex(0x2D1F0E), 0);
    lv_obj_set_style_border_width(s_buildBar, 0, 0);
    lv_obj_set_style_pad_all(s_buildBar, 3, 0);
    lv_obj_set_style_radius(s_buildBar, 0, 0);
    lv_obj_remove_flag(s_buildBar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_buildBar, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* pendingLabel = lv_label_create(s_buildBar);
    lv_label_set_text(pendingLabel, LV_SYMBOL_WARNING " Rebuild needed");
    lv_obj_set_style_text_font(pendingLabel, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(pendingLabel, lv_color_hex(0xFFAA33), 0);
    lv_obj_align(pendingLabel, LV_ALIGN_LEFT_MID, 4, 0);

    s_buildBtn = lv_button_create(s_buildBar);
    lv_obj_set_size(s_buildBtn, 60, 22);
    lv_obj_align(s_buildBtn, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(s_buildBtn, lv_color_hex(0xCC6600), 0);
    lv_obj_set_style_radius(s_buildBtn, 4, 0);
    lv_obj_t* buildLabel = lv_label_create(s_buildBtn);
    lv_label_set_text(buildLabel, "Build");
    lv_obj_set_style_text_font(buildLabel, &lv_font_montserrat_10, 0);
    lv_obj_center(buildLabel);
    lv_obj_add_event_cb(s_buildBtn, [](lv_event_t*) {
        action_build();
    }, LV_EVENT_CLICKED, nullptr);

    s_restartBtn = lv_button_create(s_buildBar);
    lv_obj_set_size(s_restartBtn, 65, 22);
    lv_obj_align(s_restartBtn, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(s_restartBtn, lv_color_hex(0x227722), 0);
    lv_obj_set_style_radius(s_restartBtn, 4, 0);
    lv_obj_t* restartLabel = lv_label_create(s_restartBtn);
    lv_label_set_text(restartLabel, "Restart");
    lv_obj_set_style_text_font(restartLabel, &lv_font_montserrat_10, 0);
    lv_obj_center(restartLabel);
    lv_obj_add_flag(s_restartBtn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_restartBtn, [](lv_event_t*) {
        // Re-exec the simulator binary
        std::string exe = get_project_root() + "/bin/CrossPad";
        printf("[AppStore] Restarting: %s\n", exe.c_str());
#ifdef _WIN32
        _execl(exe.c_str(), "CrossPad", nullptr);
#else
        execl(exe.c_str(), "CrossPad", nullptr);
#endif
    }, LV_EVENT_CLICKED, nullptr);

    // ── Status bar ──
    lv_obj_t* statusBar = lv_obj_create(root);
    lv_obj_set_size(statusBar, LV_PCT(100), 18);
    lv_obj_set_style_bg_color(statusBar, lv_color_hex(0x0A0A14), 0);
    lv_obj_set_style_border_width(statusBar, 0, 0);
    lv_obj_set_style_pad_all(statusBar, 2, 0);
    lv_obj_set_style_radius(statusBar, 0, 0);
    lv_obj_remove_flag(statusBar, LV_OBJ_FLAG_SCROLLABLE);

    s_statusLabel = lv_label_create(statusBar);
    lv_label_set_text(s_statusLabel, "Loading...");
    lv_obj_set_style_text_font(s_statusLabel, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_statusLabel, lv_color_hex(0x888888), 0);
    lv_obj_align(s_statusLabel, LV_ALIGN_LEFT_MID, 4, 0);

    // ── Start ──
    s_pollTimer = lv_timer_create(poll_timer_cb, 200, nullptr);
    std::thread([]() { load_data(); }).detach();

    printf("[AppStore] Created\n");
    return root;
}

void AppStore_destroy(lv_obj_t* app_obj) {
    if (s_pollTimer) { lv_timer_delete(s_pollTimer); s_pollTimer = nullptr; }

    s_listContainer = nullptr;
    s_statusLabel = nullptr;
    s_refreshBtn = nullptr;
    s_buildBar = nullptr;
    s_buildBtn = nullptr;
    s_restartBtn = nullptr;
    s_dataReady = false;
    s_pendingChanges = false;
    s_buildDone = false;

    lv_obj_delete_async(app_obj);
    printf("[AppStore] Destroyed\n");
}

/* ── Registration ───────────────────────────────────────────────────── */

void _register_AppStore_app() {
    static char icon_path[256];
    snprintf(icon_path, sizeof(icon_path), "%sCrossPad_Logo_110w.png",
             crosspad_gui::getGuiPlatform().assetPathPrefix());

    static const crosspad::AppEntry entry = {
        "App Store", icon_path,
        AppStore_create, AppStore_destroy,
        nullptr, nullptr, nullptr, nullptr, 5
    };
    crosspad::AppRegistry::getInstance().registerApp(entry);
}

#endif // USE_LVGL
