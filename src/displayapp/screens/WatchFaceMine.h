#pragma once

#include <lvgl/src/lv_core/lv_obj.h>
#include <chrono>
#include <cstdint>
#include "displayapp/screens/Screen.h"
#include "displayapp/apps/Apps.h"
#include "displayapp/Controllers.h"
#include "components/datetime/DateTimeController.h"
#include "components/fs/FS.h"
#include "utility/DirtyValue.h"

namespace Pinetime {
  namespace Controllers {
    class Settings;
  }

  namespace Applications {
    namespace Screens {

      class WatchFaceMine : public Screen {
      public:
        WatchFaceMine(Controllers::DateTime& dateTimeController, Controllers::Settings& settingsController);
        ~WatchFaceMine() override;

        void Refresh() override;

        static bool IsAvailable(Controllers::FS& filesystem);

      private:
        Utility::DirtyValue<std::chrono::time_point<std::chrono::system_clock, std::chrono::seconds>> currentDateTime {};

        lv_obj_t* background;
        lv_obj_t* clockBlocker;
        lv_obj_t* labelTime;

        // Flash-read instrumentation, currently commented out in the .cpp.
        // uint32_t previousFsBytesRead = 0;
        // lv_obj_t* statsBlocker;
        // lv_obj_t* labelStats;

        Controllers::DateTime& dateTimeController;
        Controllers::Settings& settingsController;

        lv_task_t* taskRefresh;
      };
    }

    template <>
    struct WatchFaceTraits<WatchFace::Mine> {
      static constexpr WatchFace watchFace = WatchFace::Mine;
      static constexpr const char* name = "Mine";

      static Screens::Screen* Create(AppControllers& controllers) {
        return new Screens::WatchFaceMine(controllers.dateTimeController, controllers.settingsController);
      };

      static bool IsAvailable(Pinetime::Controllers::FS& filesystem) {
        return Screens::WatchFaceMine::IsAvailable(filesystem);
      }
    };
  }
}
