#pragma once

#include <lvgl/lvgl.h>
#include <array>
#include <cstdint>
#include <vector>
#include "displayapp/screens/Screen.h"
#include "displayapp/apps/Apps.h"
#include "displayapp/Controllers.h"

namespace Pinetime {
  namespace Controllers {
    class DateTime;
    class FS;
  }

  namespace Applications {
    namespace Screens {

      // A watch face whose contents are described by a text config file.
      // The built-in "Default" canvas is always available; more are loaded
      // from /canvas/*.cfg on the external filesystem. See doc/CanvasWatchFace.md.
      class WatchFaceCanvas : public Screen {
      public:
        WatchFaceCanvas(Controllers::DateTime& dateTimeController, Controllers::FS& filesystem);
        ~WatchFaceCanvas() override;

        void Refresh() override;
        bool OnTouchEvent(TouchEvents event) override;
        bool OnTouchEvent(uint16_t x, uint16_t y) override;

        enum class ElementType : uint8_t { Text, Image, Rect, Line, Arc, Hand };
        enum class Field : uint8_t { Year, Month, Day, YearDay, Week, WeekDay, Hour, Minute, Second, Tenth, Count };
        enum class Operator : uint8_t { Equal, Less, Greater, LessOrEqual, GreaterOrEqual };
        enum class Resolution : uint8_t { Minute, Second, Tenth };

        struct Condition {
          Field field;
          Operator op;
          int16_t value;
        };

        static constexpr uint8_t MaxConditions = 6;

        struct Element {
          ElementType type;
          uint8_t variant = 0; // text alignment, or which hand
          uint8_t font = 0;    // index into fonts
          uint8_t nConditions = 0;
          int16_t x = 0;
          int16_t y = 0;
          int16_t w = 0; // line: x2
          int16_t h = 0; // line: y2
          int16_t width = 1;
          int16_t radius = -1;
          int16_t start = 0;
          int16_t end = 360;
          lv_color_t color;
          lv_color_t borderColor;
          uint16_t str = 0; // offset into strings: text template or image path
          std::array<Condition, MaxConditions> conditions;
          lv_obj_t* obj = nullptr;
          std::array<lv_point_t, 2> points;
        };

        struct Font {
          lv_font_t* font;
          bool loaded; // true if lv_font_load()ed and must be freed
          uint16_t name;
        };

      private:
        static constexpr uint8_t MaxFaces = 16;
        static constexpr uint8_t MaxFileName = 32;
        static constexpr uint8_t MaxElements = 48;
        static constexpr uint8_t MaxFonts = 8;
        static constexpr uint8_t MaxLoadedFonts = 4;

        void ListFaces();
        void LoadSelection();
        void SaveSelection();
        void SwitchFace(int delta);
        void BuildFace();
        void ClearFace();
        void CreateObjects();
        void ShowMessage(const char* text, lv_color_t color);

        Controllers::DateTime& dateTimeController;
        Controllers::FS& filesystem;

        std::array<std::array<char, MaxFileName>, MaxFaces> faceFiles;
        uint8_t nFaces = 1; // index 0 is the built-in default
        uint8_t currentFace = 0;

        std::vector<Element> elements;
        std::vector<char> strings;
        std::array<Font, MaxFonts> fonts;
        uint8_t nFonts = 0;
        lv_color_t background;
        Resolution resolution = Resolution::Minute;
        char name[MaxFileName] = {};
        char error[48] = {};

        lv_obj_t* message = nullptr;
        uint32_t messageHideTick = 0;
        int64_t lastTimeKey = -1;
        uint16_t lastTouchX = 0;

        lv_task_t* taskRefresh;

        friend class CanvasParser;
      };
    }

    template <>
    struct WatchFaceTraits<WatchFace::Canvas> {
      static constexpr WatchFace watchFace = WatchFace::Canvas;
      static constexpr const char* name = "Canvas";

      static Screens::Screen* Create(AppControllers& controllers) {
        return new Screens::WatchFaceCanvas(controllers.dateTimeController, controllers.filesystem);
      };

      static bool IsAvailable(Pinetime::Controllers::FS& /*filesystem*/) {
        return true;
      }
    };
  }
}
