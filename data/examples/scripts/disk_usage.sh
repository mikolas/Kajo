#!/usr/bin/env bash
# Root Partition Disk Space Usage Monitor for kajo

USAGE=$(df -h / | awk 'NR==2 {print $5}')
echo "💾 Root: $USAGE"
