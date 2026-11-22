// hearthmap-render: one frame of the panel, from a config and a readings fixture, to PNG.
//
//   hearthmap-render --config <json> --readings <json> --out frame.png
//                    [--layer temperature|humidity|soil|air] [--tap x,y ...] [--time-ms N]
//                    [--photos <dir>] [--utc-offset-min N] [--timing]
//   hearthmap-render --check --config <json>
//
// --check loads the config exactly as the panel will judge it (the JSON source, then core's
// validate()), prints every issue with its JSON-pointer path and exits 1 on any error, 0
// otherwise. It is the required step before a flash: the build checks the schema, but
// validate()'s semantic rules otherwise first run on the panel, at boot (D-048).
//
// Order of events: build at LVGL time 0, select --layer, deliver each --tap through LVGL's input
// pipeline (each one's resulting event is applied before the next tap), then advance the LVGL
// clock to --time-ms so pulses are drawn at that phase, and render.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "app.h"
#include "headless.h"
#include "json_config.h"
#include "photos.h"
#include "png_io.h"

namespace {

using namespace hearthmap;

int usage(const char* msg) {
  std::cerr << "hearthmap-render: " << msg << "\n"
            << "usage: hearthmap-render --config <json> --readings <json> --out <png>\n"
            << "         [--layer temperature|humidity|soil|air] [--tap x,y ...]\n"
            << "         [--time-ms N] [--photos <dir>] [--utc-offset-min N] [--timing]\n"
            << "       hearthmap-render --check --config <json>\n";
  return 2;
}

std::optional<std::string> slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::optional<Layer> parse_layer(const std::string& s) {
  if (s == "temperature") return Layer::Temperature;
  if (s == "humidity") return Layer::Humidity;
  if (s == "soil") return Layer::Soil;
  if (s == "air") return Layer::Air;
  return std::nullopt;
}

bool parse_int(const std::string& s, long& out) {
  char* end = nullptr;
  out = std::strtol(s.c_str(), &end, 10);
  return !s.empty() && end != nullptr && *end == '\0';
}

}  // namespace

namespace {

/// Takes every frame: the whole screen is rendered next, from the frames now in place.
class FullRenderPresenter final : public hearthmap::ui::Presenter {
 public:
  bool present(int x, int y, const hearthmap::ui::View565& tile) override {
    (void)x;
    (void)y;
    (void)tile;
    return true;
  }
};

}  // namespace

int main(int argc, char** argv) {
  std::string config_path;
  std::string readings_path;
  std::string out_path;
  std::string photos_dir;
  std::optional<Layer> layer;
  std::vector<std::pair<int, int>> taps;
  long time_ms = 0;
  long utc_offset = 0;
  bool timing = false;
  bool check = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::optional<std::string> {
      if (i + 1 >= argc) return std::nullopt;
      return std::string(argv[++i]);
    };
    if (a == "--timing") {
      timing = true;
      continue;
    }
    if (a == "--check") {
      check = true;
      continue;
    }
    const auto v = next();
    if (!v) return usage(("missing value for " + a).c_str());
    if (a == "--config") {
      config_path = *v;
    } else if (a == "--readings") {
      readings_path = *v;
    } else if (a == "--out") {
      out_path = *v;
    } else if (a == "--photos") {
      photos_dir = *v;
    } else if (a == "--layer") {
      layer = parse_layer(*v);
      if (!layer) return usage("--layer must be temperature, humidity, soil or air");
    } else if (a == "--tap") {
      const auto comma = v->find(',');
      long x = 0;
      long y = 0;
      if (comma == std::string::npos || !parse_int(v->substr(0, comma), x) ||
          !parse_int(v->substr(comma + 1), y)) {
        return usage("--tap takes x,y in canvas pixels");
      }
      taps.emplace_back(static_cast<int>(x), static_cast<int>(y));
    } else if (a == "--time-ms") {
      if (!parse_int(*v, time_ms) || time_ms < 0) return usage("--time-ms takes N >= 0");
    } else if (a == "--utc-offset-min") {
      if (!parse_int(*v, utc_offset)) return usage("--utc-offset-min takes an integer");
    } else {
      return usage(("unknown option " + a).c_str());
    }
  }
  if (check && config_path.empty()) return usage("--check needs --config");
  if (!check && (config_path.empty() || readings_path.empty() || out_path.empty())) {
    return usage("--config, --readings and --out are required");
  }

  const auto config_text = slurp(config_path);
  if (!config_text) return usage(("cannot read " + config_path).c_str());
  const ConfigLoadResult loaded = host::JsonConfigSource(*config_text).load();
  for (const auto& issue : loaded.issues) {
    std::cerr << config_path << ": "
              << (issue.severity == IssueSeverity::Error ? "error" : "warning") << " at "
              << issue.path << ": " << issue.message << "\n";
  }
  if (check) {
    std::size_t warnings = 0;
    for (const auto& issue : loaded.issues) {
      if (issue.severity == IssueSeverity::Warning) ++warnings;
    }
    if (!loaded.config) {
      std::cout << config_path << ": refused: the panel would not draw this config\n";
      return 1;
    }
    std::cout << config_path << ": ok: no errors, " << warnings << " warning"
              << (warnings == 1 ? "" : "s") << "\n";
    return 0;
  }
  if (!loaded.config) return 1;

  const auto readings_text = slurp(readings_path);
  if (!readings_text) return usage(("cannot read " + readings_path).c_str());
  const host::ReadingsLoadResult readings = host::parse_readings(*readings_text);
  if (!readings.fixture) {
    std::cerr << readings_path << ": " << readings.error << "\n";
    return 1;
  }

  const Config& config = *loaded.config;
  host::HeadlessDisplay display(config.canvas.width, config.canvas.height);
  std::unique_ptr<host::DirectoryPhotos> photos;
  if (!photos_dir.empty()) photos = std::make_unique<host::DirectoryPhotos>(photos_dir);

  const auto t0 = std::chrono::steady_clock::now();
  host::App app(display.screen(), config, readings.fixture->readings, readings.fixture->now,
                static_cast<int>(utc_offset), photos.get(), host::kPanelPulseMemory);
  const auto t1 = std::chrono::steady_clock::now();
  if (layer) app.set_layer(*layer);
  display.run_timers();
  for (const auto& [x, y] : taps) {
    display.tap(x, y);
    if (app.process_events() == 0) {
      std::cerr << "hearthmap-render: tap at " << x << "," << y << " produced no event\n";
    }
  }
  // Every pulse tile at the same instant: frames are taken as the panel takes them (deferred, then
  // shown), and the whole-screen render below draws them all. Not deferred, at most 20 tiles a
  // tick would move (LVGL's limit on invalidated areas) and a still frame could mix two instants.
  app.screen().set_frames_deferred(true);
  display.set_time_ms(static_cast<std::uint32_t>(time_ms));
  display.run_timers();
  FullRenderPresenter presenter;
  app.screen().show_due_frames(presenter);
  const auto t2 = std::chrono::steady_clock::now();
  display.render();
  const auto t3 = std::chrono::steady_clock::now();
  // A second full frame with nothing changed: the steady-state cost of a redraw.
  app.refresh();
  display.render();
  const auto t4 = std::chrono::steady_clock::now();

  const std::string err =
      host::write_png(out_path, host::from_rgb565(display.framebuffer(),
                                                  static_cast<std::uint32_t>(display.width()),
                                                  static_cast<std::uint32_t>(display.height())));
  if (!err.empty()) {
    std::cerr << "hearthmap-render: writing " << out_path << ": " << err << "\n";
    return 1;
  }
  if (timing) {
    auto ms = [](auto a, auto b) {
      return std::chrono::duration<double, std::milli>(b - a).count();
    };
    std::fprintf(stderr,
                 "build+first refresh %.1f ms, events+timers %.1f ms, full render %.1f ms, "
                 "refresh+full render %.1f ms\n"
                 "heat image %zu bytes, rasterizer %zu bytes, LVGL draw buffer %zu bytes\n",
                 ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3, t4), app.heat_image_bytes(),
                 app.rasterizer_bytes(), display.draw_buffer_bytes());
  }
  return 0;
}
