#include "flat_fixture.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <initializer_list>
#include <string>
#include <utility>

namespace hearthmap::test {
namespace {

Room room(std::string id, std::string name, Polygon polygon, bool outdoor = false) {
  Room r;
  r.id = std::move(id);
  r.name = std::move(name);
  r.polygon = std::move(polygon);
  r.outdoor = outdoor;
  return r;
}

Pin pin(std::string id, PinKind kind, std::string name, MapPoint position, std::string room_id,
        std::initializer_list<EntityBinding> entities) {
  Pin p;
  p.id = std::move(id);
  p.kind = kind;
  p.name = std::move(name);
  p.position = position;
  p.room_id = std::move(room_id);
  p.entities = entities;
  return p;
}

Calibration cal(double dry, double wet) {
  Calibration c;
  c.dry = dry;
  c.wet = wet;
  return c;
}

constexpr Timestamp kFresh = kFlatNow - 300;  // 08:55:00Z

}  // namespace

Reading value_reading(double value, Timestamp last_updated, std::string unit) {
  return Reading{ReadingState::Value, value, std::move(unit), last_updated};
}

Config flat_config() {
  Config c;
  c.canvas = Canvas{1280, 800, Orientation::Landscape, CanvasRect{0, 0, 960, 800}};
  c.temperature_unit = TemperatureUnit::Celsius;
  c.rooms = {
      room("living", "Living room", {{0.0, 0.0}, {0.4167, 0.0}, {0.4167, 0.6}, {0.0, 0.6}}),
      room("kitchen", "Kitchen", {{0.4167, 0.0}, {0.6667, 0.0}, {0.6667, 0.4}, {0.4167, 0.4}}),
      room("study", "Study", {{0.6667, 0.0}, {1.0, 0.0}, {1.0, 0.4}, {0.6667, 0.4}}),
      room("hallway", "Hallway",
           {{0.4167, 0.4}, {1.0, 0.4}, {1.0, 0.6}, {0.5833, 0.6}, {0.5833, 1.0}, {0.4167, 1.0}}),
      room("bathroom", "Bathroom", {{0.5833, 0.6}, {0.7917, 0.6}, {0.7917, 1.0}, {0.5833, 1.0}}),
      room("bedroom", "Bedroom", {{0.7917, 0.6}, {1.0, 0.6}, {1.0, 1.0}, {0.7917, 1.0}}),
      room("balcony", "Balcony", {{0.0, 0.6}, {0.4167, 0.6}, {0.4167, 1.0}, {0.0, 1.0}}, true),
  };

  using R = Role;
  c.pins.push_back(pin("clim_living", PinKind::Temperature, "Living room", {0.2083, 0.3}, "living",
                       {{R::Temperature, "sensor.example_living_temperature"},
                        {R::Humidity, "sensor.example_living_humidity"}}));
  c.pins.push_back(pin("clim_kitchen", PinKind::Temperature, "Kitchen", {0.5417, 0.2}, "kitchen",
                       {{R::Temperature, "sensor.example_kitchen_temperature"}}));
  c.pins.push_back(pin("clim_study", PinKind::Temperature, "Study", {0.8333, 0.2}, "study",
                       {{R::Temperature, "sensor.example_study_temperature"},
                        {R::Humidity, "sensor.example_study_humidity"}}));
  c.pins.push_back(pin("clim_hall", PinKind::Temperature, "Hallway", {0.5, 0.5}, "hallway",
                       {{R::Temperature, "sensor.example_hall_temperature"}}));
  c.pins.push_back(pin("clim_bedroom", PinKind::Temperature, "Bedroom", {0.8958, 0.75}, "bedroom",
                       {{R::Temperature, "sensor.example_bedroom_temperature"},
                        {R::Humidity, "sensor.example_bedroom_humidity"},
                        {R::Battery, "sensor.example_bedroom_climate_battery"}}));
  c.pins.push_back(pin("hum_bathroom", PinKind::Humidity, "Bathroom", {0.6875, 0.75}, "bathroom",
                       {{R::Humidity, "sensor.example_bathroom_humidity"},
                        {R::Temperature, "sensor.example_bathroom_temperature"}}));
  c.pins.push_back(pin("clim_balcony", PinKind::Temperature, "Balcony", {0.2083, 0.7}, "balcony",
                       {{R::Temperature, "sensor.example_balcony_temperature"},
                        {R::Humidity, "sensor.example_balcony_humidity"}}));
  c.pins.push_back(
      pin("air_living", PinKind::AirQuality, "Living room air", {0.3333, 0.1}, "living",
          {{R::Co2, "sensor.example_living_co2"}, {R::Pm25, "sensor.example_living_pm25"}}));
  c.pins.push_back(
      pin("air_bedroom", PinKind::AirQuality, "Bedroom air", {0.825, 0.66}, "bedroom",
          {{R::Co2, "sensor.example_bedroom_co2"}, {R::Voc, "sensor.example_bedroom_voc"}}));

  Pin planter = pin("soil_planter", PinKind::Soil, "Planter box", {0.2083, 0.94}, "balcony",
                    {{R::Moisture, "sensor.example_planter_moisture"}});
  planter.calibration = cal(1, 60);
  c.pins.push_back(planter);

  Pin monstera = pin("plant_monstera", PinKind::Plant, "Monstera", {0.0833, 0.1}, "living",
                     {{R::Moisture, "sensor.example_monstera_moisture"},
                      {R::Battery, "sensor.example_monstera_battery"}});
  monstera.photo = "monstera";
  monstera.calibration = cal(5, 60);
  c.pins.push_back(monstera);

  Pin fiddle = pin("plant_fiddle", PinKind::Plant, "Fiddle-leaf fig", {0.375, 0.5}, "living",
                   {{R::Moisture, "sensor.example_fiddle_moisture"},
                    {R::Temperature, "sensor.example_fiddle_temperature"},
                    {R::Humidity, "sensor.example_fiddle_humidity"},
                    {R::Battery, "sensor.example_fiddle_battery"}});
  fiddle.photo = "fiddle-leaf-fig";
  fiddle.calibration = cal(10, 70);
  fiddle.calibration->happy_min = 25;
  fiddle.calibration->happy_max = 60;
  c.pins.push_back(fiddle);

  Pin basil = pin("plant_basil", PinKind::Plant, "Basil", {0.625, 0.06}, "kitchen",
                  {{R::Moisture, "sensor.example_basil_moisture"},
                   {R::PredictedDryAt, "sensor.example_basil_predicted_dry_at"}});
  basil.photo = "basil";
  basil.calibration = cal(0, 55);
  c.pins.push_back(basil);

  Pin pothos = pin("plant_pothos", PinKind::Plant, "Pothos", {0.95, 0.06}, "study",
                   {{R::Moisture, "sensor.example_pothos_moisture_raw"}});
  pothos.calibration = cal(2900, 1300);
  c.pins.push_back(pothos);

  Pin snake = pin("plant_snake", PinKind::Plant, "Snake plant", {0.9583, 0.94}, "bedroom",
                  {{R::Moisture, "sensor.example_snake_moisture"},
                   {R::Battery, "sensor.example_snake_battery"}});
  snake.photo = "snake-plant";
  snake.calibration = cal(3, 40);
  c.pins.push_back(snake);

  Pin fern = pin(
      "plant_fern", PinKind::Plant, "Boston fern", {0.6167, 0.94}, "bathroom",
      {{R::Moisture, "sensor.example_fern_moisture"}, {R::Battery, "sensor.example_fern_battery"}});
  fern.calibration = cal(15, 80);
  fern.calibration->happy_min = 40;
  c.pins.push_back(fern);

  Pin tomato = pin("plant_tomato", PinKind::Plant, "Cherry tomato", {0.3667, 0.94}, "balcony",
                   {{R::Moisture, "sensor.example_tomato_moisture"},
                    {R::Temperature, "sensor.example_tomato_temperature"}});
  tomato.photo = "tomato";
  tomato.calibration = cal(2, 65);
  tomato.stale_after = 21600;
  c.pins.push_back(tomato);

  Pin lavender = pin("plant_lavender", PinKind::Plant, "Lavender", {0.05, 0.66}, "balcony",
                     {{R::Moisture, "sensor.example_lavender_moisture"}});
  lavender.calibration = cal(0, 50);
  c.pins.push_back(lavender);

  c.layers[index_of(Layer::Air)].band.warn_above = 1000.0;
  c.default_layer = Layer::Temperature;
  c.stale_after = 10800;
  c.hud.max_items = 4;
  c.heat = HeatConfig{48, 40, 2.0};
  return c;
}

Readings flat_all_ok() {
  Readings r;
  auto set = [&](const char* id, double v, const char* unit) {
    r.by_entity[id] = value_reading(v, kFresh, unit);
  };
  set("sensor.example_balcony_humidity", 80, "%");
  set("sensor.example_balcony_temperature", 9.0, "°C");
  set("sensor.example_basil_moisture", 30, "%");
  set("sensor.example_bathroom_humidity", 58, "%");
  set("sensor.example_bathroom_temperature", 22.5, "°C");
  set("sensor.example_bedroom_climate_battery", 88, "%");
  set("sensor.example_bedroom_co2", 750, "ppm");
  set("sensor.example_bedroom_humidity", 52, "%");
  set("sensor.example_bedroom_temperature", 19.5, "°C");
  set("sensor.example_bedroom_voc", 110, "");
  set("sensor.example_fern_battery", 95, "%");
  set("sensor.example_fern_moisture", 55, "%");
  set("sensor.example_fiddle_battery", 84, "%");
  set("sensor.example_fiddle_humidity", 50, "%");
  set("sensor.example_fiddle_moisture", 45, "%");
  set("sensor.example_fiddle_temperature", 21.0, "°C");
  set("sensor.example_hall_temperature", 20.0, "°C");
  set("sensor.example_kitchen_temperature", 22.0, "°C");
  set("sensor.example_lavender_moisture", 22, "%");
  set("sensor.example_living_co2", 620, "ppm");
  set("sensor.example_living_humidity", 48, "%");
  set("sensor.example_living_pm25", 6, "µg/m³");
  set("sensor.example_living_temperature", 21.5, "°C");
  set("sensor.example_monstera_battery", 91, "%");
  set("sensor.example_monstera_moisture", 35, "%");
  set("sensor.example_planter_moisture", 35, "%");
  set("sensor.example_pothos_moisture_raw", 2000, "");
  set("sensor.example_snake_battery", 77, "%");
  set("sensor.example_snake_moisture", 20, "%");
  set("sensor.example_study_humidity", 45, "%");
  set("sensor.example_study_temperature", 20.5, "°C");
  set("sensor.example_tomato_moisture", 40, "%");
  set("sensor.example_tomato_temperature", 8.5, "°C");
  return r;
}

Readings flat_thirsty_many() {
  Readings r = flat_all_ok();
  r.by_entity["sensor.example_basil_moisture"].value = 0;
  r.by_entity["sensor.example_fern_moisture"].value = 38;
  r.by_entity["sensor.example_lavender_moisture"].value = 2;
  r.by_entity["sensor.example_monstera_moisture"].value = 6;
  r.by_entity["sensor.example_planter_moisture"].value = 4;
  r.by_entity["sensor.example_pothos_moisture_raw"].value = 2750;
  r.by_entity["sensor.example_snake_moisture"].value = 9;
  return r;
}

Readings flat_stale_sensors() {
  Readings r = flat_all_ok();
  r.by_entity["sensor.example_bathroom_humidity"].state = ReadingState::Unavailable;
  r.by_entity.erase("sensor.example_bedroom_co2");
  r.by_entity["sensor.example_kitchen_temperature"].last_updated = kFlatNow - 12 * 3600;
  r.by_entity["sensor.example_lavender_moisture"].last_updated = kFlatNow - 3 * 3600;
  r.by_entity["sensor.example_snake_moisture"].state = ReadingState::Unknown;
  r.by_entity["sensor.example_study_temperature"].last_updated = kFlatNow - 3 * 3600 - 1800;
  r.by_entity["sensor.example_tomato_moisture"].last_updated = kFlatNow - 4 * 3600;
  return r;
}

Readings flat_humid_bathroom() {
  Readings r = flat_all_ok();
  r.by_entity["sensor.example_balcony_humidity"].value = 92;
  r.by_entity["sensor.example_bathroom_humidity"].value = 82;
  r.by_entity["sensor.example_bathroom_temperature"].value = 24.5;
  r.by_entity["sensor.example_bedroom_humidity"].value = 66;
  r.by_entity["sensor.example_fern_moisture"].value = 70;
  return r;
}

std::size_t pin_index(const Config& config, const std::string& id) {
  for (std::size_t i = 0; i < config.pins.size(); ++i) {
    if (config.pins[i].id == id) return i;
  }
  FAIL("no pin " << id);
  return 0;
}

std::size_t room_index(const Config& config, const std::string& id) {
  for (std::size_t i = 0; i < config.rooms.size(); ++i) {
    if (config.rooms[i].id == id) return i;
  }
  FAIL("no room " << id);
  return 0;
}

}  // namespace hearthmap::test
