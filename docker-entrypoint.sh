#!/bin/sh
cd /app

mkdir -p config/crashes
ln -sfn config/crashes crashes 2>/dev/null || true
ln -sf config/streamonitor.log streamonitor.log 2>/dev/null || true

exec ./StreaMonitor "$@"
