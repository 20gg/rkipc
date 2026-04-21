#!/bin/sh

echo "Stop Application ..."
killall rkipc
killall udhcpc

retry=0
while [ $retry -lt 10 ];
do
	sleep 1
	ps|grep rkipc|grep -v grep
	if [ $? -ne 0 ]; then
		echo "rkipc exit"
		break
	else
		echo "rkipc active"
		retry=$((retry + 1))
	fi
done

if [ $retry -ge 10 ]; then
	echo "rkipc still active, force stop"
	killall -9 rkipc
fi
