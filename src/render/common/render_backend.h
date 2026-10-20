#pragma once
// ============================================================================
// src/render/common/render_backend.h -- the backend-neutral renderer entry points
// ============================================================================
// NOT RETAIL. What the code outside a renderer backend calls to plug the renderer
// into Runtime's plugin protocol (`Video::Device::InterfaceSlot`,
// src/engine/video/Device.h): src/app/video_interface.cpp writes these four into
// `Video::Device::dllSlots[kInterfaceDX11]` / `[kInterfaceDX9]`. Each pair has the
// slot's exact types (`Video::Device::GetDeviceNameFn` / `CreateDeviceDataFn`).
//
// Exactly one backend defines them, picked by CMake's BANISHED_RENDER_BACKEND
// (src/render/<backend>/backend.cmake): the DX11 backend's are thin forwarders in
// src/render/dx11/render_backend_dx11.cpp to VideoDX11-x64.dll's two exports and the
// cutover-only DX9 placeholder (src/render/dx11/VideoDX11Plugin.h). A second backend
// supplies its own render_backend_<name>.cpp; nothing outside src/render/<backend>/
// names a backend namespace (src/render/common/README.md).
//
// Kept free of every graphics header: the Video types are forward-declared.
// ============================================================================

namespace Video {
class Device;
class DeviceData;
}  // namespace Video

namespace Render {

// ---- the main interface slot (dllSlots[kInterfaceDX11]) ---------------------------
// The device name shown in the launcher's interface list ("DirectX 11" on DX11).
const char* DeviceName();
// A new backend DeviceData for `device`, returned as its base.
::Video::DeviceData* CreateDeviceData(::Video::Device* device);

// ---- the second slot (dllSlots[kInterfaceDX9]) ------------------------------------
// The cutover-only placeholder that keeps one GraphicsInterface entry per slot
// (B5_RULES D5; src/render/dx11/dx9_placeholder.cpp has the invariant).
const char* PlaceholderDeviceName();
::Video::DeviceData* PlaceholderCreateDeviceData(::Video::Device* device);

}  // namespace Render
