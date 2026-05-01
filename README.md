# Trigger-based-Dynamic-Tracing


## Installing Dependencies

You should have the following libraries installed:
capstone, libpatch, yaml, libdawrf, cjson, llttng-ust

Fedora/RHEL:
```bash
sudo dnf install capstone-devel
sudo dnf install libyaml-devel
sudo dnf install libdwarf-devel
sudo dnf install cjson-devel
# libpatch dependencies
sudo dnf install guile30
sudo dnf install elfutils-devel
sudo dnf install userspace-rcu-devel
# LTTng dependencies
sudo dnf install popt-devel
sudo dnf install libuuid-devel
sudo dnf install libxml2-devel
sudo dnf install libbabeltrace2-devel
```

Ubuntu:
```bash
sudo apt install libcapstone-dev
sudo apt install libyaml-dev
sudo apt install libdwarf-dev
sudo apt install libcjson-dev
# libpatch dependencies
sudo apt install guile-3.0
sudo apt install libdw-dev
sudo apt install liburcu-dev
# LTTng dependencies
sudo apt install libpopt-dev
sudo apt install uuid-dev
sudo apt install libxml2-dev
sudo dnf install libbabeltrace2-dev
```

Install libolx (required for libpatch):
```bash
git clone https://git.sr.ht/~old/libolx
cd libolx
mkdir build; cd build; ../configuration
make -j4
sudo make install

# verify installatino
pkg-config --modversion libolx
# if not found
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH
```

Install libpatch:
```bash
git clone https://git.sr.ht/~old/libpatch
cd libpatch
mkdir build; cd build
../configuration \
    --disable-ftrace-build \
    --disable-patch-coverage-build \
    --disable-patch-integrity-build \
    --without-manpages \
    --without-lttng \
    --without-dyninst \
    --without-liteinst \
    --without-benchmarks
make -j4
sudo make install
```

Then you can confirm the installation with the following command:
```bash
ldconfig -p | grep capstone
```

install LTTng:

```bash
cd $(mktemp -d) &&
wget https://lttng.org/files/lttng-modules/lttng-modules-latest-2.15.tar.bz2 &&
tar -xf lttng-modules-latest-2.15.tar.bz2 &&
cd lttng-modules-2.15.* &&
make &&
sudo make modules_install &&
sudo depmod -a
```

```bash
cd $(mktemp -d) &&
wget https://lttng.org/files/lttng-ust/lttng-ust-latest-2.15.tar.bz2 &&
tar -xf lttng-ust-latest-2.15.tar.bz2 &&
cd lttng-ust-2.15.* &&
./configure --disable-numa &&
make &&
sudo make install &&
sudo ldconfig
```

for LTTng tools: (it gave me error)
```bash
wget https://lttng.org/files/lttng-tools/lttng-tools-latest-2.15.tar.bz2 &&
tar -xf lttng-tools-latest-2.15.tar.bz2 &&
cd lttng-tools-2.15.* &&
./configure &&
make &&
sudo make install &&
sudo ldconfig
```

## Building WYVERN

clone the repo:

```bash
git clone --recurse-submodules https://github.com/alient12/Trigger-based-Dynamic-Tracing.git
```

```bash
cd wyvern
gcc -shared -fPIC libdwscan.c -o libdwscan.so -ldwarf -lz
gcc -shared -fPIC cft-auto-data-test.c lttng_tp.c trigger_check.c trigger_compiler.c trace_config.c -o cft-auto-data-test.so -I. -L. -ldwscan -Wl,-rpath,'$ORIGIN' -lyaml -ldl -lcapstone -lpatch -llttng-ust

cd ..
gcc -O2 -Wall -Wextra -o wyvern_daemon wyvern_daemon.c
gcc -O2 -Wall -Wextra -o signal_sender signal_sender.c
```


## Test 1

install bpftrace

```bash
sudo dnf install bpftrace
```

Terminal 1:
```bash
sudo ./wyvern_daemon
```
Terminal 2:
```bash
cd trigger-examples
sudo bpftrace --unsafe thread_block.bt
```
Terminal 3:
```bash
cd sample-program
./mutex_test
```
or using taskset for cpu isolation (real isolation requires changing boot cmd)
```bash
sudo taskset -c 7 chrt -f 99 ./mutex_test
```

After recording data, use this command before openning data in TraceCompass
```bash
sudo chown -R $(whoami) /tmp/wyvern-kernel-trace
```

---
<details>
  <summary>Wyvern daemon runs these commands in background:</summary>

```bash
sudo lttng destroy wyvern-session 2>/dev/null || true

sudo lttng list | grep wyvern-session || sudo lttng create wyvern-session --snapshot --output=/tmp/wyvern-kernel-trace --trace-format=ctf-1.8
sudo lttng create wyvern-session --snapshot --output=/tmp/wyvern-kernel-trace --trace-format=ctf-1.8

sudo lttng enable-channel --kernel kchan --num-subbuf=4 --subbuf-size=4M
sudo lttng enable-event --kernel --channel=kchan --tracepoint 'sched_*'
sudo lttng enable-event --kernel --channel=kchan --tracepoint 'irq_*'
sudo lttng add-context --kernel --channel=kchan --type=callstack-kernel
sudo lttng start
```

```bash
sudo lttng snapshot record --name=wyvern-snapshot
# sudo lttng snapshot list-output

# sudo chown -R $(whoami) /tmp/wyvern-kernel-trace
```

```bash
sudo lttng stop
sudo lttng destroy
```
</details>


## Test 2

