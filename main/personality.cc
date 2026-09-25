#include "personality.h"

#include <esp_log.h>

#define TAG "Personality"

int Personality::current_index_ = 0;
std::vector<std::string> Personality::interests_;
int Personality::curiosity_ = 50;
int Personality::interactions_ = 0;
bool Personality::attributes_active_ = true;

void Personality::Load() {
    Settings settings("personality", false);
    current_index_ = settings.GetInt("current_index", 0);
    if (current_index_ < 0 || current_index_ >= static_cast<int>(PERSONALITIES.size())) {
        current_index_ = 0;
    }
    curiosity_ = settings.GetInt("curiosity", 50);
    if (curiosity_ < 0) curiosity_ = 0;
    if (curiosity_ > 100) curiosity_ = 100;
    interactions_ = settings.GetInt("interactions", 0);
    attributes_active_ = settings.GetBool("attributes_active", true);

    // Load interests
    interests_.clear();
    int interest_count = settings.GetInt("interest_count", 0);
    for (int i = 0; i < interest_count && i < 20; i++) {
        std::string key = "interest_" + std::to_string(i);
        std::string topic = settings.GetString(key, "");
        if (!topic.empty()) {
            interests_.push_back(topic);
        }
    }

    ESP_LOGI(TAG, "Loaded: personality=%d, curiosity=%d, interactions=%d, active=%d, interests=%d",
             current_index_, curiosity_, interactions_, attributes_active_,
             static_cast<int>(interests_.size()));
}

void Personality::Save() {
    Settings settings("personality", true);
    settings.SetInt("current_index", current_index_);
    settings.SetInt("curiosity", curiosity_);
    settings.SetInt("interactions", interactions_);
    settings.SetBool("attributes_active", attributes_active_);

    settings.SetInt("interest_count", static_cast<int>(interests_.size()));
    for (int i = 0; i < static_cast<int>(interests_.size()) && i < 20; i++) {
        std::string key = "interest_" + std::to_string(i);
        settings.SetString(key, interests_[i]);
    }
}

void Personality::Reset() {
    current_index_ = 0;
    interests_.clear();
    curiosity_ = 50;
    interactions_ = 0;
    attributes_active_ = true;
    Save();
    ESP_LOGI(TAG, "Personality reset to defaults");
}
