#!/bin/bash

nc -l 6000 > received_data.txt &
TCP_SERVER_PID=$!

./udp_to_tcp 127.0.0.1:5000 127.0.0.1:6000 udp_to_tcp.log TEST &
APP_PID=$!

sleep 2

echo "Message 1" | nc -u -w1 127.0.0.1 5000
echo "Message 2" | nc -u -w1 127.0.0.1 5000
echo "Message 3" | nc -u -w1 127.0.0.1 5000

sleep 2

kill $TCP_SERVER_PID
kill $APP_PID

echo "=== Log ==="
cat udp_to_tcp.log

echo "=== Received Data ==="
cat received_data.txt