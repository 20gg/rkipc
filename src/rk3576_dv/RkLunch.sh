#!/bin/sh

check_linker()
{
        [ ! -L "$2" ] && ln -sf $1 $2
}

network_init()
{
	ethaddr1=`ifconfig -a | grep "eth.*HWaddr" | awk '{print $5}'`

	if [ -f /data/ethaddr.txt ]; then
		ethaddr2=`cat /data/ethaddr.txt`
		if [ $ethaddr1 == $ethaddr2 ]; then
			echo "eth HWaddr cfg ok"
		else
			ifconfig eth0 down
			ifconfig eth0 hw ether $ethaddr2
		fi
	else
		echo $ethaddr1 > /data/ethaddr.txt
	fi
	ifconfig eth0 up && udhcpc -i eth0
}

# Define a function to get the interrupt number and bind it to CPU3
bind_interrupt_to_cpu3() {
    local interrupt_line=$1
    local interrupt_number=$(echo $interrupt_line | awk '{print $1}' | sed 's/:$//')
    local cpu_mask=$((8)) # CPU3 bitmask, assuming CPU numbers start from 0

    # Check if the interrupt number is valid
    if [[ -n $interrupt_number ]]; then
        # Check if the interrupt binding file exists
        if [ -f "/proc/irq/$interrupt_number/smp_affinity" ]; then
            # Write the CPU3 bitmask to the interrupt binding file
            echo $cpu_mask > "/proc/irq/$interrupt_number/smp_affinity"
            echo "Interrupt number $interrupt_number has been bound to CPU3"
        else
            echo "Interrupt number $interrupt_number binding file does not exist"
        fi
    else
        echo "Invalid interrupt number: $interrupt_number"
    fi
}

post_chk()
{
	#TODO: ensure /userdata mount done
	cnt=0
	while [ $cnt -lt 30 ];
	do
		cnt=$(( cnt + 1 ))
		if mount | grep -w userdata; then
			break
		fi
		sleep .1
	done

	network_init&
	check_linker /userdata   /usr/www/userdata
	check_linker /media/usb0 /usr/www/usb0
	check_linker /mnt/sdcard /usr/www/sdcard

	# if /data/rkipc not exist, cp /usr/share
	rkipc_ini=/userdata/rkipc.ini
	default_rkipc_ini=/tmp/rkipc-factory-config.ini
	if [ ! -f "$default_rkipc_ini" ];then
		ln -s -f /usr/share/rkipc-imx386-800w-30fps.ini $default_rkipc_ini
	fi
	if [ ! -f "$default_rkipc_ini" ];then
		echo "Error: not found rkipc.ini !!!"
		exit -1
	fi
	if [ ! -f "$rkipc_ini" ]; then
		cp $default_rkipc_ini $rkipc_ini -f
	fi

	killall irqbalance

	# Get interrupt numbers and bind them to CPU3, Cannot bind ISP interrupt, otherwise imu pts will fluctuate greatly
	interrupt_lines=$(cat /proc/interrupts | grep -E 'cif|spi|inv_mpu')
	while read -r line; do
		bind_interrupt_to_cpu3 "$line"
	done <<< "$interrupt_lines"

	# export normal_no_read_back=0

	echo 800 > /sys/bus/iio/devices/iio:device1/in_anglvel_sampling_frequency
	echo 800 > /sys/bus/iio/devices/iio:device1/in_accel_sampling_frequency
	echo 0.004785 > /sys/bus/iio/devices/iio:device1/in_accel_scale
	echo 0.001064225 > /sys/bus/iio/devices/iio:device1/in_anglvel_scale
	echo 121 > /sys/bus/iio/devices/iio:device1/in_accel_lpf_bw
	echo 0 > /sys/bus/iio/devices/iio:device1/in_anglvel_lpf_bw

	echo 1 > /sys/devices/platform/charger-manager/chg_en

	export test_vendor_rkeis_loglevel=0 # when 60fps, frames will lost if rkeis printf log
	export file_cache_env=1
	# echo 1 >  /sys/kernel/debug/regulator/vbus5v0_typec/force_disable
	# sleep 15 # wait dp
	# modetest -M rockchip -s 180@72:1920x1080
	export rt_vo_disable_vop=0
	modetest -e
	# change to 58fps, need v4l2-ctl 58fps, ini fps=58, echo 1200000 > cpu0/cpufreq/scaling_setspeed
	rkipc &
}

echo userspace > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
echo 600000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed

# close all big
echo userspace > /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor
echo 408000 > /sys/devices/system/cpu/cpu4/cpufreq/scaling_setspeed
echo 0 > /sys/devices/system/cpu/cpu4/online
echo 0 > /sys/devices/system/cpu/cpu5/online
echo 0 > /sys/devices/system/cpu/cpu6/online
echo 0 > /sys/devices/system/cpu/cpu7/online
# INFO: close power for big kernel can save consumption about 30mW, but the suspend-resume function will be affected
# echo 1 > /sys/kernel/debug/regulator/vdd_cpu_big_s0/force_disable

echo userspace > /sys/class/devfreq/27800000.gpu/governor
echo 300000000 > /sys/class/devfreq/27800000.gpu/min_freq
echo 300000000 > /sys/class/devfreq/27800000.gpu/userspace/set_freq

echo userspace > /sys/class/devfreq/dmc/governor
echo 1560000000 > /sys/class/devfreq/dmc/userspace/set_freq

post_chk &

