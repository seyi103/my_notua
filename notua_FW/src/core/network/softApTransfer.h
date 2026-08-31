#pragma once

#include <Arduino.h>

#if NOTUA_SOFTAP_HTTP_SPIKE
void requestSoftApStart();
void pollSoftApTransfer();
void stopSoftApTransfer(const char* reason);
String softApConnectionInfo();
bool softApTransferOwnsLifecycle();
#else
inline void requestSoftApStart() {}
inline void pollSoftApTransfer() {}
inline void stopSoftApTransfer(const char*) {}
inline String softApConnectionInfo() { return "disabled"; }
inline bool softApTransferOwnsLifecycle() { return false; }
#endif
