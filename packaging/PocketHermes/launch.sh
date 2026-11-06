#!/bin/sh
cd /mnt/UDISK/pocket-hermes
export LD_LIBRARY_PATH=/usr/trimui/lib:$LD_LIBRARY_PATH
exec ./pocket-hermes > /tmp/pocket-hermes.log 2>&1
