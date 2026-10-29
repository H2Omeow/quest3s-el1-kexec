# 目标内核：构建、特性与产物

目标内核在另一个仓库：[H2Omeow/oculus-linux-kernel](https://github.com/H2Omeow/oculus-linux-kernel)，
分支 `va48-quest3s`（CI 用 `handoff/ci`）。它和本仓库的加载器**独立构建**：

- 加载器模块必须按**设备当前运行的原厂 5.10.240 内核**的 ABI 编译；
- 目标内核有两种造法，见下；
- 两边的 `.ko` 不能混用。

## 0. 两条路线，当前主线是第二条

| | 路线 A：fork 自己的 defconfig | 路线 B：原厂 5.10.240 重建（**当前主线**） |
| --- | --- | --- |
| 源码 | `va48-quest3s`（5.10.246 派生） | 公开原厂快照 `729ec6c4`（5.10.240） |
| 配置 | `vendor/quest3s_va48_defconfig` | 设备 `/proc/config.gz` + 两个 fragment |
| `uname` | `5.10.246-quest3s-va48` | `5.10.240-g9cb2f620a9a8 #1 SMP PREEMPT Thu Jul 2 23:18:32 PDT 2026`（**与出厂完全一致**） |
| 厂商模块 | 全部要重编（KMI 变了） | **原厂模块直接可用**（KMI 未变） |
| 编译器 | 上游 LLVM 23.1.4 + ThinLTO | `clang-r450784e` + `LTO_CLANG_FULL`（与出厂一致，CFI 类型哈希才对得上） |
| workflow | `Handoff target build` | `Stock 5.10.240 rebuild with VA48, NTSync, KSU and Droidspaces` |
| 用途 | 早期验证、快速迭代 | 实机目标：Root、Android 首启 |

路线 B 的取舍：`uname` 与出厂一模一样，所以**不能靠 `uname` 判断切换是否成功**，
判据见 `docs/BRINGUP.md` 的「怎么确认跑的是我们的内核」。
好处是 KMI 不变，`/vendor/lib/modules` 里那 267 个原厂 `.ko` 全都能继续用，
不需要像路线 A 那样把 bcmdhd、音频等模块重编进 vendor 分区。

---

## 1. 为什么不直接用仓库里的 defconfig

`vendor/quest3s_va48_defconfig` 与设备真实的 `/proc/config.gz` 对比后，**少了 11 个设备实际
加载的模块**：

```
SND_SOC_MAX98388  SENSORLOCK  VIRTUAL_SENSOR  OCULUS_CHARGING_DOCK  AK09973
USB_PDFU  LEDS_CLASS_FLASH  LEDS_QTI_FLASH  LEDS_TRIGGER_DEFAULT_ON
IIO_KFIFO_BUF  IIO_TRIGGERED_BUFFER
```

它们也不在公开的原厂 vendor defconfig 里 —— 来源是 Meta 未公开的 fragment。少一个
`CONFIG_SND_SOC_MAX98388=m`，音频就在没有任何提示的情况下消失。因此目标配置**从设备真实配置
反向派生**，而不是从公开 defconfig 正向累加。

## 2. 配置生成方式

```
设备 /proc/config.gz
        │  (tools/quest3s/device-5.10.240-g9cb2f620a9a8.config)
        ▼
merge_config.sh -m  ←  tools/quest3s/handoff-target.fragment
        ▼
    olddefconfig
```

`handoff-target.fragment` 只写**必须改的东西**：

| 类别 | 配置 |
| --- | --- |
| VA48 | `ARM64_VA_BITS=48`、`ARM64_KERNEL_VA_BITS=39`、`PGTABLE_LEVELS=4`、`ARM64_VA_BITS_48` |
| GPU VA48 | `QCOM_KGSL=m`、`QCOM_KGSL_GPU_VA48=y` |
| 同步原语 | `NTSYNC=y` |
| 已 root | `KSU=y`、`KSU_TRACEPOINT_HOOK=y`、`KSU_MULTI_MANAGER_SUPPORT=y` |
| 容器 | `SYSVIPC`、`POSIX_MQUEUE`、`FHANDLE`、`CHECKPOINT_RESTORE`、全部 namespace、`CGROUP_DEVICE/PIDS`、`DEVTMPFS`、`OVERLAY_FS`、`TUN`、`VETH`、`BRIDGE`、xt `ADDRTYPE/RECENT/LOG`、`NF_LOG_IPV4/6` |
| 首启 | `QUEST_HANDOFF=y`（见下） |
| 版本串 | `LOCALVERSION="-quest3s-va48"` |
| 体积 | `LTO_CLANG_THIN`（关 FULL）、关 `DEBUG_INFO`/`BTF`/`PGO` |

## 3. 首启辅助驱动 `quest_handoff`

`drivers/misc/quest_handoff.c`，配置项 `CONFIG_QUEST_HANDOFF`。它在 `early_initcall` 阶段：

- 打印一行自检：`CurrentEL`、`sctlr_el1`（MMU 是否已开）、`tcr_el1` 的 `t0sz/t1sz`、
  `VA_BITS`、`PAGE_SHIFT`、`PGTABLE_LEVELS`、`MIDR`；
- 注册一个 **deadman 定时器**（`/sys/module/quest_handoff/parameters/deadman`，秒，0 关闭）：
  到时先打印失败点，再 `emergency_restart()` 走暖复位，让卡死的启动也能在 ramoops 里留下日志。

有它，即使目标内核在很早的阶段挂了，重启后仍能从 pstore 看到死在哪里。

## 4. 构建

### CI（推荐）

fork 仓库的 `.github/workflows/target-build.yml`：

1. 取 Android clang **r450784e**（与设备内核同代，不是上游 LLVM 23）；
2. 合并「设备配置 + fragment」，`olddefconfig`，**断言那 11 个模块仍然是 `=m`**；
3. `make Image modules`；
4. 下载本仓库的 `scripts/prepare-image.py` 导出 `Image.el1`；
5. 把编出来的 `.ko` 名与设备实际加载的 267 个模块比对，列出哪些无法从公开源码构建；
6. 上传 `Image`、`Image.el1`、`Module.symvers`、`System.map`、`config`、`kernel.release`、
   `modules/`、`SHA256SUMS`。

### 本地

```sh
cp tools/quest3s/device-5.10.240-g9cb2f620a9a8.config out/.config
scripts/kconfig/merge_config.sh -m -O out out/.config tools/quest3s/handoff-target.fragment
make ARCH=arm64 LLVM=1 LLVM_IAS=1 REAL_CC=clang CROSS_COMPILE=aarch64-linux-gnu- \
     LOCALVERSION= O=out olddefconfig
make ARCH=arm64 LLVM=1 LLVM_IAS=1 REAL_CC=clang CROSS_COMPILE=aarch64-linux-gnu- \
     LOCALVERSION= O=out -j"$(nproc)" Image modules
```

`REAL_CC=clang` 不能省：原厂 `Makefile` 里有 `CC := $(REAL_CC)`，缺了它会以
`/bin/sh: 0: Illegal option --` / `No compiler specified` 失败。

导出加载器用的镜像：

```sh
python3 scripts/prepare-image.py out/vmlinux Image.el1
```

`prepare-image.py` 只在严格核对 ELF alloc 段后，排除 `_end` 之后确认不属于运行镜像的顶层
unwind 元数据（`.eh_frame`），不改写 `Image` 或 `vmlinux`。

## 5. 产物一览（参考构建）

| 文件 | 大小 | 说明 |
| --- | --- | --- |
| `Image` | ~36 MB | 原始 ARM64 Image |
| `Image.el1` | ~34 MB | 给加载器用，头部的 `image_size` 与实际内容一致 |
| `vmlinux` | ~48 MB | 符号来源 |
| `Module.symvers` | ~1 MB | 目标内核导出符号 |
| 模块 | 257 个 | 目标内核自带 |

## 6. 与设备模块的关系

设备加载 267 个 vendor 模块，公开源码可构建的更少。目标内核启动时**不应该**加载原厂
vendor 模块（CRC/结构不兼容）。首启用的 initramfs 是完全静态的，不加载任何模块，这正是
`handoff-init` 存在的意义：先证明内核能跑到用户态，再谈 Android。

## 7. 与原厂同版本的重建（推荐路径）

`va48-quest3s` 分支基于 5.10.246。但设备的 vendor 模块是针对 **5.10.240** 编译的：
5.10.241..246 只要改动了任何一个导出函数的签名/结构布局，`Module.symvers` 的 CRC 就会变，
vendor_dlkm 里的模块就会被 modpost 拒绝。所以更稳的做法是**回到原厂源码快照**重建：

```
公开原厂源码 729ec6c4 (5.10.240)
        + handoff/ci 的功能补丁（VA48、KSU、NTSync、quest_handoff）
        + 设备 config + 特性 fragment + 身份 fragment
        = uname 与原厂完全一致，且 KMI 与 vendor 模块一致
```

对应 workflow：fork 仓库的 `Stock 5.10.240 rebuild with VA48, NTSync, KSU and Droidspaces`
（`.github/workflows/stock-va48-build.yml`）。

关键点：

- **身份**：`CONFIG_LOCALVERSION="-g9cb2f620a9a8"` + 关 `LOCALVERSION_AUTO`
  （公开快照是 `729ec6c4`，若开 AUTO 会变成 `-g729ec6c4…`），
  `KBUILD_BUILD_VERSION=1`，`KBUILD_BUILD_TIMESTAMP="Thu Jul 2 23:18:32 PDT 2026"`
  —— `scripts/mkcompile_h` 对这个变量是**原样使用**，不做日期解析。
  结果 `uname -a` 为：

  ```
  Linux localhost 5.10.240-g9cb2f620a9a8 #1 SMP PREEMPT Thu Jul 2 23:18:32 PDT 2026 aarch64 Toybox
  ```

  构建后用 `strings Image | grep -F '5.10.240-g9cb2f620a9a8 #1 SMP PREEMPT Thu Jul 2 23:18:32 PDT 2026'`
  硬校验。
- **CFI/LTO 不能关**：`CONFIG_CFI_CLANG` 依赖 full LTO，而 vendor 模块是 CFI 编译的，
  内核若没有 `__cfi_*` 运行时，模块会以 `unknown symbol` 拒绝加载——那等于白改版本号。
  因此 `stock-240.fragment` 显式把 `LTO_CLANG_FULL=y`、`LTO_CLANG_THIN` 关掉，
  覆盖 `handoff-target.fragment` 的 ThinLTO 设定。代价是链接很慢，workflow 里给 runner
  单独挂 8 GiB swapfile。
- **补丁应用**：`git diff 043e6bcc handoff/ci` 打到原厂树上，`git apply --3way`，
  然后检查是否有残留冲突标记；`features.patch` 同时包含 VA48 核心（约 250 行）、
  `drivers/kernelsu`、`fs/ntsync`、`drivers/misc/quest_handoff.c`。
- 产物：`Image`、`Image.el1`、`Module.symvers`、`System.map`、`config-used`、
  `kernel.release`、`dist-modules.tgz`。

## 8. 检查清单（上机前）

- [ ] `kernel.release` 是 `5.10.246-quest3s-va48`
- [ ] `Image.el1` 头里 `image_size` ≥ 文件大小、`text_offset = 0x80000`、flags 为 LE + 4K
- [ ] `grep -c '^CONFIG_NTSYNC=y' out/.config` = 1
- [ ] `grep -c '^CONFIG_KSU=y' out/.config` = 1
- [ ] `grep -c '^CONFIG_PGTABLE_LEVELS=4' out/.config` = 1
- [ ] 那 11 个模块配置仍然是 `=m`
- [ ] `CONFIG_QUEST_HANDOFF=y`
