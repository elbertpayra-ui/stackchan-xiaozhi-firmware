#pragma once

#include <cstdint>
#include <string>

class Tiredness {
public:
    Tiredness();

    void Load();
    void Save();
    void Reset();

    // Calculate current tiredness (0-100) based on server-provided UTC time.
    // Call once per minute; caches result.
    int GetTiredness();

    // Returns true if the robot should be in sleep mode due to tiredness.
    bool IsSleepy();

    // Geoposition for sunrise/sunset calculation
    void SetLocation(double lat, double lng);
    double GetLatitude() const { return latitude_; }
    double GetLongitude() const { return longitude_; }

    // Server time offset (UTC seconds since epoch, or 0 if unknown)
    void SetServerTime(int64_t unix_seconds);
    int64_t GetServerTime() const { return server_time_; }

    // Force a recalculation (called when time/location changes)
    void Recalculate();

private:
    // Sunrise/sunset calculation using the sunrise equation
    // Returns UTC hour of sunrise/sunset as float
    static float CalculateSunriseHour(float lat, float lng, int year, int month, int day);
    static float CalculateSunsetHour(float lat, float lng, int year, int month, int day);

    static int DaysSinceEpoch(int year, int month, int day);
    static void DateFromDays(int days, int& year, int& month, int& day);

    double latitude_ = 0.0;
    double longitude_ = 0.0;
    int64_t server_time_ = 0;
    int cached_tiredness_ = 0;
    int64_t last_calc_time_ = 0;
    bool cached_sleepy_ = false;
};
