#pragma once
#include <apps/common/key_manager/key_manager.h>
#include <apps/common/loading_page/loading_page.h>
#include <mooncake.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

enum class BleOtaState;

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
    void updateRing(BleOtaState state, std::uint32_t progress);

    std::unique_ptr<input::KeyManager> _key_manager;
    std::unique_ptr<view::LoadingPage> _page;
    lv_obj_t* _ring = nullptr;
    std::string _message;
    BleOtaState _ring_state;
    std::uint32_t _ring_progress   = 0;
    bool _eta_started              = false;
    std::uint32_t _eta_start_tick  = 0;
    std::uint32_t _eta_update_tick = 0;
    std::uint32_t _eta_start_bytes = 0;
    std::uint32_t _success_tick    = 0;
    bool _success_started          = false;
    bool _fade_started             = false;
    std::atomic_bool _fade_complete{false};
};
