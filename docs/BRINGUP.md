# 上机步骤（Quest 3S 首次热切换）

设备 BL 锁死，只能靠 kexec 在内存里换内核；工具不写任何分区，卡死后断电重启即回来。
本文件按“一次上机尽量成功”排列：前三级全部可逆，第四级才真正换内核。

## 0. 需要准备的文件

CI 产出（GitHub Actions artifact）拼成一个包：

| 文件 | 来源 | 说明 |
| --- | --- | --- |
| `quest_el1_kexec.ko` | loader 仓库 `Device-ABI module build` | 按设备 `config`/`symvers`/clang r450784e 编译，vermagic `5.10.240-g9cb2f620a9a8` |
| `quest-el1` | `Host checks` → `quest-el1-aarch64` | 静态 aarch64 规划器 |
| `Image.el1` | fork 仓库 `Handoff target build` | 目标内核（VA48/NTSync/BakaSU/Droidspaces） |
| `initramfs.cpio.gz` | `Host checks` → `quest-el1-aarch64` | 首启 `/init`，只写 pmsg 后回重启 |
| `bringup.sh` | 本仓库 `scripts/bringup.sh` | 设备端分阶段脚本 |

```bash
python3 scripts/device-package.py --out out/device \
    --module dist/quest_el1_kexec.ko --tool build/bin/quest-el1-aarch64 \
    --image Image.el1 --initramfs build/bin/handoff-initramfs.cpio.gz \
    --kernel-release out/include/config/kernel.release
```

## 1. 推到设备

```bash
adb push out/device/. /data/local/tmp/q3s/
adb shell su -c 'chmod 755 /data/local/tmp/q3s/quest-el1 /data/local/tmp/q3s/bringup.sh'
```

（KSU 越狱模式每次断电重启后都要重新越狱，才有 `su`。）

## 2. 第一级：只读事实（不换内核）

```bash
adb shell su -c 'sh /data/local/tmp/q3s/bringup.sh stage1'
adb pull /data/local/tmp/q3s/report.txt
adb pull /data/local/tmp/q3s/bringup.log
```

这一步加载模块、导出报告、生成运行时 DTB。**只读，没有任何危险操作。**

必须人工确认的几行（`report.txt`）：

- `current_el 1` 且 `arch_check 0` —— 说明在 EL1、页表格式符合预期；
- `gh-watchdog status enabled=` —— 看虚拟机看门狗现在是开还是关；
- `driver qseecom ... bound=1`、`driver smcinvoke ... bound=1`、`driver shared_memory_bridge ... bound=1`
  —— 说明 TEE 驱动在线，`exec` 时必须带 `allow_tee=1`（模块参数），否则会被拒；
- `rmem ...` 行数、`secure_summary ranges=` —— 保留区与安全内存快照；
- 最后一行必须是 `end`，否则报告被截断，重跑 stage1。

## 3. 第二级：规划并装载（仍不换内核）

```bash
adb shell su -c 'sh /data/local/tmp/q3s/bringup.sh stage2'
```

`plan` 用**实时** `/proc/iomem` 和运行时 DT 校验所有地址；`load` 再把镜像搬进目标地址并做
分段 CRC 校验。默认地址（参考机测得，可被环境变量覆盖）：

| 用途 | 地址 | 说明 |
| --- | --- | --- |
| 内核 | `0xa8000000` | 与原厂内核同一 2 MiB 基址 |
| initrd | `0xaca00000` | 与内核镜像不重叠 |
| DTB | `0xacc00000` | 2 MiB 映射块内无保留区 |

plan 失败时**不要**硬试 stage4：报告里给出的冲突区间直接决定改哪个地址。

## 4. 第三级：可逆演练（强烈建议）

```bash
adb shell su -c 'sh /data/local/tmp/q3s/bringup.sh stage3'
```

用真实的重定位代码把所有已装载页复制到临时页并校验，不切换内核。这一步通过，说明
“拷贝 + 跳转”这条最危险的路径是对的。

若看门狗与 CPU 下线也想演练：

```bash
adb shell su -c '/data/local/tmp/q3s/quest-el1 rehearse --steps watchdog,cpus,copy'
```

## 5. 第四级：热切换

```bash
adb shell su -c 'insmod /data/local/tmp/q3s/quest_el1_kexec.ko allow_tee=1'   # 若 stage1 未加载
adb shell su -c 'sh /data/local/tmp/q3s/bringup.sh stage4'
```

设备会重启。**唯一需要看的结果是重启后拿到的 pstore：**

```bash
adb shell su -c 'cat /sys/fs/pstore/console-ramoops-0'   # 名字随内核而定
adb shell su -c 'cat /sys/fs/pstore/pmsg-ramoops-0'
```

- 有 `quest-handoff: init reached userspace` → 目标内核已经跑起来了；
- 有 `quest-handoff: version Linux version 5.10.246-quest3s-va48 ...` → 是我们的内核；
- 完全没有 pstore 内容 → 切换没成功（或没走到 ramoops 初始化），回到第一级重看报告。

## 6. 调试顺序（按代价从低到高）

1. `report` 与实际不符 → 模块/ABI 问题，不上第二级；
2. `plan` 报地址冲突 → 只改地址，重跑 stage2；
3. `load` 报 `EBUSY`/`EINVAL` → 保留区重叠，看 plan 输出的区间；
4. `rehearse --steps copy` 失败 → 重定位路径问题，**不要**进 stage4；
5. `exec` 后没有任何 pstore → 看门狗没关干净或冻结没完成，先跑
   `rehearse --steps watchdog,cpus`。

## 7. 模块参数速查

| 参数 | 默认 | 用途 |
| --- | --- | --- |
| `allow_tee` | 0 | 置 1 才允许在 qseecom/smcinvoke/shmbridge 在线时切换 |
| `stop_remoteprocs` | `""` | `pas` 或逗号分隔的 rproc 名；执行前停掉它们 |
| `freeze_attempts` | 5 | 冻结进程重试次数（每 200 ms 一次） |
| `lookup_addr` | 0 | kprobe 不可用时手填 `kallsyms_lookup_name` 地址 |
