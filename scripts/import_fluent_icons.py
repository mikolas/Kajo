#!/usr/bin/env python3
import os
import sys
import glob
import re
import shutil

FLUENT_BASE = "/home/mikolas/src/fluentui-system-icons/assets"
DEST_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data", "icons", "hicolor", "scalable", "apps")

# Mapping: (Folder Name, SVG Filename) -> List of Target Icon Names in kajo
MAPPING = {
    # Volume
    ("Speaker 2", "ic_fluent_speaker_2_24_regular.svg"): [
        "audio-volume-high-symbolic",
        "audio-volume-high",
    ],
    ("Speaker 1", "ic_fluent_speaker_1_24_regular.svg"): [
        "audio-volume-medium-symbolic",
        "audio-volume-medium",
    ],
    ("Speaker 0", "ic_fluent_speaker_0_24_regular.svg"): [
        "audio-volume-low-symbolic",
        "audio-volume-low",
    ],
    ("Speaker Mute", "ic_fluent_speaker_mute_24_regular.svg"): [
        "audio-volume-muted-symbolic",
        "audio-volume-muted",
        "tb-volume-off-symbolic",
    ],
    ("Speaker Off", "ic_fluent_speaker_off_24_regular.svg"): [
        "tb-volume-off-symbolic-2"
    ],
    # Network / WiFi
    ("WiFi 4", "ic_fluent_wifi_4_24_regular.svg"): [
        "network-wireless-signal-excellent-symbolic",
        "tb-wifi-symbolic",
    ],
    ("WiFi 3", "ic_fluent_wifi_3_24_regular.svg"): [
        "network-wireless-signal-good-symbolic",
    ],
    ("WiFi 2", "ic_fluent_wifi_2_24_regular.svg"): [
        "network-wireless-signal-ok-symbolic",
    ],
    ("WiFi 1", "ic_fluent_wifi_1_24_regular.svg"): [
        "network-wireless-signal-weak-symbolic",
    ],
    ("WiFi Off", "ic_fluent_wifi_off_24_regular.svg"): [
        "network-wireless-offline-symbolic",
        "tb-wifi-off-symbolic",
    ],
    ("Router", "ic_fluent_router_24_regular.svg"): [
        "network-wired-symbolic"
    ],
    # Bluetooth
    ("Bluetooth", "ic_fluent_bluetooth_24_regular.svg"): [
        "bluetooth-active-symbolic",
        "bluetooth-symbolic",
        "tb-bluetooth-symbolic",
    ],
    ("Bluetooth Disabled", "ic_fluent_bluetooth_disabled_24_regular.svg"): [
        "bluetooth-disabled-symbolic",
        "tb-bluetooth-off-symbolic",
    ],
    # Battery
    ("Battery 10", "ic_fluent_battery_10_24_regular.svg"): [
        "battery-level-100-symbolic",
        "tb-battery-symbolic",
        "tb-battery-3-symbolic",
    ],
    ("Battery 6", "ic_fluent_battery_6_24_regular.svg"): [
        "tb-battery-2-symbolic"
    ],
    ("Battery 2", "ic_fluent_battery_2_24_regular.svg"): [
        "tb-battery-1-symbolic"
    ],
    ("Battery Charge", "ic_fluent_battery_charge_24_regular.svg"): [
        "battery-level-100-charging-symbolic",
        "tb-battery-charging-symbolic",
    ],
    # Power & Session Controls
    ("Power", "ic_fluent_power_24_regular.svg"): [
        "system-shutdown-symbolic",
        "tb-power-symbolic",
    ],
    ("Arrow Rotate Clockwise", "ic_fluent_arrow_rotate_clockwise_24_regular.svg"): [
        "tb-refresh-symbolic",
        "view-refresh-symbolic",
    ],
    ("Weather Moon", "ic_fluent_weather_moon_24_regular.svg"): [
        "tb-moon-symbolic"
    ],
    ("Sign Out", "ic_fluent_sign_out_24_regular.svg"): [
        "tb-logout-symbolic"
    ],
    # Launcher / Grid / Window
    ("Apps", "ic_fluent_apps_24_regular.svg"): [
        "view-app-grid-symbolic",
        "tb-app-window-symbolic",
    ],
    # Clock & Calendar
    ("Clock", "ic_fluent_clock_24_regular.svg"): [
        "preferences-system-time-symbolic",
        "tb-clock-symbolic",
    ],
    # Notifications / DND
    ("Alert", "ic_fluent_alert_24_regular.svg"): [
        "preferences-desktop-notification-symbolic",
        "preferences-system-notifications-symbolic",
        "tb-bell-symbolic",
    ],
    ("Alert Off", "ic_fluent_alert_off_24_regular.svg"): [
        "notifications-disabled-symbolic",
        "tb-bell-off-symbolic",
    ],
    # Idle Inhibitor (Coffee)
    ("Drink Coffee", "ic_fluent_drink_coffee_24_regular.svg"): [
        "idle-inhibitor-symbolic",
        "tb-coffee-symbolic",
    ],
    # Keyboard
    ("Keyboard", "ic_fluent_keyboard_24_regular.svg"): [
        "input-keyboard-symbolic",
        "tb-keyboard-symbolic",
    ],
    # Media
    ("Play", "ic_fluent_play_24_regular.svg"): [
        "media-playback-start-symbolic"
    ],
    ("Pause", "ic_fluent_pause_24_regular.svg"): [
        "media-playback-pause-symbolic"
    ],
    ("Next", "ic_fluent_next_24_regular.svg"): [
        "media-skip-forward-symbolic"
    ],
    ("Previous", "ic_fluent_previous_24_regular.svg"): [
        "media-skip-backward-symbolic"
    ],
    # Privacy
    ("Mic", "ic_fluent_mic_24_regular.svg"): [
        "privacy-mic-symbolic",
        "audio-input-microphone-symbolic"
    ],
    ("Camera", "ic_fluent_camera_24_regular.svg"): [
        "privacy-camera-symbolic",
        "camera-web-symbolic"
    ],
    # Control Center / Settings / Adjustments
    ("Options", "ic_fluent_options_24_regular.svg"): [
        "tb-adjustments-horizontal-symbolic",
        "emblem-system-symbolic",
    ],
    # Brightness
    ("Brightness High", "ic_fluent_brightness_high_24_regular.svg"): [
        "display-brightness-symbolic"
    ],
}

def process_svg_content(content):
    # Replace any hex fill/stroke colors (e.g. fill="#212121") with currentColor
    # Keep fill="none" intact
    content = re.sub(r'fill="#(?:[0-9a-fA-F]{3}){1,2}"', 'fill="currentColor"', content)
    content = re.sub(r'stroke="#(?:[0-9a-fA-F]{3}){1,2}"', 'stroke="currentColor"', content)
    return content

def main():
    if not os.path.exists(FLUENT_BASE):
        print(f"Error: Fluent assets directory not found at {FLUENT_BASE}")
        sys.exit(1)

    os.makedirs(DEST_DIR, exist_ok=True)
    imported = 0

    for (folder, fname), target_names in MAPPING.items():
        src_path = os.path.join(FLUENT_BASE, folder, "SVG", fname)
        if not os.path.exists(src_path):
            print(f"[WARNING] Source SVG missing: {src_path}")
            continue

        with open(src_path, "r", encoding="utf-8") as f:
            raw = f.read()

        cleaned = process_svg_content(raw)

        for target in target_names:
            if not target.endswith(".svg"):
                target = target + ".svg"
            dest_path = os.path.join(DEST_DIR, target)
            with open(dest_path, "w", encoding="utf-8") as f:
                f.write(cleaned)
            print(f"Imported: {folder}/{fname} -> {target}")
            imported += 1

    print(f"\nDone! Successfully imported {imported} Fluent Metro icons to {DEST_DIR}")

if __name__ == "__main__":
    main()
