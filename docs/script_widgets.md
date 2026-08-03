# Custom Script Widgets: End-User Configuration Guide

`kajo` provides a powerful `custom-script` widget engine allowing users to display real-time output from custom Bash, Python, or binary scripts directly in the Wayland panel.

---

## ⚙️ Configuration File Location

Custom script widgets are configured in `~/.config/kajo/config.json`.

```json
{
  "panel": {
    "position": "top",
    "height": 32,
    "widgets": {
      "left": ["launcher", "workspaces", "window"],
      "center": ["clock"],
      "right": [
        "custom_weather",
        "custom_crypto",
        "custom_vpn",
        "custom_disk",
        "tray",
        "volume",
        "battery",
        "power"
      ]
    }
  },
  "widgets": {
    "custom_weather": {
      "type": "custom-script",
      "exec_cmd": "/home/mikolas/src/desktop/data/examples/scripts/weather.sh Helsinki",
      "interval_sec": 60,
      "click_left_cmd": "zen https://wttr.in/Helsinki",
      "click_right_cmd": "kitty -e curl wttr.in/Helsinki",
      "mode": "both",
      "icon": "weather-clear-symbolic"
    },
    "custom_crypto": {
      "type": "custom-script",
      "exec_cmd": "python3 /home/mikolas/src/desktop/data/examples/scripts/crypto.py",
      "interval_sec": 30,
      "click_left_cmd": "zen https://coingecko.com",
      "mode": "both",
      "icon": "security-high-symbolic"
    },
    "custom_vpn": {
      "type": "custom-script",
      "exec_cmd": "/home/mikolas/src/desktop/data/examples/scripts/vpn_status.sh",
      "interval_sec": 5,
      "click_left_cmd": "nm-connection-editor",
      "mode": "both",
      "icon": "network-vpn-symbolic"
    },
    "custom_disk": {
      "type": "custom-script",
      "exec_cmd": "/home/mikolas/src/desktop/data/examples/scripts/disk_usage.sh",
      "interval_sec": 10,
      "click_left_cmd": "baobab",
      "mode": "both",
      "icon": "drive-harddisk-symbolic"
    }
  }
}
```

---

## 🛠️ Field Definitions & Parameters

| Parameter | Type | Required | Description | Example |
|:---|:---|:---|:---|:---|
| `type` | String | **Yes** | Must be `"custom-script"` | `"type": "custom-script"` |
| `exec_cmd` | String | **Yes** | Command or script to execute at regular intervals. The first line of stdout is displayed in the panel label. | `"exec_cmd": "~/scripts/weather.sh"` |
| `interval_sec` | Integer | **Yes** | Polling interval in seconds between script executions. | `"interval_sec": 10` |
| `click_left_cmd` | String | Optional | Shell command executed when the user left-clicks the widget. | `"click_left_cmd": "kitty -e htop"` |
| `click_right_cmd` | String | Optional | Shell command executed when the user right-clicks the widget. | `"click_right_cmd": "pavucontrol"` |
| `mode` | String | Optional | Visibility mode: `"both"` (icon + text), `"icon-only"`, or `"text-only"`. Defaults to `"both"`. | `"mode": "both"` |
| `icon` | String | Optional | GTK symbolic icon name displayed alongside the text label. | `"icon": "utilities-terminal-symbolic"` |

---

## 📂 Bundled Sample Scripts (`data/examples/scripts/`)

1. **Weather Monitor** (`data/examples/scripts/weather.sh`):
   - **Exec**: `data/examples/scripts/weather.sh "New York"`
   - **Output**: `🌤️ +22°C`

2. **Bitcoin & Ethereum Ticker** (`data/examples/scripts/crypto.py`):
   - **Exec**: `python3 data/examples/scripts/crypto.py`
   - **Output**: `BTC $67,420 | ETH $3,450`

3. **NetworkManager VPN Status** (`data/examples/scripts/vpn_status.sh`):
   - **Exec**: `data/examples/scripts/vpn_status.sh`
   - **Output**: `🔒 WireGuard-Office`

4. **Root Disk Space Monitor** (`data/examples/scripts/disk_usage.sh`):
   - **Exec**: `data/examples/scripts/disk_usage.sh`
   - **Output**: `💾 Root: 42%`

---

## 💡 Troubleshooting & Best Practices

1. **Script Permissions**: Ensure your custom script files have execute permissions (`chmod +x ~/scripts/myscript.sh`).
2. **Stdout Buffer**: The widget reads the **first line of output** emitted to `stdout`. Use `echo` or `print()` to output single-line strings.
3. **Execution Timeout**: Avoid long-running blocking commands in `exec_cmd`. Scripts execute asynchronously without blocking the UI looper, but fast execution (<500ms) provides the smoothest experience.
