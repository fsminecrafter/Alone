#include "world/Clock.h"
#include "core/Math.h"
#include "core/Noise.h"
#include "core/Rng.h"
#include <cmath>
#include <cstdio>

namespace alone {

const char* weatherName(WeatherKind w) {
    static const char* n[] = {"Clear", "Partly cloudy", "Overcast", "Drizzle", "Rain", "Heavy rain", "Fog", "Snow"};
    return n[(int)w];
}

namespace {
// Alingsås monthly mean temperatures (approx. SMHI normals), January..December
const float kMonthlyMean[12] = {-1.5f, -1.5f, 1.0f, 5.5f, 10.5f, 14.5f, 16.5f, 16.0f, 12.0f, 7.5f, 3.5f, 0.5f};
const int kMonthStart[13] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365};
const char* kMonthNames[12] = {"January", "February", "March", "April", "May", "June", "July",
                               "August", "September", "October", "November", "December"};
const char* kDayNames[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
}

Clock::Clock(uint32_t seed, float latitude, int startDayOfYear, float startHour)
    : seed_(seed), latitude_(latitude), startDay_(startDayOfYear), minutes_(startHour * 60.0) {
    updateWeather();
}

void Clock::advance(double minutes) {
    minutes_ += minutes;
    updateWeather();
}

float Clock::hourOfDay() const { return (float)std::fmod(minutes_ / 60.0, 24.0); }

int Clock::dayOfYear() const { return (startDay_ + (int)(minutes_ / 1440.0)) % 365; }

std::string Clock::dateString() const {
    int doy = dayOfYear();
    int m = 0;
    while (m < 11 && doy >= kMonthStart[m + 1]) ++m;
    // 2026-01-01 was a Thursday
    int absDay = startDay_ + (int)(minutes_ / 1440.0);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s %d %s", kDayNames[(absDay + 3) % 7], doy - kMonthStart[m] + 1, kMonthNames[m]);
    return buf;
}

std::string Clock::timeString() const {
    float h = hourOfDay();
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02d:%02d", (int)h, (int)((h - (int)h) * 60));
    return buf;
}

float Clock::sunElevationDeg() const {
    float doy = (float)dayOfYear() + hourOfDay() / 24.0f;
    float decl = -23.44f * std::cos(2 * kPi / 365.0f * (doy + 10));
    // Solar noon in Alingsås is roughly 12:10 local standard time (13:10 summer time)
    bool summer = doy > 88 && doy < 300;
    float solarHour = hourOfDay() - (summer ? 1.15f : 0.15f);
    float ha = (solarHour - 12.0f) * 15.0f;
    float lat = latitude_ * kPi / 180, d = decl * kPi / 180, h = ha * kPi / 180;
    float s = std::sin(lat) * std::sin(d) + std::cos(lat) * std::cos(d) * std::cos(h);
    return std::asin(std::clamp(s, -1.0f, 1.0f)) * 180.0f / kPi;
}

float Clock::daylight() const {
    float e = sunElevationDeg();
    float l = smoothstep(-8.0f, 12.0f, e);   // civil twilight to full day
    l *= 1.0f - 0.35f * weather_.cloud;
    return clamp01(l);
}

float Clock::moonlight() const {
    // Days since a known new moon (2026-01-18 19:52 UTC), synodic month 29.53 days
    double day2026 = startDay_ + minutes_ / 1440.0;           // days since 1 Jan 2026
    double phase = std::fmod(day2026 - 17.83, 29.530589) / 29.530589;
    if (phase < 0) phase += 1;
    float illum = (float)(0.5 * (1 - std::cos(phase * 2 * 3.14159265)));
    // Roughly: the moon is up for half the night, more so when it is full
    return illum * (1.0f - 0.75f * weather_.cloud);
}

float Clock::outdoorTempC() const {
    float doy = (float)dayOfYear() + hourOfDay() / 24.0f;
    // interpolate monthly means at mid-month
    float monthPos = 0;
    for (int m = 0; m < 12; ++m)
        if (doy >= kMonthStart[m]) monthPos = m + (doy - kMonthStart[m]) / (float)(kMonthStart[m + 1] - kMonthStart[m]);
    monthPos -= 0.5f;
    int m0 = ((int)std::floor(monthPos) + 12) % 12, m1 = (m0 + 1) % 12;
    float t = monthPos - std::floor(monthPos);
    float mean = lerp(kMonthlyMean[m0], kMonthlyMean[m1], t);
    // Daily swing, coldest around 05, warmest around 15; smaller under clouds
    float amp = lerp(4.5f, 1.8f, weather_.cloud);
    float swing = -std::cos((hourOfDay() - 4.0f) / 24.0f * 2 * kPi) * amp;
    // Weather systems: slow random departures of several degrees
    float anomaly = (fbm((float)(minutes_ / 1440.0) * 0.4f, 3.1f, seed_ + 404, 3) - 0.5f) * 10.0f;
    return mean + swing + anomaly - weather_.rain * 1.5f;
}

float Clock::indoorTempC() const {
    // Unheated buildings lag the outdoors and stay a few degrees warmer
    float doy = (float)dayOfYear();
    float mean = 0;
    for (int m = 0; m < 12; ++m)
        if (doy >= kMonthStart[m] && doy < kMonthStart[m + 1]) mean = kMonthlyMean[m];
    return std::max(outdoorTempC(), mean + 4.0f);
}

void Clock::updateWeather() {
    if (minutes_ < forcedUntil_) { weather_ = forced_; return; }
    // Weather changes in blocks of ~3 hours, smoothly varying underneath
    int block = (int)(minutes_ / 180.0);
    if (block == weatherBlock_) return;
    weatherBlock_ = block;
    float t = (float)(minutes_ / 1440.0);
    float wet = fbm(t * 1.3f, 7.7f, seed_ + 1, 3);          // > 0.55 rainy (autumn is wet)
    float cloud = fbm(t * 1.1f, 1.3f, seed_ + 2, 3);
    float windN = fbm(t * 2.0f, 4.4f, seed_ + 3, 2);
    float fogN = fbm(t * 3.0f, 9.9f, seed_ + 4, 2);
    Weather w;
    w.cloud = clamp01(cloud * 1.4f - 0.1f);
    w.wind = 1.0f + windN * 9.0f;
    if (wet > 0.66f) { w.kind = WeatherKind::HeavyRain; w.rain = 1.0f; w.cloud = 1; }
    else if (wet > 0.58f) { w.kind = WeatherKind::Rain; w.rain = 0.6f; w.cloud = 1; }
    else if (wet > 0.53f) { w.kind = WeatherKind::Drizzle; w.rain = 0.25f; w.cloud = std::max(w.cloud, 0.8f); }
    else if (fogN > 0.68f && hourOfDay() < 11) { w.kind = WeatherKind::Fog; w.fog = 0.8f; w.cloud = 0.9f; }
    else if (w.cloud > 0.75f) w.kind = WeatherKind::Overcast;
    else if (w.cloud > 0.35f) w.kind = WeatherKind::Cloudy;
    else w.kind = WeatherKind::Clear;
    weather_ = w;
    if (weather_.rain > 0 && outdoorTempC() < 0.5f) weather_.kind = WeatherKind::Snow;
}

} // namespace alone
