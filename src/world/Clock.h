#pragma once
#include <cstdint>
#include <string>

namespace alone {

enum class WeatherKind { Clear, Cloudy, Overcast, Drizzle, Rain, HeavyRain, Fog, Snow };
const char* weatherName(WeatherKind w);

struct Weather {
    WeatherKind kind = WeatherKind::Cloudy;
    float rain = 0;        // 0..1 precipitation intensity
    float cloud = 0.5f;    // 0..1 cover
    float wind = 3;        // m/s
    float fog = 0;         // 0..1
};

// Game time, calendar, sun and weather for Alingsås.
class Clock {
public:
    Clock(uint32_t seed, float latitude, int startDayOfYear = 264, float startHour = 9.0f);

    void advance(double minutes);
    void setTotalMinutes(double m) { minutes_ = m; weatherBlock_ = -1; updateWeather(); }
    double totalMinutes() const { return minutes_; }
    int day() const { return (int)(minutes_ / 1440.0) + 1; }       // day 1, 2, ...
    float hourOfDay() const;                                        // 0..24
    int dayOfYear() const;                                          // 0..364
    std::string dateString() const;                                 // "Tue 21 September"
    std::string timeString() const;                                 // "14:05"

    float sunElevationDeg() const;
    float daylight() const;            // 0 night .. 1 full day (cloud dimmed)
    float moonlight() const;           // 0..1 from the lunar phase, dimmed by cloud
    float outdoorTempC() const;
    float indoorTempC() const;         // unheated houses
    const Weather& weather() const { return weather_; }
    // Keep this weather until the clock reaches `untilMinutes` (screenshots, tests)
    void forceWeather(const Weather& w, double untilMinutes) { weather_ = forced_ = w; forcedUntil_ = untilMinutes; weatherBlock_ = -1; }

private:
    void updateWeather();

    uint32_t seed_;
    float latitude_;
    int startDay_;
    double minutes_;
    Weather weather_, forced_;
    double forcedUntil_ = -1;
    int weatherBlock_ = -1;
};

} // namespace alone
