#include "displayapp/screens/WatchFaceCanvas.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <optional>
#include "components/alarm/AlarmController.h"
#include "components/battery/BatteryController.h"
#include "components/ble/BleController.h"
#include "components/ble/NotificationManager.h"
#include "components/ble/SimpleWeatherService.h"
#include "components/datetime/DateTimeController.h"
#include "components/fs/FS.h"
#include "components/heartrate/HeartRateController.h"
#include "components/motion/MotionController.h"
#include "components/settings/Settings.h"
#include "displayapp/InfiniTimeTheme.h"
#include "displayapp/screens/BatteryIcon.h"
#include "displayapp/screens/BleIcon.h"
#include "displayapp/screens/NotificationIcon.h"
#include "displayapp/screens/Symbols.h"
#include "displayapp/screens/WeatherSymbols.h"

using namespace Pinetime::Applications::Screens;

namespace {
  constexpr const char* canvasDir = "/canvas";
  constexpr const char* selectionFile = "/canvas/.current";

  // The built-in face, parsed by the same code as the files in /canvas.
  constexpr const char defaultFace[] = "name Default\n"
                                       "arc 4 4 232 232 color=#003060 width=3 if hour<12\n"
                                       "arc 4 4 232 232 color=#603000 width=3 if hour>=12\n"
                                       "hand second 0 0 240 240 color=#404040 radius=112 width=2\n"
                                       "text 120 52 color=#999999 align=center \"{dddd}\"\n"
                                       "text 120 78 font=jetbrains_mono_76 align=center \"{HH}:{mm}\"\n"
                                       "text 120 160 color=#999999 align=center \"{D} {MMM} {YYYY}\"\n";

  struct BuiltinFont {
    const char* name;
    lv_font_t* font;
  };

  const BuiltinFont builtinFonts[] = {
    {"jetbrains_mono_bold_20", &jetbrains_mono_bold_20},
    {"jetbrains_mono_42", &jetbrains_mono_42},
    {"jetbrains_mono_76", &jetbrains_mono_76},
    {"jetbrains_mono_extrabold_compressed", &jetbrains_mono_extrabold_compressed},
    {"open_sans_light", &open_sans_light},
    {"lv_font_sys_48", &lv_font_sys_48},
    {"fontawesome_weathericons", &fontawesome_weathericons},
  };

  // Indexed by WatchFaceCanvas::Field, up to Clock12
  constexpr const char* fieldNames[] = {"year",   "month",  "day",     "yday",  "daysLeft", "week",     "weekUS",  "dow",  "hour",
                                        "hour12", "minute", "second",  "tenth", "battery",  "charging", "power",   "ble",  "bleOn",
                                        "notify", "steps",  "stepPct", "hr",    "hrOn",     "alarm",    "weather", "temp", "clock12"};

  static_assert(std::size(fieldNames) == static_cast<size_t>(WatchFaceCanvas::Field::Clock12) + 1);

  constexpr const char* handNames[] = {"hour", "minute", "second", "steps"};
  constexpr const char* barDirections[] = {"up", "down", "left", "right"};

  template <size_t N>
  int IndexOf(const char* const (&names)[N], const char* name) {
    for (size_t i = 0; i < N; i++) {
      if (strcmp(names[i], name) == 0) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  bool EndsWith(const char* s, const char* suffix) {
    size_t sLen = strlen(s);
    size_t suffixLen = strlen(suffix);
    return sLen >= suffixLen && strcmp(s + sLen - suffixLen, suffix) == 0;
  }

  bool ParseInt(const char* s, int16_t& value) {
    char* end;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < INT16_MIN || v > INT16_MAX) {
      return false;
    }
    value = static_cast<int16_t>(v);
    return true;
  }

  bool ParseColor(const char* s, lv_color_t& color) {
    if (s[0] != '#' || strlen(s) != 7) {
      return false;
    }
    char* end;
    unsigned long v = strtoul(s + 1, &end, 16);
    if (*end != '\0') {
      return false;
    }
    color = lv_color_hex(v);
    return true;
  }

  bool ParseOnOff(const char* s, bool& value) {
    value = strcmp(s, "on") == 0;
    return value || strcmp(s, "off") == 0;
  }

  // Returns the next whitespace-separated token, or nullptr at the end of the line.
  // A token may be a "quoted string"; the quotes are removed and quoted is set.
  char* NextToken(char*& p, bool& quoted) {
    while (*p == ' ' || *p == '\t') {
      p++;
    }
    if (*p == '\0') {
      return nullptr;
    }
    quoted = (*p == '"');
    char terminator = quoted ? '"' : ' ';
    if (quoted) {
      p++;
    }
    char* token = p;
    while (*p != '\0' && *p != terminator && (quoted || *p != '\t')) {
      p++;
    }
    if (*p != '\0') {
      *p++ = '\0';
    }
    return token;
  }

  // Expands {frame}, {frame:N} (space padded) or {frame:0N} (zero padded) in an image path template.
  void FormatFramePath(const char* in, int frame, char* out, size_t size) {
    size_t len = 0;
    while (*in != '\0' && len < size - 1) {
      const char* close = (strncmp(in, "{frame", 6) == 0) ? strchr(in, '}') : nullptr;
      if (close == nullptr) {
        out[len++] = *in++;
        continue;
      }
      int width = 1;
      bool zeroPad = false;
      if (in[6] == ':') {
        zeroPad = (in[7] == '0');
        width = atoi(in + 7);
      }
      char number[8];
      snprintf(number, sizeof(number), zeroPad ? "%0*d" : "%*d", width, frame);
      for (const char* n = number; *n != '\0' && len < size - 1; n++) {
        out[len++] = *n;
      }
      in = close + 1;
    }
    out[len] = '\0';
  }

  bool IsLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  }

  bool IsLongIsoYear(int year) {
    auto p = [](int y) {
      return (y + y / 4 - y / 100 + y / 400) % 7;
    };
    return p(year) == 4 || p(year - 1) == 3;
  }

  int IsoWeek(int year, int yearDay, int weekDay) {
    int week = (yearDay - weekDay + 10) / 7;
    if (week < 1) {
      return IsLongIsoYear(year - 1) ? 53 : 52;
    }
    if (week == 53 && !IsLongIsoYear(year)) {
      return 1;
    }
    return week;
  }
}

namespace Pinetime {
  namespace Applications {
    namespace Screens {
      // Reads a canvas description, from flash or from a file, into a WatchFaceCanvas.
      class CanvasParser {
      public:
        CanvasParser(WatchFaceCanvas& face, Controllers::FS& filesystem, const char* sourceName)
          : face {face}, filesystem {filesystem}, sourceName {sourceName} {
        }

        bool ParseText(const char* text) {
          source = text;
          return Parse();
        }

        bool ParseFile(const char* path) {
          if (filesystem.FileOpen(&file, path, LFS_O_RDONLY) < 0) {
            return Fail("can't open");
          }
          reading = true;
          bool ok = Parse();
          filesystem.FileClose(&file);
          return ok;
        }

      private:
        using Element = WatchFaceCanvas::Element;
        using ElementType = WatchFaceCanvas::ElementType;
        using Field = WatchFaceCanvas::Field;
        using Resolution = WatchFaceCanvas::Resolution;
        using ColorMode = WatchFaceCanvas::ColorMode;
        using Relative = WatchFaceCanvas::Relative;

        static constexpr size_t MaxLine = 128;

        bool Parse() {
          char line[MaxLine];
          bool tooLong = false;
          while (NextLine(line, sizeof(line), tooLong)) {
            lineNumber++;
            if (tooLong) {
              return Fail("line too long");
            }
            if (!ParseLine(line)) {
              return false;
            }
          }
          return true;
        }

        bool ReadChar(char& c) {
          if (!reading) {
            if (*source == '\0') {
              return false;
            }
            c = *source++;
            return true;
          }
          if (chunkPos == chunkLen) {
            int n = filesystem.FileRead(&file, reinterpret_cast<uint8_t*>(chunk), sizeof(chunk));
            if (n <= 0) {
              return false;
            }
            chunkLen = n;
            chunkPos = 0;
          }
          c = chunk[chunkPos++];
          return true;
        }

        bool NextLine(char* buffer, size_t size, bool& tooLong) {
          size_t len = 0;
          bool any = false;
          char c;
          while (ReadChar(c)) {
            any = true;
            if (c == '\n') {
              break;
            }
            if (c == '\r') {
              continue;
            }
            if (len < size - 1) {
              buffer[len++] = c;
            } else {
              tooLong = true;
            }
          }
          buffer[len] = '\0';
          return any;
        }

        bool Fail(const char* message) {
          snprintf(face.error, sizeof(face.error), "%s:%d %s", sourceName, lineNumber, message);
          return false;
        }

        uint16_t AddString(const char* prefix, const char* s) {
          auto offset = static_cast<uint16_t>(face.strings.size());
          face.strings.insert(face.strings.end(), prefix, prefix + strlen(prefix));
          face.strings.insert(face.strings.end(), s, s + strlen(s) + 1);
          return offset;
        }

        void NeedResolution(Resolution resolution) {
          face.resolution = std::max(face.resolution, resolution);
        }

        void NeedResolutionFor(Field field) {
          if (field == Field::Tenth) {
            NeedResolution(Resolution::Tenth);
          } else if (field == Field::Second) {
            NeedResolution(Resolution::Second);
          }
        }

        // Finds the time resolution a text template needs from the placeholders it uses.
        void ScanPlaceholders(const char* text) {
          while ((text = strchr(text, '{')) != nullptr) {
            text++;
            size_t len = strcspn(text, ":}");
            if (strncmp(text, "ss", len) == 0 && len == 2) {
              NeedResolution(Resolution::Second);
            } else if (strncmp(text, "t", len) == 0 && len == 1) {
              NeedResolution(Resolution::Tenth);
            } else if (strncmp(text, "second", len) == 0 && len == 6) {
              NeedResolution(Resolution::Second);
            } else if (strncmp(text, "tenth", len) == 0 && len == 5) {
              NeedResolution(Resolution::Tenth);
            }
          }
        }

        bool ParseLine(char* line) {
          char* p = line;
          while (*p == ' ' || *p == '\t') {
            p++;
          }
          // Whole-line comments only, so '#' can still start a color elsewhere
          if (*p == '#') {
            return true;
          }
          bool quoted;
          char* command = NextToken(p, quoted);
          if (command == nullptr) {
            return true;
          }

          if (strcmp(command, "requires") == 0) {
            char* value = NextToken(p, quoted);
            int16_t version;
            if (value == nullptr || !ParseInt(value, version) || version < 1) {
              return Fail("bad version");
            }
            if (version > WatchFaceCanvas::FormatVersion) {
              char message[32];
              snprintf(message, sizeof(message), "needs format v%d (have v%d)", version, WatchFaceCanvas::FormatVersion);
              return Fail(message);
            }
            return true;
          }

          if (strcmp(command, "name") == 0) {
            // The name is the rest of the line, so it may contain spaces
            while (*p == ' ' || *p == '\t') {
              p++;
            }
            char* value = (*p == '"') ? NextToken(p, quoted) : p;
            if (value == nullptr || *value == '\0') {
              return Fail("missing name");
            }
            snprintf(face.name, sizeof(face.name), "%s", value);
            return true;
          }
          if (strcmp(command, "bg") == 0) {
            char* value = NextToken(p, quoted);
            if (value == nullptr || !ParseColor(value, face.background)) {
              return Fail("bad color");
            }
            return true;
          }

          Element e {};
          e.color = LV_COLOR_WHITE;
          e.borderColor = LV_COLOR_WHITE;
          int nNumbers = 4;
          if (strcmp(command, "text") == 0) {
            e.type = ElementType::Text;
            nNumbers = 2;
          } else if (strcmp(command, "image") == 0) {
            e.type = ElementType::Image;
            nNumbers = 2;
          } else if (strcmp(command, "rect") == 0) {
            e.type = ElementType::Rect;
            e.width = 0;
            e.radius = 0;
          } else if (strcmp(command, "line") == 0) {
            e.type = ElementType::Line;
          } else if (strcmp(command, "arc") == 0) {
            e.type = ElementType::Arc;
            e.width = 2;
          } else if (strcmp(command, "hand") == 0) {
            e.type = ElementType::Hand;
            e.width = 3;
            e.count = 60; // frames, for image hands
            e.flags |= WatchFaceCanvas::FlagRounded;
            char* which = NextToken(p, quoted);
            int index = (which == nullptr) ? -1 : IndexOf(handNames, which);
            if (index < 0) {
              return Fail("bad hand");
            }
            e.variant = static_cast<uint8_t>(index);
            if (index == 1 || index == 2) {
              NeedResolution(Resolution::Second);
            }
          } else if (strcmp(command, "ticks") == 0) {
            e.type = ElementType::Ticks;
          } else if (strcmp(command, "battery") == 0) {
            e.type = ElementType::Battery;
            e.colorMode = ColorMode::BatteryLow;
            nNumbers = 2;
          } else if (strcmp(command, "bar") == 0) {
            e.type = ElementType::Bar;
            e.start = 0;
            e.end = 100;
          } else {
            return Fail("unknown element");
          }

          if (face.elements.size() >= WatchFaceCanvas::MaxElements) {
            return Fail("too many elements");
          }

          int16_t* numbers[] = {&e.x, &e.y, &e.w, &e.h};
          for (int i = 0; i < nNumbers; i++) {
            char* token = NextToken(p, quoted);
            if (token == nullptr || !ParseInt(token, *numbers[i])) {
              return Fail("expected number");
            }
          }

          bool needsString = (e.type == ElementType::Text || e.type == ElementType::Image);
          bool haveString = false;
          char* token;
          while ((token = NextToken(p, quoted)) != nullptr) {
            if (!quoted && strcmp(token, "if") == 0) {
              if (!ParseConditions(p, e)) {
                return false;
              }
              break;
            }
            char* equals = quoted ? nullptr : strchr(token, '=');
            if (equals != nullptr) {
              *equals = '\0';
              if (!ParseAttribute(token, equals + 1, e)) {
                return false;
              }
            } else if (needsString && !haveString) {
              if (e.type == ElementType::Image) {
                lfs_info info;
                if (filesystem.Stat(token, &info) < 0) {
                  return Fail("missing image");
                }
                e.str = AddString("F:", token);
              } else {
                e.str = AddString("", token);
                ScanPlaceholders(token);
              }
              haveString = true;
            } else {
              return Fail("unexpected token");
            }
          }
          if (needsString && !haveString) {
            return Fail(e.type == ElementType::Image ? "missing path" : "missing text");
          }
          if (e.relative != Relative::None && face.elements.empty()) {
            return Fail("nothing before");
          }

          face.elements.push_back(e);
          return true;
        }

        bool ParseAttribute(const char* key, const char* value, Element& e) {
          bool ok = true;
          if (strcmp(key, "color") == 0) {
            if (strcmp(value, "battery") == 0) {
              e.colorMode = ColorMode::Battery;
            } else if (strcmp(value, "batteryLow") == 0) {
              e.colorMode = ColorMode::BatteryLow;
            } else {
              e.colorMode = ColorMode::Fixed;
              ok = ParseColor(value, e.color);
            }
          } else if (strcmp(key, "border_color") == 0) {
            ok = ParseColor(value, e.borderColor);
          } else if (strcmp(key, "width") == 0 || strcmp(key, "border") == 0) {
            ok = ParseInt(value, e.width);
          } else if (strcmp(key, "radius") == 0 || strcmp(key, "outer") == 0) {
            ok = ParseInt(value, e.radius);
          } else if (strcmp(key, "inner") == 0) {
            ok = ParseInt(value, e.inner);
          } else if (strcmp(key, "image") == 0) {
            // Only the first frame is checked; the rest are assumed to follow the same pattern
            char path[64];
            FormatFramePath(value, 0, path, sizeof(path));
            lfs_info info;
            if (filesystem.Stat(path, &info) < 0) {
              return Fail("missing image");
            }
            e.str = AddString("F:", value);
            e.flags |= WatchFaceCanvas::FlagImage;
          } else if (strcmp(key, "frames") == 0) {
            ok = ParseInt(value, e.count) && e.count >= 1;
          } else if (strcmp(key, "count") == 0) {
            ok = ParseInt(value, e.count) && e.count >= 2;
          } else if (strcmp(key, "start") == 0 || strcmp(key, "min") == 0) {
            ok = ParseInt(value, e.start);
          } else if (strcmp(key, "end") == 0 || strcmp(key, "max") == 0) {
            ok = ParseInt(value, e.end);
          } else if (strcmp(key, "opa") == 0) {
            int16_t opa;
            ok = ParseInt(value, opa) && opa >= 0 && opa <= 255;
            e.opa = static_cast<uint8_t>(opa);
          } else if (strcmp(key, "left") == 0 || strcmp(key, "right") == 0) {
            e.relative = (key[0] == 'l') ? Relative::Left : Relative::Right;
            ok = ParseInt(value, e.gap);
          } else if (strcmp(key, "rounded") == 0 || strcmp(key, "recolor") == 0) {
            bool on;
            ok = ParseOnOff(value, on);
            uint8_t flag = (key[1] == 'o') ? WatchFaceCanvas::FlagRounded : WatchFaceCanvas::FlagRecolor;
            e.flags = on ? (e.flags | flag) : (e.flags & ~flag);
          } else if (strcmp(key, "case") == 0) {
            if (strcmp(value, "upper") == 0) {
              e.textCase = 1;
            } else if (strcmp(value, "lower") == 0) {
              e.textCase = 2;
            } else {
              ok = false;
            }
          } else if (strcmp(key, "align") == 0) {
            if (strcmp(value, "left") == 0) {
              e.variant = 0;
            } else if (strcmp(value, "center") == 0) {
              e.variant = 1;
            } else if (strcmp(value, "right") == 0) {
              e.variant = 2;
            } else {
              ok = false;
            }
          } else if (strcmp(key, "dir") == 0) {
            int index = IndexOf(barDirections, value);
            ok = index >= 0;
            e.variant = static_cast<uint8_t>(std::max(index, 0));
          } else if (strcmp(key, "value") == 0) {
            int index = IndexOf(fieldNames, value);
            ok = index >= 0;
            e.valueField = static_cast<Field>(std::max(index, 0));
            NeedResolutionFor(e.valueField);
          } else if (strcmp(key, "font") == 0) {
            return ParseFont(value, e);
          } else {
            return Fail("unknown attribute");
          }
          return ok || Fail("bad value");
        }

        bool ParseFont(const char* value, Element& e) {
          lv_font_t* font = nullptr;
          for (const auto& builtin : builtinFonts) {
            if (strcmp(builtin.name, value) == 0) {
              font = builtin.font;
            }
          }
          for (uint8_t i = 0; i < face.nFonts; i++) {
            const auto& f = face.fonts[i];
            if ((font != nullptr && f.font == font) || (font == nullptr && f.loaded && strcmp(&face.strings[f.name] + 2, value) == 0)) {
              e.font = i;
              return true;
            }
          }
          if (face.nFonts >= WatchFaceCanvas::MaxFonts) {
            return Fail("too many fonts");
          }
          uint16_t name = 0;
          bool loaded = false;
          if (font == nullptr) {
            if (value[0] != '/') {
              return Fail("unknown font");
            }
            auto nLoaded = std::count_if(face.fonts.begin(), face.fonts.begin() + face.nFonts, [](const WatchFaceCanvas::Font& f) {
              return f.loaded;
            });
            if (nLoaded >= WatchFaceCanvas::MaxLoadedFonts) {
              return Fail("too many fonts");
            }
            name = AddString("F:", value);
            font = lv_font_load(&face.strings[name]);
            if (font == nullptr) {
              return Fail("can't load font");
            }
            loaded = true;
          }
          face.fonts[face.nFonts] = {font, loaded, name};
          e.font = face.nFonts++;
          return true;
        }

        bool ParseConditions(char*& p, Element& e) {
          bool quoted;
          char* token;
          while ((token = NextToken(p, quoted)) != nullptr) {
            if (e.nConditions >= WatchFaceCanvas::MaxConditions) {
              return Fail("too many conditions");
            }
            char* op = strpbrk(token, "=<>");
            if (op == nullptr) {
              return Fail("bad condition");
            }
            WatchFaceCanvas::Condition& c = e.conditions[e.nConditions];
            char* value = op + 1;
            if (op[0] == '=') {
              c.op = WatchFaceCanvas::Operator::Equal;
            } else if (op[1] == '=') {
              c.op = (op[0] == '<') ? WatchFaceCanvas::Operator::LessOrEqual : WatchFaceCanvas::Operator::GreaterOrEqual;
              value++;
            } else {
              c.op = (op[0] == '<') ? WatchFaceCanvas::Operator::Less : WatchFaceCanvas::Operator::Greater;
            }
            *op = '\0';
            int index = IndexOf(fieldNames, token);
            if (index < 0) {
              return Fail("unknown field");
            }
            c.field = static_cast<Field>(index);
            if (!ParseInt(value, c.value)) {
              return Fail("bad condition");
            }
            NeedResolutionFor(c.field);
            e.nConditions++;
          }
          return true;
        }

        WatchFaceCanvas& face;
        Controllers::FS& filesystem;
        const char* sourceName;
        const char* source = nullptr;
        lfs_file_t file = {};
        bool reading = false;
        char chunk[32];
        int chunkLen = 0;
        int chunkPos = 0;
        int lineNumber = 0;
      };
    }
  }
}

namespace {
  using Field = WatchFaceCanvas::Field;
  using Values = WatchFaceCanvas::Values;
  using ElementType = WatchFaceCanvas::ElementType;
  using Icons = Pinetime::Controllers::SimpleWeatherService::Icons;

  int32_t Get(const Values& values, Field field) {
    return values[static_cast<size_t>(field)];
  }

  bool IsVisible(const WatchFaceCanvas::Element& e, const Values& values) {
    for (uint8_t i = 0; i < e.nConditions; i++) {
      const auto& c = e.conditions[i];
      int32_t v = Get(values, c.field);
      bool ok = false;
      switch (c.op) {
        case WatchFaceCanvas::Operator::Equal:
          ok = v == c.value;
          break;
        case WatchFaceCanvas::Operator::Less:
          ok = v < c.value;
          break;
        case WatchFaceCanvas::Operator::Greater:
          ok = v > c.value;
          break;
        case WatchFaceCanvas::Operator::LessOrEqual:
          ok = v <= c.value;
          break;
        case WatchFaceCanvas::Operator::GreaterOrEqual:
          ok = v >= c.value;
          break;
      }
      if (!ok) {
        return false;
      }
    }
    return true;
  }

  // Expands {placeholders} and \n in a text template.
  void FormatText(const char* in, char* out, size_t size, const Values& values) {
    using Pinetime::Controllers::DateTime;
    size_t len = 0;
    auto append = [&](const char* s) {
      while (*s != '\0' && len < size - 1) {
        out[len++] = *s++;
      }
    };
    auto appendNumber = [&](int32_t value, int width, bool zeroPad) {
      char number[12];
      snprintf(number, sizeof(number), zeroPad ? "%0*ld" : "%*ld", width, static_cast<long>(value));
      append(number);
    };
    auto appendIf = [&](Field field, const char* s) {
      append(Get(values, field) != 0 ? s : "");
    };

    int hour = Get(values, Field::Hour);
    bool haveWeather = Get(values, Field::Weather) != 0;
    auto weatherIcon = static_cast<Icons>(Get(values, Field::WeatherIcon) & 0xff);
    while (*in != '\0' && len < size - 1) {
      if (in[0] == '\\' && in[1] == 'n') {
        out[len++] = '\n';
        in += 2;
        continue;
      }
      const char* close = (*in == '{') ? strchr(in, '}') : nullptr;
      if (close == nullptr || close - in > 15) {
        out[len++] = *in++;
        continue;
      }
      char key[16];
      size_t keyLen = close - in - 1;
      memcpy(key, in + 1, keyLen);
      key[keyLen] = '\0';

      if (strcmp(key, "YYYY") == 0) {
        appendNumber(Get(values, Field::Year), 4, true);
      } else if (strcmp(key, "YY") == 0) {
        appendNumber(Get(values, Field::Year) % 100, 2, true);
      } else if (strcmp(key, "M") == 0 || strcmp(key, "MM") == 0) {
        appendNumber(Get(values, Field::Month), static_cast<int>(keyLen), true);
      } else if (strcmp(key, "MMM") == 0) {
        append(DateTime::MonthShortToStringLow(static_cast<DateTime::Months>(Get(values, Field::Month))));
      } else if (strcmp(key, "D") == 0 || strcmp(key, "DD") == 0) {
        appendNumber(Get(values, Field::Day), static_cast<int>(keyLen), true);
      } else if (strcmp(key, "ddd") == 0) {
        append(DateTime::DayOfWeekShortToStringLow(static_cast<DateTime::Days>(Get(values, Field::WeekDay))));
      } else if (strcmp(key, "dddd") == 0) {
        append(DateTime::DayOfWeekToStringLow(static_cast<DateTime::Days>(Get(values, Field::WeekDay))));
      } else if (strcmp(key, "H") == 0 || strcmp(key, "HH") == 0) {
        appendNumber(hour, static_cast<int>(keyLen), true);
      } else if (strcmp(key, "h") == 0 || strcmp(key, "hh") == 0) {
        appendNumber(Get(values, Field::Hour12), static_cast<int>(keyLen), true);
      } else if (strcmp(key, "mm") == 0) {
        appendNumber(Get(values, Field::Minute), 2, true);
      } else if (strcmp(key, "ss") == 0) {
        appendNumber(Get(values, Field::Second), 2, true);
      } else if (strcmp(key, "A") == 0) {
        append(hour < 12 ? "AM" : "PM");
      } else if (strcmp(key, "t") == 0) {
        appendNumber(Get(values, Field::Tenth), 1, true);
      } else if (strcmp(key, "tempU") == 0) {
        append(Get(values, Field::Imperial) != 0 ? "°F" : "°C");
      } else if (strcmp(key, "wIcon") == 0) {
        append(haveWeather ? Symbols::GetSymbol(weatherIcon, Get(values, Field::WeatherIcon) > 0xff) : "");
      } else if (strcmp(key, "wText") == 0) {
        append(haveWeather ? Symbols::GetSimpleCondition(weatherIcon) : "");
      } else if (strcmp(key, "ble") == 0) {
        append(BleIcon::GetIcon(Get(values, Field::Ble) != 0));
      } else if (strcmp(key, "plug") == 0) {
        appendIf(Field::Power, Symbols::plug);
      } else if (strcmp(key, "notify") == 0) {
        append(NotificationIcon::GetIcon(Get(values, Field::Notify) != 0));
      } else if (strcmp(key, "alarm") == 0) {
        appendIf(Field::Alarm, Symbols::bell);
      } else if (strcmp(key, "heart") == 0) {
        append(Symbols::heartBeat);
      } else if (strcmp(key, "shoe") == 0) {
        append(Symbols::shoe);
      } else {
        // {field} or {field:N} (space padded) or {field:0N} (zero padded)
        char* format = strchr(key, ':');
        int width = 1;
        bool zeroPad = false;
        if (format != nullptr) {
          *format++ = '\0';
          zeroPad = (*format == '0');
          width = atoi(format);
        }
        int index = IndexOf(fieldNames, key);
        if (index < 0) {
          out[len++] = *in++;
          continue;
        }
        appendNumber(Get(values, static_cast<Field>(index)), width, zeroPad);
      }
      in = close + 1;
    }
    out[len] = '\0';
  }

  // Changes case of ASCII letters only, so UTF-8 symbols are left intact.
  void ApplyCase(char* text, uint8_t textCase) {
    for (; *text != '\0'; text++) {
      if (textCase == 1 && *text >= 'a' && *text <= 'z') {
        *text -= 'a' - 'A';
      } else if (textCase == 2 && *text >= 'A' && *text <= 'Z') {
        *text += 'a' - 'A';
      }
    }
  }

  void UpdateHand(WatchFaceCanvas::Element& e, const Values& values, const char* strings) {
    int32_t angle;
    switch (e.variant) {
      case 0:
        angle = (Get(values, Field::Hour) % 12) * 30 + Get(values, Field::Minute) / 2;
        break;
      case 1:
        angle = Get(values, Field::Minute) * 6 + Get(values, Field::Second) / 10;
        break;
      case 2:
        angle = Get(values, Field::Second) * 6;
        break;
      default:
        angle = (Get(values, Field::StepPct) % 100) * 36 / 10;
        break;
    }
    constexpr int32_t trigScale = INT16_MAX; // = _lv_trigo_sin(90)
    int32_t sin = _lv_trigo_sin(static_cast<int16_t>(angle));
    int32_t cos = _lv_trigo_sin(static_cast<int16_t>(angle + 90));
    int32_t length = (e.radius >= 0) ? e.radius : std::min(e.w, e.h) / 2;
    int32_t start = (e.inner == INT16_MIN) ? 0 : e.inner;
    lv_coord_t cx = e.x + e.w / 2;
    lv_coord_t cy = e.y + e.h / 2;
    lv_point_t tip {static_cast<lv_coord_t>(cx + length * sin / trigScale), static_cast<lv_coord_t>(cy - length * cos / trigScale)};
    if ((e.flags & WatchFaceCanvas::FlagImage) != 0) {
      // Show the frame nearest the angle, centred on the tip; points[0].x remembers the frame shown
      int32_t frame = (angle * e.count + 180) / 360 % e.count;
      if (frame != e.points[0].x) {
        char path[66];
        FormatFramePath(&strings[e.str], static_cast<int>(frame), path, sizeof(path));
        lv_img_set_src(e.obj, path);
        e.points[0].x = static_cast<lv_coord_t>(frame);
      }
      lv_obj_set_pos(e.obj, tip.x - lv_obj_get_width(e.obj) / 2, tip.y - lv_obj_get_height(e.obj) / 2);
      return;
    }
    if (tip.x != e.points[1].x || tip.y != e.points[1].y) {
      e.points[0] = {static_cast<lv_coord_t>(cx + start * sin / trigScale), static_cast<lv_coord_t>(cy - start * cos / trigScale)};
      e.points[1] = tip;
      lv_line_set_points(e.obj, e.points.data(), 2);
    }
  }

  void UpdateBar(const WatchFaceCanvas::Element& e, const Values& values) {
    bool vertical = e.variant < 2;
    int32_t extent = vertical ? e.h : e.w;
    int32_t range = e.end - e.start;
    int32_t length = (range == 0) ? 0 : (Get(values, e.valueField) - e.start) * extent / range;
    length = std::clamp<int32_t>(length, 0, extent);
    lv_coord_t x = e.x;
    lv_coord_t y = e.y;
    lv_coord_t w = vertical ? e.w : length;
    lv_coord_t h = vertical ? length : e.h;
    if (e.variant == 0) {
      y += e.h - length;
    } else if (e.variant == 2) {
      x += e.w - length;
    }
    lv_obj_set_pos(e.obj, x, y);
    lv_obj_set_size(e.obj, w, h);
  }

  void UpdateBattery(const WatchFaceCanvas::Element& e, const Values& values) {
    lv_obj_t* juice = lv_obj_get_child(e.obj, nullptr);
    lv_coord_t height = Get(values, Field::Battery) * 14 / 100;
    if (lv_obj_get_height(juice) != height) {
      lv_obj_set_height(juice, height);
      lv_obj_realign(juice);
    }
  }

  lv_color_t BatteryColor(WatchFaceCanvas::ColorMode mode, int32_t percent) {
    if (mode == WatchFaceCanvas::ColorMode::Battery) {
      return BatteryIcon::ColorFromPercentage(percent);
    }
    return (percent > 15) ? LV_COLOR_WHITE : ((percent > 5) ? LV_COLOR_ORANGE : Colors::deepOrange);
  }

  void SetColor(const WatchFaceCanvas::Element& e) {
    switch (e.type) {
      case ElementType::Text:
        lv_obj_set_style_local_text_color(e.obj, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, e.color);
        break;
      case ElementType::Rect:
      case ElementType::Bar:
        lv_obj_set_style_local_bg_color(e.obj, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, e.color);
        break;
      case ElementType::Line:
      case ElementType::Hand:
        lv_obj_set_style_local_line_color(e.obj, LV_LINE_PART_MAIN, LV_STATE_DEFAULT, e.color);
        break;
      case ElementType::Arc:
        lv_obj_set_style_local_line_color(e.obj, LV_ARC_PART_BG, LV_STATE_DEFAULT, e.color);
        break;
      case ElementType::Ticks:
        lv_obj_set_style_local_line_color(e.obj, LV_LINEMETER_PART_MAIN, LV_STATE_DEFAULT, e.color);
        lv_obj_set_style_local_scale_grad_color(e.obj, LV_LINEMETER_PART_MAIN, LV_STATE_DEFAULT, e.color);
        lv_obj_set_style_local_scale_end_color(e.obj, LV_LINEMETER_PART_MAIN, LV_STATE_DEFAULT, e.color);
        break;
      case ElementType::Battery:
        lv_obj_set_style_local_image_recolor(e.obj, LV_IMG_PART_MAIN, LV_STATE_DEFAULT, e.color);
        lv_obj_set_style_local_bg_color(lv_obj_get_child(e.obj, nullptr), LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, e.color);
        break;
      case ElementType::Image:
        break;
    }
  }

  void SetOpacity(const WatchFaceCanvas::Element& e) {
    switch (e.type) {
      case ElementType::Text:
        lv_obj_set_style_local_text_opa(e.obj, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, e.opa);
        break;
      case ElementType::Image:
      case ElementType::Battery:
        lv_obj_set_style_local_image_opa(e.obj, LV_IMG_PART_MAIN, LV_STATE_DEFAULT, e.opa);
        break;
      case ElementType::Rect:
      case ElementType::Bar:
        lv_obj_set_style_local_bg_opa(e.obj, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, e.opa);
        lv_obj_set_style_local_border_opa(e.obj, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, e.opa);
        break;
      case ElementType::Hand:
        if ((e.flags & WatchFaceCanvas::FlagImage) != 0) {
          lv_obj_set_style_local_image_opa(e.obj, LV_IMG_PART_MAIN, LV_STATE_DEFAULT, e.opa);
          break;
        }
        [[fallthrough]];
      case ElementType::Line:
      case ElementType::Ticks:
        lv_obj_set_style_local_line_opa(e.obj, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, e.opa);
        break;
      case ElementType::Arc:
        lv_obj_set_style_local_line_opa(e.obj, LV_ARC_PART_BG, LV_STATE_DEFAULT, e.opa);
        break;
    }
  }

  bool IsCollapsed(const WatchFaceCanvas::Element& e) {
    return lv_obj_get_hidden(e.obj) || lv_obj_get_width(e.obj) == 0;
  }
}

WatchFaceCanvas::WatchFaceCanvas(AppControllers& controllers) : controllers {controllers}, filesystem {controllers.filesystem} {
  ListFaces();
  LoadSelection();
  BuildFace();
  taskRefresh = lv_task_create(RefreshTaskCallback, LV_DISP_DEF_REFR_PERIOD, LV_TASK_PRIO_MID, this);
}

WatchFaceCanvas::~WatchFaceCanvas() {
  lv_task_del(taskRefresh);
  ClearFace();
}

void WatchFaceCanvas::ListFaces() {
  lfs_dir_t dir;
  if (filesystem.DirOpen(canvasDir, &dir) < 0) {
    return;
  }
  lfs_info info;
  while (filesystem.DirRead(&dir, &info) > 0 && nFaces < MaxFaces) {
    if (info.type != LFS_TYPE_REG || info.name[0] == '.' || !EndsWith(info.name, ".cfg") || strlen(info.name) >= MaxFileName) {
      continue;
    }
    // Insertion sort, so the order doesn't depend on the filesystem
    uint8_t i = nFaces++;
    while (i > 1 && strcmp(faceFiles[i - 1].data(), info.name) > 0) {
      faceFiles[i] = faceFiles[i - 1];
      i--;
    }
    strcpy(faceFiles[i].data(), info.name);
  }
  filesystem.DirClose(&dir);
}

void WatchFaceCanvas::LoadSelection() {
  lfs_file_t file;
  if (filesystem.FileOpen(&file, selectionFile, LFS_O_RDONLY) < 0) {
    return;
  }
  char selected[MaxFileName] = {};
  filesystem.FileRead(&file, reinterpret_cast<uint8_t*>(selected), sizeof(selected) - 1);
  filesystem.FileClose(&file);
  for (uint8_t i = 1; i < nFaces; i++) {
    if (strcmp(faceFiles[i].data(), selected) == 0) {
      currentFace = i;
    }
  }
}

void WatchFaceCanvas::SaveSelection() {
  lfs_file_t file;
  if (filesystem.FileOpen(&file, selectionFile, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) < 0) {
    return;
  }
  if (currentFace != 0) {
    filesystem.FileWrite(&file, reinterpret_cast<const uint8_t*>(faceFiles[currentFace].data()), strlen(faceFiles[currentFace].data()));
  }
  filesystem.FileClose(&file);
}

void WatchFaceCanvas::SwitchFace(int delta) {
  currentFace = (currentFace + nFaces + delta) % nFaces;
  SaveSelection();
  BuildFace();
  if (error[0] == '\0') {
    ShowMessage(name, LV_COLOR_WHITE);
    messageHideTick = lv_tick_get() + 1500;
  }
}

void WatchFaceCanvas::ClearFace() {
  lv_obj_clean(lv_scr_act());
  message = nullptr;
  for (uint8_t i = 0; i < nFonts; i++) {
    if (fonts[i].loaded) {
      lv_font_free(fonts[i].font);
    }
  }
  elements.clear();
  strings.clear();
  fonts[0] = {&jetbrains_mono_bold_20, false, 0};
  nFonts = 1;
  background = LV_COLOR_BLACK;
  resolution = Resolution::Minute;
}

void WatchFaceCanvas::BuildFace() {
  error[0] = '\0';
  ClearFace();
  bool ok;
  if (currentFace == 0) {
    ok = CanvasParser(*this, filesystem, "Default").ParseText(defaultFace);
  } else {
    const char* file = faceFiles[currentFace].data();
    char path[sizeof("/canvas/") + MaxFileName];
    snprintf(path, sizeof(path), "%s/%s", canvasDir, file);
    snprintf(name, sizeof(name), "%.*s", static_cast<int>(strlen(file) - 4), file);
    ok = CanvasParser(*this, filesystem, file).ParseFile(path);
  }
  if (!ok) {
    ClearFace();
    CanvasParser(*this, filesystem, "Default").ParseText(defaultFace);
  }
  elements.shrink_to_fit();
  strings.shrink_to_fit();
  CreateObjects();
  if (error[0] != '\0') {
    ShowMessage(error, LV_COLOR_RED);
    messageHideTick = 0;
  }
  shownValid = false;
  Refresh();
}

void WatchFaceCanvas::CreateObjects() {
  lv_obj_t* bg = lv_obj_create(lv_scr_act(), nullptr);
  lv_obj_set_size(bg, LV_HOR_RES, LV_VER_RES);
  lv_obj_set_style_local_bg_color(bg, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, background);
  lv_obj_set_style_local_border_width(bg, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, 0);
  lv_obj_set_style_local_radius(bg, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, 0);

  for (size_t i = 0; i < elements.size(); i++) {
    Element& e = elements[i];
    switch (e.type) {
      case ElementType::Text:
        e.obj = lv_label_create(lv_scr_act(), nullptr);
        lv_obj_set_style_local_text_font(e.obj, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, fonts[e.font].font);
        lv_label_set_align(e.obj, e.variant == 1 ? LV_LABEL_ALIGN_CENTER : (e.variant == 2 ? LV_LABEL_ALIGN_RIGHT : LV_LABEL_ALIGN_LEFT));
        lv_label_set_recolor(e.obj, (e.flags & FlagRecolor) != 0);
        lv_label_set_text_static(e.obj, "");
        Place(i);
        break;
      case ElementType::Image:
        e.obj = lv_img_create(lv_scr_act(), nullptr);
        lv_img_set_src(e.obj, &strings[e.str]);
        lv_obj_set_pos(e.obj, e.x, e.y);
        break;
      case ElementType::Rect:
      case ElementType::Bar:
        e.obj = lv_obj_create(lv_scr_act(), nullptr);
        lv_obj_set_pos(e.obj, e.x, e.y);
        lv_obj_set_size(e.obj, e.w, e.h);
        lv_obj_set_style_local_radius(e.obj, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, (e.type == ElementType::Rect) ? e.radius : 0);
        lv_obj_set_style_local_border_width(e.obj, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, (e.type == ElementType::Rect) ? e.width : 0);
        lv_obj_set_style_local_border_color(e.obj, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, e.borderColor);
        break;
      case ElementType::Hand:
        if ((e.flags & FlagImage) != 0) {
          e.obj = lv_img_create(lv_scr_act(), nullptr);
          e.points[0].x = -1;
          break;
        }
        [[fallthrough]];
      case ElementType::Line:
        e.obj = lv_line_create(lv_scr_act(), nullptr);
        lv_obj_set_style_local_line_width(e.obj, LV_LINE_PART_MAIN, LV_STATE_DEFAULT, e.width);
        lv_obj_set_style_local_line_rounded(e.obj, LV_LINE_PART_MAIN, LV_STATE_DEFAULT, (e.flags & FlagRounded) != 0);
        if (e.type == ElementType::Line) {
          e.points = {{{e.x, e.y}, {e.w, e.h}}};
        } else {
          e.points = {{{-1, -1}, {-1, -1}}};
        }
        lv_line_set_points(e.obj, e.points.data(), 2);
        break;
      case ElementType::Arc:
        e.obj = lv_arc_create(lv_scr_act(), nullptr);
        lv_obj_set_pos(e.obj, e.x, e.y);
        lv_obj_set_size(e.obj, e.w, e.h);
        lv_arc_set_rotation(e.obj, 270);
        lv_arc_set_bg_angles(e.obj, e.start, e.end);
        lv_obj_set_style_local_bg_opa(e.obj, LV_ARC_PART_BG, LV_STATE_DEFAULT, LV_OPA_TRANSP);
        lv_obj_set_style_local_border_width(e.obj, LV_ARC_PART_BG, LV_STATE_DEFAULT, 0);
        lv_obj_set_style_local_pad_all(e.obj, LV_ARC_PART_BG, LV_STATE_DEFAULT, 0);
        lv_obj_set_style_local_line_width(e.obj, LV_ARC_PART_BG, LV_STATE_DEFAULT, e.width);
        lv_obj_set_style_local_line_opa(e.obj, LV_ARC_PART_INDIC, LV_STATE_DEFAULT, LV_OPA_TRANSP);
        lv_obj_set_style_local_bg_opa(e.obj, LV_ARC_PART_KNOB, LV_STATE_DEFAULT, LV_OPA_TRANSP);
        break;
      case ElementType::Ticks: {
        // lv_linemeter spreads its lines over scale_angle inclusive, with the gap centred at the bottom.
        // A full circle leaves out the last tick so it doesn't land on the first.
        lv_coord_t outer = (e.radius >= 0) ? e.radius : std::min(e.w, e.h) / 2;
        lv_coord_t inner = (e.inner == INT16_MIN) ? outer - 6 : e.inner;
        uint16_t count = e.count;
        int span = e.end - e.start;
        uint16_t scale = (span >= 360) ? 360 * (count - 1) / count : std::max(span, 0);
        e.obj = lv_linemeter_create(lv_scr_act(), nullptr);
        lv_obj_set_size(e.obj, outer * 2, outer * 2);
        lv_obj_set_pos(e.obj, e.x + e.w / 2 - outer, e.y + e.h / 2 - outer);
        lv_linemeter_set_scale(e.obj, scale, count);
        lv_linemeter_set_angle_offset(e.obj, static_cast<uint16_t>((e.start + 180 - (360 - scale) / 2 + 360) % 360));
        lv_linemeter_set_value(e.obj, lv_linemeter_get_min_value(e.obj));
        lv_obj_set_style_local_bg_opa(e.obj, LV_LINEMETER_PART_MAIN, LV_STATE_DEFAULT, LV_OPA_TRANSP);
        lv_obj_set_style_local_border_width(e.obj, LV_LINEMETER_PART_MAIN, LV_STATE_DEFAULT, 0);
        lv_obj_set_style_local_pad_all(e.obj, LV_LINEMETER_PART_MAIN, LV_STATE_DEFAULT, 0);
        lv_obj_set_style_local_scale_width(e.obj, LV_LINEMETER_PART_MAIN, LV_STATE_DEFAULT, outer - inner);
        lv_obj_set_style_local_line_width(e.obj, LV_LINEMETER_PART_MAIN, LV_STATE_DEFAULT, e.width);
        lv_obj_set_style_local_scale_end_line_width(e.obj, LV_LINEMETER_PART_MAIN, LV_STATE_DEFAULT, e.width);
        lv_obj_set_style_local_scale_border_width(e.obj, LV_LINEMETER_PART_MAIN, LV_STATE_DEFAULT, 0);
        lv_obj_set_style_local_scale_end_border_width(e.obj, LV_LINEMETER_PART_MAIN, LV_STATE_DEFAULT, 0);
        break;
      }
      case ElementType::Battery: {
        BatteryIcon icon(false);
        icon.Create(lv_scr_act());
        e.obj = icon.GetObject();
        lv_obj_set_pos(e.obj, e.x, e.y);
        lv_obj_set_style_local_image_recolor_opa(e.obj, LV_IMG_PART_MAIN, LV_STATE_DEFAULT, LV_OPA_COVER);
        break;
      }
    }
    if (e.colorMode == ColorMode::Fixed) {
      SetColor(e);
    } else {
      e.color = LV_COLOR_BLACK; // forces the first Refresh to set the real color
    }
    if (e.opa != LV_OPA_COVER) {
      SetOpacity(e);
    }
    lv_obj_set_hidden(e.obj, true);
  }
}

void WatchFaceCanvas::Place(size_t index) {
  Element& e = elements[index];
  lv_coord_t width = lv_obj_get_width(e.obj);
  lv_coord_t x = e.x;
  lv_coord_t y = e.y;
  if (e.relative != Relative::None) {
    // Skip back over empty or hidden neighbours in the chain, so they take no space
    size_t a = index - 1;
    while (a > 0 && elements[a].relative != Relative::None && IsCollapsed(elements[a])) {
      a--;
    }
    lv_area_t box;
    lv_obj_get_coords(elements[a].obj, &box);
    lv_coord_t gap = IsCollapsed(elements[a]) ? 0 : e.gap;
    if (e.relative == Relative::Left) {
      x += box.x1 - gap - width;
    } else {
      x += box.x2 + 1 + gap;
    }
    y += (box.y1 + box.y2 + 1) / 2 - lv_obj_get_height(e.obj) / 2;
  } else if (e.type == ElementType::Text && e.variant == 1) {
    x -= width / 2;
  } else if (e.type == ElementType::Text && e.variant == 2) {
    x -= width;
  }
  lv_obj_set_pos(e.obj, x, y);
}

void WatchFaceCanvas::ShowMessage(const char* text, lv_color_t color) {
  message = lv_label_create(lv_scr_act(), nullptr);
  lv_label_set_long_mode(message, LV_LABEL_LONG_BREAK);
  lv_obj_set_width(message, LV_HOR_RES - 20);
  lv_label_set_align(message, LV_LABEL_ALIGN_CENTER);
  lv_obj_set_style_local_text_color(message, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, color);
  lv_obj_set_style_local_bg_color(message, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_BLACK);
  lv_obj_set_style_local_bg_opa(message, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, LV_OPA_COVER);
  lv_label_set_text(message, text);
  lv_obj_align(message, nullptr, LV_ALIGN_IN_BOTTOM_MID, 0, -8);
}

void WatchFaceCanvas::Refresh() {
  if (message != nullptr && messageHideTick != 0 && static_cast<int32_t>(lv_tick_get() - messageHideTick) >= 0) {
    lv_obj_del(message);
    message = nullptr;
  }

  auto& dateTime = controllers.dateTimeController;
  auto now = std::chrono::time_point_cast<std::chrono::seconds>(dateTime.CurrentDateTime());
  int64_t second = now.time_since_epoch().count();
  if (second != lastPollSecond) {
    lastPollSecond = second;
    ReadData();
  }
  auto set = [this](Field field, int32_t value) {
    current[static_cast<size_t>(field)] = value;
  };
  int year = dateTime.Year();
  int yearDay = dateTime.DayOfYear();
  int weekDay = static_cast<int>(dateTime.DayOfWeek());
  int hour = dateTime.Hours();
  set(Field::Year, year);
  set(Field::Month, static_cast<int32_t>(dateTime.Month()));
  set(Field::Day, dateTime.Day());
  set(Field::YearDay, yearDay);
  set(Field::DaysLeft, (IsLeapYear(year) ? 366 : 365) - yearDay);
  set(Field::Week, IsoWeek(year, yearDay, weekDay));
  set(Field::WeekUS, (yearDay - 1 + 7 - weekDay % 7) / 7);
  set(Field::WeekDay, weekDay);
  set(Field::Hour, hour);
  set(Field::Hour12, (hour % 12 == 0) ? 12 : hour % 12);
  set(Field::Minute, dateTime.Minutes());
  // Fields finer than the face needs stay 0, so they never trigger a redraw
  set(Field::Second, (resolution >= Resolution::Second) ? dateTime.Seconds() : 0);
  set(Field::Tenth, (resolution >= Resolution::Tenth) ? dateTime.Tenths() : 0);

  if (shownValid && current == shown) {
    return;
  }
  shown = current;
  shownValid = true;
  const Values& values = shown;

  for (size_t i = 0; i < elements.size(); i++) {
    Element& e = elements[i];
    bool visible = IsVisible(e, values);
    if (lv_obj_get_hidden(e.obj) == visible) {
      lv_obj_set_hidden(e.obj, !visible);
    }
    if (!visible) {
      continue;
    }
    if (e.colorMode != ColorMode::Fixed) {
      lv_color_t color = BatteryColor(e.colorMode, Get(values, Field::Battery));
      if (color.full != e.color.full) {
        e.color = color;
        SetColor(e);
      }
    }
    switch (e.type) {
      case ElementType::Text: {
        char text[64];
        FormatText(&strings[e.str], text, sizeof(text), values);
        ApplyCase(text, e.textCase);
        bool changed = strcmp(lv_label_get_text(e.obj), text) != 0;
        if (changed) {
          lv_label_set_text(e.obj, text);
        }
        if (changed || e.relative != Relative::None) {
          Place(i);
        }
        break;
      }
      case ElementType::Hand:
        UpdateHand(e, values, strings.data());
        break;
      case ElementType::Bar:
        UpdateBar(e, values);
        break;
      case ElementType::Battery:
        UpdateBattery(e, values);
        [[fallthrough]];
      case ElementType::Image:
        if (e.relative != Relative::None) {
          Place(i);
        }
        break;
      default:
        break;
    }
  }
}

void WatchFaceCanvas::ReadData() {
  auto set = [this](Field field, int32_t value) {
    current[static_cast<size_t>(field)] = value;
  };
  set(Field::Battery, controllers.batteryController.PercentRemaining());
  set(Field::Charging, controllers.batteryController.IsCharging());
  set(Field::Power, controllers.batteryController.IsPowerPresent());
  set(Field::Ble, controllers.bleController.IsConnected());
  set(Field::BleOn, controllers.bleController.IsRadioEnabled());
  set(Field::Notify, controllers.notificationManager.AreNewNotificationsAvailable());
  uint32_t steps = controllers.motionController.NbSteps();
  uint32_t goal = controllers.settingsController.GetStepsGoal();
  set(Field::Steps, static_cast<int32_t>(steps));
  set(Field::StepPct, (goal == 0) ? 0 : static_cast<int32_t>(steps * 100 / goal));
  auto heartRateState = controllers.heartRateController.State();
  set(Field::HeartRate, controllers.heartRateController.HeartRate());
  set(Field::HeartRateOn, heartRateState != Controllers::HeartRateController::States::Stopped);
  set(Field::Alarm, controllers.alarmController.IsEnabled());
  set(Field::Clock12, controllers.settingsController.GetClockType() == Controllers::Settings::ClockType::H12);
  bool imperial = controllers.settingsController.GetWeatherFormat() == Controllers::Settings::WeatherFormat::Imperial;
  set(Field::Imperial, imperial);

  std::optional<Controllers::SimpleWeatherService::CurrentWeather> weather;
  if (controllers.weatherController != nullptr) {
    weather = controllers.weatherController->Current();
  }
  set(Field::Weather, weather.has_value());
  if (weather) {
    set(Field::Temp, imperial ? weather->temperature.Fahrenheit() : weather->temperature.Celsius());
    set(Field::WeatherIcon, static_cast<int32_t>(weather->iconId) | (controllers.weatherController->IsNight() ? 0x100 : 0));
  } else {
    set(Field::Temp, 0);
    set(Field::WeatherIcon, 0);
  }
}

bool WatchFaceCanvas::OnTouchEvent(uint16_t x, uint16_t /*y*/) {
  lastTouchX = x;
  return false;
}

bool WatchFaceCanvas::OnTouchEvent(TouchEvents event) {
  if (event != TouchEvents::DoubleTap || nFaces < 2) {
    return false;
  }
  if (lastTouchX < LV_HOR_RES / 3) {
    SwitchFace(-1);
    return true;
  }
  if (lastTouchX >= LV_HOR_RES * 2 / 3) {
    SwitchFace(1);
    return true;
  }
  return false;
}
