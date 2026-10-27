#include "serial_port.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace {

/**
 * @brief Convert the configured baud value into the POSIX termios constant.
 * @param baudRate Requested serial speed.
 * @return Matching termios constant, or B115200 for unsupported values.
 */
#ifndef _WIN32
speed_t ToTermiosBaud(unsigned int baudRate) {
    switch (baudRate) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        default: return B115200;
    }
}
#endif

} // namespace

/**
 * @brief Construct a serial wrapper with no device open.
 * @param none No arguments are required.
 * @return A closed wrapper.
 */
SerialPort::SerialPort()
    : handle_(-1), incomingBuffer_() { // Start closed and with no pending bytes.
}

/**
 * @brief Ensure the operating-system serial handle is released.
 * @param none The object owns the handle being closed.
 * @return No value.
 */
SerialPort::~SerialPort() {
    Close(); // RAII prevents a successful open from leaking on normal exit.
}

/**
 * @brief Open and configure a serial device for non-blocking newline messages.
 * @param portName Device path or COM port supplied by the command line.
 * @param baudRate Baud rate shared with the Arduino sketch.
 * @return true after successful setup; false if opening/configuration fails.
 */
bool SerialPort::Open(const std::string& portName, unsigned int baudRate) {
    Close(); // Reopening first releases any previous device and stale partial line.

#ifdef _WIN32
    std::string windowsPath = portName; // Keep the caller's port string intact.
    if (windowsPath.rfind("\\\\.\\", 0) != 0) {
        windowsPath = "\\\\.\\" + windowsPath; // Win32 requires this form for COM10+.
    }

    HANDLE device = CreateFileA(windowsPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                                0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                nullptr); // Open without sharing to avoid port contention.
    if (device == INVALID_HANDLE_VALUE) {
        return false; // Leave the wrapper closed if Windows could not open the port.
    }

    DCB settings{}; // Zero-initialize the Windows serial configuration structure.
    settings.DCBlength = sizeof(settings); // Tell Win32 the structure size before use.
    if (!GetCommState(device, &settings)) {
        CloseHandle(device); // Release the port if configuration cannot be inspected.
        return false;
    }
    settings.BaudRate = baudRate; // Match the Arduino serial speed.
    settings.ByteSize = 8; // Use eight data bits for each transmitted byte.
    settings.Parity = NOPARITY; // Disable parity for the agreed 8-N-1 protocol.
    settings.StopBits = ONESTOPBIT; // Use one stop bit after each byte.
    settings.fDtrControl = DTR_CONTROL_ENABLE; // Reset many Arduino boards on open.
    if (!SetCommState(device, &settings)) {
        CloseHandle(device); // Do not retain a half-configured connection.
        return false;
    }

    COMMTIMEOUTS timeouts{}; // Start with zero timeouts for non-blocking reads.
    timeouts.ReadIntervalTimeout = MAXDWORD; // Return immediately when no bytes are waiting.
    timeouts.ReadTotalTimeoutConstant = 0; // Do not wait a fixed amount per read.
    timeouts.ReadTotalTimeoutMultiplier = 0; // Do not scale read wait by byte count.
    if (!SetCommTimeouts(device, &timeouts)) {
        CloseHandle(device); // Release the device if non-blocking mode cannot be set.
        return false;
    }
    handle_ = reinterpret_cast<std::intptr_t>(device); // Retain the Win32 handle without truncation.
#else
    const int descriptor = open(portName.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (descriptor < 0) {
        return false; // Keep the wrapper closed if the device path is unavailable.
    }

    termios settings{}; // Zero-initialize termios before loading device settings.
    if (tcgetattr(descriptor, &settings) != 0) {
        close(descriptor); // Release the descriptor when serial configuration fails.
        return false;
    }
    cfmakeraw(&settings); // Disable line processing so bytes arrive exactly as sent.
    const speed_t speed = ToTermiosBaud(baudRate); // Select a supported POSIX baud constant.
    cfsetispeed(&settings, speed); // Set the incoming data rate.
    cfsetospeed(&settings, speed); // Set the outgoing data rate.
    settings.c_cflag |= (CLOCAL | CREAD); // Ignore modem-control lines and enable reception.
    settings.c_cflag &= ~CSTOPB; // Select one stop bit.
    settings.c_cflag &= ~PARENB; // Disable parity bits.
    settings.c_cflag &= ~CSIZE; // Clear the old character-size selection.
    settings.c_cflag |= CS8; // Select eight data bits.
    settings.c_cc[VMIN] = 0; // Permit a read to return immediately with no bytes.
    settings.c_cc[VTIME] = 0; // Disable the inter-byte read timeout.
    if (tcsetattr(descriptor, TCSANOW, &settings) != 0) {
        close(descriptor); // Do not retain a device that failed configuration.
        return false;
    }
    handle_ = static_cast<std::intptr_t>(descriptor); // Store the configured non-blocking descriptor.
#endif

    incomingBuffer_.clear(); // A newly opened port must not inherit old partial data.
    return true;
}

/**
 * @brief Release the serial device and forget an incomplete incoming line.
 * @param none The current OS handle and receive buffer are used.
 * @return No value; safe when called more than once.
 */
void SerialPort::Close() {
    if (handle_ < 0) {
        return; // Nothing is open, so there is no operating-system resource to release.
    }
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
    close(handle_); // POSIX closes its file descriptor directly.
#endif
    handle_ = -1; // Mark the wrapper closed after releasing the underlying resource.
    incomingBuffer_.clear(); // Discard bytes that cannot form a complete message.
}

/**
 * @brief Check whether this wrapper currently owns an open serial device.
 * @param none The stored handle is inspected.
 * @return true when Open() succeeded and Close() has not been called.
 */
bool SerialPort::IsOpen() const {
    return handle_ >= 0; // Negative is the sentinel used for a closed device.
}

/**
 * @brief Read waiting bytes and split complete newline-terminated messages.
 * @param none The open device and its retained partial-line buffer are inputs.
 * @return All complete lines available now; partial line content remains buffered.
 */
std::vector<std::string> SerialPort::ReadLines() {
    std::vector<std::string> lines; // Collect complete messages for the caller.
    if (!IsOpen()) {
        return lines; // A disconnected Arduino produces no input events.
    }

    std::array<char, 256> bytes{}; // Limit each poll to a small, frame-friendly chunk.
#ifdef _WIN32
    DWORD bytesRead = 0; // Win32 fills this with the number of bytes received.
    const HANDLE device = reinterpret_cast<HANDLE>(handle_);
    if (ReadFile(device, bytes.data(), static_cast<DWORD>(bytes.size()), &bytesRead, nullptr)
        && bytesRead > 0) {
        incomingBuffer_.append(bytes.data(), bytesRead); // Preserve bytes until newline parsing.
    }
#else
    const ssize_t bytesRead = read(handle_, bytes.data(), bytes.size());
    if (bytesRead > 0) {
        incomingBuffer_.append(bytes.data(), static_cast<std::size_t>(bytesRead));
    } else if (bytesRead < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        Close(); // A real I/O error disconnects the port rather than blocking the game.
        return lines;
    }
#endif

    std::size_t newline = incomingBuffer_.find('\n'); // Locate the first complete message.
    while (newline != std::string::npos) {
        std::string line = incomingBuffer_.substr(0, newline); // Extract bytes before newline.
        if (!line.empty() && line.back() == '\r') {
            line.pop_back(); // Accept CRLF senders as well as LF-only senders.
        }
        if (!line.empty()) {
            lines.push_back(std::move(line)); // Ignore blank lines, retain useful protocol events.
        }
        incomingBuffer_.erase(0, newline + 1); // Remove this consumed line from the byte buffer.
        newline = incomingBuffer_.find('\n'); // Continue while more complete messages are buffered.
    }

    if (incomingBuffer_.size() > 1024) {
        incomingBuffer_.clear(); // Recover safely if a malformed sender never transmits newline.
    }
    return lines;
}

/**
 * @brief Write a single newline-terminated command to the connected Arduino.
 * @param line Command text; an existing trailing newline is accepted.
 * @return true only when every byte was written successfully.
 */
bool SerialPort::WriteLine(const std::string& line) {
    if (!IsOpen()) {
        return false; // The caller may continue with keyboard-only gameplay.
    }
    std::string message = line; // Copy the command so this method can normalize its terminator.
    if (message.empty() || message.back() != '\n') {
        message.push_back('\n'); // Every protocol message ends with one line feed.
    }

#ifdef _WIN32
    DWORD bytesWritten = 0; // Win32 reports the actual number of bytes sent.
    const HANDLE device = reinterpret_cast<HANDLE>(handle_);
    return WriteFile(device, message.data(), static_cast<DWORD>(message.size()),
                     &bytesWritten, nullptr)
        && bytesWritten == message.size(); // Reject partial writes to preserve command framing.
#else
    std::size_t offset = 0; // Track progress in case the OS accepts a partial write.
    while (offset < message.size()) { // Continue until the full short command is transmitted.
        const ssize_t bytesWritten = write(handle_, message.data() + offset,
                                           message.size() - offset);
        if (bytesWritten > 0) {
            offset += static_cast<std::size_t>(bytesWritten); // Advance past bytes already sent.
        } else if (bytesWritten < 0 && errno == EINTR) {
            continue; // Retry when a signal interrupted the system call.
        } else {
            return false; // Report a partial/error write without hanging the render loop.
        }
    }
    return true; // The complete command, including its newline, reached the OS driver.
#endif
}
