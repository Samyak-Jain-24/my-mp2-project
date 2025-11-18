#!/usr/bin/env bash
# LangOS cleanup helper: terminate lingering name_server and storage_server processes and show port status.
set -euo pipefail
PIDS_NS=$(pgrep -f ./name_server || true)
PIDS_SS=$(pgrep -f "./storage_server" || true)
if [[ -n "$PIDS_NS" || -n "$PIDS_SS" ]]; then
  echo "Stopping lingering processes..."
  pkill -INT -f ./name_server || true
  pkill -INT -f ./storage_server || true
  sleep 0.5
fi
# Force kill if still alive
PIDS_NS2=$(pgrep -f ./name_server || true)
PIDS_SS2=$(pgrep -f "./storage_server" || true)
if [[ -n "$PIDS_NS2" || -n "$PIDS_SS2" ]]; then
  echo "Force killing remaining processes..."
  pkill -9 -f ./name_server || true
  pkill -9 -f ./storage_server || true
fi
sleep 0.3
echo "Port usage (8080, 9001, 9002, 9101, 9102):"
ss -ltnp '( sport = :8080 or sport = :9001 or sport = :9002 or sport = :9101 or sport = :9102 )' 2>/dev/null || true
echo "Done. You can now restart with: ./name_server & then storage_server instances."