# Quest 3S EL1 热切换

在 Quest 3S 的原厂 Linux 内核里加载一个模块，把控制权交给内存中的自定义内核——不写分区、
不需要 `CONFIG_KEXEC`、不需要解锁 BL。仓库包含两部分：内核模块（自带 kexec 装载核心与
可逆自检）和静态 aarch64 用户态规划器 `quest-el1`。

> **状态**：模块已能针对本机真实 ABI 构建（设备 `config` + 从 `boot_a.img` 恢复的
> `Module.symvers` + 设备同代 clang r450784e），modpost 与 CRC 校验全部通过；**尚未在
> Quest 3S 上执行过切换**。BL 锁死时 kexec 是运行自定义内核的唯一途径，卡死可断电恢复。

---

## 1. 五分钟看懂它怎么工作

1. `insmod quest_el1_kexec.ko` —— 用 kprobe 取到未导出的内核符号，注册 `/dev/quest_el1`。
2. `quest-el1 report` —— 读出设备真实状态（EL、页表、watchdog、TEE 驱动、远程处理器、
   保留区、被 hypervisor 拿走的安全内存）。
3. `quest-el1 dump-dt` —— 从 `/proc/device-tree` 生成**运行时** DTB（不是 `boot-fdt`）。
4. `quest-el1 plan/load` —— 用实时 `/proc/iomem` 校验地址，把 Image/DTB/initrd 搬进目标
   物理页并逐段 CRC 校验。
5. `quest-el1 rehearse --steps copy` —— 用真实重定位代码把每个页复制到临时页并校验，
   **不切换**。这是最有价值的一步。
6. `quest-el1 exec --yes` —— 冻结进程、关看门狗、下线从核、迁移到重启 CPU、复位 PCIe、
   冲刷缓存，跳到新内核。

---

## 2. 一次上机的完整教程

下面每一级都可单独停下来，前三级完全可逆。设备端脚本是
[`scripts/bringup.sh`](scripts/bringup.sh)，细节与排障见 [docs/BRINGUP.md](docs/BRINGUP.md)。

### 2.1 收集产物

**最省事：直接下发行包。** [Releases → `bringup-1`](https://github.com/H2Omeow/quest3s-el1-kexec/releases/tag/bringup-1)
里就是下面这 5 个文件（外加 `SHA256SUMS`、`manifest.json`），**文件名与 `bringup.sh` 里写死的
路径一模一样**，解压到 `/data/local/tmp/q3s/` 就能跑，不用改任何变量：

```powershell
New-Item -ItemType Directory -Force $env:TEMP\bringup | Out-Null
Invoke-WebRequest -UseBasicParsing -OutFile $env:TEMP\bringup\quest_el1_kexec.ko `
  https://github.com/H2Omeow/quest3s-el1-kexec/releases/download/bringup-1/quest_el1_kexec.ko
# quest-el1 / Image.el1 / initramfs.cpio.gz / bringup.sh / SHA256SUMS 同理换文件名
```

也可以从 workflow artifact 拼（本轮构建的对应关系）：

| 文件 | 来自哪个 workflow | 说明 |
| --- | --- | --- |
| `quest_el1_kexec.ko` | `Device-ABI module build` → job `loader` | 按设备 ABI 编译，vermagic `5.10.240-g9cb2f620a9a8` |
| `quest-el1` | `Host checks` → artifact `quest-el1` | 静态 aarch64 规划器 |
| `initramfs.cpio.gz` | `Host checks` → artifact `initramfs` | **首启必需**：设备 bootargs 是 `init=/init`，缺了它目标内核会 panic |
| `Image.el1` | fork 仓库 `Stock 5.10.240 rebuild`（推荐）或 `Handoff target build` | 目标内核：与原厂同版本 5.10.240 + VA48 / NTSync / BakaSU / Droidspaces，uname 与原厂一致 |

打包成一个目录（顺便生成 `manifest.json` 与 `SHA256SUMS`）：

```bash
python3 scripts/device-package.py --out out/device \
    --module dist/quest_el1_kexec.ko \
    --tool build/bin/quest-el1 \
    --image Image.el1 \
    --initramfs build/bin/initramfs.cpio.gz \
    --kernel-release out/include/config/kernel.release
```

### 2.2 推到设备

```bash
adb push out/device/. /data/local/tmp/q3s/
adb shell su -c 'chmod 755 /data/local/tmp/q3s/quest-el1 /data/local/tmp/q3s/bringup.sh'
```

KSU 越狱模式的 `su` 不持久：断电重启后要先重新越狱，再跑下面任何一步。

### 2.3 第一级：只读事实（零风险）

```bash
adb shell su -c 'sh /data/local/tmp/q3s/bringup.sh stage1'
adb pull /data/local/tmp/q3s/report.txt
adb pull /data/local/tmp/q3s/bringup.log
```

这一级加载模块、导出报告、生成 `runtime.dtb`。**必须人工确认的报告行**：

| 报告行 | 期望 | 不符时的含义 |
| --- | --- | --- |
| `current_el 1` + `arch_check 0` | 是 | 不在 EL1 或页表格式不符，`exec` 一定失败 |
| `cpus online N possible M` | N<M 且能下线 | 从核下线能力 |
| `gh-watchdog status enabled=` | 能读到 | 读不到说明 Gunyah watchdog 没绑定 |
| `driver qseecom ... bound=1` 等 | 记录下来 | 在线时 `exec` 必须带 `allow_tee=1` |
| `rmem ...` | 40+ 行 | 动态保留区会被 `--report` 钉住 |
| `secure_summary ranges=` | 记录 | 这些页会变成目标 DTB 的 `no-map` 节点 |
| 最后一行 `end` | 是 | 报告被截断，重跑 stage1 |

### 2.4 第二级：规划并装载（仍不切换）

```bash
adb shell su -c 'sh /data/local/tmp/q3s/bringup.sh stage2'
```

`plan` 用**实时** `/proc/iomem` + 运行时 DT 复核每个地址，`load` 才真正搬运。
默认布局（参考机实测，可用环境变量覆盖）：

| 用途 | 默认地址 | 依据 |
| --- | --- | --- |
| 内核 | `0xa8000000` | 与原厂内核同一 2 MiB 基址 |
| initrd | `0xaca00000` | 与 Image（0x20A0000）不重叠 |
| DTB | `0xacc00000` | 2 MiB 映射块内无保留区 |

覆盖示例：

```bash
adb shell su -c 'KERNEL_BASE=0xa8000000 INITRD_ADDR=0xaca00000 DTB_ADDR=0xacc00000 \
                 sh /data/local/tmp/q3s/bringup.sh stage2'
```

`plan` 失败时不要硬闯下一级——它打印的冲突区间就是唯一要改的东西。

### 2.5 第三级：可逆演练（强烈建议）

```bash
adb shell su -c 'sh /data/local/tmp/q3s/bringup.sh stage3'   # = rehearse --steps copy
```

还想过一遍看门狗和从核下线：

```bash
adb shell su -c '/data/local/tmp/q3s/quest-el1 rehearse --steps watchdog,cpus,copy'
```

每一步都在返回前恢复原状；任何一步失败都会把失败码留在报告里。

### 2.6 第四级：热切换

```bash
adb shell su -c 'insmod /data/local/tmp/q3s/quest_el1_kexec.ko allow_tee=1'   # stage1 已加载则跳过
adb shell su -c 'sh /data/local/tmp/q3s/bringup.sh stage4'
```

设备会重启。**唯一能看到的证据是重启后的 pstore：**

```bash
adb shell su -c 'ls /sys/fs/pstore/'
adb shell su -c 'cat /sys/fs/pstore/console-ramoops-0'
adb shell su -c 'cat /sys/fs/pstore/pmsg-ramoops-0'
```

| pstore 内容 | 结论 |
| --- | --- |
| `quest-handoff: init reached userspace` | 目标内核跑到了用户态 |
| `quest-handoff: version Linux version 5.10.240-g9cb2f620a9a8` | 跑的是我们的内核 |
| 只有原厂内核的旧日志 / 空的 | 切换未生效，回到第一级 |

**注意**：主线的目标内核是用原厂 5.10.240 源码重建的，`uname -r/-v` 与出厂**刻意一致**，
所以判断依据不能是 `uname`，而是 `quest-handoff` 的横幅、`/sys/module/quest_handoff/`
和 `/proc/config.gz` 里的 `CONFIG_ARM64_VA_BITS=48` / `CONFIG_KSU` / `CONFIG_NTSYNC`。
详见 `docs/BRINGUP.md`。

---

## 3. 命令参考

```text
quest-el1 plan|load --image IMG --dtb DTB --kernel-base ADDR --dtb-addr ADDR
    (--ram S:Z ... | --iomem /proc/iomem) [--reserve S:Z ...]
    [--initrd FILE --initrd-addr ADDR] [--bootargs TEXT]
    [--report FILE] [--nomap S:Z ...] [--disable /dt/path ...]
quest-el1 dump-dt OUT.dtb [--root DIR] [--boot-fdt FILE]
quest-el1 report [--device PATH]
quest-el1 rehearse [--steps watchdog,rproc,cpus,mmu,copy|all] [--device PATH]
quest-el1 status | unload | exec --yes [--device PATH]
```

要点：

- `plan` 与 `load` 接受**完全相同**的参数；`plan` 只打印，不碰设备。
- 地址与大小接受十进制或 `0x` 前缀。
- `--report` 把目标 DTB 里「只有 size 没有 reg」的动态保留区钉到当前内核选定的地址
  （`ramoops` 因此能跨切换保留），并把 hypervisor 已拿走的安全内存加成 `no-map` 节点；
  报告被截断（缺 `end`）会直接拒绝。
- `--iomem` 把顶层 `System RAM` 区间当作 `--ram`，省得手抄。
- `--disable` 用于在目标 DTB 里关掉节点，典型值是 `/soc/qseecom@c1700000`：TEE 驱动在
  目标内核里没有可靠的重新握手协议，关掉比半死不活好。
- ARM64 启动协议的 1 GiB 对齐 / 32 GiB 窗口、2 MiB 镜像基址、DTB 2 MiB 映射块、
   以及 Image 头的 `text_offset`/`image_size`/flags 约束都会在规划阶段检查。

---

## 4. 模块参数

| 参数 | 默认 | 作用 |
| --- | --- | --- |
| `allow_tee` | `0` | `0`：`shared_memory_bridge`/`qseecom`/`smcinvoke` 在线即拒绝切换；`1`：只告警 |
| `stop_remoteprocs` | `""` | `pas` 表示停掉所有父驱动为 `qcom_q6v5_pas` 的 rproc；也可写逗号分隔的名字 |
| `freeze_attempts` | `5` | 冻结进程遇 `-EBUSY` 重试次数（每次间隔 200 ms），上限 20 |
| `lookup_addr` | `0` | kprobe 不可用时，手填 `kallsyms_lookup_name` 地址 |

`rehearse --steps rproc` 也读 `stop_remoteprocs`，演练后自动重启这些远程处理器。

---

## 5. 报告（`cat /dev/quest_el1`）字段

| 前缀 | 含义 |
| --- | --- |
| `abi` / `kernel` | 模块 ABI 版本、内核 release |
| `current_el` / `tcr_el1` / `sctlr_el1` | 异常级别与 MMU 配置 |
| `cpus online/possible` | CPU 热插拔能力 |
| `arch_check N` | 0 表示全部架构前置条件满足 |
| `allow_tee N` | 当前 TEE 策略 |
| `driver NAME present= bound=` | TEE / watchdog / rproc 驱动绑定情况 |
| `watchdog NODE compatible= available= driver=` | DT 中每个 watchdog 节点 |
| `gh-watchdog status enabled= expired= since_pet_ms=` | Gunyah 看门狗实时状态 |
| `rmem NAME BASE SIZE` | 已解析的保留区（含动态区实际地址） |
| `secure BASE SIZE` / `secure_summary` | hypervisor 拿走的内存页（只读扫描 `page->private` 标记） |
| `dmabuf_shared NAME size=` / `dmabuf_summary` | 被 mem-buf 借出、未独占的 dma-buf |
| `rehearse ...` | 最近一次可逆演练结果 |
| `end` | 报告完整；缺失即截断 |

---

## 6. 构建（本地）

模块必须针对**设备当前运行的**原厂内核输出树构建，需要该内核的 `.config`、生成头文件、
工具链和 `Module.symvers`：

```sh
scripts/build-module.sh /path/to/exact-stock-kernel-output
scripts/build-module.sh --check-only /path/to/candidate-kernel-output   # 无符号表时的链接检查
```

用户态（需要 C 编译器与 `libfdt-dev`）：

```sh
make -C user quest-el1
make -C user test
scripts/build-user-arm64.sh /path/to/dtc-source      # 静态 aarch64
make -C user CC=aarch64-linux-gnu-gcc handoff-init   # 首启 init
scripts/build-initramfs.sh user/handoff-init out/initramfs.cpio.gz
```

导出目标内核的 `Image.el1`（需要 Python 3、pyelftools、llvm-objcopy，只排除 `_end` 之后
确认不属于运行镜像的 unwind 元数据，不改写原 `Image`/`vmlinux`）：

```sh
scripts/prepare-image.py /path/to/target-vmlinux Image.el1
```

### 设备 ABI 工具

```sh
# 从 boot.img / 原始 Image 恢复 Module.symvers（KASLR 无关）
scripts/stock-symvers.py boot_a.img device/Module.symvers

# 校验一个 .ko 的每个 __versions 条目与 vermagic
scripts/check-module-abi.py --symvers device/Module.symvers \
    --vermagic '5.10.240-g9cb2f620a9a8 SMP preempt mod_unload modversions aarch64' \
    quest_el1_kexec.ko
```

`device/` 目录保存了本机实测数据：`stock-5.10.240-g9cb2f620a9a8.config`（`/proc/config.gz`
原样）、`stock-5.10.240-g9cb2f620a9a8.symvers`（14 049 个导出）、
`stock-loaded-modules.txt`（设备实际加载的 267 个 vendor 模块）。

### CI

| workflow | 内容 |
| --- | --- |
| `Host checks` | planner 测试、全部 host 单测、静态 `quest-el1`、首启 initramfs |
| `Stock-tree module build` | 对公开原厂源码做零告警编译检查 |
| `Device-ABI module build` | 用设备 config/symvers + clang r450784e 构建可加载模块，并逐项比对 CRC |
| fork `Handoff target build` | 目标内核 Image / Image.el1 / 模块 / 配置差异报告 |

---

## 7. 排障

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `insmod` 报 `invalid module format` | vermagic/CRC 不符 | 用 `Device-ABI` 产物；对照 `check-module-abi.py` |
| `lookup_addr` 相关报错 | kprobe 不可用 | 从设备 kallsyms 取 `kallsyms_lookup_name` 地址填参数 |
| `plan` 报 `EBUSY`/`EINVAL` | 目标地址与保留区/安全内存重叠 | 改 `--kernel-base`/`--initrd-addr`/`--dtb-addr` |
| `plan` 报「dynamically placed (size only)」 | 没给 `--report` | 先跑 stage1 拿报告 |
| `load` 报 `EBUSY` | 模块侧二次校验发现冲突 | 同上，看 plan 打印的区间 |
| `exec` 后毫无 pstore | 看门狗没关干净 / 冻结没完成 | 先 `rehearse --steps watchdog,cpus` |
| TEE 驱动在线导致拒绝 | 默认 fail-closed | 加 `allow_tee=1`，并在目标 DTB `--disable /soc/qseecom@c1700000` |

---

## 8. 限制与已知风险

- 切换没有回退路径；失败只能断电。
- 目标内核重新初始化硬件时，SCM/TEE、Gunyah 看门狗、远程处理器都依赖“先停干净”的前提；
  TEE 句柄所有权转移与 SCM WAITQ 排空**未实现**（见 [TEE_HANDOFF.md](TEE_HANDOFF.md)）。
- `ramoops` 能跨切换保留的前提是它被钉在设备实际选定的地址上（`--report` 自动完成）。
- 目标内核的模块必须与目标内核一起构建；原厂 vendor 模块不与 VA48 目标内核通用。

## 9. 相关文档

- [docs/BRINGUP.md](docs/BRINGUP.md)：上机逐级教程与排障顺序
- [docs/TARGET.md](docs/TARGET.md)：目标内核的配置派生、构建与产物
- [DEVICE_LADDER.md](DEVICE_LADDER.md)：验证阶梯（只读 → 可逆 → 计划 → 最小目标）
- [VALIDATION.md](VALIDATION.md)：主机端验证与其边界
- [TEE_HANDOFF.md](TEE_HANDOFF.md)：TEE 现状与未实现部分
- [PROVENANCE.md](PROVENANCE.md)：来源与移植边界

## 10. 许可

GPLv2，见 `COPYING.txt`。
