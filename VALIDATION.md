# 验证记录

本记录汇总仓库的**Quest 3S 真机热切换验证**与**主机侧自动化验证**。

## 真机热切换验证（Quest 3S 5.10.240-g9cb2f620a9a8）

**当前状态：Stage 4 尚未成功。** 仓库旧版本声称的 Stage 1–4 端到端成功没有可复核的 pstore/console 原始日志，且与内核分支 QUEST3S_VA48.md 的 No headset boot or handoff has been performed 矛盾，不能作为验证结论。

已由代码、CI 或可复现实验证明的部分：

1. **Stage 1（只读采集与 ABI 校验）**：Device-ABI module build 产出的 quest_el1_kexec.ko 已在真机 insmod 成功，kprobe 符号绑定、/dev/quest_el1 状态报告及 /proc/device-tree 运行时 DTB 生成可用（current_el 1、arch_check 0）。
2. **Stage 2（地址规划与装载）**：默认布局（内核 0xa8000000、initrd 0xaca00000、DTB 0xacc00000）下的冲突检查、分段装载与 CRC 校验可完成；plan 仍依赖设备实时 /proc/iomem 与 runtime.dtb。
3. **Stage 3/4 首跳根因（2026-11-01）**：stock 与目标都启用 CONFIG_ARM64_BTI_KERNEL=y，SCTLR_EL1.BT0=1。quest_enter_stub 用 br 跳入 quest_el1_transition，而后者以 hint #34（BTI c）开头；跳转目标必须是 BTI j/jc，所以第一条指令即触发 BTI 异常，目标内核没有执行机会，表现为无日志、无 panic 的外部复位。rehearse 的 quest_el1_return 有同样缺陷。
4. **修复**：commit 823f76c6d7c6b6dce86bb46561edebc93fd64beb（arm64: use jump-type BTI landing pads for kexec stubs）把两个入口改为 hint #38（实际是 BTI jc）。对应 Device-ABI 构建 run 41282297624 的 quest_el1_kexec.ko 为 306,752 字节，SHA256 06ad4f3253e1a1eab2a52876ddacfd25ee53cf667d6424bf47e0619148698a08；反汇编确认入口字节由 5f 24 03 d5 变为 df 24 03 d5。
5. **尚未完成**：用修复后的 .ko 在真机重跑 Stage 4，并从 /sys/fs/pstore/ 取 console-ramoops-0 与 pmsg-ramoops-0；Android 用户态、TEE 句柄、vendor 模块 ABI 均未验证。

---

## 模块构建验证

仓库包含两类模块构建流水线：

### 1. 设备精确 ABI 构建（`Device-ABI module build`）

使用真机提取的 `device/stock-5.10.240-g9cb2f620a9a8.config`、从设备 `boot_a.img` 恢复的 `device/stock-5.10.240-g9cb2f620a9a8.symvers`（14,049 个导出符号）以及与设备同代的 Android clang `r450784e` 构建可加载的 `quest_el1_kexec.ko`。构建产物通过 `scripts/check-module-abi.py` 逐项核对 `__versions` CRC 与 `vermagic`（`5.10.240-g9cb2f620a9a8 SMP preempt mod_unload modversions aarch64`），并已在真机加载验证。

要使用完整设备构建资料编译可加载模块，可运行：

```sh
scripts/build-module.sh /path/to/exact-stock-kernel-output
```

### 2. 公开源码树候选检查（`Stock-tree module build`）

GitHub Actions 的 `Stock-tree module build` 在公开 stock 源码 `729ec6c4b2086454e0157cbc810f5a460789cc67` 上用 `vendor/oculus_anorak_defconfig` 和 Android clang `r416183b` 准备候选输出树，然后对本模块执行 `--check-only`：全部 20 个目标文件和 Full LTO 链接零编译警告（该配置为 VA39、三级页表、CFI、SCS、MODVERSIONS，`QCOM_WDT_CORE=m`、`GH_VIRT_WATCHDOG=m`）。

候选检查用于在没有设备 `Module.symvers` 的通用环境中做编译与链接回归测试，`--check-only` 会跳过 unresolved symbol 与 CRC 校验且不生成 `.ko`：

```sh
scripts/build-module.sh --check-only /path/to/candidate-stock-output
```

构建流程及上游来源见 [README.md](README.md) 与 [PROVENANCE.md](PROVENANCE.md)。

---

## 主机单元测试

`tests/test_memory_guard.py` 把实际 `kernel/memory_guard.c` 函数编译进带 UBSan 的主机 harness，模拟 stock resource tree、FDT reserve map 和 OF reserved-memory inventory。当前 36 个用例覆盖可用 RAM、固件与嵌套资源冲突、资源树祖先与 sibling 遍历、旧内核通用 init reservation 豁免及两侧残余保留区、DTB 2 MiB 映射块、静态与动态 reserved-memory、多个 `reg` 元组、FDT 格式错误及整数端点边界。

`tests/test_handoff.py` 从生产源码提取控制流和平台函数，再注入 host stubs。当前覆盖 EXEC 的 11 个控制流场景（含远程处理器停止失败与回滚重启）、兼容符号绑定的 13 个初始化/分发场景（含 freeze 重试上限），以及平台回滚的 38 个场景（含 gh-watchdog、DT watchdog 清单 fail closed、`allow_tee` 与 watchdog 演练恢复）。`make -C user test` 另外覆盖 report 解析、动态保留区固定、secure 区间 no-map 合并、`--disable`、`/proc/iomem` 解析及 `dump-dt` 与 FDT 的双向等价。EXEC 流程验证 transition serialization、冻结用户进程、在目标内存复查前阻断 device probing、内存拒绝后的解除与解冻、平台预检失败回滚，以及进入不可逆 shutdown 后失败时 panic 和成功交接不返回。

运行这些测试：

```sh
python3 tests/test_memory_guard.py
python3 tests/test_handoff.py
```

---

## 平台策略边界

当前 Qualcomm 适配通过 warm shutdown callback 保留运行中的 SCM/PMIC 固件状态，避免原 SCM shutdown 停止 PMIC arbiter 等不可逆操作。它会替换 watchdog shutdown，尝试停用 secure watchdog、停止 pet timer 并禁用 app watchdog；shutdown callback 报错或 watchdog 仍启用时不继续交接。

如果 `shared_memory_bridge`、`qseecom` 或 `smcinvoke` 存在已绑定设备，预检默认会 fail closed。当前实现没有 secure-world handle 所有权转移协议，也不会手工调用这些驱动的 `remove()` 或尝试通过卸载/重载驱动来夺取资源。

模块报告列出 stock `reserved_mem` 表中全部已解析的保留区（`rmem` 行）。CLI 通过 `--report` 把目标 DTB 中只有 `size` 的节点固定为对应 `reg`；没有报告时这些节点仍会拒绝规划，额外指定 `--reserve` 也不会跳过这一限制。

allow_tee=1 只把 TEE 驱动已绑定从拒绝改为告警，不实现句柄交接；此前“已在真机首启切换中验证可行”的说法随旧的成功声明一并撤回，修复后需重新验证。

---

## 合成规划回归与本次目标镜像

仓库跟踪了可复现的合成 DTS、运行脚本和输入说明，位于 [tests/fixtures/synthetic-plan](tests/fixtures/synthetic-plan/README.md)。合成测试中的所有 RAM、DTB 和保留区地址仅用于 CI 回归验证规划器对内核冲突、reserved-memory 冲突和 DTB 映射块冲突的拦截逻辑；真机装机使用的是设备端 `bringup.sh` 采集的实时 `/proc/iomem` 与 `runtime.dtb`。

目标构建已迁移到 handoff/240-H2O：直接基于 5.10.240 vendor commit 729ec6c4b208，设备 5.10.240 配置 + fragment，版本 5.10.240-va48-H2O，BakaSU 35259，并加入 Droidspaces 所需的内核前置项。run 41292654740（commit c8092688d333）成功：Image 36,438,068 B SHA256 4e6f4a98172c7bf702d507d5f3fdecc0e0cdfa68512f9920ff74b485cacfddcd，Image.el1 34,804,224 B SHA256 ff317fb1a97fb5b163f014e24fb32d0f423a795a5d7eba1cf543a34d0a4aec28，265 个模块；QCOM_RPMH 和 QCOM_COMMAND_DB 内建，时钟/regulator/interconnect provider 保持模块；artifact id 13078212080，release bringup-3。旧 5.10.246 / 34,208,256 字节的 Image.el1 不再使用。
