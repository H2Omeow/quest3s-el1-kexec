# 设备验证阶梯

目标：用尽量少的头显会话拿到决定性信息，并把不可逆的 `exec` 推迟到所有可逆步骤都通过之后。每一级只在上一级结果完全符合预期时才进入；任何一级失败，都先把输出带回主机分析，不要在设备上反复试。

所有命令都以 root 在头显上运行。`quest-el1` 指 CI 产物 `quest-el1-aarch64`（静态链接，不依赖设备 libc）。

## 第 0 级：主机（无需设备）

CI 已覆盖：

- 模块对 stock `729ec6c` + `vendor/oculus_anorak_defconfig`（VA39、三级页表、CFI、Full LTO、SCS、MODVERSIONS）零警告编译和 LTO 链接；
- planner 及 dump-dt、report 解析、动态保留区固定、no-map 合并的主机测试；
- 实际生产函数注入的 EXEC 控制流、平台回滚、兼容绑定 harness；
- ARM64 静态 CLI 构建。

候选编译不等于设备 ABI。第 1 级之前仍需要与设备固件对应的 `Module.symvers`（或从设备 `.ko` 提取 CRC）以生成可加载 `.ko`。

## 第 1 级：只读采集（一次会话，零风险）

模块加载本身不改变任何设备状态；`read()` 报告和 `dump-dt` 只读。

```sh
insmod quest_el1_kexec.ko
cat /dev/quest_el1 > /data/local/tmp/q3s-report.txt
quest-el1 dump-dt /data/local/tmp/q3s-live.dtb
cat /proc/iomem > /data/local/tmp/q3s-iomem.txt
cat /proc/cmdline > /data/local/tmp/q3s-cmdline.txt
zcat /proc/config.gz > /data/local/tmp/q3s-config.txt
dmesg > /data/local/tmp/q3s-dmesg.txt
```

报告的每一行都有固定格式，最后一行必须是 `end`（缺失表示截断）。需要确认：

| 报告行 | 期望 | 不符合时 |
| --- | --- | --- |
| `current_el` / `arch_check` | EL1，`arch_check 0` | 设备不是 EL1 启动，项目不适用 |
| `watchdog ...` | 每个可用 watchdog 节点都有已绑定的 `msm_watchdog` 或 `gh-watchdog` | 预检会 fail closed；需要新增该驱动支持 |
| `gh-watchdog status` | 出现该行（SMC 可用），`enabled=1` | 若 SMC 报错，gh 路径无法验证禁用 |
| `driver qseecom/smcinvoke/shared_memory_bridge` | 记录 `bound=` | 已绑定时 EXEC 默认拒绝，见下文 `allow_tee` |
| `rproc ...` | 列出 adsp/cdsp/spss 名称、状态、父驱动 | 决定 `stop_remoteprocs=` 取值 |
| `rmem ...` | 包含 `ramoops_region` 等动态保留区的实际地址 | `rmem_unavailable` 时 planner 无法固定动态保留区 |
| `secure ...` / `secure_summary` | HLOS 已移交给安全 VM 的页（`page->private == 0xEEEEEEEE`），合并为区间 | `overflow` 时 planner 拒绝使用该报告 |
| `dmabuf_summary` / `dmabuf_shared` | 共享给其他 VM 的 mem-buf 缓冲 | 非零时目标内核接管前这些缓冲仍被其他 VM 使用 |

把以上文件全部带回主机；之后的地址计划只用这些真实数据，不用 DTS 推算。

## 第 2 级：可逆演练（每项可单独执行）

```sh
quest-el1 rehearse --steps watchdog
quest-el1 rehearse --steps cpus
quest-el1 rehearse --steps mmu
```

- `watchdog`：按 EXEC 相同的绑定和检查停用 watchdog（msm：pet suspend + app disable + 读回 `WDT0_EN==0`；gh：pet suspend + hypervisor CONTROL=2 + STATUS 读回），随后立即恢复。不调用 secure watchdog deactivate（该操作不可逆）。
- `cpus`：下线全部次级 CPU，确认只剩一个在线且没有卡在内核里的 CPU，然后全部重新上线。
- `mmu`：在 `stop_machine` 中安装 identity map，通过**真实的** relocate blob 关闭 MMU、复制一个测试页，再经返回桩恢复 MMU 和页表，最后核对 cookie、SCTLR 及页内容。

这些步骤在 watchdog 仍启用时运行，若 MMU-off 往返卡死，watchdog 会 bite 重启设备，属于可恢复的失败。每步都会把结果写入下一次报告的 `rehearse` 行。

`rproc` 演练会关闭并重启远程处理器（ADSP 关闭期间音频/传感器中断），只在决定使用 `stop_remoteprocs=` 时运行：

```sh
echo pas > /sys/module/quest_el1_kexec/parameters/stop_remoteprocs
quest-el1 rehearse --steps rproc
```

## 第 3 级：生成真实计划并演练完整复制

在主机或设备上用第 1 级数据生成计划。`--report` 会把 DTB 中 `size`-only 的动态保留区固定到当前内核的实际地址（包括 `ramoops_region`，使目标内核和 warm reset 后都能从同一地址取回 pstore 日志），并把 secure 区间加成 `no-map` 节点：

```sh
quest-el1 plan \
  --image Image.el1 --dtb q3s-live.dtb \
  --report q3s-report.txt --iomem q3s-iomem.txt \
  --kernel-base "$KERNEL_PHYS" --dtb-addr "$DTB_PHYS" \
  --disable /soc/qseecom@c1700000 \
  --bootargs '... nokaslr ramoops.mem_address=... '
```

`plan` 会在改写后的 DTB 上重新检查全部内存冲突，即检查的就是目标内核实际收到的 DTB。确认无误后：

```sh
quest-el1 load [与 plan 相同的参数]
quest-el1 rehearse --steps copy
```

`copy` 把已暂存映像的每一页通过真实 relocate blob 在 MMU 关闭状态下复制到临时页，逐页核对并报告耗时。它验证的正是 EXEC 最后一步要执行的代码和描述符链，只是目的地址换成了临时页。

## 第 4 级：EXEC 到最小目标

第一次 EXEC 只用最小目标：BusyBox initramfs、低地址内存、目标 DTB 中禁用 qseecom、`nokaslr`、固定 ramoops。

```sh
quest-el1 exec --yes
```

预期失败模式与诊断：

- 立即重启：查看下一次启动的 PMIC 上电原因与 pstore（ramoops 已固定到同一地址）。
- 黑屏无重启：说明 watchdog 已被停用而目标未起来；目标内核入口应尽早写日志到 ramoops。

完整 Android 需要目标内核侧的接管补丁（QMP/AOP 通道、GIC 残留中断清理、SPMI 残留中断、显示 splash 区等），这些不在本模块范围内。

## 模块参数

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `allow_tee` | `0` | 为 `1` 时，已绑定的 TEE 驱动只告警不拒绝；目标 DTB 应禁用 qseecom 节点，并先停止 TEE 客户端 |
| `stop_remoteprocs` | 空 | `pas`：停止父驱动为 `qcom_q6v5_pas` 的运行中 rproc；也可写逗号分隔的 rproc 名称。EXEC 失败回滚时会重启 |
| `freeze_attempts` | `5` | `freeze_processes()` 因唤醒事件返回 `-EBUSY` 时的尝试次数（1–20，间隔 200 ms） |
| `lookup_addr` | `0` | kprobe 不可用时手工提供 `kallsyms_lookup_name` 地址 |
