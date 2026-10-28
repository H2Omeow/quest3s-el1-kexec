# 验证记录

本记录汇总仓库中的主机侧验证。**没有任何一项代表已在 Quest 3S 上加载模块、启动目标内核或完成热切换。** 设备验证仍需要准确匹配的原厂内核构建资料、设备运行时 DT 和真实物理内存布局。

## 模块候选构建

GitHub Actions 的 `Stock-tree module build` 在 stock `729ec6c4b2086454e0157cbc810f5a460789cc67` 上用 `vendor/oculus_anorak_defconfig` 和 Android clang `r416183b` 准备输出树，然后对本模块执行 `--check-only`：全部 20 个目标文件和 Full LTO 链接零编译警告（该配置为 VA39、三级页表、CFI、SCS、MODVERSIONS，`QCOM_WDT_CORE=m`、`GH_VIRT_WATCHDOG=m`）。它仍然没有设备的 `Module.symvers`，所以不检查符号 CRC。


EL1 loader 已在候选 stock 输出树上完成 C/汇编编译、LTO 到原生 ELF 的链接和 modpost 检查。候选配置是手工合并配置，不是设备完整 `.config`，而且没有设备对应的 `Module.symvers`。因此 modpost 跳过 unresolved symbol 检查，没有 CRC 校验，`--check-only` 也不会生成 `.ko`。这不是可加载模块的 ABI 兼容性证明。

要检查一个候选输出树，可运行：

```sh
scripts/build-module.sh --check-only /path/to/candidate-stock-output
```

精确设备构建需要该运行内核的配置、生成头文件、工具链和 `Module.symvers`。构建流程及上游来源见 [README.md](README.md) 与 [PROVENANCE.md](PROVENANCE.md)。

## 主机测试

`tests/test_memory_guard.py` 把实际 `kernel/memory_guard.c` 函数编译进带 UBSan 的主机 harness，模拟 stock resource tree、FDT reserve map 和 OF reserved-memory inventory。当前 36 个用例覆盖可用 RAM、固件与嵌套资源冲突、资源树祖先与 sibling 遍历、旧内核通用 init reservation 豁免及两侧残余保留区、DTB 2 MiB 映射块、静态与动态 reserved-memory、多个 `reg` 元组、FDT 格式错误及整数端点边界。

`tests/test_handoff.py` 从生产源码提取控制流和平台函数，再注入 host stubs。当前覆盖 EXEC 的 11 个控制流场景（含远程处理器停止失败与回滚重启）、兼容符号绑定的 13 个初始化/分发场景（含 freeze 重试上限），以及平台回滚的 38 个场景（含 gh-watchdog、DT watchdog 清单 fail closed、`allow_tee` 与 watchdog 演练恢复）。`make -C user test` 另外覆盖 report 解析、动态保留区固定、secure 区间 no-map 合并、`--disable`、`/proc/iomem` 解析及 `dump-dt` 与 FDT 的双向等价。EXEC 流程验证 transition serialization、冻结用户进程、在目标内存复查前阻断 device probing、内存拒绝后的解除与解冻、平台预检失败回滚，以及进入不可逆 shutdown 后失败时 panic 和成功交接不返回。

运行这些测试：

```sh
python3 tests/test_memory_guard.py
python3 tests/test_handoff.py
```

这些 harness 检查真实生产函数的控制流与内存判断，但依赖是主机模拟对象。它们不能验证内核并发语义、实际设备驱动布局、固件副作用、ARM64 交接指令或硬件行为。

## 平台策略边界

当前 Qualcomm 适配通过 warm shutdown callback 保留运行中的 SCM/PMIC 固件状态，避免原 SCM shutdown 停止 PMIC arbiter 等不可逆操作。它会替换 watchdog shutdown，尝试停用 secure watchdog、停止 pet timer 并禁用 app watchdog；shutdown callback 报错或 watchdog 仍启用时不继续交接。

如果 `shared_memory_bridge`、`qseecom` 或 `smcinvoke` 存在已绑定设备，预检会 fail closed。当前实现没有 secure-world handle 所有权转移协议，也不会手工调用这些驱动的 `remove()` 或尝试通过卸载/重载驱动来夺取资源。

模块报告列出 stock `reserved_mem` 表中全部已解析的保留区（`rmem` 行）。CLI 通过 `--report` 把目标 DTB 中只有 `size` 的节点固定为对应 `reg`；没有报告时这些节点仍会拒绝规划，额外指定 `--reserve` 也不会跳过这一限制。

`allow_tee=1` 只把 TEE 驱动已绑定从拒绝改为告警，不实现句柄交接；适用于目标 DTB 已禁用 qseecom、且 TEE 客户端已停止的实验。

## 合成规划输入

仓库跟踪了可复现的合成 DTS、运行脚本和输入说明，位于 [tests/fixtures/synthetic-plan](tests/fixtures/synthetic-plan/README.md)。所有 RAM、DTB 和保留区地址都是合成测试数据，不是 Quest 设备信息。脚本只运行 planner 的 `plan` 命令，把生成 DTB 放在临时目录，不保存机器绝对路径或镜像指纹；它验证规划器对内核冲突、reserved-memory 冲突和 DTB 映射块冲突的处理，不会生成可供设备执行的真实地址计划。

## 本次目标镜像

目标 `5.10.246-quest3s-va48` 与 257 个模块已完成本地构建。由最终 ELF 导出的
`Image.el1` 为 34,208,256 字节，SHA256
`f7aa14c33cf72312789ac12f2be3063b409f929321576a92ab209f0b9bbf7454`；运行区间
为 35,848,192 字节。导出脚本验证了 25 个 alloc sections，并仅排除 `_end` 后
206,348 字节的顶层 `.eh_frame`。四个合成 planner 案例重新通过。完整配置、
内核和模块指纹见[目标构建报告](https://github.com/H2Omeow/oculus-linux-kernel/blob/va48-quest3s/tools/quest3s/BUILD_ARTIFACTS.md)。这些地址规划仍是合成输入，不能直接用于头显。
