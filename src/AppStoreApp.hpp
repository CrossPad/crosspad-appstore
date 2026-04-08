#pragma once

#if USE_LVGL

#include "lvgl.h"

class App;

lv_obj_t* AppStore_create(lv_obj_t* parent, App* app);
void      AppStore_destroy(lv_obj_t* obj);

enum class AppStoreState {
    Idle,
    Loading,
    Installing,
    Removing,
    Updating,
    Error
};

#endif
