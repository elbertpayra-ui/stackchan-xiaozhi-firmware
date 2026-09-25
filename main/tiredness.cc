#include "tiredness.h"
#include "settings.h"

#include <esp_log.h>
#include <math.h>

#define TAG "Tiredness"

static const int kCacheTtlSec = 60;       // Recalculate at most once per minute
static const int kBedtimeStartHour = 23;   // 11 PM
static const int kBedtimeEndHour = 1;     // 1 AM
static const int kSunriseBufferHours = 2;  // Stay sleepy for 2 hours after sunrise

Tiredness::Tiredness() {
    Load();
}

void Tiredness::Load() {
    Settings settings("tiredness", false);
    latitude_ = settings.GetInt("latitude", 0);
    longitude_ = settings.GetInt("longitude", 0);
    server_time_ = settings.GetInt("server_time", 0);
    ESP_LOGI(TAG, "Loaded: lat=%f, lng=%f, server_time=%lld",
             latitude_, longitude_, (long long)server_time_);
}

void Tiredness::Save() {
    Settings settings("tiredness", true);
    settings.SetInt("latitude", static_cast<int32_t>(latitude_));
    settings.SetInt("longitude", static_cast<int32_t>(longitude_));
    settings.SetInt("server_time", static_cast<int32_t>(server_time_));
}

void Tiredness::Reset() {
    latitude_ = 0.0;
    longitude_ = 0.0;
    server_time_ = 0;
    cached_tiredness_ = 0;
    last_calc_time_ = 0;
    cached_sleepy_ = false;
    Save();
}

void Tiredness::SetLocation(double lat, double lng) {
    latitude_ = lat;
    longitude_ = lng;
    Save();
    Recalculate();
}

void Tiredness::SetServerTime(int64_t unix_seconds) {
    server_time_ = unix_seconds;
    Save();
    Recalculate();
}

void Tiredness::Recalculate() {
    last_calc_time_ = 0;  // Force recompute
}

int Tiredness::GetTiredness() {
    if (server_time_ == 0) {
        return 0;
    }

    int64_t now = server_time_;
    if (now - last_calc_time_ < kCacheTtlSec) {
        return cached_tiredness_;
    }

    last_calc_time_ = now;

    // Convert to local-ish hours from UTC
    // The server time is UTC; we approximate local time as UTC + lng/15
    double local_offset = longitude_ / 15.0;  // hours offset from UTC
    double local_time = (double)(now % 86400) / 3600.0 + local_offset;
    if (local_time < 0) local_time += 24.0;
    if (local_time >= 24.0) local_time -= 24.0;

    int hour = (int)local_time;

    // Bedtime tiredness: 11 PM to 1 AM
    // Ramps from ~20 at 10 PM to ~100 at 11 PM, stays ~100 until 1 AM,
    // then ramps down until 3 AM
    int bedtime_tiredness = 0;

    // Ramp UP: 22:00 -> 23:00 (40 -> 100)
    if (hour == 22) {
        float frac = (float)(local_time - hour);
        bedtime_tiredness = (int)(40 + frac * 60);
    }
    // Full sleep: 23:00 -> 01:00
    else if (hour == 23 || hour == 0) {
        bedtime_tiredness = 100;
    }
    // Ramp DOWN: 01:00 -> 03:00
    else if (hour == 1) {
        float frac = (float)(local_time - hour);
        bedtime_tiredness = (int)(100 - frac * 100);
    }

    // Daytime tiredness based on sunrise/sunset
    int daytime_tiredness = 0;

    // Compute sunrise/sunset for today
    int year, month, day;
    DateFromDays((int)(now / 86400), year, month, day);

    float sunrise = CalculateSunriseHour((float)latitude_, (float)longitude_, year, month, day);
    float sunset = CalculateSunsetHour((float)latitude_, (float)longitude_, year, month, day);

    // Sunrise buffer: sleepy for 2 hours after sunrise
    if (local_time >= sunrise && local_time < sunrise + kSunriseBufferHours) {
        float frac = (float)(local_time - sunrise) / kSunriseBufferHours;
        daytime_tiredness = (int)(60 - frac * 60);
    }
    // Sunset ramp: get sleepy 1 hour before sunset
    else if (local_time >= sunset - 1.0f && local_time < sunset) {
        float frac = (float)(local_time - (sunset - 1.0f));
        daytime_tiredness = (int)(frac * 50);
    }
    // After sunset: moderately tired
    else if (local_time >= sunset && local_time < (float)kBedtimeStartHour) {
        daytime_tiredness = 40;
    }

    cached_tiredness_ = bedtime_tiredness > daytime_tiredness ? bedtime_tiredness : daytime_tiredness;

    // Clamp
    if (cached_tiredness_ < 0) cached_tiredness_ = 0;
    if (cached_tiredness_ > 100) cached_tiredness_ = 100;

    ESP_LOGI(TAG, "Tiredness: %d (hour=%.1f, sunrise=%.1f, sunset=%.1f, bed=%d, day=%d)",
             cached_tiredness_, local_time, sunrise, sunset, bedtime_tiredness, daytime_tiredness);

    return cached_tiredness_;
}

bool Tiredness::IsSleepy() {
    if (server_time_ == 0) {
        return false;
    }

    // Recalculate if cache is stale
    if (server_time_ - last_calc_time_ >= kCacheTtlSec) {
        GetTiredness();
    }

    // Sleepy threshold: tiredness >= 80
    return cached_tiredness_ >= 80;
}

// ---- Sunrise equation implementation ----

static float Clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

int Tiredness::DaysSinceEpoch(int year, int month, int day) {
    // Convert YYYY-MM-DD to days since 1970-01-01
    static const int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    
    int days = 0;
    for (int y = 1970; y < year; y++) {
        bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
        days += leap ? 366 : 365;
    }
    for (int m = 1; m < month; m++) {
        days += days_in_month[m - 1];
        if (m == 2) {
            bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
            if (leap) days += 1;
        }
    }
    days += day - 1;
    
    return days;
}

void Tiredness::DateFromDays(int days, int& year, int& month, int& day) {
    year = 1970;
    while (true) {
        bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        int year_days = leap ? 366 : 365;
        if (days < year_days) break;
        days -= year_days;
        year++;
    }
    
    static const int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    month = 1;
    while (true) {
        int m_days = days_in_month[month - 1];
        if (month == 2) {
            bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
            if (leap) m_days = 29;
        }
        if (days < m_days) break;
        days -= m_days;
        month++;
    }
    day = days + 1;
}

float Tiredness::CalculateSunriseHour(float lat, float lng, int year, int month, int day) {
    // Sunrise equation: https://en.wikipedia.org/wiki/Sunrise_equation
    // Returns UTC hour of sunrise
    
    int N = DaysSinceEpoch(year, month, day);
    
    // Solar declination
    float decl = 23.45f * sinf((float)(2 * M_PI * (284 + N) / 365.0));
    
    // Hour angle
    float cos_omega = -tanf(lat * M_PI / 180.0f) * tanf(decl * M_PI / 180.0f);
    cos_omega = Clamp(cos_omega, -1.0f, 1.0f);
    float omega = acosf(cos_omega) * 180.0f / M_PI;
    
    // Solar noon offset (in hours, positive = west)
    float lng_hour = lng / 15.0f;
    
    float sunrise = 12.0f - omega / 15.0f - lng_hour;
    // Normalize to 0-24
    while (sunrise < 0) sunrise += 24.0f;
    while (sunrise >= 24.0f) sunrise -= 24.0f;
    
    return sunrise;
}

float Tiredness::CalculateSunsetHour(float lat, float lng, int year, int month, int day) {
    int N = DaysSinceEpoch(year, month, day);
    
    float decl = 23.45f * sinf((float)(2 * M_PI * (284 + N) / 365.0));
    
    float cos_omega = -tanf(lat * M_PI / 180.0f) * tanf(decl * M_PI / 180.0f);
    cos_omega = Clamp(cos_omega, -1.0f, 1.0f);
    float omega = acosf(cos_omega) * 180.0f / M_PI;
    
    float lng_hour = lng / 15.0f;
    
    float sunset = 12.0f + omega / 15.0f - lng_hour;
    while (sunset < 0) sunset += 24.0f;
    while (sunset >= 24.0f) sunset -= 24.0f;
    
    return sunset;
}
