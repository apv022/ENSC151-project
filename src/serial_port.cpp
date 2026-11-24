/*
 * SerialPort for Windows, macOS, and Linux, in two layers:
 *   1. Platform layer (OsOpen/OsRead/OsWrite/OsClose): the only OS-specific code,
 *      one Windows and one POSIX version of each.
 *   2. SerialPort methods, written once: turn bytes into text lines and back.
 * Reads never block, so the game never waits on the Arduino. A real I/O error
 * closes the port and the game retries every two seconds.
 * Settings: 115200 baud, 8-N-1, no flow control (matches the Arduino sketch).
 */

#include "serial_port.h"

#include <array>

#ifdef _WIN32 // Windows
#include <windows.h> // CreateFile, DCB, ReadFile, WriteFile
#else // macOS and Linux
#include <cerrno> // errno, EAGAIN
#include <fcntl.h> // open() flags
#include <termios.h> // Serial line settings
#include <unistd.h> // read(), write(), close()
#endif

namespace {

constexpr std::intptr_t ClosedHandle = -1; // Value of handle_ when no port is open.

// ------------------------------- Platform layer ---------------------------------

#ifdef _WIN32

/**
 * @brief Open and configure a Windows COM port.
 * @param portName Port such as "COM3".
 * @return The port handle, or ClosedHandle on failure.
 */
std::intptr_t OsOpen(const std::string& portName) {
    // The "\\.\" prefix is required for COM10 and above and harmless for COM1-9.
    const std::string path = portName.rfind("\\\\.\\", 0) == 0 ? portName : "\\\\.\\" + portName;
    HANDLE device = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                0, nullptr, OPEN_EXISTING, 0, nullptr); // 0 = no sharing.
    if (device == INVALID_HANDLE_VALUE) {
        return ClosedHandle;
    }

    // Settings built from scratch, not inherited from another program, in Windows
    // "mode" syntax: 115200 8-N-1, all flow control off, DTR and RTS on (some USB
    // chips only send data then). The zeroed DCB keeps fAbortOnError off, so one
    // bad byte while the board resets can't break every later read.
    DCB settings{};
    settings.DCBlength = sizeof(settings);
    const bool built = BuildCommDCBA(
        "baud=115200 parity=N data=8 stop=1 xon=off octs=off odsr=off idsr=off dtr=on rts=on",
        &settings);
    settings.fBinary = TRUE; // Windows only supports binary mode.

    COMMTIMEOUTS timeouts{}; // MAXDWORD interval + zero totals = reads return immediately.
    timeouts.ReadIntervalTimeout = MAXDWORD;

    if (!built || !SetCommState(device, &settings) || !SetCommTimeouts(device, &timeouts)) {
        CloseHandle(device); // Don't keep a half-configured port.
        return ClosedHandle;
    }
    return reinterpret_cast<std::intptr_t>(device);
}

/**
 * @brief Read whatever bytes are waiting, without blocking.
 * @return Number of bytes read (0 = nothing waiting), or -1 on a real error such as unplugging.
 */
long OsRead(std::intptr_t handle, char* buffer, unsigned long size) {
    DWORD bytesRead = 0;
    if (!ReadFile(reinterpret_cast<HANDLE>(handle), buffer, size, &bytesRead, nullptr)) {
        return -1;
    }
    return static_cast<long>(bytesRead);
}

/**
 * @brief Send bytes to the port.
 * @return true only if every byte was written.
 */
bool OsWrite(std::intptr_t handle, const char* data, unsigned long size) {
    DWORD bytesWritten = 0;
    return WriteFile(reinterpret_cast<HANDLE>(handle), data, size, &bytesWritten, nullptr)
        && bytesWritten == size;
}

/** @brief Release the port handle. */
void OsClose(std::intptr_t handle) {
    CloseHandle(reinterpret_cast<HANDLE>(handle));
}

#else // macOS and Linux

/**
 * @brief Open and configure a serial device such as /dev/ttyACM0 or /dev/cu.usbmodem1101.
 * @param portName Device path.
 * @return The file descriptor, or ClosedHandle on failure.
 */
std::intptr_t OsOpen(const std::string& portName) {
    // Read/write, don't become this terminal's controlling process, never block.
    const int descriptor = open(portName.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (descriptor < 0) {
        return ClosedHandle; // Wrong path, unplugged, or no permission.
    }

    termios settings{};
    if (tcgetattr(descriptor, &settings) != 0) {
        close(descriptor);
        return ClosedHandle;
    }
    cfmakeraw(&settings); // Raw bytes: no line editing or echo; 8 data bits, no parity.
    settings.c_cflag &= ~CSTOPB; // One stop bit.
    settings.c_cflag |= CLOCAL | CREAD; // Ignore modem-control lines; enable receiving.
    cfsetispeed(&settings, B115200); // Must match Serial.begin(115200) in the sketch.
    cfsetospeed(&settings, B115200);
    if (tcsetattr(descriptor, TCSANOW, &settings) != 0) {
        close(descriptor);
        return ClosedHandle;
    }
    return descriptor;
}

/**
 * @brief Read whatever bytes are waiting, without blocking.
 * @return Number of bytes read (0 = nothing waiting), or -1 on a real error such as unplugging.
 */
long OsRead(std::intptr_t handle, char* buffer, unsigned long size) {
    const long bytesRead = static_cast<long>(read(static_cast<int>(handle), buffer, size));
    if (bytesRead < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return 0; // Not an error: there is simply nothing to read right now.
    }
    if (bytesRead == 0) {
        return -1; // On a non-blocking port "no data" is EAGAIN above; 0 means the device hung up (unplugged).
    }
    return bytesRead;
}

/**
 * @brief Send bytes to the port.
 * @return true only if every byte was written. Messages are ~15 bytes, so one call is enough.
 */
bool OsWrite(std::intptr_t handle, const char* data, unsigned long size) {
    return write(static_cast<int>(handle), data, size) == static_cast<long>(size);
}

/** @brief Release the file descriptor. */
void OsClose(std::intptr_t handle) {
    close(static_cast<int>(handle));
}

#endif // _WIN32

} // namespace

// ------------------------------- SerialPort -------------------------------------
// Everything below is the same on every operating system.

/**
 * @brief Construct a wrapper with no port open.
 */
SerialPort::SerialPort()
    : handle_(ClosedHandle), incomingBuffer_() {
}

/**
 * @brief Close the port automatically when the object goes away (RAII).
 */
SerialPort::~SerialPort() {
    Close();
}

/**
 * @brief Open and configure a port, closing any previously open one first.
 * @param portName "COM3" on Windows, "/dev/ttyACM0" or "/dev/cu.usbmodem…" elsewhere.
 * @return true if the port is now open.
 */
bool SerialPort::Open(const std::string& portName) {
    Close();
    handle_ = OsOpen(portName);
    return IsOpen();
}

/**
 * @brief Release the port and forget any half-received line. Safe to call twice.
 */
void SerialPort::Close() {
    if (IsOpen()) {
        OsClose(handle_);
    }
    handle_ = ClosedHandle;
    incomingBuffer_.clear();
}

/**
 * @brief Report whether a port is open.
 * @return true after a successful Open() until Close() or an I/O error.
 */
bool SerialPort::IsOpen() const {
    return handle_ != ClosedHandle;
}

/**
 * @brief Return every complete line received so far, without blocking.
 * USB can split a message across packets, so a partial line stays in
 * incomingBuffer_ until its newline arrives on a later call.
 * @return Complete lines with "\n" / "\r\n" removed; empty lines are skipped.
 */
std::vector<std::string> SerialPort::ReadLines() {
    std::vector<std::string> lines;
    if (!IsOpen()) {
        return lines; // Keyboard mode or disconnected: no input.
    }

    std::array<char, 256> bytes{};
    const long bytesRead = OsRead(handle_, bytes.data(), static_cast<unsigned long>(bytes.size()));
    if (bytesRead < 0) {
        Close(); // Real error (usually unplugged); the game will retry opening.
        return lines;
    }
    incomingBuffer_.append(bytes.data(), static_cast<std::size_t>(bytesRead));

    // Split off every complete line.
    std::size_t newline;
    while ((newline = incomingBuffer_.find('\n')) != std::string::npos) {
        std::string line = incomingBuffer_.substr(0, newline);
        incomingBuffer_.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back(); // Accept Windows-style "\r\n" endings too.
        }
        if (!line.empty()) {
            lines.push_back(line);
        }
    }

    if (incomingBuffer_.size() > 1024) {
        incomingBuffer_.clear(); // Garbage with no newline: don't let it grow forever.
    }
    return lines;
}

/**
 * @brief Send one command line to the Arduino, adding the newline if missing.
 * @param line Command such as "BUZZER 900 70".
 * @return true if sent; false (doing nothing) when no port is open, so callers
 *         can write unconditionally in keyboard mode.
 */
bool SerialPort::WriteLine(const std::string& line) {
    if (!IsOpen()) {
        return false;
    }
    std::string message = line;
    if (message.empty() || message.back() != '\n') {
        message.push_back('\n'); // Every protocol message ends with a newline.
    }
    if (!OsWrite(handle_, message.data(), static_cast<unsigned long>(message.size()))) {
        Close(); // The connection is no longer trustworthy; the game will retry.
        return false;
    }
    return true;
}
