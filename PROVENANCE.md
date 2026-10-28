# 来源与构建边界

模块核心起点是 [fabianishere/kexec-mod](https://github.com/fabianishere/kexec-mod)
的提交 `4ba956fd60b29d2fea324515596b392afdcfa2c5`。上游副本独立保留，
本目录单独进行 Quest EL1 适配。保留代码原有版权和 GPLv2 许可，详见
[COPYING.txt](COPYING.txt)。

本地候选编译检查使用公开 Quest 3S Linux 5.10.240 源码快照，
对应的 stock 源码提交为 `729ec6c4b2086454e0157cbc810f5a460789cc67`；
候选输出和手工 anorak+panther 合并配置不随仓库分发，也不是设备导出的完整配置。
配置保留 VA39、4 KiB 页、三级页表、CFI、LTO 和 MODVERSIONS，并关闭
`CONFIG_KEXEC`。设备报告的内核版本为 `5.10.240-g9cb2f620a9a8`，但版本字符串
不能证明候选源码、配置、工具链或模块 ABI 与设备匹配。

VA48 目标内核 5.10.246 在另一棵树中构建，已完成本地 Image/模块编译，记录见
[目标构建报告](https://github.com/H2Omeow/oculus-linux-kernel/blob/va48-quest3s/tools/quest3s/BUILD_ARTIFACTS.md)。它的
`Module.symvers`、四级页表和 CFI 工具链产物不能当成 stock 模块的构建资料。

`scripts/build-module.sh --check-only` 会执行 C/汇编编译、LTO 到原生 ELF
的链接以及 modpost；设置 `KBUILD_MODPOST_NOFINAL=1`，不生成 `.ko`。
候选树缺少原厂 `Module.symvers`，modpost 会跳过 unresolved symbol 检查，
也没有模块版本 CRC 验证。这个检查只能证明候选配置上的源码和本机链接通过，
不能证明设备可加载或运行。

用户态 CLI 使用外部 libfdt；宿主通过系统开发包链接，静态 ARM64 构建脚本
从调用者提供的 dtc 源码编译所需文件。已验证 dtc 1.7.2；不在仓库中复制该
依赖，dtc/libfdt 的许可和版权以其源码为准。各项主机验证和边界记录见
[VALIDATION.md](VALIDATION.md)。
