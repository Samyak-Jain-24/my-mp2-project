#!/bin/bash

# Test script for WRITE operation

echo "====== Testing WRITE Operation ======"
echo ""
echo "This script will:"
echo "1. Start Name Server"
echo "2. Start Storage Server"
echo "3. Test CREATE and WRITE operations"
echo ""

# Kill any existing processes
killall -9 name_server storage_server 2>/dev/null

# Start Name Server
echo "Starting Name Server..."
./name_server &
NM_PID=$!
sleep 2

# Start Storage Server
echo "Starting Storage Server..."
./storage_server &
SS_PID=$!
sleep 2

echo ""
echo "Servers are running!"
echo "Name Server PID: $NM_PID"
echo "Storage Server PID: $SS_PID"
echo ""
echo "You can now test the WRITE operation:"
echo ""
echo "Run in another terminal:"
echo "  ./client"
echo ""
echo "Then try:"
echo "  CREATE test.txt"
echo "  WRITE test.txt 0"
echo "  1 Hello world."
echo "  ETIRW"
echo ""
echo "Press Ctrl+C to stop servers..."
echo ""

# Wait for user interrupt
trap "echo ''; echo 'Stopping servers...'; kill $NM_PID $SS_PID 2>/dev/null; exit 0" INT

wait
