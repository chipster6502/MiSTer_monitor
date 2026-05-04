#!/bin/bash
# MiSTer Monitor startup script

SCRIPT_DIR="/media/fat/Scripts/.config/mister_monitor"
PID_FILE="/tmp/mister_monitor.pid"
LOG_FILE="/tmp/mister_monitor.log"

start_server() {
    if [ -f "$PID_FILE" ]; then
        PID=$(cat "$PID_FILE")
        if kill -0 "$PID" 2>/dev/null; then
            echo "The server is already running (PID: $PID)"
            return 0
        fi
    fi
    
    echo "Starting MiSTer Monitor Server..."
    cd "$SCRIPT_DIR"
    nohup python3 mister_status_server.py > "$LOG_FILE" 2>&1 &
    echo $! > "$PID_FILE"
    echo "Server started (PID: $(cat $PID_FILE))"
}

stop_server() {
    if [ -f "$PID_FILE" ]; then
        PID=$(cat "$PID_FILE")
        if kill -0 "$PID" 2>/dev/null; then
            kill "$PID"
            rm -f "$PID_FILE"
            echo "Server stopped"
        else
            echo "The server was not running"
            rm -f "$PID_FILE"
        fi
    else
        echo "PID file not found"
    fi
}

case "$1" in
    start)
        start_server
        ;;
    stop)
        stop_server
        ;;
    restart)
        stop_server
        sleep 2
        start_server
        ;;
    status)
        if [ -f "$PID_FILE" ]; then
            PID=$(cat "$PID_FILE")
            if kill -0 "$PID" 2>/dev/null; then
                echo "Server running (PID: $PID)"
            else
                echo "Server is not running"
            fi
        else
            echo "Server is not running"
        fi
        ;;
    *)
        echo "Use: $0 {start|stop|restart|status}"
        exit 1
        ;;
esac
