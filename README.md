## ![skynet logo](https://github.com/cloudwu/skynet/wiki/image/skynet_metro.jpg)

Skynet is a lightweight online game framework which can be used in many other fields.

## 说明

1. 将核心框架封装为 sncore.so 库.
2. 原有 skynet 程序仅是个壳子,依赖 sncore.so.
3. 新增 lskynet.so 导出 start 函数供第三方 lua 脚本启动框架.

```lua
local lskynet = require "lskynet"
lskynet.start("example/config", {EVN_VAR1 = 'enviroment variable 1'})
```

第一个参数指定配置文件,第二个参数可以传递环境变量到配置文件中.

4. 为防止动态加载的 luaclib 和 cservice 与第三方 lua 引擎冲突,这里将 lua 编译为 liblua.so 库,所有 luaclib、cservice、sncore 都强制依赖 liblua.so 库。

```s
   ➜  skynet git:(sncore_master) ✗ ldd ./sncore.so
   linux-vdso.so.1 => (0x00007fffe98eb000)
   libpthread.so.0 => /lib/x86_64-linux-gnu/libpthread.so.0 (0x00007fca57c90000)
   libdl.so.2 => /lib/x86_64-linux-gnu/libdl.so.2 (0x00007fca57a80000)
   librt.so.1 => /lib/x86_64-linux-gnu/librt.so.1 (0x00007fca57870000)
   liblua.so => not found <- 依赖liblua.so,设置好LD_LIBRARY_PATH保证找到依赖
   libc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 (0x00007fca57490000)
   /lib64/ld-linux-x86-64.so.2 (0x00007fca58600000)
```

```s
➜  skynet git:(sncore_master) ✗ LD_LIBRARY_PATH=. ./skynet examples/config
[:01000002] LAUNCH snlua bootstrap
[:01000003] LAUNCH snlua launcher
[:01000004] LAUNCH snlua cmaster
[:01000004] master listen socket 0.0.0.0:2013
[:01000005] LAUNCH snlua cslave
[:01000005] slave connect to master 127.0.0.1:2013
[:01000006] LAUNCH harbor 1 16777221
[:01000004] connect from 127.0.0.1:63881 4
[:01000004] Harbor 1 (fd=4) report 127.0.0.1:2526
[:01000005] Waiting for 0 harbors
[:01000005] Shakehand ready
[:01000007] LAUNCH snlua datacenterd
[:01000008] LAUNCH snlua service_mgr
[:01000009] LAUNCH snlua main
[:01000009] Server start
[:0100000a] LAUNCH snlua protoloader
[:0100000b] LAUNCH snlua console
[:0100000c] LAUNCH snlua debug_console 8000
[:0100000c] Start debug console at 127.0.0.1:8000
[:0100000d] LAUNCH snlua simpledb
[:0100000e] LAUNCH snlua watchdog
[:0100000f] LAUNCH snlua gate
[:0100000f] Listen on 0.0.0.0:8888
[:01000009] Watchdog listen on 8888
[:01000009] KILL self
[:01000002] KILL self
```
