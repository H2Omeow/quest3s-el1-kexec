# Quest 3S EL1 热切换

本项目提供一个在原厂 Linux 内核中运行的 ARM64 内核模块和静态用户态工具。模块自带 kexec 装载核心；在满足启动模式和内存布局检查时，它可把控制权交给自定义内核，不要求原厂配置 `CONFIG_KEXEC`。当前目标是 Quest 3S 的 VA48 自定义内核。本项目仍为实验性开发，**尚未在 Quest 3S 上加载模块或执行切换**。

## 适用条件

模块必须按设备当前运行的原厂内核构建。当前适配目标是 Linux 5.10、ARM64、4 KiB 页、VA39、三级页表、EL1 启动，且内核未启用 `CONFIG_KEXEC_CORE`。运行时会再次检查 EL1、启动 CPU 模式和页大小；不符合条件时拒绝装载或执行。只看 `uname` 不能证明模块 ABI 匹配。

目前记录的设备版本是 `5.10.240-g9cb2f620a9a8`。公开源码快照和手工候选配置没有设备的完整 `.config`、`Module.symvers`、工具链版本及最终 ABI 资料。因此本机产物只是候选编译检查，不能拿来声称可以在头显加载。需要取得与该设备固件准确匹配的原厂构建输出，特别是开启 `CONFIG_MODVERSIONS` 时所需的 `Module.symvers`。

切换会停掉当前内核且没有回退路径。目标 Image、DTB、可选 initrd、启动参数和物理内存区间必须先准确确认。内核模块会检查物理资源树、FDT reserve map、`/reserved-memory` 的 `reg` 项及已解析的动态保留区，并拒绝与映像或 DTB 映射块冲突的目标地址。用户态规划器要求显式传入 RAM 和保留区；当前 CLI 无法从 `size` 属性还原动态保留区的实际分配，因此目标 DTB 中含有 `size` 而没有 `reg` 的保留区时仍会拒绝规划。不要猜测地址，也不要把构建主机或其他 Quest 型号的数据当作本机数据。

执行阶段先串行化系统转换并冻结用户进程，再获取已暂存映像锁、检查 EL1 架构条件、阻断设备 probing，并在阻断后重新检查目标内存。平台预检成功后才进入不可返回的关机阶段。Quest 的 SCM warm shutdown 会保留运行中的 SCM/PMIC 固件状态；watchdog shutdown 会尝试停用 secure watchdog、pet timer 和 app watchdog，并验证 app watchdog 已禁用。若发现已绑定的 `shared_memory_bridge`、`qseecom` 或 `smcinvoke` 驱动，模块会拒绝切换。TEE 句柄所有权转移协议，以及 SCM WAITQ / in-flight 调用排空均未实现；源码分析与限制见 [TEE_HANDOFF.md](TEE_HANDOFF.md)。

## 构建

先在准确匹配运行内核的已准备输出树上构建模块。需要该内核的 `.config`、生成头文件、工具链和 `Module.symvers`：

```sh
scripts/build-module.sh /path/to/exact-stock-kernel-output
```

并行度默认是 6，可通过 `QUEST_EL1_JOBS` 调整。没有符号版本表时，只能运行不产出 `.ko` 的候选链接检查：

```sh
scripts/build-module.sh --check-only /path/to/candidate-kernel-output
```

构建宿主用户态工具及 planner 测试需要 C 编译器和 libfdt 开发包（Debian/Ubuntu 为 `libfdt-dev`）：

```sh
make -C user quest-el1
make -C user test
```

构建 ARM64 静态用户态工具需要交叉 GCC 和 dtc 源码中的 libfdt（已验证 dtc 1.7.2）：

```sh
scripts/build-user-arm64.sh /path/to/dtc-source
```

在自定义内核 ELF 构建后导出供本加载器使用的 `Image.el1`。脚本需要 Python 3、pyelftools 和 llvm-objcopy，只在严格核对 ELF alloc 段后，排除 `_end` 之后确认不属于运行镜像的顶层 unwind 元数据；不会改写原 `Image` 或 `vmlinux`：

```sh
scripts/prepare-image.py /path/to/target-vmlinux build/artifacts/Image.el1
```

本仓库不分发目标内核或原厂构建输出；主机端验证及其限制见 [VALIDATION.md](VALIDATION.md)。可复现的合成 planner 输入见 [tests/fixtures/synthetic-plan](tests/fixtures/synthetic-plan/README.md)。

## 使用

`plan` 只在主机上验证并显示装载计划，不访问设备。`load` 将映像暂存到模块；它不会切换。`exec` 才会停止当前系统并跳入新内核，需显式写 `--yes`。实际的 `--ram`、`--reserve`、kernel、DTB 和 initrd 地址必须来自目标设备的真实启动布局：

```sh
quest-el1-aarch64 plan \
  --image Image.el1 --dtb board.dtb \
  --kernel-base "$KERNEL_PHYS" --dtb-addr "$DTB_PHYS" \
  --ram "$RAM_START:$RAM_SIZE" \
  --reserve "$RESERVED_START:$RESERVED_SIZE" \
  --bootargs 'console=...'
```

有 initrd 时同时指定文件和目标物理地址；ARM64 启动协议的 1 GiB 对齐及 32 GiB 窗口约束会被检查：

```sh
  --initrd initrd.img --initrd-addr "$INITRD_PHYS"
```

只有模块已按设备原厂内核成功构建并加载、真实地址计划复核无误后，才执行装载和切换：

```sh
quest-el1-aarch64 load [与 plan 相同的映像、DTB、地址及内存参数]
quest-el1-aarch64 status
quest-el1-aarch64 exec --yes
```

按准确原厂输出树构建的模块名是 `kernel/quest_el1_kexec.ko`，可用设备已有的模块加载方式插入。模块通过 kprobe 解析未导出的 `kallsyms_lookup_name`；kprobe 不可用时，需要在模块参数 `lookup_addr` 中提供该运行内核中已准确核对的地址。模块加载限制和签名策略仍由原厂内核决定，本项目不绕过它们。

模块设备节点为 `/dev/quest_el1`，权限设为 root 可读写（0600），ioctl 要求 `CAP_SYS_BOOT`。`unload` 清除已暂存的目标内核而不切换。执行后旧内核不会继续运行。

## 目标内核

热切换模块和目标内核是两套独立构建。VA48 目标内核源码及其功能说明见 [oculus-linux-kernel 的 va48-quest3s 分支](https://github.com/H2Omeow/oculus-linux-kernel/tree/va48-quest3s)。目标 `5.10.246-quest3s-va48` 已完成 Image 与 257 个模块的本地构建，并集成 CPU/GPU VA48、NTSync、BakaSU 和 Droidspaces 内核前置项；见[目标验证记录](https://github.com/H2Omeow/oculus-linux-kernel/blob/va48-quest3s/tools/quest3s/BUILD_ARTIFACTS.md)。这些结果不代表设备运行验证，也不改变本模块必须针对原厂 5.10.240 内核 ABI 构建的要求。

## 许可

本项目依 GPLv2 发布；见 `COPYING.txt`。来源与移植边界见 `PROVENANCE.md`。
