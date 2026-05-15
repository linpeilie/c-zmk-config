# IQS550 ZMK trackpad debug

GR-Trackpad65 已经能用 RP2040 Arduino sketch 正常输出触摸数据时，硬件和 IQS550 配置基本可以确认没问题。ZMK 下无响应时按下面顺序排查。

## 先刷 USB debug 固件

GitHub Actions 的 `futaba_debug` 目标现在是：

```yaml
board: nice_nano_v2
shield: futaba futaba_debug
snippet: zmk-usb-logging
artifact-name: futaba_debug
```

它是在普通 `futaba` shield 上叠加 USB 日志，不再是独立键盘定义。刷入后用 USB 串口看日志。

## 日志判断

启动后应看到：

```text
IQS5xx product ... project ... version ...
RDY GPIO not present, using polling mode ...
IQS5xx trackpad initialized
```

如果没有 `IQS5xx product`：

- 驱动没有初始化，检查 `CONFIG_ZMK_POINTING=y` 和 `iqs550` 节点是否 `status = "okay"`。
- 如果用的是旧的 `futaba_debug` artifact，重新构建；旧版本会关闭 I2C。

如果有 `Failed to read IQS5xx product/version` 或 `Failed to read IQS5xx touch data`：

- I2C 没读到 `0x74`。检查 FFC 方向、SDA/SCL、3V3/GND。
- 确认当前刷的是普通 ZMK 固件，不是 `futaba_iqs550_flash` writer 固件。
- 可以临时把 `poll-interval-ms` 调到 `<20>` 或 `<30>`，降低轮询频率。

触摸时 debug 日志应出现：

```text
IQS5xx move rel=(...)
IQS5xx one-finger tap
IQS5xx two-finger tap
IQS5xx scroll fingers=... rel=(...)
```

如果这些日志存在但电脑鼠标不动：

- USB 测试时确认刷的是 `futaba_debug`，它强制 `CONFIG_ZMK_USB=y`。
- BLE 测试时删除系统里原来的 Futaba 蓝牙设备，然后重新配对。添加鼠标 HID 后，很多系统会缓存旧的 keyboard-only HID 描述符。
- 在系统蓝牙设备详情里确认它被识别为键盘和鼠标/指针设备。

如果没有 movement 日志，但有初始化成功：

- IQS550 正在被读到，但没有上报触摸事件。确认触控板表面有覆盖材料。
- 先保持 `movement-divisor = <1>`，避免被缩放到 0。
- 如果没有接 RDY，这块 Futaba PCB 使用 polling mode，`poll-interval-ms = <12>` 是默认值；可试 `<8>`, `<20>`, `<30>`。

## 正常固件注意事项

普通 `futaba` 固件用于日常使用；`futaba_iqs550_flash` 只用于写入 IQS550 固件配置。IQS550 已经由 RP2040 或 writer 固件写好后，日常不要再刷 `futaba_iqs550_flash`。

如果通过 BLE 使用，改动 `CONFIG_ZMK_POINTING` 或加入鼠标报告后，需要清除旧配对并重新配对。
