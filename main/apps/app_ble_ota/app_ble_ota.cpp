#include "app_ble_ota.h"
#include <assets/assets.h>
#include <fmt/format.h>
#include <hal/hal.h>
#include <mooncake_log.h>

namespace {

constexpr std::uint32_t RingWaitingColor   = 0xAEB4B8;
constexpr std::uint32_t RingConnectedColor = 0xF2F2F2;
constexpr std::uint32_t RingReceivingColor = 0xF0A044;
constexpr std::uint32_t RingErrorColor     = 0xFF646B;
constexpr std::uint32_t FadeOutDuration    = 500;

void setRingValue(void* ring, std::int32_t value)
{
    lv_arc_set_value(static_cast<lv_obj_t*>(ring), value);
}

void setRingOpacity(void* ring, std::int32_t opacity)
{
    lv_obj_set_style_arc_opa(static_cast<lv_obj_t*>(ring), static_cast<lv_opa_t>(opacity), LV_PART_INDICATOR);
}

void setBaseOpacity(void* ring, std::int32_t opacity)
{
    lv_obj_set_style_arc_opa(static_cast<lv_obj_t*>(ring), static_cast<lv_opa_t>(opacity), LV_PART_MAIN);
}

void setMessageOpacity(void* page, std::int32_t opacity)
{
    static_cast<view::LoadingPage*>(page)->setMessageOpacity(static_cast<lv_opa_t>(opacity));
}

void finishFade(lv_anim_t* animation)
{
    static_cast<std::atomic_bool*>(lv_anim_get_user_data(animation))->store(true, std::memory_order_release);
}

lv_anim_t makeAnimation(void* target, lv_anim_exec_xcb_t callback, std::int32_t from, std::int32_t to,
                        std::uint32_t duration)
{
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, target);
    lv_anim_set_exec_cb(&animation, callback);
    lv_anim_set_values(&animation, from, to);
    lv_anim_set_duration(&animation, duration);
    return animation;
}

void startBreathing(lv_obj_t* ring)
{
    auto animation = makeAnimation(ring, setRingOpacity, LV_OPA_COVER, LV_OPA_50, 650);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
    lv_anim_set_reverse_duration(&animation, 650);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&animation);
}

void keepErrorVisible(lv_anim_t* animation)
{
    setRingOpacity(animation->var, LV_OPA_80);
}

void pulseError(lv_obj_t* ring)
{
    auto animation = makeAnimation(ring, setRingOpacity, LV_OPA_50, LV_OPA_COVER, 140);
    lv_anim_set_reverse_duration(&animation, 140);
    lv_anim_set_repeat_count(&animation, 1);
    lv_anim_set_completed_cb(&animation, keepErrorVisible);
    lv_anim_start(&animation);
}

void animateProgress(lv_obj_t* ring, std::uint32_t progress)
{
    auto animation = makeAnimation(ring, setRingValue, lv_arc_get_value(ring), progress, 150);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_start(&animation);
}

}  // namespace

AppBleOta::AppBleOta()
{
    setAppInfo().name = "BLE OTA";
    setAppInfo().icon = (void*)&icon_fft;
}

void AppBleOta::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppBleOta::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    _eta_started     = false;
    _success_started = false;
    _fade_started    = false;
    _fade_complete.store(false, std::memory_order_relaxed);
    _key_manager = std::make_unique<input::KeyManager>();
    {
        LvglLockGuard lock;
        _page = std::make_unique<view::LoadingPage>();

        _ring = lv_arc_create(lv_screen_active());
        lv_obj_set_size(_ring, 446, 446);
        lv_obj_center(_ring);
        lv_obj_remove_style(_ring, nullptr, LV_PART_KNOB);
        lv_obj_remove_flag(_ring, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(_ring, 8, LV_PART_MAIN);
        lv_obj_set_style_arc_width(_ring, 8, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(_ring, true, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(_ring, true, LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(_ring, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_color(_ring, lv_color_hex(RingWaitingColor), LV_PART_INDICATOR);
        lv_arc_set_range(_ring, 0, 1000);
        lv_arc_set_bg_angles(_ring, 0, 360);
        lv_arc_set_value(_ring, 1000);
        startBreathing(_ring);
    }
    _ring_state    = BleOtaState::Advertising;
    _ring_progress = 0;
    show("BLE OTA\n\nStarting...");

    GetHAL().startBleOta();
}

void AppBleOta::onRunning()
{
    const auto status = GetHAL().updateBleOta();

    if (_key_manager && _key_manager->update() == input::KeyEvent::GoHome) {
        if (GetHAL().cancelBleOta()) {
            close();
        }
        return;
    }

    const auto progress =
        status.total == 0
            ? 0U
            : static_cast<std::uint32_t>(static_cast<std::uint64_t>(status.transferred) * 1000 / status.total);
    const auto percent = progress / 10;
    updateRing(status.state, progress);

    std::string message;
    switch (status.state) {
        case BleOtaState::Advertising:
            message = fmt::format("BLE OTA\n\n{}\nWaiting for client...\n\nHold A + B to exit", status.device_name);
            break;
        case BleOtaState::Connected:
            message = "BLE OTA\n\nConnected\nWaiting for firmware...\n\nHold A + B to cancel";
            break;
        case BleOtaState::Receiving: {
            const auto now = GetHAL().millis();
            if (!_eta_started || status.transferred < _eta_start_bytes) {
                _eta_started     = true;
                _eta_start_tick  = now;
                _eta_update_tick = now;
                _eta_start_bytes = status.transferred;
            } else if (status.transferred < status.total && now - _eta_update_tick < 1000) {
                return;
            }
            _eta_update_tick = now;

            std::string eta                = "--:--";
            const std::uint64_t elapsed_ms = now - _eta_start_tick;
            const std::uint64_t received   = status.transferred - _eta_start_bytes;
            if (status.total > status.transferred && elapsed_ms >= 1000 && received > 0) {
                const std::uint64_t remaining = status.total - status.transferred;
                const auto remaining_ms       = (remaining * elapsed_ms + received - 1) / received;
                const auto remaining_seconds  = (remaining_ms + 999) / 1000;
                eta = fmt::format("{:02}:{:02}", remaining_seconds / 60, remaining_seconds % 60);
            } else if (status.total != 0 && status.transferred >= status.total) {
                eta = "00:00";
            }

            message = fmt::format("BLE OTA\n\n{}%\n{} / {} bytes\nETA {}\n\nHold A + B to cancel", percent,
                                  status.transferred, status.total, eta);
            break;
        }
        case BleOtaState::Verifying:
            message = "BLE OTA\n\nVerifying firmware...";
            break;
        case BleOtaState::Success: {
            const auto now = GetHAL().millis();
            if (!_success_started) {
                _success_started = true;
                _success_tick    = now;
                show("BLE OTA\n\nUpdate complete\nRestarting...");
            } else if (!_fade_started && now - _success_tick >= 1000) {
                _fade_started = true;
                fadeOut();
            } else if (_fade_complete.load(std::memory_order_acquire)) {
                GetHAL().stopBleOta();
                GetHAL().reboot();
            }
            return;
        }
        case BleOtaState::Error:
            message = fmt::format("BLE OTA failed\n\n{}\n\nHold A + B to exit", status.message);
            break;
    }

    show(std::move(message));
}

void AppBleOta::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    GetHAL().stopBleOta();
    _key_manager.reset();

    LvglLockGuard lock;
    lv_obj_delete(_ring);
    _ring = nullptr;
    lv_anim_delete(_page.get(), nullptr);
    _page.reset();
    _message.clear();
}

void AppBleOta::updateRing(BleOtaState state, std::uint32_t progress)
{
    const bool stateChanged  = state != _ring_state;
    const bool sameAnimation = _ring_state == BleOtaState::Verifying && state == BleOtaState::Success;
    if (sameAnimation) {
        _ring_state = state;
        return;
    }
    if (!stateChanged && (state != BleOtaState::Receiving || progress == _ring_progress)) {
        return;
    }

    LvglLockGuard lock;
    if (stateChanged) {
        lv_anim_delete(_ring, nullptr);
        lv_arc_set_rotation(_ring, 270);
        lv_obj_set_style_arc_opa(_ring, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_color(_ring, lv_color_hex(RingWaitingColor), LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(_ring, LV_OPA_COVER, LV_PART_INDICATOR);

        switch (state) {
            case BleOtaState::Advertising:
                lv_arc_set_value(_ring, 1000);
                startBreathing(_ring);
                break;
            case BleOtaState::Connected: {
                lv_obj_set_style_arc_color(_ring, lv_color_hex(RingConnectedColor), LV_PART_INDICATOR);
                lv_arc_set_value(_ring, 1000);
                auto animation = makeAnimation(_ring, setRingOpacity, LV_OPA_50, LV_OPA_COVER, 180);
                lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
                lv_anim_start(&animation);
                break;
            }
            case BleOtaState::Receiving: {
                lv_obj_set_style_arc_color(_ring, lv_color_hex(RingConnectedColor), LV_PART_MAIN);
                lv_obj_set_style_arc_opa(_ring, LV_OPA_COVER, LV_PART_MAIN);
                lv_obj_set_style_arc_color(_ring, lv_color_hex(RingReceivingColor), LV_PART_INDICATOR);
                lv_arc_set_value(_ring, 0);
                auto fade = makeAnimation(_ring, setBaseOpacity, LV_OPA_COVER, LV_OPA_TRANSP, 300);
                lv_anim_set_path_cb(&fade, lv_anim_path_ease_out);
                lv_anim_start(&fade);
                animateProgress(_ring, progress);
                break;
            }
            case BleOtaState::Verifying:
            case BleOtaState::Success:
                lv_obj_set_style_arc_color(_ring, lv_color_hex(RingReceivingColor), LV_PART_INDICATOR);
                lv_arc_set_value(_ring, 1000);
                startBreathing(_ring);
                break;
            case BleOtaState::Error:
                lv_obj_set_style_arc_color(_ring, lv_color_hex(RingErrorColor), LV_PART_INDICATOR);
                lv_arc_set_value(_ring, 1000);
                pulseError(_ring);
                break;
        }
    } else {
        lv_anim_delete(_ring, setRingValue);
        animateProgress(_ring, progress);
    }

    _ring_state    = state;
    _ring_progress = progress;
}

void AppBleOta::fadeOut()
{
    LvglLockGuard lock;
    lv_anim_delete(_ring, nullptr);

    auto ring = makeAnimation(_ring, setRingOpacity, lv_obj_get_style_arc_opa(_ring, LV_PART_INDICATOR), LV_OPA_TRANSP,
                              FadeOutDuration);
    lv_anim_set_path_cb(&ring, lv_anim_path_ease_out);
    lv_anim_start(&ring);

    auto message = makeAnimation(_page.get(), setMessageOpacity, LV_OPA_COVER, LV_OPA_TRANSP, FadeOutDuration);
    lv_anim_set_path_cb(&message, lv_anim_path_ease_out);
    lv_anim_set_user_data(&message, &_fade_complete);
    lv_anim_set_completed_cb(&message, finishFade);
    lv_anim_start(&message);
}

void AppBleOta::show(std::string message)
{
    if (!_page || message == _message) {
        return;
    }

    _message = std::move(message);
    LvglLockGuard lock;
    _page->setMessage(_message);
}
