#!/usr/bin/env sh
set -eu

echo "host: $(hostname)"
echo "kernel: $(uname -a)"

echo
echo "mellanox pci devices:"
if command -v lspci >/dev/null 2>&1; then
  lspci -Dnn | grep -iE 'mellanox|nvidia.*connectx|15b3:' || true
else
  echo "lspci not found"
fi

echo
echo "sysfs summary:"
for dev in /sys/bus/pci/devices/*; do
  [ -r "$dev/vendor" ] || continue
  vendor="$(cat "$dev/vendor")"
  [ "$vendor" = "0x15b3" ] || continue
  bdf="$(basename "$dev")"
  device="$(cat "$dev/device" 2>/dev/null || echo unknown)"
  driver="none"
  if [ -L "$dev/driver" ]; then
    driver="$(basename "$(readlink -f "$dev/driver")")"
  fi
  netdevs="none"
  if [ -d "$dev/net" ]; then
    netdevs="$(ls "$dev/net" 2>/dev/null | tr '\n' ' ' | sed 's/[[:space:]]*$//')"
    [ -n "$netdevs" ] || netdevs="none"
  fi
  echo "$bdf vendor=$vendor device=$device driver=$driver net=$netdevs"
  if command -v ethtool >/dev/null 2>&1 && [ -d "$dev/net" ]; then
    for net in "$dev"/net/*; do
      [ -e "$net" ] || continue
      name="$(basename "$net")"
      echo "  ethtool -i $name:"
      ethtool -i "$name" 2>/dev/null | sed 's/^/    /' || true
    done
  fi
done

echo
echo "ssh route hint:"
client_ip="$(who am i | awk '{print $5}' | tr -d '()' || true)"
if [ -n "$client_ip" ]; then
  ip route get "$client_ip" 2>/dev/null || true
else
  echo "could not infer ssh client ip"
fi
