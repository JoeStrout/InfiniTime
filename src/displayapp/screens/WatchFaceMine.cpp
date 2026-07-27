#include "displayapp/screens/WatchFaceMine.h"

#include <lvgl/lvgl.h>

#include "components/settings/Settings.h"
#include "displayapp/LittleVgl.h"

using namespace Pinetime::Applications::Screens;

namespace {
  constexpr const char* backgroundPath = "F:/images/fuji.bin";

  // Sized generously around the text they contain: the cover check in lv_obj_design() only reports
  // LV_DESIGN_RES_COVER when the invalidated area lies entirely inside the object, so a blocker
  // that merely hugs its label stops working the moment the label grows.
  constexpr int16_t clockBlockerWidth = 168;
  constexpr int16_t clockBlockerHeight = 62;

  // For the flash-read instrumentation, currently commented out below.
  // constexpr int16_t statsBlockerWidth = 128;
  // constexpr int16_t statsBlockerHeight = 28;

  // Opaque, square-cornered, fully solid: every one of these is required by the cover check.
  void MakeBlocker(lv_obj_t* obj, int16_t width, int16_t height) {
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_local_radius(obj, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, 0);
    lv_obj_set_style_local_border_width(obj, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, 0);
    lv_obj_set_style_local_clip_corner(obj, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, false);
    lv_obj_set_style_local_bg_color(obj, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_BLACK);
    lv_obj_set_style_local_bg_opa(obj, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_OPA_COVER);
  }
}

bool WatchFaceMine::IsAvailable(Pinetime::Controllers::FS& filesystem) {
  lfs_file f = {};
  if (filesystem.FileOpen(&f, "/images/fuji.bin", LFS_O_RDONLY) < 0) {
    return false;
  }
  filesystem.FileClose(&f);
  return true;
}

WatchFaceMine::WatchFaceMine(Controllers::DateTime& dateTimeController, Controllers::Settings& settingsController)
  : dateTimeController {dateTimeController}, settingsController {settingsController} {

  // Full-screen 240x240 CF_TRUE_COLOR image, streamed from external flash a line at a time
  // (automatically, by LVGL, on an as-needed basis).
  background = lv_img_create(lv_scr_act(), nullptr);
  lv_img_set_src(background, backgroundPath);
  lv_obj_align(background, nullptr, LV_ALIGN_IN_TOP_LEFT, 0, 0);

  // Centered a third of the way up from the bottom, leaving the top of the background clear.
  clockBlocker = lv_obj_create(lv_scr_act(), nullptr);
  MakeBlocker(clockBlocker, clockBlockerWidth, clockBlockerHeight);
  lv_obj_align(clockBlocker, nullptr, LV_ALIGN_CENTER, 0, 40);

  labelTime = lv_label_create(clockBlocker, nullptr);
  lv_obj_set_style_local_text_font(labelTime, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, &jetbrains_mono_42);
  lv_obj_set_style_local_text_color(labelTime, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_WHITE);
  lv_label_set_text_static(labelTime, "00:00");
  lv_obj_align(labelTime, nullptr, LV_ALIGN_CENTER, 0, 0);

  // Flash-read instrumentation. Re-enable this block (with the matching one in Refresh(), the
  // members in the header, and Components::lvglFsBytesRead in LittleVgl) to confirm the blocker
  // above is still covering the image: it reads 0 B/s when the image is being skipped.
  //
  // statsBlocker = lv_obj_create(lv_scr_act(), nullptr);
  // MakeBlocker(statsBlocker, statsBlockerWidth, statsBlockerHeight);
  // lv_obj_align(statsBlocker, nullptr, LV_ALIGN_IN_TOP_LEFT, 0, 0);
  //
  // labelStats = lv_label_create(statsBlocker, nullptr);
  // lv_obj_set_style_local_text_color(labelStats, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_LIME);
  // lv_label_set_text_static(labelStats, "0 B/s");
  // lv_obj_align(labelStats, nullptr, LV_ALIGN_CENTER, 0, 0);
  //
  // previousFsBytesRead = Components::lvglFsBytesRead;

  taskRefresh = lv_task_create(RefreshTaskCallback, LV_DISP_DEF_REFR_PERIOD, LV_TASK_PRIO_MID, this);
  Refresh();
}

WatchFaceMine::~WatchFaceMine() {
  lv_task_del(taskRefresh);
  lv_obj_clean(lv_scr_act());
}

void WatchFaceMine::Refresh() {
  currentDateTime = std::chrono::time_point_cast<std::chrono::seconds>(dateTimeController.CurrentDateTime());
  if (!currentDateTime.IsUpdated()) {
    return;
  }

  uint8_t hour = dateTimeController.Hours();
  const uint8_t minute = dateTimeController.Minutes();
  const uint8_t second = dateTimeController.Seconds();

  if (settingsController.GetClockType() == Controllers::Settings::ClockType::H12) {
    if (hour == 0) {
      hour = 12;
    } else if (hour > 12) {
      hour = hour - 12;
    }
  }

  // JetBrains Mono is monospaced, so swapping ':' for ' ' blinks the separator without changing
  // the label's width -- which keeps the invalidated area inside the blocker. This relies on 0x20
  // being in the jetbrains_mono_42 range in displayapp/fonts/fonts.json: LVGL drops glyphs it
  // can't find with no advance at all, so a missing space would silently close the digits up.
  lv_label_set_text_fmt(labelTime, "%02d%c%02d", hour, (second % 2) == 0 ? ':' : ' ', minute);

  // Bytes streamed from flash during the previous second. This settles at 0:
  // lv_refr_get_top_obj() finds the opaque blocker covering the invalidated area and never
  // descends to the image behind it.
  // const uint32_t bytesRead = Components::lvglFsBytesRead;
  // lv_label_set_text_fmt(labelStats, "%lu B/s", static_cast<unsigned long>(bytesRead - previousFsBytesRead));
  // previousFsBytesRead = bytesRead;
}
