# EL1 热切换中的安全世界交接

当前实现保留正在运行的安全固件，并修正普通关机路径中不适合 warm
handoff 的 SCM/PMIC 与 watchdog 操作。**安全世界重载和活动 TEE 对象的跨内核
所有权交接尚未实现，也未在 Quest 3S 设备上验证。** 当
`shared_memory_bridge`、`qseecom` 或 `smcinvoke` 有绑定设备时，加载器会在进入
不可返回的关机阶段前返回 `-EOPNOTSUPP`。这可能是实际原厂系统首先遇到的阻断项。

本文的原厂源码证据固定在
[facebookincubator/oculus-linux-kernel 的提交
`729ec6c4b2086454e0157cbc810f5a460789cc67`][stock]。它是公开源码快照，
尚不能证明与设备报告的 `5.10.240-g9cb2f620a9a8` 具有完全相同的配置或 ABI。
本仓库实现见 [kernel/platform_quest.c](kernel/platform_quest.c) 和
[kernel/kexec_drv.c](kernel/kexec_drv.c)；候选编译与主机检查的范围见
[VALIDATION.md](VALIDATION.md)。

## EL1 切换不会重新启动安全固件

这里的 EL1 是 Non-secure EL1。替换该层的 Linux 不会使处理器重新经过 Boot ROM、
引导链或安全固件的冷启动初始化，也不会赋予新内核直接重写 Secure 内存或
EL3 状态的权限。若需要从 EL1 请求重载安全固件，必须由已运行的可信固件提供并
授权相应协议；普通内存复制、修改异常级寄存器或跳入新 Linux 都不能替代该协议。

原厂 [SCM SMC 传输实现][scm-smc] 把调用交给固件，并处理固件返回的状态。
它本身不是安全固件的装载器。[`qcom_scm_pas_auth_and_reset()` 和
`qcom_scm_pas_shutdown()`][scm-pas] 面向指定 `peripheral` 的远端处理器；
不能从函数名推导出它们能重载当前执行 SCM/TEE 服务的安全固件。
QSEE 的应用卸载接口也只处理应用，不代表 QSEE OS 本身已重置。

在本文核对的公开 SCM、QSEE 和 SMC Invoke 路径中，没有找到可供本加载器使用的
“排空所有 Linux 客户端、重载安全固件、重新建立全部客户端状态”的完整协议。
这不证明设备固件不存在私有服务；本项目没有这种服务的可核查规范、调用授权或
设备验证，因此不对其进行猜测性调用。

## SCM：避免 halt PMIC arbiter

原厂 [`qcom_scm_shutdown()`][scm-shutdown] 会销毁 Linux 的 waitq IDR，
按状态停用 SDI，调用 `qcom_scm_halt_spmi_pmic_arbiter()`，并在配置要求时退出
download mode。[PMIC halt 接口的注释][scm-pmic] 明确说明：调用后 MSM 不再向
PMIC 发送 SPMI 事务，其用途是避免降低 `PS_HOLD` 时正在进行的事务造成锁死。
这个目的与直接跳入另一个 Linux 的 warm handoff 不同；所核对的公开接口中也没有
供本加载器配套使用的 PMIC arbiter re-enable 路径。

本仓库的 `quest_scm_warm_shutdown()` 在核实设备仍绑定到预期 SCM 驱动后，
只记录该回调已执行，保留已有 SCM/PMIC 状态。适配器仅替换这个经过身份和回调
检查的 SCM shutdown；系统仍执行其他设备的普通 shutdown。替换不等于安全世界
重启，也不代表新内核能够继承旧 Linux 的 IDR、指针或等待队列。

尤其需要继续解决的是 [SCM WAITQ 流程][scm-waitq]：固件可能返回
`QCOM_SCM_WAITQ_SLEEP`，Linux 随后通过 IDR 查找 completion、等待 IRQ 并用
`smc_call_ctx` 恢复调用。保留 SCM shutdown 不会自动排空这些上下文。冻结用户
进程和阻断新设备 probe 也不能证明所有内核线程、workqueue 或其他 SCM 使用者已
停止调用。目前没有实现跨内核转移这些上下文的协议，也没有证明它们在跳转前为空。

## Watchdog：在不可返回阶段停用

原厂 [`msm_watchdog` 驱动][wdt-driver] 提供 remove 和 PM 回调，没有 shutdown。
仅调用 [`qcom_wdt_pet_suspend()`][wdt-suspend] 不足以覆盖热切换：该函数在
`wakeup_irq_enable` 为真时会保留 app watchdog，只停止 pet timer 并重置计数器。
新内核尚未接管时，旧计数器仍可能超时。

原厂 [`qcom_wdt_disable_set()`][wdt-disable] 的正常 disable 路径先请求
[`qcom_scm_sec_wdog_deactivate()`][scm-wdt]，再停用 app watchdog；该 sysfs
接口只接受 disable，没有 enable 操作。核对的 SCM 头文件和实现仅提供 secure
watchdog deactivate/trigger，没有相应的恢复服务。因此当前加载器把 secure
deactivate 视为**不可回滚的固件状态变更**：这里的含义是没有已验证的恢复协议，
并非声称固件在任何重启条件下都不能再次启用它。

本仓库的 `quest_watchdog_shutdown()` 只在进入不可返回的设备 shutdown 阶段后
执行该请求。它要求 secure deactivate 成功，再暂停已初始化的 pet 路径，显式
调用 app watchdog 的 `disable_wdt`，并检查 ENABLE 寄存器为零。
[原厂 app disable 实现][wdt-mmio] 是向该寄存器写零并完成内存屏障。
平台预检失败时不会预先停用 secure watchdog；预检中的回调替换可以撤销。
一旦设备 shutdown 已开始，平台回调缺失或停用失败会触发 panic，拒绝跳入目标
内核，不会尝试返回旧用户态继续运行。

这些检查仍不能证明设备上的固件接受该服务、新内核将正确初始化 watchdog，
或 panic 后一定存在自动复位路径；特别是 secure deactivate 已成功时，不能再
假设原 watchdog 会完成故障恢复。

## TEE：存在释放接口，但缺少完整交接协议

不能把“没有安全释放接口”作为阻断理由：原厂源码确实有合法的逐对象释放接口。
问题在于它们各自具有生命周期、调用者和完成条件，尚未组成可验证的全系统交接。

| 组件 | 公开源码提供的释放路径 | 为什么当前不能据此直接热切换 |
| --- | --- | --- |
| shared memory bridge | [`qtee_shmbridge_deregister(handle)`][bridge-deregister] 调用 SCM 删除指定 bridge，成功后删除 Linux 列表项。 | 需要先证明所有使用该 bridge 的客户端和异步调用已经结束，且所有固件引用已消失。它不转移 bridge 给新内核。 |
| QSEECom | [`qseecom_shutdown_app(handle)`][qsee-shutdown] 对已登记的 kernel client 卸载应用；[`qseecom_release()`][qsee-release] 根据文件类型注销 listener、安排应用卸载或释放 DMA 映射。 | file release 可能转交后台 worker 完成；单个 handle 的关闭不能证明所有 listener、应用、kernel client 和映射均已回收。 |
| SMC Invoke | [`smcinvoke_release_filp()`][smci-release] 释放 callback server 或 TZ object；`-EBUSY` 时会把 object 放入 pending list，由 worker 后续处理。 | 函数返回成功可能只代表释放工作已排队；源码还明确指出 root object 是不可销毁的。没有“关闭文件即清空整个安全世界”的保证。 |

原厂的 remove 也不是可直接借用的 warm handoff 协议：

- [`qtee_shmbridge_remove()`][bridge-remove] 在调用 deregister 后直接销毁
  genpool、取消 DMA 映射并释放 backing pages，没有传播 deregister 的错误。
  在仍有使用者时手工调用它，无法证明固件已经停止访问这些页。
- [`qseecom_remove()`][qsee-remove] 遍历 kernel client、卸载 commonlib、
  注销若干 bridge，停止 worker 并释放设备资源。
  [`qseecom_control`][qsee-state] 同时保存应用、listener、kernel client 和
  pending 状态；当前加载器没有这些资源的完整清点与排空证明，也没有实现适用于
  该 remove 路径的全客户端同步条件。
- [`smcinvoke_remove()` 和 suspend][smci-remove] 分别停止 worker/注销字符
  设备、检查 `cb_reqs_inflight`。它们没有遍历并移交全部 TZ object 的实现。

这三个原厂 platform driver 均没有 shutdown 回调。因此普通 `device_shutdown()`
不会自动执行上述 remove。强行补一次 remove、停止守护进程、关闭可见文件，或
为了通过预检而提前 unbind 驱动，都不能替代对 secure-side ownership 的证明。

当前 `quest_platform_prepare()` 在已冻结用户进程、阻断 probe 并持有模块锁的
条件下检查这三个驱动；发现任何绑定设备即拒绝执行。这个限制保留了可返回的失败
路径，不是已经解决了 TEE 交接。没有绑定设备也只能说明该项检查通过，不能证明
其他 SCM 客户端、安全固件或外设不存在未处理的状态。

## 放开阻断前所需的协议和证据

后续适配至少需要逐项取得以下证据，才能把“拒绝有绑定 TEE 驱动”替换为有依据的
交接实现：

1. 与设备固件匹配的服务规范或经验证的调用契约，明确哪些 TA、listener、
   callback server、root object、bridge 和安全内存状态能关闭、保留或重建。
2. 完整的客户端与对象清点；阻止新增引用，停止全部内核及用户态请求来源，
   排空 SCM WAITQ、回调、worker、pending release 和 DMA，并确认释放响应成功。
3. 明确每块共享页、DMA buffer、SMMU 映射和安全权限在交接中的存续期。只要固件
   仍可能引用，就不能让目标 Image、DTB、initrd 或新内核分配器覆盖这些页。
4. 目标内核 probe 协议：重新创建或明确接管 bridge 和对象，处理固件可能保留的
   注册、地址和状态；新的 Linux 指针、锁和引用计数不能直接复用旧内核内存。
5. 在准确匹配原厂 ABI 的模块上验证正常关闭、服务拒绝、超时和异步释放失败，
   并在目标内核验证 SCM、PMIC、TEE 和 watchdog 的重新工作情况。

这些协议和设备证据仍未齐备。当前代码审查与候选编译不能给出有统计意义的开机
成功率，也不能证明安全世界重载已被攻克。发布的实现应据此被理解为具备明确拒绝
条件的 EL1 热切换开发基础。

[stock]: https://github.com/facebookincubator/oculus-linux-kernel/tree/729ec6c4b2086454e0157cbc810f5a460789cc67
[scm-smc]: https://github.com/facebookincubator/oculus-linux-kernel/blob/729ec6c4b2086454e0157cbc810f5a460789cc67/drivers/firmware/qcom_scm-smc.c#L35-L51
[scm-pas]: https://github.com/facebookincubator/oculus-linux-kernel/blob/729ec6c4b2086454e0157cbc810f5a460789cc67/drivers/firmware/qcom_scm.c#L642-L699
[scm-shutdown]: https://github.com/facebookincubator/oculus-linux-kernel/blob/729ec6c4b2086454e0157cbc810f5a460789cc67/drivers/firmware/qcom_scm.c#L2930-L2939
[scm-pmic]: https://github.com/facebookincubator/oculus-linux-kernel/blob/729ec6c4b2086454e0157cbc810f5a460789cc67/drivers/firmware/qcom_scm.c#L997-L1020
[scm-waitq]: https://github.com/facebookincubator/oculus-linux-kernel/blob/729ec6c4b2086454e0157cbc810f5a460789cc67/drivers/firmware/qcom_scm-smc.c#L115-L168
[wdt-driver]: https://github.com/facebookincubator/oculus-linux-kernel/blob/729ec6c4b2086454e0157cbc810f5a460789cc67/drivers/soc/qcom/qcom_soc_wdt.c#L119-L128
[wdt-suspend]: https://github.com/facebookincubator/oculus-linux-kernel/blob/729ec6c4b2086454e0157cbc810f5a460789cc67/drivers/soc/qcom/qcom_wdt_core.c#L278-L304
[wdt-disable]: https://github.com/facebookincubator/oculus-linux-kernel/blob/729ec6c4b2086454e0157cbc810f5a460789cc67/drivers/soc/qcom/qcom_wdt_core.c#L463-L498
[scm-wdt]: https://github.com/facebookincubator/oculus-linux-kernel/blob/729ec6c4b2086454e0157cbc810f5a460789cc67/drivers/firmware/qcom_scm.c#L424-L439
[wdt-mmio]: https://github.com/facebookincubator/oculus-linux-kernel/blob/729ec6c4b2086454e0157cbc810f5a460789cc67/drivers/soc/qcom/qcom_soc_wdt.c#L58-L64
[bridge-deregister]: https://github.com/facebookincubator/oculus-linux-kernel/blob/729ec6c4b2086454e0157cbc810f5a460789cc67/drivers/firmware/qtee_shmbridge.c#L258-L280
[bridge-remove]: https://github.com/facebookincubator/oculus-linux-kernel/blob/729ec6c4b2086454e0157cbc810f5a460789cc67/drivers/firmware/qtee_shmbridge.c#L484-L507
[qsee-shutdown]: https://github.com/facebookincubator/oculus-linux-kernel/blob/729ec6c4b2086454e0157cbc810f5a460789cc67/drivers/misc/qseecom.c#L5220-L5274
[qsee-release]: https://github.com/facebookincubator/oculus-linux-kernel/blob/729ec6c4b2086454e0157cbc810f5a460789cc67/drivers/misc/qseecom.c#L8437-L8492
[qsee-remove]: https://github.com/facebookincubator/oculus-linux-kernel/blob/729ec6c4b2086454e0157cbc810f5a460789cc67/drivers/misc/qseecom.c#L9730-L9774
[qsee-state]: https://github.com/facebookincubator/oculus-linux-kernel/blob/729ec6c4b2086454e0157cbc810f5a460789cc67/drivers/misc/qseecom.c#L273-L335
[smci-release]: https://github.com/facebookincubator/oculus-linux-kernel/blob/729ec6c4b2086454e0157cbc810f5a460789cc67/drivers/soc/qcom/smcinvoke.c#L2724-L2791
[smci-remove]: https://github.com/facebookincubator/oculus-linux-kernel/blob/729ec6c4b2086454e0157cbc810f5a460789cc67/drivers/soc/qcom/smcinvoke.c#L2889-L2939
