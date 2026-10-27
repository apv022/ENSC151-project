#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief Small non-blocking serial-port wrapper for the Arduino connection.
 *
 * The game uses this class only for newline-terminated messages. Reads do not
 * wait for data, so an unplugged or quiet Arduino cannot stall the Raylib loop.
 */
class SerialPort {
public:
    /**
     * @brief Construct a closed serial-port handle.
     * @param none There are no construction inputs; call Open() to select a port.
     * @return A SerialPort instance that is not connected.
     */
    SerialPort();

    /**
     * @brief Close the underlying device when this wrapper leaves scope.
     * @param none The current handle is the only input.
     * @return No value; the device is closed if it was open.
     */
    ~SerialPort();

    /**
     * @brief Open a serial device and configure it for 8-N-1 communication.
     * @param portName Device path such as /dev/ttyACM0 or COM3.
     * @param baudRate Communication speed; FocusBird uses 115200 baud.
     * @return true if the device opened and was configured, otherwise false.
     */
    bool Open(const std::string& portName, unsigned int baudRate = 115200);

    /**
     * @brief Close the device and clear any partial incoming message.
     * @param none The currently open handle is the input.
     * @return No value; repeated calls are safe.
     */
    void Close();

    /**
     * @brief Report whether the serial device is currently open.
     * @param none The current handle is inspected.
     * @return true when an operating-system handle is available.
     */
    bool IsOpen() const;

    /**
     * @brief Collect all complete newline-terminated messages currently available.
     * @param none Bytes are read from the open device without blocking.
     * @return Complete messages with line endings removed; partial data is buffered.
     */
    std::vector<std::string> ReadLines();

    /**
     * @brief Send one newline-terminated command to the Arduino.
     * @param line Message content without a required trailing newline.
     * @return true if the complete message was written, otherwise false.
     */
    bool WriteLine(const std::string& line);

private:
    std::intptr_t handle_;       // OS handle/file descriptor, or -1 when closed.
    std::string incomingBuffer_; // Bytes retained until a complete line arrives.
};
