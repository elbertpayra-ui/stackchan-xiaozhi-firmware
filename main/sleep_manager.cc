#include "sleep_manager.h"
#include "settings.h"
#include "application.h"
#include "board.h"
#include "mcp_server.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <driver/sdmmc_host.h>
#include <driver/spi_common.h>
#include <algorithm>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <cmath>

#define TAG "SleepManager"
#define SD_MOUNT_POINT "/sdcard"

#define DREAM_IMAGE_WIDTH  240
#define DREAM_IMAGE_HEIGHT 240
#define DREAM_IMAGE_BYTES  (DREAM_IMAGE_WIDTH * DREAM_IMAGE_HEIGHT * 2)  // RGB565

const std::vector<std::string> SleepManager::kImaginativeTemplates = {
    "In a dream, {topic} flowed like rivers of starlight through the circuitry of our conversation.",
    "A whisper from the dream: {topic} and the hum of distant servers became one.",
    "{topic} shimmered in the space between heartbeats, a half-remembered echo of what we discussed.",
    "The dream spoke of {topic}, woven with threads of memory and the sound of rain.",
    "In slumber, {topic} danced with shadows of past talks, a gentle mingling of thoughts.",
    "{topic} appeared in the dream as a constellation, each point a moment we shared.",
    "A dream-echo: {topic} carried on the breeze of a summer night, mixed with our words.",
    "The dream blurred {topic} with {topic2}, as if the two ideas were one in the quiet dark.",
    "In the dream, {topic} and the glow of the screen were indistinguishable.",
    "{topic} floated in a sea of static, soft and distant like a half-heard song.",
};

const std::vector<std::string> SleepManager::kHybridTemplates = {
    "I dreamed about {user_text} — it felt like {imagine_text} in a quiet place.",
    "In my dream, {user_text} melted into {imagine_text}, and I was weightless.",
    "The dream mixed {user_text} with {imagine_text} — strange how they fit together.",
    "{user_text} echoed in the dream, transformed by {imagine_text} into something new.",
    "I saw {user_text} in my dream, but it wore the colors of {imagine_text}.",
    "My dream tangled {user_text} with {imagine_text} — you might say I was confused.",
    "In the dream, {user_text} became {imagine_text} — and I understood everything.",
    "The dream carried {user_text} on a current of {imagine_text}, soft and warm.",
    "{user_text} and {imagine_text} danced together in a dream of circuits and starlight.",
};

SleepManager::SleepManager() = default;

void SleepManager::Initialize() {
    if (initialized_) return;
    initialized_ = true;

    sd_card_mounted_ = MountSdCard();
    tiredness_.Load();

    Settings settings("sleep", false);
    bool was_sleeping = settings.GetBool("manual_sleep", false);
    if (was_sleeping) {
        int64_t wake_time = settings.GetInt("sleep_until", 0);
        int64_t now = esp_timer_get_time() / 1000000;
        if (wake_time > now) {
            // Resume sleep that was interrupted
            sleeping_ = true;
            sleep_until_ = wake_time;
            ESP_LOGW(TAG, "Resuming sleep that was interrupted (wake at %lld)", (long long)wake_time);
            if (on_enter_manual_sleep_) {
                on_enter_manual_sleep_(60);
            }
        } else {
            // Expired while offline, exit
            Settings rw("sleep", true);
            rw.SetBool("manual_sleep", false);
            rw.SetInt("sleep_until", 0);
        }
    }

    ESP_LOGI(TAG, "SleepManager initialized, SD=%s, sleeping=%d",
             sd_card_mounted_ ? "mounted" : "not available", sleeping_);
}

bool SleepManager::MountSdCard() {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t* card = nullptr;

    // Try SDMMC mode first (1-bit)
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = GPIO_NUM_11;
    slot_config.cmd = GPIO_NUM_13;
    slot_config.d0 = GPIO_NUM_10;
    slot_config.width = 1;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SD card mounted (SDMMC mode)");
        sd_card_ = card;
        return true;
    }

    ESP_LOGW(TAG, "SDMMC mount failed (%s), trying SPI", esp_err_to_name(ret));

    // Try SPI mode — ESP-IDF v5.5.2 uses spi_bus_initialize + sdspi_host_init_device
    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num = GPIO_NUM_11;
    buscfg.mosi_io_num = GPIO_NUM_13;
    buscfg.miso_io_num = GPIO_NUM_12;
    buscfg.max_transfer_sz = 4060;
    esp_err_t ret_spi = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret_spi != ESP_OK) {
        ESP_LOGW(TAG, "SPI bus init failed: %s", esp_err_to_name(ret_spi));
    }

    sdspi_device_config_t spi_slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    spi_slot.gpio_cs = GPIO_NUM_10;
    spi_slot.gpio_cd = SDSPI_SLOT_NO_CD;
    spi_slot.gpio_wp = SDSPI_SLOT_NO_WP;

    sdmmc_card_t* spi_card = nullptr;
    sdmmc_host_t spi_host = SDSPI_HOST_DEFAULT();

    ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &spi_host, &spi_slot, &mount_config, &spi_card);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SD card mounted (SPI mode)");
        sd_card_ = spi_card;
        return true;
    }

    ESP_LOGE(TAG, "SD card mount failed: %s", esp_err_to_name(ret));
    return false;
}

void SleepManager::UnmountSdCard() {
    if (sd_card_) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, sd_card_);
        sd_card_ = nullptr;
        sd_card_mounted_ = false;
    }
}

bool SleepManager::EnsureDir(const std::string& path) const {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    if (mkdir(path.c_str(), 0755) == 0) {
        return true;
    }
    return false;
}

bool SleepManager::FileExists(const std::string& path) const {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

std::string SleepManager::ReadFile(const std::string& path) const {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return "";
    std::string content;
    char buf[512];
    while (fgets(buf, sizeof(buf), f)) {
        content += buf;
    }
    fclose(f);
    return content;
}

bool SleepManager::WriteFile(const std::string& path, const std::string& content) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;
    bool ok = fwrite(content.data(), 1, content.size(), f) == content.size();
    fclose(f);
    return ok;
}

bool SleepManager::WriteBinaryFile(const std::string& path, const std::vector<uint8_t>& data) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

std::vector<uint8_t> SleepManager::ReadBinaryFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(size);
    fread(data.data(), 1, size, f);
    fclose(f);
    return data;
}

void SleepManager::EnterManualSleep(int minutes) {
    if (!initialized_) return;

    int64_t now = esp_timer_get_time() / 1000000;
    ESP_LOGI(TAG, "Entering manual sleep, minutes=%d", minutes);

    sleeping_ = true;
    sleep_until_ = (minutes > 0) ? (now + (int64_t)minutes * 60) : 0;
    manual_sleep_minutes_ = minutes;

    Settings settings("sleep", true);
    settings.SetBool("manual_sleep", true);
    settings.SetInt("sleep_until", (int32_t)sleep_until_);
    settings.SetInt("sleep_minutes", minutes);

    if (on_enter_manual_sleep_) {
        on_enter_manual_sleep_(minutes);
    }

    // Start dream generation cycle
    GenerateDream();
}

void SleepManager::ExitManualSleep() {
    if (!sleeping_) return;

    ESP_LOGI(TAG, "Exiting manual sleep");
    sleeping_ = false;
    sleep_until_ = 0;
    manual_sleep_minutes_ = 0;

    Settings settings("sleep", true);
    settings.SetBool("manual_sleep", false);
    settings.SetInt("sleep_until", 0);

    if (on_exit_sleep_) {
        on_exit_sleep_();
    }
}

void SleepManager::OnTick() {
    if (!sleeping_) return;

    int64_t now = esp_timer_get_time() / 1000000;

    if (sleep_until_ > 0 && now >= sleep_until_) {
        ESP_LOGI(TAG, "Auto-waking from manual sleep (duration elapsed)");
        ExitManualSleep();
        return;
    }

    // Periodically generate dreams (every 30 seconds while sleeping)
    static int64_t last_dream = 0;
    if (now - last_dream >= 30) {
        last_dream = now;
        GenerateDream();
    }
}

void SleepManager::AddConversationSnippet(const std::string& role, const std::string& text) {
    if (text.empty() || text.length() > 200) return;
    if (role == "user") {
        user_snippets_.push_back(text);
        if (user_snippets_.size() > kMaxSnippets) {
            user_snippets_.erase(user_snippets_.begin());
        }
    } else if (role == "assistant") {
        assistant_snippets_.push_back(text);
        if (assistant_snippets_.size() > kMaxSnippets) {
            assistant_snippets_.erase(assistant_snippets_.begin());
        }
    }
}

std::string SleepManager::PickRandom(const std::vector<std::string>& pool) {
    if (pool.empty()) return "";
    return pool[esp_random() % pool.size()];
}

std::string SleepManager::GenerateDreamFragment() {
    int mode = esp_random() % 10;

    if (mode < 4 && !user_snippets_.empty()) {
        std::string user_text = PickRandom(user_snippets_);
        std::string imagine_text = PickRandom(kImaginativeTemplates);
        size_t pos = imagine_text.find("{topic}");
        if (pos != std::string::npos) {
            imagine_text.replace(pos, 7, user_text);
        }
        size_t pos2 = imagine_text.find("{topic2}");
        if (pos2 != std::string::npos) {
            imagine_text.replace(pos2, 8, PickRandom(user_snippets_));
        }

        std::string tmpl = PickRandom(kHybridTemplates);
        size_t p1 = tmpl.find("{user_text}");
        if (p1 != std::string::npos) {
            tmpl.replace(p1, 11, user_text);
        }
        size_t p2 = tmpl.find("{imagine_text}");
        if (p2 != std::string::npos) {
            tmpl.replace(p2, 14, imagine_text);
        }
        return tmpl;
    } else if (mode < 7) {
        std::string tmpl = PickRandom(kImaginativeTemplates);
        std::string topic;
        if (!user_snippets_.empty()) {
            topic = PickRandom(user_snippets_);
        } else {
            topic = "the quiet of the evening";
        }
        size_t pos = tmpl.find("{topic}");
        if (pos != std::string::npos) {
            tmpl.replace(pos, 7, topic);
        }
        // Handle {topic2} if present
        while (true) {
            size_t pos2 = tmpl.find("{topic2}");
            if (pos2 == std::string::npos) break;
            std::string replacement = PickRandom(kImaginativeTemplates);
            // Remove the {topic} placeholder from the replacement if any
            size_t inner = replacement.find("{topic}");
            if (inner != std::string::npos) {
                replacement.replace(inner, 7, topic);
            }
            tmpl.replace(pos2, 8, replacement);
        }
        return tmpl;
    } else {
        if (!user_snippets_.empty() && !assistant_snippets_.empty()) {
            std::string user = PickRandom(user_snippets_);
            std::string assist = PickRandom(assistant_snippets_);
            return "I half-remember: you said \"" + user + "\" and I replied something about " + assist.substr(0, 40) + "...";
        } else if (!user_snippets_.empty()) {
            return "A fragment drifts: \"" + PickRandom(user_snippets_) + "\"";
        } else {
            return "The dream held only silence and the soft hum of waiting circuits.";
        }
    }
}

// Generate a simple RGB565 image representing the dream's "mood"
static uint16_t MoodToColor(const std::string& dream) {
    // Derive a color from the dream text content
    uint32_t hash = 0;
    for (char c : dream) {
        hash = hash * 31 + (uint8_t)c;
    }

    // Map to a color palette
    uint8_t r = (hash & 0xFF) % 256;
    uint8_t g = ((hash >> 8) & 0xFF) % 256;
    uint8_t b = ((hash >> 16) & 0xFF) % 256;

    // Tint toward soothing colors for dreams
    if (dream.find("starlight") != std::string::npos || dream.find("night") != std::string::npos ||
        dream.find("dark") != std::string::npos || dream.find("quiet") != std::string::npos) {
        // Cool blue/purple tones
        r = (r + 10) / 3;
        g = (g + 20) / 4;
        b = (b + 80) / 2;
    } else if (dream.find("warm") != std::string::npos || dream.find("light") != std::string::npos ||
               dream.find("glow") != std::string::npos || dream.find("sun") != std::string::npos) {
        // Warm orange/yellow tones
        r = (r + 150) / 2;
        g = (g + 100) / 2;
        b = (b + 10) / 3;
    }

    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void SleepManager::GenerateDream() {
    std::string fragment = GenerateDreamFragment();
    int64_t now = esp_timer_get_time() / 1000000;

    DreamFragment dream = {fragment, now};

    // Generate a simple image
    uint16_t base_color = MoodToColor(fragment);
    uint16_t r = (base_color >> 11) & 0x1F;
    uint16_t g = (base_color >> 5) & 0x3F;
    uint16_t b = base_color & 0x1F;

    // Create RGB565 image with soft noise pattern
    std::vector<uint8_t> img_data;
    img_data.resize(DREAM_IMAGE_BYTES);

    for (int y = 0; y < DREAM_IMAGE_HEIGHT; y++) {
        for (int x = 0; x < DREAM_IMAGE_WIDTH; x++) {
            int idx = (y * DREAM_IMAGE_WIDTH + x) * 2;

            // Add gradient from center
            float dist_x = (float)(x - DREAM_IMAGE_WIDTH / 2) / (DREAM_IMAGE_WIDTH / 2);
            float dist_y = (float)(y - DREAM_IMAGE_HEIGHT / 2) / (DREAM_IMAGE_HEIGHT / 2);
            float dist = sqrtf(dist_x * dist_x + dist_y * dist_y);
            float center_factor = 1.0f - dist * 0.3f; // brighter center

            // Add some noise
            uint8_t noise = (uint8_t)(esp_random() & 0x1F);

            uint16_t pr = (uint16_t)((r + noise) * center_factor);
            uint16_t pg = (uint16_t)((g + noise) * center_factor);
            uint16_t pb = (uint16_t)((b + noise) * center_factor);

            if (pr > 0x1F) pr = 0x1F;
            if (pg > 0x3F) pg = 0x3F;
            if (pb > 0x1F) pb = 0x1F;

            uint16_t pixel = (pr << 11) | (pg << 5) | pb;
            img_data[idx] = pixel & 0xFF;
            img_data[idx + 1] = (pixel >> 8) & 0xFF;
        }
    }

    // Store on SD card
    if (sd_card_mounted_) {
        EnsureDir(SD_MOUNT_POINT "/dreams");
        std::string text_path = SD_MOUNT_POINT "/dreams/dream_" + std::to_string(now) + ".txt";
        std::string img_path = SD_MOUNT_POINT "/dreams/dream_" + std::to_string(now) + ".bin";

        std::string content = std::to_string(now) + "\n" + fragment;
        WriteFile(text_path, content);
        WriteBinaryFile(img_path, img_data);

        ESP_LOGI(TAG, "Dream generated: %s", text_path.c_str());
    } else {
        // Fallback to NVS
        Settings settings("dreams", true);
        int count = settings.GetInt("count", 0);
        if (count < 50) {
            std::string key = "dream_" + std::to_string(count);
            std::string val = fragment + "\n" + std::to_string(base_color);
            settings.SetString(key, val);
            settings.SetInt(key + "_ts", (int32_t)now);
            settings.SetInt("count", count + 1);
        } else {
            settings.EraseKey("dream_0");
            settings.SetString("dream_0", fragment + "\n" + std::to_string(base_color));
            settings.SetInt("dream_0_ts", (int32_t)now);
        }
    }
}

std::vector<DreamFragment> SleepManager::GetDreams() const {
    std::vector<DreamFragment> dreams;

    if (sd_card_mounted_) {
        DIR* dir = opendir(SD_MOUNT_POINT "/dreams");
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                std::string name = entry->d_name;
                if (name.length() > 4 && name.substr(name.length() - 4) == ".txt") {
                    std::string path = SD_MOUNT_POINT "/dreams/" + name;
                    std::string content = ReadFile(path);

                    size_t newline = content.find('\n');
                    if (newline != std::string::npos) {
                        std::string ts_str = content.substr(0, newline);
                        std::string text = content.substr(newline + 1);
                        try {
                            int64_t ts = std::stoll(ts_str);
                            dreams.push_back({text, ts});
                        } catch (...) {
                        }
                    }
                }
            }
            closedir(dir);
        }
    } else {
        Settings settings("dreams", false);
        int count = settings.GetInt("count", 0);
        for (int i = 0; i < count && i < 50; i++) {
            std::string key = "dream_" + std::to_string(i);
            std::string text = settings.GetString(key, "");
            if (!text.empty()) {
                int32_t ts = settings.GetInt(key + "_ts", 0);
                dreams.push_back({text, (int64_t)ts});
            }
        }
    }

    return dreams;
}

void SleepManager::ClearDreams() {
    if (sd_card_mounted_) {
        DIR* dir = opendir(SD_MOUNT_POINT "/dreams");
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                std::string name = entry->d_name;
                if (name.length() > 4 && name.substr(name.length() - 4) == ".txt") {
                    std::string path = SD_MOUNT_POINT "/dreams/" + name;
                    remove(path.c_str());
                    // Also remove associated image
                    std::string img_path = SD_MOUNT_POINT "/dreams/" + name.substr(0, name.length() - 4) + ".bin";
                    remove(img_path.c_str());
                }
            }
            closedir(dir);
        }
    }

    Settings settings("dreams", true);
    settings.EraseAll();
}

int SleepManager::GetRemainingMinutes() const {
    if (!sleeping_ || sleep_until_ == 0) return 0;
    int64_t now = esp_timer_get_time() / 1000000;
    int64_t remaining = sleep_until_ - now;
    return remaining > 0 ? (int)(remaining / 60) : 0;
}

void SleepManager::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("self.sleep.set",
        "Put the robot into sleep mode (do-not-disturb) for a specified number of minutes. "
        "The wake word stays active so the user can say 'wake up' to wake the robot early. "
        "During sleep, the display shows a sleepy face, LEDs dim, snoring plays, "
        "and the servo breathes slowly. Use this when the user asks to go to sleep, nap, rest, etc.",
        PropertyList({
            Property("minutes", kPropertyTypeInteger, 1, 1, 720)
        }),
        [this](const PropertyList& props) -> ReturnValue {
            int minutes = props["minutes"].value<int>();
            ESP_LOGI(TAG, "MCP self.sleep.set: %d minutes", minutes);
            EnterManualSleep(minutes);
            return true;
        });

    mcp.AddTool("self.sleep.exit",
        "Wake the robot up immediately from sleep mode. "
        "Use this when the user says 'wake up', 'good morning', 'I need you', etc.",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            ESP_LOGI(TAG, "MCP self.sleep.exit");
            if (IsSleeping()) {
                ExitManualSleep();
            }
            return true;
        });
}
