#!/bin/sh



# 前台启动（最适合排障，日志直接看）
# /usr/bin/rkipc-manual-start.sh foreground
# # 后台启动
# /usr/bin/rkipc-manual-start.sh start
# # 停止
# /usr/bin/rkipc-manual-start.sh stop
# # 重启
# /usr/bin/rkipc-manual-start.sh restart

set -e

RKIPC_BIN="${RKIPC_BIN:-rkipc}"
IQ_DIR="${IQ_DIR:-/etc/iqfiles}"
CONF_SRC_DEFAULT="/usr/share/rkipc-2x.ini"
CONF_DST_DEFAULT="/userdata/rkipc.ini"
RUN_MODE="${1:-start}"

print_usage() {
	echo "Usage:"
	echo "  $0 [start|foreground|stop|restart] [conf_src] [conf_dst]"
	echo ""
	echo "Examples:"
	echo "  $0 start"
	echo "  $0 foreground /usr/share/rkipc-2x.ini /userdata/rkipc.ini"
}

start_rkipc() {
	CONF_SRC="${2:-$CONF_SRC_DEFAULT}"
	CONF_DST="${3:-$CONF_DST_DEFAULT}"

	if [ ! -x "$(command -v "$RKIPC_BIN")" ]; then
		echo "ERROR: cannot find '$RKIPC_BIN' in PATH"
		exit 1
	fi

	if [ ! -f "$CONF_SRC" ]; then
		echo "ERROR: config source not found: $CONF_SRC"
		exit 1
	fi

	mkdir -p "$(dirname "$CONF_DST")"
	cp -f "$CONF_SRC" "$CONF_DST"
	sync

	echo "Using config: $CONF_DST (from $CONF_SRC)"
	echo "IQ path: $IQ_DIR"

	if [ "$RUN_MODE" = "foreground" ]; then
		if [ -d "$IQ_DIR" ]; then
			exec "$RKIPC_BIN" -a "$IQ_DIR"
		else
			exec "$RKIPC_BIN"
		fi
	else
		killall rkipc 2>/dev/null || true
		sleep 1
		if [ -d "$IQ_DIR" ]; then
			"$RKIPC_BIN" -a "$IQ_DIR" >/tmp/rkipc.log 2>&1 &
		else
			"$RKIPC_BIN" >/tmp/rkipc.log 2>&1 &
		fi
		echo "rkipc started in background, log: /tmp/rkipc.log"
	fi
}

stop_rkipc() {
	killall rkipc 2>/dev/null || true
}

case "$RUN_MODE" in
	start|foreground)
		start_rkipc "$@"
		;;
	stop)
		stop_rkipc
		;;
	restart)
		stop_rkipc
		sleep 1
		RUN_MODE=start
		start_rkipc start "$2" "$3"
		;;
	-h|--help|help)
		print_usage
		;;
	*)
		echo "ERROR: unknown mode: $RUN_MODE"
		print_usage
		exit 1
		;;
esac
