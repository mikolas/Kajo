# Custom Script Widgets: End-User Configuration Guide

`kajo` provides a powerful `custom-script` widget engine allowing users to display real-time output from custom Bash, Python, or binary scripts directly in the Wayland panel.

---

## ⚙️ Configuration File Location

Custom script widgets are configured in `~/.config/kajo/shell.toml` under `[layout]` (`left`, `center`, `right`):

```toml
[layout]
right = [
    { id = "custom-script", mode = "full", interval = 60, exec = '/home/mikolas/src/desktop/data/examples/scripts/weather.sh Helsinki', click_left = 'zen https://wttr.in/Helsinki', click_right = 'kitty -e curl wttr.in/Helsinki' },
    { id = "custom-script", mode = "full", interval = 30, exec = 'python3 /home/mikolas/src/desktop/data/examples/scripts/crypto.py', click_left = 'zen https://coingecko.com' },
    { id = "tray", mode = "icon" },
    { id = "volume", mode = "icon" },
    { id = "battery", mode = "icon" },
    { id = "power", mode = "icon" },
]
```

---

## 🛠️ Field Definitions & Parameters

| Parameter | Type | Required | Description | Example |
|:---|:---|:---|:---|:---|
| `id` | String | **Yes** | Must be `"custom-script"` | `id = "custom-script"` |
| `exec` | String | **Yes** | Command or script to execute at regular intervals (use single-quoted TOML literal strings `'...'` so shell backslashes and quotes need no escaping). | `exec = '~/scripts/weather.sh'` |
| `interval` | Integer | **Yes** | Polling interval in seconds between script executions. | `interval = 10` |
| `click_left` | String | Optional | Shell command executed when the user left-clicks the widget. | `click_left = 'kitty -e htop'` |
| `click_right` | String | Optional | Shell command executed when the user right-clicks the widget. | `click_right = 'pavucontrol'` |
| `mode` | String | Optional | Visibility mode: `"full"` (icon + text), `"icon"`, or `"label"`. | `mode = "full"` |

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
3. **Execution Timeout**: Avoid long-running blocking commands in `exec`. Scripts execute asynchronously without blocking the UI looper, but fast execution (<500ms) provides the smoothest experience.
