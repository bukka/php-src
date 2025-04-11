#!/usr/bin/bash

set -euo pipefail

DOMAIN="dnstest.php.net"
LOCAL_DNS="127.0.0.1"

echo "Looking for a DNS-enabled network interface..."

# Detect the first interface with non-local DNS configured
IFACE=$(resolvectl status | awk '
  /^Link [0-9]+ \(/ {
    sub(/^Link [0-9]+ \(/, "", $0)
    sub(/\)$/, "", $0)
    iface=$1
  }
  /^ *DNS Servers:/ {
    if ($2 !~ /^127\./) {
      print iface
    }
  }
')

if [[ -z "$IFACE" ]]; then
  echo "Could not find a suitable interface with external DNS."
  exit 1
fi

echo "Using interface: $IFACE"

# Extract current DNS servers (excluding any 127.*)
CURRENT_DNS=$(resolvectl status "$IFACE" | awk '
  $1 == "DNS" && $2 == "Servers:" { collecting=1; next }
  collecting && $1 ~ /^[0-9]/ && $1 !~ /^127\./ { print $1 }
  collecting && $1 !~ /^[0-9]/ { exit }
')

if echo "$CURRENT_DNS_RAW" | grep -q "^$LOCAL_DNS$"; then
  echo "$LOCAL_DNS already configured for $IFACE – skipping DNS update"
else
  # Build combined DNS list with LOCAL_DNS first
  CURRENT_DNS=$(echo "$CURRENT_DNS_RAW" | grep -v "^$LOCAL_DNS$" | tr '\n' ' ')
  COMBINED_DNS="$LOCAL_DNS $CURRENT_DNS"

  echo "Applying DNS for ~$DOMAIN:"
  echo " DNS Servers: $COMBINED_DNS"

  # Reset to ensure clean state
  sudo resolvectl revert "$IFACE"

  # Apply config
  sudo resolvectl domain "$IFACE" "~$DOMAIN"
  sudo resolvectl dns "$IFACE" $COMBINED_DNS
fi

# Confirm setup
resolvectl status "$IFACE"
