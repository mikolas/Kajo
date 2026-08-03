#!/usr/bin/env bash
# Weather Widget Script for kajo
# Displays current weather emoji & temperature from wttr.in

LOCATION="${1:-Helsinki}"
WEATHER=$(curl -s "wttr.in/${LOCATION}?format=%c+%t" --connect-timeout 2 | xargs)

if [ -n "$WEATHER" ]; then
    echo "$WEATHER"
else
    echo "Weather Unavailable"
fi
