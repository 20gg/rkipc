cd /home/industio/work/rk3588_linux6.1_rkr6_v1

# 若首次编译或环境有变，先同步 buildroot 配置
#./build.sh bmake:olddefconfig

# 只重编 rkipc
#./build.sh bmake:rkipc-clean
./build.sh bmake:rkipc-rebuild
# 或
#./build.sh bmake:rkipc


# 1. 先停止板子上的进程
ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@192.168.144.119 "/usr/bin/RkLunch-stop.sh 2>/dev/null || true"


# 2. 拷贝所有 usr 下的内容
# 使用 -p 选项来尝试保留执行权限（防止脚本无法运行）
cd /home/industio/work/rk3588_linux6.1_rkr6_v1/buildroot/output/rockchip_rk3588_multi_ipc/target/usr
tar -cf - {bin/rk*,bin/Rk*,share/rkipc*,share/*.bmp,share/*.ttf,share/*.rknn,share/*.txt,lib/libwpa*} | \
ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@192.168.144.119 "tar -xf - -C /usr"
cd -

# 3. 拷贝 rkipc.ini 到 /userdata 下
ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@192.168.144.119 "cp -f /usr/share/rkipc-2x.ini /userdata/rkipc.ini && rm -rf /usr/bin/RkLunch.sh"