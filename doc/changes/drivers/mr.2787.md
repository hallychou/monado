survive/cosmos: Add support for the HTC Vive Cosmos and Cosmos Elite. The headset is
tracked via libsurvive, its configuration (distortion, etc.) is downloaded and parsed
over HID, and its panel can be acquired in NVIDIA direct mode. On Wayland the display is
acquired via DRM-leasing, which requires the connector to be marked non-desktop by a
kernel EDID quirk for `HVR`/`0xAA03`.
