# 目标内核：构建、特性与产物

目标内核在另一个仓库：[H2Omeow/oculus-linux-kernel](https://github.com/H2Omeow/oculus-linux-kernel)，
分支 va48-quest3s（target 构建用 handoff/240-H2O）。它和本仓库的加载器独立构建：

- 加载器模块必须按**设备当前运行的原厂 5.10.240 内核**的 ABI 编译；
- 目标内核是 5.10.240-va48-H2O（设备 5.10.240 配置 + VA48/NTSync/BakaSU/Droidspaces fragment），自带自己的模块集合；
- 两者的 `.ko` 不能混用。

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
| 版本串 | LOCALVERSION="-va48-H2O"；BakaSU 35259 |
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
| Image | 36,438,068 B | 原始 ARM64 Image，SHA256 4e6f4a98172c7bf702d507d5f3fdecc0e0cdfa68512f9920ff74b485cacfddcd |
| Image.el1 | 34,804,224 B | run 41292654740 产物，SHA256 ff317fb1a97fb5b163f014e24fb32d0f423a795a5d7eba1cf543a34d0a4aec28 |
| `vmlinux` | ~48 MB | 符号来源 |
| `Module.symvers` | ~1 MB | 目标内核导出符号 |
| 模块 | 265 个 | 目标内核自带；与设备实际加载的 268 个 vendor 模块不是同一集合 |

## 6. 与设备模块的关系

设备加载 268 个 vendor 模块，公开源码可构建的更少。目标把 QCOM_RPMH/QCOM_COMMAND_DB 内建，provider 仍为模块。目标内核启动时不应加载原厂 vendor 模块
（CRC/struct module/页表假设不兼容）。handoff/240-H2O 在 kernel/module.c 绕过了 CRC、vermagic、
签名和 CFI 检查，并设 CONFIG_CFI_PERMISSIVE=y；这只能避免“加载时被拒绝”，不能修复 ABI。
首选与目标同一次构建的 modules/，不要用原厂 .ko。首启 initramfs 完全静态，不加载任何模块，
用于先证明内核能跑到用户态。

## 7. 检查清单（上机前）

- [ ] kernel.release 是 5.10.240-va48-H2O
- [ ] Image.el1 头里 image_size ≥ 文件大小、text_offset = 0、flags 为 LE + 4K（5.10 arm64 的 Image 头 load offset 是 0，不是 0x80000）
- [ ] `grep -c '^CONFIG_NTSYNC=y' out/.config` = 1
- [ ] `grep -c '^CONFIG_KSU=y' out/.config` = 1
- [ ] `grep -c '^CONFIG_PGTABLE_LEVELS=4' out/.config` = 1
- [ ] 那 11 个模块配置仍然是 `=m`
- [ ] `CONFIG_QUEST_HANDOFF=y`
