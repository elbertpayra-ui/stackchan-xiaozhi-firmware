#pragma once

#include <string>
#include <array>
#include <vector>
#include <cstdint>
#include <algorithm>
#include "settings.h"

struct PersonalityAttributes {
    const char* name;
    int servo_scan_min_yaw;
    int servo_scan_max_yaw;
    int servo_scan_pitch;
    uint8_t led_neutral_r;
    uint8_t led_neutral_g;
    uint8_t led_neutral_b;
    int servo_nod_intensity;
    int servo_shake_intensity;
    const char* description;
};

class Personality {
public:
    static constexpr std::array<PersonalityAttributes, 4> PERSONALITIES = {{
        {"Energetic", -45, 45, 35, 255, 180, 0, 80, 80,
         "wide scanning, vibrant LEDs, big enthusiastic nods"},
        {"Calm", -20, 20, 35, 60, 35, 10, 30, 30,
         "narrow scanning, soft warm LEDs, subtle movements"},
        {"Playful", -35, 35, 30, 100, 255, 0, 70, 70,
         "wide scanning, colorful LEDs, frequent playful nods"},
        {"Serious", -15, 15, 35, 80, 80, 80, 40, 40,
         "minimal scanning, neutral gray LEDs, formal demeanor"},
    }};

    static const PersonalityAttributes& GetCurrent() {
        return PERSONALITIES(current_index_);
    }

    static void SetCurrent(int index) {
        if (index >= 0 && index < static_cast<int>(PERSONALITIES.size())) {
            current_index_ = index;
            Save();
        }
    }

    static int GetCurrentIndex() { return current_index_; }
    static int GetCount() { return static_cast<int>(PERSONALITIES.size()); }
    static void Cycle() { current_index_ = (current_index_ + 1) % PERSONALITIES.size(); Save(); }
    static const char* GetName() { return PERSONALITIES[current_index_].name; }

    // Interest tracking — the personality grows through interaction, not idle time.
    // Interests are topics the robot is curious about. The LLM can add/remove them.
    static void AddInterest(const std::string& topic) {
        if (!topic.empty()) {
            auto it = std::find(interests_.begin(), interests_.end(), topic);
            if (it == interests_.end()) {
                interests_.push_back(topic);
                Save();
            }
        }
    }

    static void RemoveInterest(const std::string& topic) {
        auto it = std::find(interests_.begin(), interests_.end(), topic);
        if (it != interests_.end()) {
            interests_.erase(it);
            Save();
        }
    }

    static const std::vector<std::string>& GetInterests() { return interests_; }

    // Curiosity level 0-100 — increases with engagement, persists across idle.
    static void SetCuriosity(int level) {
        if (level < 0) level = 0;
        if (level > 100) level = 100;
        curiosity_ = level;
        Save();
    }

    static int GetCuriosity() { return curiosity_; }
    static const char* GetCuriosityLabel() {
        if (curiosity_ < 20) return "disinterested";
        if (curiosity_ < 50) return "mildly curious";
        if (curiosity_ < 80) return "very curious";
        return "intensely curious";
    }

    // Interaction count — tracks how many conversations the robot has had.
    // Personality grows through engagement, not idle time.
    static void IncrementInteractions() { interactions_++; Save(); }
    static int GetInteractions() { return interactions_; }

    // Attribute computation gate — disables LED/servo attribute
    // updates during sleep or do-not-disturb. Persists across reboots.
    static void SetAttributesActive(bool active) {
        attributes_active_ = active;
        Save();
    }
    static bool AreAttributesActive() { return attributes_active_; }

    // Load/save state from NVS — ensures personality persists across reboots
    // and is NOT affected by idle/sleep time.
    static void Load();
    static void Save();
    static void Reset();  // factory reset personality state

private:
    static int current_index_;
    static std::vector<std::string> interests_;
    static int curiosity_;
    static int interactions_;
    static bool attributes_active_;
};
