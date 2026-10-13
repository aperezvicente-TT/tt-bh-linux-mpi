#!/bin/sh
# One line per guest: CPUs online, memory and LAN address.
mem=$(awk '/MemTotal/ {printf "%.1f GB", $2 / 1048576}' /proc/meminfo)
lan=$(ip -br -4 addr show lan0 | awk '{print $3}')
echo "$(hostname): cpus $(cat /sys/devices/system/cpu/online), $mem RAM, lan0 $lan"
