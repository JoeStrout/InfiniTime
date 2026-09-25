#include "displayapp/screens/WatchFaceCanvas.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "components/datetime/DateTimeController.h"
#include "components/fs/FS.h"

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
  };

  // Indexed by WatchFaceCanvas::Field
  constexpr const char* fieldNames[] = {"year", "month", "day", "yday", "week", "dow", "hour", "minute", "second", "tenth"};

  constexpr const char* handNames[] = {"hour", "minute", "second"};

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

        void NeedResolution(WatchFaceCanvas::Resolution resolution) {
          face.resolution = std::max(face.resolution, resolution);
        }

        bool ParseLine(char* line) {
          char* p = line;
          bool quoted;
          char* command = NextToken(p, quoted);
          if (command == nullptr || command[0] == '#') {
            return true;
          }

          if (strcmp(command, "name") == 0) {
            char* value = NextToken(p, quoted);
            if (value == nullptr) {
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

          WatchFaceCanvas::Element e {};
          e.color = LV_COLOR_WHITE;
          e.borderColor = LV_COLOR_WHITE;
          int nNumbers = 4;
          if (strcmp(command, "text") == 0) {
            e.type = WatchFaceCanvas::ElementType::Text;
            nNumbers = 2;
          } else if (strcmp(command, "image") == 0) {
            e.type = WatchFaceCanvas::ElementType::Image;
            nNumbers = 2;
          } else if (strcmp(command, "rect") == 0) {
            e.type = WatchFaceCanvas::ElementType::Rect;
            e.width = 0;
            e.radius = 0;
          } else if (strcmp(command, "line") == 0) {
            e.type = WatchFaceCanvas::ElementType::Line;
          } else if (strcmp(command, "arc") == 0) {
            e.type = WatchFaceCanvas::ElementType::Arc;
            e.width = 2;
          } else if (strcmp(command, "hand") == 0) {
            e.type = WatchFaceCanvas::ElementType::Hand;
            e.width = 3;
            char* which = NextToken(p, quoted);
            if (which == nullptr) {
              return Fail("missing hand");
            }
            auto* found = std::find_if(std::begin(handNames), std::end(handNames), [which](const char* n) {
              return strcmp(n, which) == 0;
            });
            if (found == std::end(handNames)) {
              return Fail("bad hand");
            }
            e.variant = static_cast<uint8_t>(found - std::begin(handNames));
            NeedResolution(e.variant == 0 ? WatchFaceCanvas::Resolution::Minute : WatchFaceCanvas::Resolution::Second);
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

          bool needsString = (e.type == WatchFaceCanvas::ElementType::Text || e.type == WatchFaceCanvas::ElementType::Image);
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
              if (e.type == WatchFaceCanvas::ElementType::Image) {
                lfs_info info;
                if (filesystem.Stat(token, &info) < 0) {
                  return Fail("missing image");
                }
                e.str = AddString("F:", token);
              } else {
                e.str = AddString("", token);
                if (strstr(token, "{t}") != nullptr) {
                  NeedResolution(WatchFaceCanvas::Resolution::Tenth);
                } else if (strstr(token, "{ss}") != nullptr) {
                  NeedResolution(WatchFaceCanvas::Resolution::Second);
                }
              }
              haveString = true;
            } else {
              return Fail("unexpected token");
            }
          }
          if (needsString && !haveString) {
            return Fail(e.type == WatchFaceCanvas::ElementType::Image ? "missing path" : "missing text");
          }

          face.elements.push_back(e);
          return true;
        }

        bool ParseAttribute(const char* key, const char* value, WatchFaceCanvas::Element& e) {
          bool ok = true;
          if (strcmp(key, "color") == 0) {
            ok = ParseColor(value, e.color);
          } else if (strcmp(key, "border_color") == 0) {
            ok = ParseColor(value, e.borderColor);
          } else if (strcmp(key, "width") == 0 || strcmp(key, "border") == 0) {
            ok = ParseInt(value, e.width);
          } else if (strcmp(key, "radius") == 0) {
            ok = ParseInt(value, e.radius);
          } else if (strcmp(key, "start") == 0) {
            ok = ParseInt(value, e.start);
          } else if (strcmp(key, "end") == 0) {
            ok = ParseInt(value, e.end);
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
          } else if (strcmp(key, "font") == 0) {
            return ParseFont(value, e);
          } else {
            return Fail("unknown attribute");
          }
          return ok || Fail("bad value");
        }

        bool ParseFont(const char* value, WatchFaceCanvas::Element& e) {
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

        bool ParseConditions(char*& p, WatchFaceCanvas::Element& e) {
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
            auto* found = std::find_if(std::begin(fieldNames), std::end(fieldNames), [token](const char* n) {
              return strcmp(n, token) == 0;
            });
            if (found == std::end(fieldNames)) {
              return Fail("unknown field");
            }
            c.field = static_cast<WatchFaceCanvas::Field>(found - std::begin(fieldNames));
            if (!ParseInt(value, c.value)) {
              return Fail("bad condition");
            }
            if (c.field == WatchFaceCanvas::Field::Tenth) {
              NeedResolution(WatchFaceCanvas::Resolution::Tenth);
            } else if (c.field == WatchFaceCanvas::Field::Second) {
              NeedResolution(WatchFaceCanvas::Resolution::Second);
            }
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
  using Values = std::array<int16_t, static_cast<size_t>(Field::Count)>;

  int16_t Get(const Values& values, Field field) {
    return values[static_cast<size_t>(field)];
  }

  bool IsVisible(const WatchFaceCanvas::Element& e, const Values& values) {
    for (uint8_t i = 0; i < e.nConditions; i++) {
      const auto& c = e.conditions[i];
      int16_t v = Get(values, c.field);
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

  // Expands {placeholders} in a text template.
  void FormatText(const char* in, char* out, size_t size, const Values& values) {
    using Pinetime::Controllers::DateTime;
    size_t len = 0;
    auto append = [&](const char* s) {
      while (*s != '\0' && len < size - 1) {
        out[len++] = *s++;
      }
    };
    auto appendNumber = [&](int value, int digits) {
      char number[8];
      snprintf(number, sizeof(number), "%0*d", digits, value);
      append(number);
    };

    int hour = Get(values, Field::Hour);
    int hour12 = (hour % 12 == 0) ? 12 : hour % 12;
    while (*in != '\0' && len < size - 1) {
      const char* close = (*in == '{') ? strchr(in, '}') : nullptr;
      if (close == nullptr || close - in > 5) {
        out[len++] = *in++;
        continue;
      }
      char key[6];
      size_t keyLen = close - in - 1;
      memcpy(key, in + 1, keyLen);
      key[keyLen] = '\0';

      if (strcmp(key, "YYYY") == 0) {
        appendNumber(Get(values, Field::Year), 4);
      } else if (strcmp(key, "YY") == 0) {
        appendNumber(Get(values, Field::Year) % 100, 2);
      } else if (strcmp(key, "M") == 0 || strcmp(key, "MM") == 0) {
        appendNumber(Get(values, Field::Month), static_cast<int>(keyLen));
      } else if (strcmp(key, "MMM") == 0) {
        append(DateTime::MonthShortToStringLow(static_cast<DateTime::Months>(Get(values, Field::Month))));
      } else if (strcmp(key, "D") == 0 || strcmp(key, "DD") == 0) {
        appendNumber(Get(values, Field::Day), static_cast<int>(keyLen));
      } else if (strcmp(key, "ddd") == 0) {
        append(DateTime::DayOfWeekShortToStringLow(static_cast<DateTime::Days>(Get(values, Field::WeekDay))));
      } else if (strcmp(key, "dddd") == 0) {
        append(DateTime::DayOfWeekToStringLow(static_cast<DateTime::Days>(Get(values, Field::WeekDay))));
      } else if (strcmp(key, "H") == 0 || strcmp(key, "HH") == 0) {
        appendNumber(hour, static_cast<int>(keyLen));
      } else if (strcmp(key, "h") == 0 || strcmp(key, "hh") == 0) {
        appendNumber(hour12, static_cast<int>(keyLen));
      } else if (strcmp(key, "mm") == 0) {
        appendNumber(Get(values, Field::Minute), 2);
      } else if (strcmp(key, "ss") == 0) {
        appendNumber(Get(values, Field::Second), 2);
      } else if (strcmp(key, "A") == 0) {
        append(hour < 12 ? "AM" : "PM");
      } else if (strcmp(key, "t") == 0) {
        appendNumber(Get(values, Field::Tenth), 1);
      } else {
        out[len++] = *in++;
        continue;
      }
      in = close + 1;
    }
    out[len] = '\0';
  }

  void PositionText(const WatchFaceCanvas::Element& e) {
    lv_coord_t width = lv_obj_get_width(e.obj);
    lv_coord_t x = e.x;
    if (e.variant == 1) {
      x -= width / 2;
    } else if (e.variant == 2) {
      x -= width;
    }
    lv_obj_set_pos(e.obj, x, e.y);
  }

  void UpdateHand(WatchFaceCanvas::Element& e, const Values& values) {
    int16_t angle;
    switch (e.variant) {
      case 0:
        angle = (Get(values, Field::Hour) % 12) * 30 + Get(values, Field::Minute) / 2;
        break;
      case 1:
        angle = Get(values, Field::Minute) * 6 + Get(values, Field::Second) / 10;
        break;
      default:
        angle = Get(values, Field::Second) * 6;
        break;
    }
    constexpr int32_t trigScale = INT16_MAX; // = _lv_trigo_sin(90)
    int32_t length = (e.radius >= 0) ? e.radius : std::min(e.w, e.h) / 2;
    lv_coord_t cx = e.x + e.w / 2;
    lv_coord_t cy = e.y + e.h / 2;
    lv_point_t tip {static_cast<lv_coord_t>(cx + length * _lv_trigo_sin(angle) / trigScale),
                    static_cast<lv_coord_t>(cy - length * _lv_trigo_sin(angle + 90) / trigScale)};
    if (tip.x != e.points[1].x || tip.y != e.points[1].y) {
      e.points[0] = {cx, cy};
      e.points[1] = tip;
      lv_line_set_points(e.obj, e.points.data(), 2);
    }
  }
}

WatchFaceCanvas::WatchFaceCanvas(Controllers::DateTime& dateTimeController, Controllers::FS& filesystem)
  : dateTimeController {dateTimeController}, filesystem {filesystem} {
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
  lastTimeKey = -1;
  Refresh();
}

void WatchFaceCanvas::CreateObjects() {
  lv_obj_t* bg = lv_obj_create(lv_scr_act(), nullptr);
  lv_obj_set_size(bg, LV_HOR_RES, LV_VER_RES);
  lv_obj_set_style_local_bg_color(bg, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, background);
  lv_obj_set_style_local_border_width(bg, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, 0);
  lv_obj_set_style_local_radius(bg, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, 0);

  for (auto& e : elements) {
    switch (e.type) {
      case ElementType::Text:
        e.obj = lv_label_create(lv_scr_act(), nullptr);
        lv_obj_set_style_local_text_color(e.obj, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, e.color);
        lv_obj_set_style_local_text_font(e.obj, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, fonts[e.font].font);
        lv_label_set_align(e.obj, e.variant == 1 ? LV_LABEL_ALIGN_CENTER : (e.variant == 2 ? LV_LABEL_ALIGN_RIGHT : LV_LABEL_ALIGN_LEFT));
        lv_label_set_text_static(e.obj, "");
        break;
      case ElementType::Image:
        e.obj = lv_img_create(lv_scr_act(), nullptr);
        lv_img_set_src(e.obj, &strings[e.str]);
        lv_obj_set_pos(e.obj, e.x, e.y);
        break;
      case ElementType::Rect:
        e.obj = lv_obj_create(lv_scr_act(), nullptr);
        lv_obj_set_pos(e.obj, e.x, e.y);
        lv_obj_set_size(e.obj, e.w, e.h);
        lv_obj_set_style_local_bg_color(e.obj, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, e.color);
        lv_obj_set_style_local_radius(e.obj, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, e.radius);
        lv_obj_set_style_local_border_width(e.obj, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, e.width);
        lv_obj_set_style_local_border_color(e.obj, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, e.borderColor);
        break;
      case ElementType::Line:
      case ElementType::Hand:
        e.obj = lv_line_create(lv_scr_act(), nullptr);
        lv_obj_set_style_local_line_width(e.obj, LV_LINE_PART_MAIN, LV_STATE_DEFAULT, e.width);
        lv_obj_set_style_local_line_color(e.obj, LV_LINE_PART_MAIN, LV_STATE_DEFAULT, e.color);
        lv_obj_set_style_local_line_rounded(e.obj, LV_LINE_PART_MAIN, LV_STATE_DEFAULT, true);
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
        lv_obj_set_style_local_line_color(e.obj, LV_ARC_PART_BG, LV_STATE_DEFAULT, e.color);
        lv_obj_set_style_local_line_width(e.obj, LV_ARC_PART_BG, LV_STATE_DEFAULT, e.width);
        lv_obj_set_style_local_line_opa(e.obj, LV_ARC_PART_INDIC, LV_STATE_DEFAULT, LV_OPA_TRANSP);
        lv_obj_set_style_local_bg_opa(e.obj, LV_ARC_PART_KNOB, LV_STATE_DEFAULT, LV_OPA_TRANSP);
        break;
    }
    lv_obj_set_hidden(e.obj, true);
  }
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

  auto now = std::chrono::time_point_cast<std::chrono::seconds>(dateTimeController.CurrentDateTime());
  int64_t timeKey = now.time_since_epoch().count();
  uint8_t tenth = 0;
  if (resolution == Resolution::Tenth) {
    tenth = dateTimeController.Tenths();
    timeKey = timeKey * 10 + tenth;
  } else if (resolution == Resolution::Minute) {
    timeKey /= 60;
  }
  if (timeKey == lastTimeKey) {
    return;
  }
  lastTimeKey = timeKey;

  int year = dateTimeController.Year();
  int weekDay = static_cast<int>(dateTimeController.DayOfWeek());
  Values values;
  values[static_cast<size_t>(Field::Year)] = year;
  values[static_cast<size_t>(Field::Month)] = static_cast<int16_t>(dateTimeController.Month());
  values[static_cast<size_t>(Field::Day)] = dateTimeController.Day();
  values[static_cast<size_t>(Field::YearDay)] = dateTimeController.DayOfYear();
  values[static_cast<size_t>(Field::Week)] = IsoWeek(year, dateTimeController.DayOfYear(), weekDay);
  values[static_cast<size_t>(Field::WeekDay)] = weekDay;
  values[static_cast<size_t>(Field::Hour)] = dateTimeController.Hours();
  values[static_cast<size_t>(Field::Minute)] = dateTimeController.Minutes();
  values[static_cast<size_t>(Field::Second)] = dateTimeController.Seconds();
  values[static_cast<size_t>(Field::Tenth)] = tenth;

  for (auto& e : elements) {
    bool visible = IsVisible(e, values);
    if (lv_obj_get_hidden(e.obj) == visible) {
      lv_obj_set_hidden(e.obj, !visible);
    }
    if (!visible) {
      continue;
    }
    if (e.type == ElementType::Text) {
      char text[64];
      FormatText(&strings[e.str], text, sizeof(text), values);
      if (strcmp(lv_label_get_text(e.obj), text) != 0) {
        lv_label_set_text(e.obj, text);
        PositionText(e);
      }
    } else if (e.type == ElementType::Hand) {
      UpdateHand(e, values);
    }
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
