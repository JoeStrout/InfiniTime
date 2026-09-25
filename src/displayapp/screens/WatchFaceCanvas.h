#pragma once

#include <lvgl/lvgl.h>
#include <array>
#include <cstdint>
#include <vector>
#include "displayapp/screens/Screen.h"
#include "displayapp/apps/Apps.h"
#include "displayapp/Controllers.h"

namespace Pinetime {
  namespace Applications {
    namespace Screens {

      // A watch face whose contents are described by a text config file.
      // The built-in "Default" canvas is always available; more are loaded
      // from /canvas/*.cfg on the external filesystem.
      class WatchFaceCanvas : public Screen {
      public:
        explicit WatchFaceCanvas(AppControllers& controllers);
        ~WatchFaceCanvas() override;

        void Refresh() override;
        bool OnTouchEvent(TouchEvents event) override;
        bool OnTouchEvent(uint16_t x, uint16_t y) override;

        enum class ElementType : uint8_t { Text, Image, Rect, Line, Arc, Hand, Ticks, Battery, Bar };
        // Fields up to Clock12 can be used in conditions and {placeholders}; keep in sync with fieldNames
        enum class Field : uint8_t {
          Year,
          Month,
          Day,
          YearDay,
          DaysLeft,
          Week,
          WeekUS,
          WeekDay,
          Hour,
          Hour12,
          Minute,
          Second,
          Tenth,
          Battery,
          Charging,
          Power,
          Ble,
          BleOn,
          Notify,
          Steps,
          StepPct,
          HeartRate,
          HeartRateOn,
          Alarm,
          Weather,
          Temp,
          Clock12,
          WeatherIcon,
          Imperial,
          Count
        };
        using Values = std::array<int32_t, static_cast<size_t>(Field::Count)>;
        enum class Operator : uint8_t { Equal, Less, Greater, LessOrEqual, GreaterOrEqual };
        enum class Resolution : uint8_t { Minute, Second, Tenth };
        enum class ColorMode : uint8_t { Fixed, Battery, BatteryLow };
        enum class Relative : uint8_t { None, Left, Right };

        struct Condition {
          Field field;
          Operator op;
          int16_t value;
        };

        // Bump when the config format gains features, so faces can say "requires N"
        static constexpr int16_t FormatVersion = 1;
        static constexpr uint8_t MaxConditions = 6;
        static constexpr uint8_t FlagRounded = 1;
        static constexpr uint8_t FlagRecolor = 2;

        struct Element {
          ElementType type;
          uint8_t variant = 0;  // text alignment, which hand, or bar direction
          uint8_t textCase = 0; // 0 as written, 1 upper, 2 lower
          uint8_t font = 0;     // index into fonts
          uint8_t nConditions = 0;
          uint8_t flags = 0;
          uint8_t opa = LV_OPA_COVER;
          ColorMode colorMode = ColorMode::Fixed;
          Relative relative = Relative::None;
          Field valueField = Field::Battery; // bar
          int16_t x = 0;
          int16_t y = 0;
          int16_t w = 0; // line: x2
          int16_t h = 0; // line: y2
          int16_t width = 1;
          int16_t radius = -1;
          int16_t start = 0; // arc/ticks start angle, bar min
          int16_t end = 360; // arc/ticks end angle, bar max
          int16_t inner = INT16_MIN;
          int16_t count = 12;
          int16_t gap = 0;
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
        void ReadData();
        void Place(size_t index);

        AppControllers& controllers;
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
        char error[64] = {};

        lv_obj_t* message = nullptr;
        uint32_t messageHideTick = 0;
        Values current {};
        Values shown {};
        bool shownValid = false;
        int64_t lastPollSecond = -1;
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
        return new Screens::WatchFaceCanvas(controllers);
      };

      static bool IsAvailable(Pinetime::Controllers::FS& /*filesystem*/) {
        return true;
      }
    };
  }
}
