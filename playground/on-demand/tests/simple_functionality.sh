#!/bin/bash

set -euo pipefail

# find unused port
while
    quiggeldy_port=$(shuf -n 1 -i 49152-65535 --random-source=/dev/urandom)
    netstat -atun | grep -q "${quiggeldy_port}"
do
  continue
done

on-demand-reload-server ${quiggeldy_port} &

sleep 1

on-demand-reload-client ${quiggeldy_port}

wait
