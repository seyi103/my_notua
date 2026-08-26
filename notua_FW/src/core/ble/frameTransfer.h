#pragma once

namespace frame_transfer {

/** Start the offline BLE GATT receiver. No IP network stack is initialized. */
bool begin(const char* deviceName = "Notua Frame");

/** Consume one completed-upload notification from the BLE callback. */
bool takeDisplayRequest();

/** Publish the result of the deferred EPD update to a connected client. */
void reportDisplayResult(bool success);

} // namespace frame_transfer
