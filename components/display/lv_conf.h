#ifndef LV_CONF_H
#define LV_CONF_H
/* One owner, software rendering, fixed heap. Windowing and input drivers belong to Q2-SoC. */
#define LV_COLOR_DEPTH 32
#define LV_USE_OS LV_OS_NONE
#ifndef LV_USE_STDLIB_MALLOC
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#endif
/* PNG decode plus retained ARGB images; ESP32 currently does not enable this provider. */
#define LV_MEM_SIZE (16 * 1024 * 1024U)
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_DEF_REFR_PERIOD 16
#define LV_DRAW_BUF_STRIDE_ALIGN 4
#define LV_DRAW_BUF_ALIGN 4
#define LV_USE_DRAW_SW 1
#define LV_DRAW_SW_DRAW_UNIT_CNT 1
#define LV_USE_LOG 0
#define LV_USE_ASSERT_MALLOC 0
#define LV_USE_ASSERT_NULL 0
#define LV_USE_THEME_DEFAULT 0
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO 0
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_DEFAULT &lv_font_montserrat_20
#define LV_USE_LABEL 1
#define LV_USE_BUTTON 1
#define LV_USE_SYSMON 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#define LV_USE_SDL 0
#define LV_USE_WINDOWS 0
#define LV_USE_LINUX_FBDEV 0
#define LV_USE_LINUX_DRM 0
#define LV_USE_LODEPNG 1
#define LV_USE_LIBPNG 0
#define LV_USE_TJPGD 1
#define LV_USE_FREETYPE 0
/* The build option removes both the embedded CJK asset and its rasterizer. */
#ifndef Q2_ENABLE_CJK_FONT
#define Q2_ENABLE_CJK_FONT 1
#endif
#define LV_USE_TINY_TTF Q2_ENABLE_CJK_FONT
#define LV_TINY_TTF_FILE_SUPPORT 0
#define LV_USE_THORVG_INTERNAL 0
#define LV_USE_THORVG_EXTERNAL 0
#define LV_USE_QRCODE 1
#endif
