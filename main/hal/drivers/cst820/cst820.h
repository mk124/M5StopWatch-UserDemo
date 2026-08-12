#pragma once

#include <driver/i2c_master.h>
#include <cstdint>

class Cst820 {
public:
    ~Cst820();

    /**
     * @brief Initialize the CST820 driver
     *
     * @param bus_handle The I2C master bus handle
     * @param addr The I2C address of the device (default 0x15)
     * @return true if initialization and identification succeeded
     * @return false otherwise
     */
    bool begin(i2c_master_bus_handle_t bus_handle, uint8_t addr = 0x15);

    /**
     * @brief Read touch data from the device
     *
     * @return true if read successful
     * @return false otherwise
     */
    bool read();

    // Getters
    uint16_t getX() const
    {
        return _x;
    }
    uint16_t getY() const
    {
        return _y;
    }
    bool isPressed() const
    {
        return _pressed;
    }
    uint8_t getFingerNum() const
    {
        return _finger_num;
    }

    /**
     * @brief Put the device into sleep mode
     */
    void sleep();

private:
    i2c_master_dev_handle_t _i2c_dev = nullptr;

    uint16_t _x         = 0;
    uint16_t _y         = 0;
    bool _pressed       = false;
    uint8_t _finger_num = 0;

    // Registers
    static constexpr uint8_t REG_STATUS   = 0x00;
    static constexpr uint8_t REG_CHIP_ID  = 0xA7;
    static constexpr uint8_t REG_SOFT_VER = 0xA9;
    static constexpr uint8_t REG_SLEEP    = 0xE5;

    bool read_register(uint8_t reg, uint8_t* data, size_t len);
    bool check_communication();
};
