#pragma once
#include <apps/common/loading_page/loading_page.h>
#include <mooncake.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

enum class BleOtaState;
enum class BleOtaMode;

class AppBleOta : public mooncake::AppAbility {
public:
    AppBleOta();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    void fadeOut();
    void show(std::string message);
    void animateRingColor(std::uint32_t color);
    static void setRingColor(void* app, std::int32_t mix);
    void updateRing(BleOtaState state, BleOtaMode mode, std::uint32_t progress);

    std::unique_ptr<view::LoadingPage> _page;
    lv_obj_t* _ring = nullptr;
    std::string _message;
    BleOtaState _ring_state;
    BleOtaMode _ring_mode;
    std::uint32_t _ring_progress   = 0;
    std::uint32_t _ring_color_from = 0;
    std::uint32_t _ring_color_to   = 0;
    bool _buttons_latched          = false;
    bool _eta_started              = false;
    std::uint32_t _eta_start_tick  = 0;
    std::uint32_t _eta_update_tick = 0;
    std::uint32_t _eta_start_bytes = 0;
    std::uint32_t _success_tick    = 0;
    bool _success_started          = false;
    bool _fade_started             = false;
    std::atomic_bool _fade_complete{false};
};
