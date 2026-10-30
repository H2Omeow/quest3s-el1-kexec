# 验证记录

本记录汇总仓库的**Quest 3S 真机热切换验证**与**主机侧自动化验证**。

## 真机热切换验证（Quest 3S `5.10.240-g9cb2f620a9a8`）

已在零售版 Quest 3S（运行原厂内核 `5.10.240-g9cb2f620a9a8`）上完成从模块加载到目标内核启动的端到端热切换验证：

1. **Stage 1（只读采集与 ABI 校验）**：使用 `Device-ABI module build` 产出的 `quest_el1_kexec.ko` 在真机 `insmod` 成功，kprobe 符号绑定、`/dev/quest_el1` 状态报告导出及 `/proc/device-tree` 运行时 DTB（`runtime.dtb`）生成均符合预期（`current_el 1`、`arch_check 0`）。
2. **Stage 2（实机地址规划与装载）**：基于设备实时 `/proc/iomem`、运行时 DTB 与模块报告固定动态保留区（含 `ramoops`）及 hypervisor 安全内存 `no-map` 区间，在默认物理地址布局（内核 `0xa8000000`、initrd `0xaca00000`、DTB `0xacc00000`）下通过冲突检查、完成分段装载与 CRC 校验。
3. **Stage 3（可逆重定位演练）**：`quest-el1 rehearse --steps copy`（以及看门狗/从核演练）通过，在 MMU 关闭状态下经由真实 relocate blob 完成物理页复制校验并安全返回原厂内核。
4. **Stage 4（EL1 热切换执行）**：携带 `allow_tee=1` 并在目标 DTB 禁用 `/soc/qseecom@c1700000` 后执行 `quest-el1 exec --yes`，成功冻结进程、停用看门狗、下线从核并跳转至目标 `5.10.240` 内核（已合入 VA48 / NTSync / BakaSU / Droidspaces 扩展特性）。重启后通过 `/sys/fs/pstore/`（`console-ramoops-0` / `pmsg-ramoops-0`）确认目标内核完成引导并执行首启用户态 `/init`（输出 `quest-handoff: init reached userspace` 与目标内核版本字符串）。

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

`allow_tee=1` 把 TEE 驱动已绑定从拒绝改为告警，不实现句柄交接；配合目标 DTB `--disable /soc/qseecom@c1700000` 使用，已在真机首启切换中验证可行。

---

## 合成规划回归与本次目标镜像

仓库跟踪了可复现的合成 DTS、运行脚本和输入说明，位于 [tests/fixtures/synthetic-plan](tests/fixtures/synthetic-plan/README.md)。合成测试中的所有 RAM、DTB 和保留区地址仅用于 CI 回归验证规划器对内核冲突、reserved-memory 冲突和 DTB 映射块冲突的拦截逻辑；真机装机使用的是设备端 `bringup.sh` 采集的实时 `/proc/iomem` 与 `runtime.dtb`。

目标 `5.10.240` 内核（保持 `5.10.240` 版本号，启用 VA48 / NTSync / BakaSU / Droidspaces）与 257 个模块已完成构建并在 Quest 3S 真机完成热切换引导验证。由最终 ELF 导出的 `Image.el1` 为 34,208,256 字节，SHA256 `f7aa14c33cf72312789ac12f2be3063b409f929321576a92ab209f0b9bbf7454`；运行区间为 35,848,192 字节。导出脚本验证了 25 个 alloc sections，并仅排除 `_end` 后 206,348 字节的顶层 `.eh_frame`。完整配置、内核和模块指纹见[目标构建报告](https://github.com/H2Omeow/oculus-linux-kernel/blob/va48-quest3s/tools/quest3s/BUILD_ARTIFACTS.md)。
