#include "hal.h"
#include "utils/ble_ota/ble_ota_worker.h"

#include <cstring>
#include <memory>
#include <mutex>

#include <esp_ble_conn_mgr.h>
#include <esp_ble_ota_raw.h>
#include <esp_ble_ota_svc.h>
#include <esp_app_format.h>
#include <esp_bt.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <fmt/format.h>

namespace {

constexpr char Tag[]                   = "BLE OTA";
constexpr char ProjectName[]           = "StopWatch-UserDemo";
constexpr uint32_t InactivityTimeoutMs = 120000;

class BleOtaSession;
std::unique_ptr<BleOtaSession> session;

class BleOtaSession {
public:
    BleOtaSession()
    {
        const auto mac      = GetHAL().getFactoryMac();
        _status.device_name = fmt::format("M5StopWatch-{:02X}{:02X}", mac[4], mac[5]);
    }

    ~BleOtaSession()
    {
        stopTransport();
    }

    void start()
    {
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            setError("Event loop unavailable");
            stopTransport();
            return;
        }

        err = esp_event_handler_register(BLE_CONN_MGR_EVENTS, ESP_EVENT_ANY_ID, eventHandler, this);
        if (err != ESP_OK) {
            setError("BLE event setup failed");
            stopTransport();
            return;
        }
        _handler_registered = true;

        esp_ble_conn_config_t config = {};
        strlcpy(reinterpret_cast<char*>(config.device_name), _status.device_name.c_str(), sizeof(config.device_name));
        config.include_service_uuid = 1;
        config.adv_uuid_type        = BLE_CONN_UUID_TYPE_16;
        config.adv_uuid16           = BLE_OTA_SERVICE_UUID16;

        err = esp_ble_conn_init(&config);
        if (err != ESP_OK) {
            setError("BLE initialization failed");
            stopTransport();
            return;
        }
        _manager_initialized = true;

        err = esp_ble_ota_raw_init();
        if (err != ESP_OK) {
            setError("OTA service setup failed");
            stopTransport();
            return;
        }
        _profile_initialized = true;

        const ble_ota_worker_config_t worker_config = {
            .project_name = ProjectName,
            .chip_id      = ESP_CHIP_ID_ESP32S3,
        };
        err = ble_ota_worker_init(&worker_config);
        if (err != ESP_OK) {
            setError("OTA worker setup failed");
            stopTransport();
            return;
        }
        _worker_initialized = true;

        {
            std::lock_guard<std::mutex> lock(_mutex);
            _status.state = BleOtaState::Advertising;
            touchLocked();
        }
        err = esp_ble_conn_start();
        if (err != ESP_OK || esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED) {
            setError("BLE start failed");
            ble_ota_worker_stop();
            ESP_LOGE(Tag, "BLE start failed (%d); restarting", err);
            esp_restart();
        }
        _manager_started = true;
        ESP_LOGI(Tag, "advertising as %s", _status.device_name.c_str());
    }

    BleOtaStatus update()
    {
        bool terminal;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            terminal = _status.state == BleOtaState::Success || _status.state == BleOtaState::Error;
        }

        ble_ota_worker_snapshot_t snapshot = {};
        if (!terminal && _worker_initialized && ble_ota_worker_get_snapshot(&snapshot) == ESP_OK) {
            applySnapshot(snapshot);
        }

        bool timeout;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            timeout = !_stop_requested && _status.state != BleOtaState::Success &&
                      _status.state != BleOtaState::Error && nowMs() - _last_activity_ms >= InactivityTimeoutMs;
        }
        if (timeout) {
            setError("Transfer timed out");
        }

        bool teardown;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            teardown = _status.state == BleOtaState::Error && !_stop_requested;
        }
        if (teardown) {
            stopTransport();
        }

        std::lock_guard<std::mutex> lock(_mutex);
        return _status;
    }

    bool cancel()
    {
        setError("Transfer canceled");
        bool canceled;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            canceled = _status.state != BleOtaState::Success;
        }
        if (canceled) {
            stopTransport();
        }
        return canceled;
    }

    bool toggleMode()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_status.state != BleOtaState::Advertising || _stop_requested || !_worker_initialized) {
            return false;
        }

        const auto mode = _status.mode == BleOtaMode::Update ? BleOtaMode::Install : BleOtaMode::Update;
        if (ble_ota_worker_set_project_check(mode == BleOtaMode::Update) != ESP_OK) {
            return false;
        }
        _status.mode = mode;
        touchLocked();
        return true;
    }

private:
    static uint64_t nowMs()
    {
        return static_cast<uint64_t>(esp_timer_get_time()) / 1000;
    }

    static void eventHandler(void* arg, esp_event_base_t, int32_t id, void*)
    {
        static_cast<BleOtaSession*>(arg)->handleEvent(id);
    }

    void applySnapshot(const ble_ota_worker_snapshot_t& snapshot)
    {
        bool worker_failed = false;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_status.state == BleOtaState::Success || _status.state == BleOtaState::Error || _stop_requested) {
                return;
            }

            const bool progress       = snapshot.transferred != _status.transferred;
            const auto previous_state = _status.state;
            _status.total             = snapshot.total;
            _status.transferred       = snapshot.transferred;

            switch (snapshot.state) {
                case BLE_OTA_WORKER_IDLE:
                    break;
                case BLE_OTA_WORKER_RECEIVING:
                    _status.state = BleOtaState::Receiving;
                    break;
                case BLE_OTA_WORKER_VERIFYING:
                    _status.state = BleOtaState::Verifying;
                    break;
                case BLE_OTA_WORKER_READY:
                    _status.state = BleOtaState::Success;
                    break;
                case BLE_OTA_WORKER_ERROR:
                    worker_failed = true;
                    break;
            }

            if (progress || previous_state != _status.state) {
                touchLocked();
            }
        }

        if (worker_failed) {
            applyWorkerFailure(snapshot.failure);
        }
    }

    void applyWorkerFailure(ble_ota_worker_failure_t failure)
    {
        switch (failure) {
            case BLE_OTA_WORKER_FAILURE_INVALID_SIZE:
                setError("Firmware does not fit OTA partition");
                break;
            case BLE_OTA_WORKER_FAILURE_INVALID_IMAGE:
                setError("OTA application image is invalid");
                break;
            case BLE_OTA_WORKER_FAILURE_WRONG_PROJECT:
                setError("Firmware project mismatch");
                break;
            case BLE_OTA_WORKER_FAILURE_FLASH:
                setError("OTA flash operation failed");
                break;
            case BLE_OTA_WORKER_FAILURE_NONE:
            case BLE_OTA_WORKER_FAILURE_INTERNAL:
                setError("OTA worker failed");
                break;
        }
    }

    void handleEvent(int32_t id)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        switch (id) {
            case ESP_BLE_CONN_EVENT_CONNECTED:
                if (_status.state == BleOtaState::Advertising) {
                    _status.state = BleOtaState::Connected;
                }
                touchLocked();
                break;
            case ESP_BLE_CONN_EVENT_MTU:
            case ESP_BLE_CONN_EVENT_CCCD_UPDATE:
                touchLocked();
                break;
            case ESP_BLE_CONN_EVENT_DISCONNECTED: {
                if (_stop_requested || _status.state == BleOtaState::Error || _status.state == BleOtaState::Success) {
                    break;
                }
                ble_ota_worker_snapshot_t snapshot = {};
                const bool have_snapshot = _worker_initialized && ble_ota_worker_get_snapshot(&snapshot) == ESP_OK;
                if (have_snapshot && snapshot.total != 0 && snapshot.transferred == snapshot.total) {
                    touchLocked();
                } else if (_status.state == BleOtaState::Connected || _status.state == BleOtaState::Receiving ||
                           _status.state == BleOtaState::Verifying) {
                    setErrorLocked("Client disconnected");
                }
                break;
            }
            default:
                break;
        }
    }

    void setError(const char* message)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        setErrorLocked(message);
    }

    void setErrorLocked(const char* message)
    {
        if (_status.state == BleOtaState::Success || _status.state == BleOtaState::Error) {
            return;
        }
        if (_worker_initialized && ble_ota_worker_request_stop()) {
            _status.state       = BleOtaState::Success;
            _status.transferred = _status.total;
            return;
        }
        _status.state   = BleOtaState::Error;
        _status.message = message;
        ESP_LOGE(Tag, "%s", _status.message.c_str());
    }

    void touchLocked()
    {
        _last_activity_ms = nowMs();
    }

    void stopTransport()
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_stop_requested) {
                return;
            }
            _stop_requested = true;
            if (_worker_initialized) {
                ble_ota_worker_request_stop();
            }
        }

        if (_handler_registered) {
            esp_event_handler_unregister(BLE_CONN_MGR_EVENTS, ESP_EVENT_ANY_ID, eventHandler);
            _handler_registered = false;
        }
        if (_worker_initialized) {
            ble_ota_worker_stop();
        }
        if (_manager_started) {
            const esp_err_t err = esp_ble_conn_stop();
            if (err != ESP_OK || esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE) {
                ESP_LOGE(Tag, "BLE stop failed (%d); restarting", err);
                esp_restart();
            }
            _manager_started = false;
        }
        if (_worker_initialized) {
            const esp_err_t err = ble_ota_worker_deinit();
            if (err != ESP_OK) {
                std::lock_guard<std::mutex> lock(_mutex);
                _status.state   = BleOtaState::Error;
                _status.message = "OTA worker cleanup failed";
                ESP_LOGE(Tag, "%s", _status.message.c_str());
            } else {
                _worker_initialized = false;
            }
        }

        // ble_conn_mgr owns copies of profile service tables. Deinitialize it
        // before the profiles so its GATT database and service list agree.
        if (_manager_initialized) {
            const esp_err_t err = esp_ble_conn_deinit();
            if (err != ESP_OK) {
                ESP_LOGE(Tag, "BLE cleanup failed: %s", esp_err_to_name(err));
            }
            _manager_initialized = false;
        }
        if (_profile_initialized) {
            // The manager has already released its copied GATT tables, so the
            // profile's service removals report ESP_ERR_INVALID_ARG by design.
            const esp_err_t err = esp_ble_ota_raw_deinit();
            if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
                ESP_LOGE(Tag, "OTA service cleanup failed: %s", esp_err_to_name(err));
            }
            _profile_initialized = false;
        }
    }

    std::mutex _mutex;
    BleOtaStatus _status;
    uint64_t _last_activity_ms = 0;
    bool _stop_requested       = false;
    bool _handler_registered   = false;
    bool _manager_initialized  = false;
    bool _profile_initialized  = false;
    bool _worker_initialized   = false;
    bool _manager_started      = false;
};

}  // namespace

void Hal::startBleOta()
{
    if (session != nullptr) {
        return;
    }
    session = std::make_unique<BleOtaSession>();
    session->start();
}

BleOtaStatus Hal::updateBleOta()
{
    if (session != nullptr) {
        return session->update();
    }
    return {};
}

bool Hal::toggleBleOtaMode()
{
    return session != nullptr && session->toggleMode();
}

bool Hal::cancelBleOta()
{
    return session == nullptr || session->cancel();
}

void Hal::stopBleOta()
{
    session.reset();
}
