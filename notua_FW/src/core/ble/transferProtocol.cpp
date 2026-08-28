#include "core/ble/transferProtocol.h"

namespace notua::transfer {

uint32_t readLe32(const uint8_t* value) {
    return static_cast<uint32_t>(value[0]) | (static_cast<uint32_t>(value[1]) << 8)
        | (static_cast<uint32_t>(value[2]) << 16) | (static_cast<uint32_t>(value[3]) << 24);
}

void writeLe32(uint8_t* destination, uint32_t value) {
    destination[0] = value & 0xff;
    destination[1] = (value >> 8) & 0xff;
    destination[2] = (value >> 16) & 0xff;
    destination[3] = (value >> 24) & 0xff;
}

bool parseStart(const uint8_t* data, size_t length, StartCommand& command) {
    if (!data || length != 12 || data[0] != VERSION
        || data[1] != static_cast<uint8_t>(Opcode::start) || data[3] != 0) return false;
    command.slot = data[2];
    command.size = readLe32(data + 4);
    command.crc32 = readLe32(data + 8);
    return true;
}

bool isSimpleCommand(const uint8_t* data, size_t length, Opcode opcode) {
    return data && length == 2 && data[0] == VERSION && data[1] == static_cast<uint8_t>(opcode);
}

void encodeStatus(uint8_t output[STATUS_BYTES], Status status, uint32_t nextOffset,
    uint32_t detail) {
    output[0] = VERSION;
    output[1] = static_cast<uint8_t>(status);
    output[2] = output[3] = 0;
    writeLe32(output + 4, nextOffset);
    writeLe32(output + 8, detail);
}

void Crc32::reset() { crc_ = 0xffffffffU; }
void Crc32::update(const uint8_t* data, size_t length) {
    while (length--) {
        crc_ ^= *data++;
        for (uint8_t bit = 0; bit < 8; ++bit)
            crc_ = (crc_ >> 1) ^ (0xedb88320U & (0U - (crc_ & 1U)));
    }
}
uint32_t Crc32::value() const { return crc_ ^ 0xffffffffU; }

} // namespace notua::transfer
