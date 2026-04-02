#!/bin/bash
LOADEDGAME_PATH="/tmp/LOADEDGAME"
LOADEDFULLPATH_PATH="/tmp/LOADEDFULLPATH"
SETTLE_TIME=1
LOAD_TIMEOUT=4

resolve_watch_target() {
    local currentpath="$1"
    local fullpath="$2"

    [ -z "$currentpath" ] && return 1

    # ZIP case: FULLPATH contains a .zip path
    if echo "$fullpath" | grep -qi '\.zip'; then
        local zip_path
        zip_path=$(echo "$fullpath" | sed 's/\(.*\.zip\).*/\1/I')
        [[ "$zip_path" != /* ]] && zip_path="/media/fat/$zip_path"
        echo "$zip_path"   # ← return even if it doesn't exist; inotifywait handles the error
        return 0
    fi

    # Loose file case
    local target
    if [[ "$currentpath" == /* ]]; then
        target="$currentpath"
    elif echo "$currentpath" | grep -q '/'; then
        target="/media/fat/$currentpath"
    else
        local dir="${fullpath%/}"
        [[ "$dir" != /* ]] && dir="/media/fat/$dir"
        target="$dir/$currentpath"
    fi

    [ -n "$target" ] && echo "$target" && return 0
    return 1
}

monitor_pid=""
last_currentpath="$current"
last_corename=""

cleanup() {
    [ -n "$monitor_pid" ] && kill "$monitor_pid" 2>/dev/null
    exit 0
}
trap cleanup SIGTERM SIGINT

echo "$(date '+%H:%M:%S') detect_game_load: started"

while true; do
    current=$(cat /tmp/CURRENTPATH 2>/dev/null || echo "")
    full=$(cat /tmp/FULLPATH 2>/dev/null || echo "")
    corename=$(cat /tmp/CORENAME 2>/dev/null || echo "")

    if [ -n "$last_corename" ] && [ "$corename" != "$last_corename" ]; then
        echo "$(date '+%H:%M:%S') Core changed: '$last_corename' -> '$corename' - clearing LOADEDGAME"
        rm -f "$LOADEDGAME_PATH" "$LOADEDFULLPATH_PATH"
        if [ -n "$monitor_pid" ]; then
            kill "$monitor_pid" 2>/dev/null
            wait "$monitor_pid" 2>/dev/null
            monitor_pid=""
        fi
        last_currentpath=""
    fi
    last_corename="$corename"

    if [ "$current" != "$last_currentpath" ]; then

        if [ -n "$monitor_pid" ]; then
            kill "$monitor_pid" 2>/dev/null
            wait "$monitor_pid" 2>/dev/null
            monitor_pid=""
        fi

        last_currentpath="$current"

        if [ -n "$current" ]; then
            watch_target=$(resolve_watch_target "$current" "$full")
            current_snap="$current"
            full_snap="$full"

            (
                sleep $SETTLE_TIME
                [ "$(cat /tmp/CURRENTPATH 2>/dev/null)" != "$current_snap" ] && exit 0

                timeout_arg="-t $LOAD_TIMEOUT"

                if [ -n "$watch_target" ]; then
                    inotifywait -q -e open -e close_nowrite $timeout_arg "$watch_target" 2>/dev/null
                    inotify_exit=$?

                    if [ $inotify_exit -eq 1 ]; then
                        echo "$(date '+%H:%M:%S') inotifywait failed for '$watch_target' - falling back to time-based"
                        inotify_exit=2
                    fi
                else
                    # Route could not be built: wait for timeout directly
                    echo "$(date '+%H:%M:%S') No watch target for '$current_snap' - using time-based detection"
                    sleep $LOAD_TIMEOUT
                    inotify_exit=2
                fi

                [ "$(cat /tmp/CURRENTPATH 2>/dev/null)" != "$current_snap" ] && exit 0

                echo "$current_snap"  > "$LOADEDGAME_PATH"
                echo "$full_snap"     > "$LOADEDFULLPATH_PATH"

                if [ $inotify_exit -eq 0 ]; then
                    echo "$(date '+%H:%M:%S') LOADED (filesystem event): $current_snap"
                else
                    echo "$(date '+%H:%M:%S') LOADED (timeout): $current_snap"
                fi

            ) &
            monitor_pid=$!
        fi
    fi

    sleep 0.3
done