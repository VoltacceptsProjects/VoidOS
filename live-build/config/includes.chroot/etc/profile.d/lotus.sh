# VoidOS Lotus - system info banner on interactive shell start.
# Remove this file (or comment the block below) if you'd rather run
# `lotus` manually instead of on every new terminal.
case "$-" in
    *i*)
        if command -v lotus >/dev/null 2>&1; then
            lotus
        fi
        ;;
esac
