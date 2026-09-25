#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include "tiredness.h"

struct DreamFragment {
    std::string text;
    int64_t timestamp;  // unix seconds when the dream was generated
};

class SleepManager {
public:
    static SleepManager& GetInstance() {
        static SleepManager instance;
        return instance;
    }

    SleepManager(const SleepManager&) = delete;
    SleepManager& operator=(const SleepManager&) = delete;

    // Board provides callbacks for visual/audio changes during sleep
    using EnterManualSleepCallback = std::function<void(int minutes)>;
    using ExitSleepCallback = std::function<void()>;

    void Initialize();

    // Callbacks set by the board during initialization
    void OnEnterManualSleep(EnterManualSleepCallback cb) { on_enter_manual_sleep_ = cb; }
    void OnExitSleep(ExitSleepCallback cb) { on_exit_sleep_ = cb; }

    // Manual sleep (do-not-disturb). Wake word stays active.
    // minutes=0 means sleep indefinitely until "wake up" command
    void EnterManualSleep(int minutes);
    void ExitManualSleep();

    // Returns true if currently in manual sleep mode
    bool IsSleeping() const { return sleeping_; }
    int GetRemainingMinutes() const;

    // Called periodically (every 1s) from the board task
    void OnTick();

    // Dream generation
    void AddConversationSnippet(const std::string& role, const std::string& text);
    void GenerateDream();
    std::vector<DreamFragment> GetDreams() const;
    void ClearDreams();

    // Tiredness access
    Tiredness& GetTiredness() { return tiredness_; }

private:
    SleepManager();
    ~SleepManager() = default;

    // SD card helpers
    bool MountSdCard();
    void UnmountSdCard();
    std::string ReadFile(const std::string& path);
    bool WriteFile(const std::string& path, const std::string& content);
    bool WriteBinaryFile(const std::string& path, const std::vector<uint8_t>& data);
    std::vector<uint8_t> ReadBinaryFile(const std::string& path);
    bool FileExists(const std::string& path);
    bool EnsureDir(const std::string& path);

    // Dream generation helpers
    std::string GenerateDreamFragment();
    std::string PickRandom(const std::vector<std::string>& pool);

    // State
    bool initialized_ = false;
    bool sd_card_mounted_ = false;
    bool sleeping_ = false;
    int64_t sleep_until_ = 0;  // unix timestamp to wake, 0 = no auto-wake
    int manual_sleep_minutes_ = 0;

    Tiredness tiredness_;

    // Recent conversation snippets for dream generation
    std::vector<std::string> user_snippets_;
    std::vector<std::string> assistant_snippets_;
    static constexpr size_t kMaxSnippets = 30;

    EnterManualSleepCallback on_enter_manual_sleep_;
    ExitSleepCallback on_exit_sleep_;

    // Dream templates
    static const std::vector<std::string> kImaginativeTemplates;
    static const std::vector<std::string> kHybridTemplates;
};
