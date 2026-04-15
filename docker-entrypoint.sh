#!/bin/sh
cd /app

for f in config.json app_config.json; do
    if [ ! -L "$f" ]; then
        [ -f "config/$f" ] || touch "config/$f"
        ln -sf "config/$f" "$f"
    fi
done

ln -sfn config/crashes crashes 2>/dev/null || true
ln -sf config/streamonitor.log streamonitor.log 2>/dev/null || true
mkdir -p config/crashes

exec ./StreaMonitor "$@"
