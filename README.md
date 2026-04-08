# CrossPad App Store

Built-in LVGL app for browsing, installing, and managing CrossPad apps directly from the simulator.

## Features

- Browse the [CrossPad App Registry](https://github.com/CrossPad/crosspad-apps)
- Install/remove/update apps as git submodules
- Encoder navigation with focus highlight
- App icons with symbol fallback
- Platform compatibility tags (pc, arduino, esp-idf)
- Background operations with status feedback

## How It Works

The App Store reads `app-registry.json` (cached registry) and `apps.json` (installed manifest) via ArduinoJson. Actions (install/remove/update) delegate to `scripts/app_manager.py` via subprocess.

## Built-in App

This app is **built-in** — it cannot be installed or removed via the app manager. It ships with crosspad-pc as a git submodule and is always available in the launcher.

## Creating a CrossPad App

To create an app that appears in the App Store:

1. Create a GitHub repo with the structure:
   ```
   your-app/
     crosspad-app.json    # App metadata (required)
     src/                 # C++ source files
       YourApp.cpp
     include/             # Headers (optional)
       crosspad-your-app/
         YourApp.hpp
   ```

2. Add `crosspad-app.json`:
   ```json
   {
     "name": "My App",
     "id": "my-app",
     "version": "0.1.0",
     "description": "What it does",
     "category": "music",
     "icon": "my-icon.png",
     "component_path": "components/crosspad-my-app",
     "platforms": ["pc", "arduino", "esp-idf"],
     "requires": {
       "crosspad-core": ">=0.3.0",
       "crosspad-gui": ">=0.2.0"
     },
     "changelog": [
       "0.1.0: Initial release"
     ]
   }
   ```

3. Add the `crosspad-app` topic to your GitHub repo:
   ```bash
   gh repo edit your-org/your-app --add-topic crosspad-app
   ```

4. The registry CI auto-discovers your app every 6 hours. Users can then install it from the App Store or via CLI:
   ```bash
   python3 scripts/app_manager.py install my-app
   ```

### App Registration Pattern

Your main `.cpp` must define a registration function matching `void _register_<Name>_app()`:

```cpp
#include "crosspad/app/AppRegistrar.hpp"
#include "crosspad-gui/platform/IGuiPlatform.h"

// LVGL create/destroy callbacks
static lv_obj_t* MyApp_create(lv_obj_t* parent, App* app) { ... }
static void MyApp_destroy(lv_obj_t* obj) { ... }

void _register_MyApp_app() {
    static char icon_path[256];
    snprintf(icon_path, sizeof(icon_path), "%smy-icon.png",
             crosspad_gui::getGuiPlatform().assetPathPrefix());
    static const crosspad::AppEntry entry = {
        "My App", icon_path,
        MyApp_create, MyApp_destroy,
        nullptr, nullptr, nullptr, nullptr, 0
    };
    crosspad::AppRegistry::getInstance().registerApp(entry);
}
```

Or use the `REGISTER_APP` macro for cross-platform apps:

```cpp
REGISTER_APP(MyApp, nullptr, "my-icon.png",
             MyApp_create, MyApp_destroy,
             nullptr, nullptr, nullptr, 0)
```

### Platforms

| Platform | Install dir | Build system | Icon resolution |
|----------|-------------|-------------|-----------------|
| **PC** | `src/apps/crosspad-*` | CMake (auto-discovered) | `assetPathPrefix()` |
| **Arduino** | `lib/crosspad-*` | PlatformIO | `I:/` prefix |
| **ESP-IDF** | `components/crosspad-*` | idf.py (GLOB) | `A:` prefix |

## License

MIT
