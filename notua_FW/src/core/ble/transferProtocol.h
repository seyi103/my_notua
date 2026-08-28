#pragma once

#include <stddef.h>
#include <stdint.h>

namespace notua::transfer {

constexpr uint8_t VERSION = 1;
constexpr size_t IMAGE_BYTES = 1600U * 1200U;
constexpr size_t MAX_GATT_VALUE_BYTES = 512;
constexpr size_t STATUS_BYTES = 12;

enum class Opcode : uint8_t { start = 0x01, finish = 0x02, abort = 0x03, apply = 0x04 };
enum class Status : uint8_t {
    startAccepted = 0x01, ack = 0x02, committed = 0x03, applying = 0x04,
    badCommand = 0x80, badSize = 0x81, badOffset = 0x82, queueFull = 0x83,
    crcMismatch = 0x84, storageError = 0x85, notReady = 0x86,
};

struct StartCommand { uint8_t slot; uint32_t size; uint32_t crc32; };
enum class OffsetDisposition : uint8_t { expected, duplicate, future };

uint32_t readLe32(const uint8_t* value);
void writeLe32(uint8_t* destination, uint32_t value);
bool parseStart(const uint8_t* data, size_t length, StartCommand& command);
bool isSimpleCommand(const uint8_t* data, size_t length, Opcode opcode);
void encodeStatus(uint8_t output[STATUS_BYTES], Status status, uint32_t nextOffset,
    uint32_t detail);
OffsetDisposition classifyOffset(uint32_t received, uint32_t expected);

class AckCoalescer {
public:
    bool persistedPacket();
    bool flush(bool queueEmpty);
    void reset() { pending_ = 0; }
private:
    uint8_t pending_ = 0;
};

class Crc32 {
public:
    void reset();
    void update(const uint8_t* data, size_t length);
    uint32_t value() const;
private:
    uint32_t crc_ = 0xffffffffU;
};

} // namespace notua::transfer
