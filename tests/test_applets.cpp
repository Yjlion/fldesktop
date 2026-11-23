// fl-panel's weather and system monitor applets
#include "applets_util.h"
#include "test.h"

#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

using namespace applets;

int main() {
  auto places = parse_places(
      "{\"results\":[{\"name\":\"Berlin\",\"latitude\":52.52437,\"longitude\":13.41053,\"country\":\"Germany\","
      "\"admin1\":\"Land Berlin\"},{\"name\":\"Berlin\",\"latitude\":44.4687,\"longitude\":-71.1851,"
      "\"country\":\"United States\",\"admin1\":\"New Hampshire\"}],\"generationtime_ms\":0.6}");
  CHECK_EQ(places.size(), (size_t)2);
  CHECK_EQ(places[0].region, std::string("Land Berlin"));
  CHECK(places[1].lon < -71);
  CHECK(parse_places("{\"generationtime_ms\":0.1}").empty());
  CHECK(parse_places("garbage").empty());
  CHECK_EQ(search_url("São Paulo"), std::string("https://geocoding-api.open-meteo.com/v1/search?count=6&format=json&name=S%C3%A3o%20Paulo"));
  std::string u = forecast_url(52.52, 13.41, true);
  CHECK(u.find("latitude=52.5200") != std::string::npos);
  CHECK(u.find("temperature_unit=fahrenheit") != std::string::npos);

  Weather w = parse_forecast(
      "{\"latitude\":52.52,\"current\":{\"time\":\"2026-11-22T21:00\",\"temperature_2m\":3.4,"
      "\"apparent_temperature\":-0.6,\"relative_humidity_2m\":88,\"weather_code\":61,\"wind_speed_10m\":14.2,"
      "\"is_day\":0},\"daily\":{\"time\":[\"2026-11-22\",\"2026-11-23\"],\"weather_code\":[61,3],"
      "\"temperature_2m_max\":[5.1,6.0],\"temperature_2m_min\":[1.2,-0.4]}}");
  CHECK(w.ok);
  CHECK_NEAR(w.temp, 3.4);
  CHECK_EQ(w.code, 61);
  CHECK_EQ(w.humidity, 88);
  CHECK(!w.day);
  CHECK_EQ(w.days.size(), (size_t)2);
  CHECK_EQ(w.days[1].date, std::string("2026-11-23"));
  CHECK_NEAR(w.days[1].tmin, -0.4);
  CHECK(!parse_forecast("{}").ok);
  CHECK_EQ(weather_text(0), std::string("Clear"));
  CHECK_EQ(weather_text(63), std::string("Rain"));
  CHECK_EQ(weather_text(95), std::string("Thunderstorm"));
  CHECK_EQ(weather_icon(0, false), std::string("weather-clear-night"));
  CHECK_EQ(weather_icon(73, true), std::string("weather-snow"));
  CHECK_EQ(weather_icon(81, true), std::string("weather-showers"));
  CHECK_EQ(degrees(3.4), std::string("3\xc2\xb0"));
  CHECK_EQ(degrees(-0.4), std::string("0\xc2\xb0"));
  CHECK_EQ(degrees(-2.6), std::string("-3\xc2\xb0"));

  CpuTimes a, b;
  CHECK(parse_proc_stat("cpu  100 0 50 800 50 0 0 0 0 0\ncpu0 ...", a));
  CHECK_EQ(a.total, 1000ull);
  CHECK_EQ(a.busy, 150ull);
  CHECK(parse_proc_stat("cpu  200 0 100 1550 50 0 0 0 0 0\n", b));
  CHECK_NEAR(cpu_usage(a, b), 150.0 / 900.0);
  CHECK_NEAR(cpu_usage(b, a), 0.0);
  CHECK(!parse_proc_stat("intr 1 2 3", a));
  unsigned long long total = 0;
  CHECK_NEAR(memory_usage("MemTotal:       16000000 kB\nMemFree:  1000 kB\nMemAvailable:    4000000 kB\n", &total), 0.75);
  CHECK_EQ(total, 16000000ull);
  CHECK_NEAR(memory_usage(""), 0.0);
  // privacy: a fake /proc with a process holding /dev/video0
  {
    char tmpl[] = "/tmp/fl-proc-XXXXXX";
    std::string root = mkdtemp(tmpl);
    auto proc = [&](const char *pid, const char *comm, const char *fd_target) {
      std::string d = root + "/" + pid;
      mkdir(d.c_str(), 0755);
      mkdir((d + "/fd").c_str(), 0755);
      FILE *f = fopen((d + "/comm").c_str(), "w");
      fprintf(f, "%s\n", comm);
      fclose(f);
      if (symlink(fd_target, (d + "/fd/3").c_str()) != 0) {}
    };
    proc("100", "fl-webcam", "/dev/video0");
    proc("101", "bash", "/dev/pts/1");
    proc("102", "zoom", "/dev/video2");
    proc("103", "fl-webcam", "/dev/video0");
    mkdir((root + "/self").c_str(), 0755);
    auto u = camera_users(root);
    CHECK_EQ(u.size(), (size_t)2);
    CHECK_EQ(u[0], std::string("fl-webcam"));
    CHECK_EQ(u[1], std::string("zoom"));
    CHECK(camera_users(root + "/none").empty());
    if (system(("rm -rf " + root).c_str()) != 0) {}
  }
  CHECK_EQ(name_list({}), std::string(""));
  CHECK_EQ(name_list({"a"}), std::string("a"));
  CHECK_EQ(name_list({"a", "b", "c"}), std::string("a, b and c"));
  CHECK_EQ(name_list({"a", "b", "c", "d", "e"}), std::string("a, b, c and 2 more"));
  return test_result("applets");
}
