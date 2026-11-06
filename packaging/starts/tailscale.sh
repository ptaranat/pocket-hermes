#!/bin/sh
trap "" HUP
iptables -I INPUT -p tcp --dport 5037 ! -i lo -j DROP
/mnt/UDISK/tailscale/tailscaled --state=/mnt/UDISK/tailscale/tailscaled.state --socket=/tmp/tailscaled.sock --statedir=/mnt/UDISK/tailscale </dev/null >/tmp/tailscaled.log 2>&1 &
