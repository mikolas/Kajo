#!/usr/bin/env bash
# NetworkManager VPN Monitor for kajo
# Displays active VPN connection name or 'VPN Disconnected'

VPN_NAME=$(nmcli -t -f NAME,TYPE connection show --active | grep ':vpn' | cut -d: -f1 | head -n1)

if [ -n "$VPN_NAME" ]; then
    echo "🔒 $VPN_NAME"
else
    echo "🔓 Disconnected"
fi
