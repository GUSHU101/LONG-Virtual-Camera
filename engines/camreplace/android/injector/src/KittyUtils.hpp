#pragma once

#include <android/log.h>

#define KITTY_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "cr_sepolicy", __VA_ARGS__)
#define KITTY_LOGW(...) __android_log_print(ANDROID_LOG_WARN, "cr_sepolicy", __VA_ARGS__)
#define KITTY_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "cr_sepolicy", __VA_ARGS__)
