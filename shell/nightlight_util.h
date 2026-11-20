// fl-nightlight: colour temperature, schedules and sun times (no I/O except
// the zone table reader; unit tested)
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nightlight {

// Multipliers for a colour temperature in kelvin, 1.0 at 6500 K.
struct Rgb {
  double r = 1, g = 1, b = 1;
};
Rgb temperature_rgb(int kelvin);

// A gamma ramp of `size` entries per channel (r, then g, then b), as
// wlr-gamma-control wants it.
std::vector<uint16_t> gamma_ramp(uint32_t size, const Rgb &c);

// Sunrise and sunset in minutes after local midnight for a day of the year
// (1-366), latitude/longitude in degrees and the UTC offset in minutes.
// false when the sun does not rise or set that day (polar day or night);
// then *polar_day says which.
bool sun_times(int day_of_year, double lat, double lon, int utc_offset_min, int *sunrise, int *sunset,
               bool *polar_day);

// ISO 6709 coordinates from zone1970.tab, e.g. "+5230+01322" or
// "+404251-0740023", to degrees.
bool parse_iso6709(const std::string &s, double *lat, double *lon);
// Looks a zone name ("Europe/Berlin") up in a zone1970.tab / zone.tab file.
bool zone_coordinates(const std::string &tab_path, const std::string &zone, double *lat, double *lon);

// "HH:MM" -> minutes after midnight, or -1
int parse_hhmm(const std::string &s);

// How much of the night tint applies at `now` (minutes after midnight) for a
// night from `start` to `end` (wrapping past midnight): 0 by day, 1 at night,
// in between during the `fade` minutes after start and before end.
double night_amount(int now, int start, int end, int fade);

// Temperature for an amount between day (6500 K) and the night temperature.
int blend_temperature(int night_kelvin, double amount);

} // namespace nightlight
