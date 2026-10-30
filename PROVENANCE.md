# 来源与构建边界

模块核心起点是 [fabianishere/kexec-mod](https://github.com/fabianishere/kexec-mod)
的提交 `4ba956fd60b29d2fea324515596b392afdcfa2c5`。上游副本独立保留，
本目录单独进行 Quest EL1 适配。保留代码原有版权和 GPLv2 许可，详见
[COPYING.txt](COPYING.txt)。

## 设备精确 ABI 构建资料（已真机验证）

仓库 `device/` 目录保存了从零售版 Quest 3S（内核版本 `5.10.240-g9cb2f620a9a8`）
实机提取的构建基准资料：

- `device/stock-5.10.240-g9cb2f620a9a8.config`：由真机 `/proc/config.gz` 原样导出（保留 VA39、4 KiB 页、三级页表、CFI、Full LTO、SCS 和 MODVERSIONS，且未开启 `CONFIG_KEXEC`）；
- `device/stock-5.10.240-g9cb2f620a9a8.symvers`：通过 `scripts/stock-symvers.py` 从真机 `boot_a.img` 恢复的 14,049 个导出符号 CRC；
- `device/stock-loaded-modules.txt`：设备实际加载的 267 个原厂 vendor 模块清单。

`Device-ABI module build` 工作流使用上述真机配置、符号表以及与设备同代的 Android
clang `r450784e` 构建 `quest_el1_kexec.ko`，并通过 `scripts/check-module-abi.py`
完成全量符号 CRC 与 `vermagic` 校验。该产物已在 Quest 3S `5.10.240-g9cb2f620a9a8`
真机上成功加载并完成向目标内核的热切换。

## 公开源码候选检查边界

除真机 ABI 构建外，仓库保留了基于公开 Quest 3S Linux 5.10.240 源码快照
（提交 `729ec6c4b2086454e0157cbc810f5a460789cc67`）与 `vendor/oculus_anorak_defconfig`
的候选编译检查（`scripts/build-module.sh --check-only`）。

`--check-only` 会执行 C/汇编编译、LTO 到原生 ELF 的链接以及 modpost，并设置
`KBUILD_MODPOST_NOFINAL=1`，不生成 `.ko`。由于候选配置不包含设备 `Module.symvers`，
modpost 会跳过 unresolved symbol 与 CRC 检查；该模式仅用于无设备资料时的编译回归检查，
实际装机必须使用基于 `device/` 真机资料构建的 `.ko`。

## 目标内核与用户态依赖

目标内核基于 `5.10.240` 在另一棵树中构建（保持 `5.10.240` 版本号不变，合入 VA48、NTSync、BakaSU、Droidspaces 等扩展功能），已完成 Image/模块编译与真机热切换引导验证，记录见
[目标构建报告](https://github.com/H2Omeow/oculus-linux-kernel/blob/va48-quest3s/tools/quest3s/BUILD_ARTIFACTS.md)。尽管版本号同为 `5.10.240`，但目标内核启用了四级页表（VA48）及扩展特性，其 `Module.symvers` 和工具链产物属于目标内核环境，不能混作原厂 VA39 loader 模块的构建资料。

用户态 CLI 使用外部 libfdt；宿主通过系统开发包链接，静态 ARM64 构建脚本
从调用者提供的 dtc 源码编译所需文件。已验证 dtc 1.7.2；不在仓库中复制该
依赖，dtc/libfdt 的许可和版权以其源码为准。各项验证记录见
[VALIDATION.md](VALIDATION.md)。
