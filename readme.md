# 项目说明：双 ESP32 无线 CMSIS-DAP Bridge

## 1. 项目目标

我要做一个**双 ESP32 的无线 DAP 系统**，不是单个 ESP32 直接连电脑和目标板。

系统由两个 ESP32 组成：

- **A 端 ESP32**
  - 连接电脑
  - 负责和 PC 通信
  - 后续希望能作为 **CMSIS-DAP 前端**
  - 把调试请求通过 Wi-Fi 发给 B 端

- **B 端 ESP32**
  - 连接目标板
  - 负责真正执行 **SWD/JTAG** 调试操作
  - 把执行结果通过 Wi-Fi 返回给 A 端

- **A 和 B 之间**
  - 通过 **Wi-Fi** 通信

整体链路是：

**PC ↔ A端 ESP32 ↔ Wi-Fi ↔ B端 ESP32 ↔ Target**

---

## 2. 关键澄清

这个项目**不是**：

- 单个 ESP32 做网络版 CMSIS-DAP probe
- 单个 ESP32 直接 USB 连 PC、GPIO 连 target

这个项目**是**：

- 一个**双端分布式无线 CMSIS-DAP bridge**
- A 端偏“前端桥接器”
- B 端偏“调试执行器”

---

## 3. 设计原则

### 原则 1：不要通过 Wi-Fi 传 GPIO bit 级时序

A 和 B 之间传输的不能是这种东西：

- SWCLK 拉高
- SWCLK 拉低
- SWDIO 输出 1
- 再读 1 bit

因为 Wi-Fi 延迟太大，这样一定会很慢且不稳定。

### 原则 2：A/B 之间传的是“调试事务”

A 发给 B 的应该是较高层的命令，例如：

- SWD line reset
- 读取 DP IDCODE
- 读/写 DP 寄存器
- 读/写 AP 寄存器
- 连续 block read/write
- target reset
- 设置 SWD 频率

也就是：

**Wi-Fi 上传输的是 SWD/JTAG 事务，不是引脚时序。**

### 原则 3：先做最小可行版本，再接入标准 CMSIS-DAP

开发顺序不要一上来就追求：

- A 端直接枚举为完整 CMSIS-DAP
- B 端支持全部 DAP 命令
- OpenOCD 立即全功能打通

更稳的路线是先把 A/B 无线事务链路和 B 端 SWD 执行做通。

---

## 4. 开发环境与约束

- 框架：**ESP-IDF**
- 不使用 Arduino
- 开发系统：**Ubuntu**
- 芯片默认：**ESP32-S3**
- 优先支持：**SWD**
- JTAG 后续再考虑
- 所有代码必须是**真实可编译工程**
- 先保证正确性，再考虑性能优化

---

## 5. 推荐职责划分

## A 端职责

A 端连接电脑，建议分层如下：

- `host_link`
  - 初期可先用 USB CDC 或 UART
  - 后续升级为 USB CMSIS-DAP 前端

- `dap_frontend`
  - 解析来自 PC 的前端请求
  - 初期可以先是自定义命令
  - 后续替换/扩展为 CMSIS-DAP 命令处理

- `transport_proto`
  - 封装 A/B 间私有无线协议
  - 管理包格式、序号、超时、重试

- `wifi_link`
  - Wi-Fi 通信
  - 与 B 端建立连接
  - 发送请求，接收响应

- `session_mgr`
  - 会话状态
  - 请求-响应匹配
  - 心跳、重连、错误恢复

## B 端职责

B 端连接目标板，建议分层如下：

- `wifi_link`
  - 与 A 端通信

- `transport_proto`
  - 解析 A 发来的事务命令
  - 回传执行结果

- `dap_backend`
  - 将上层命令映射为 SWD/JTAG 操作

- `swd_phy`
  - GPIO 方向控制
  - SWCLK/SWDIO 时序
  - turnaround、ack、parity 等底层处理

- `swd_engine`
  - 完成 SWD 事务级实现
  - 例如：
    - line reset
    - read DP IDCODE
    - 读写 DP/AP 寄存器
    - block read/write
    - target reset

- `board_support`
  - 板级 GPIO、LED、reset pin、电平相关控制

---

## 6. A/B 间私有无线协议建议

建议先定义一个私有事务协议，不要直接裸转 CMSIS-DAP 原始包。

### 建议命令类型

- `PING`
- `GET_VERSION`
- `GET_CAPS`
- `SET_SWD_FREQ`
- `SWD_LINE_RESET`
- `TARGET_RESET`
- `SWD_READ_DP`
- `SWD_WRITE_DP`
- `SWD_READ_AP`
- `SWD_WRITE_AP`
- `SWD_READ_BLOCK`
- `SWD_WRITE_BLOCK`

### 每个请求建议包含

- `magic`
- `version`
- `cmd`
- `seq`
- `payload_len`
- `payload`
- `crc` 或校验字段

### 每个响应建议包含

- `magic`
- `version`
- `cmd`
- `seq`
- `status`
- `ack`
- `payload_len`
- `payload`
- `crc`

### 必须具备的机制

- 请求序号 `seq`
- 请求/响应匹配
- 超时
- 重发策略
- 心跳
- 版本兼容
- 最大 payload 协商

---

## 7. 推荐分阶段开发顺序

## 第一阶段：先打通 A/B 无线链路

先不要做完整 CMSIS-DAP。

目标：

- A 端接电脑
- 电脑向 A 发送测试命令
- A 通过 Wi-Fi 转发给 B
- B 执行一个简单动作并返回
- A 把结果回传给电脑

最小闭环示例：

- `PING`
- `GET_VERSION`
- `TARGET_RESET`
- `READ_DP_IDCODE`

### 第一阶段验收标准

- A/B 通信稳定
- 支持断线重连
- 能成功拿到目标 DP IDCODE
- 有清晰日志，便于定位链路和 SWD 问题

## 第二阶段：完善 B 端 SWD 执行器

B 端至少支持：

- SWD line reset
- 读取 DP IDCODE
- 读/写 DP 寄存器
- 读/写 AP 寄存器
- target reset

### 第二阶段验收标准

- 在固定目标板上稳定读出 IDCODE
- 基本寄存器访问正确
- 失败时能区分链路错误和 SWD ACK 错误

## 第三阶段：A 端接入 CMSIS-DAP 前端

此时让 A 端对电脑暴露标准接口。

建议路线：

- 初期：A 端先保留自定义 `host_link` 方便调试
- 后期：A 端增加 USB CMSIS-DAP 前端
- A 把收到的 CMSIS-DAP 请求转换为 A/B 私有事务协议
- B 端继续只做“调试事务执行器”

### 第三阶段验收标准

- PC 侧 OpenOCD 能通过 A 端发起基础调试请求
- 可完成基础连接与 IDCODE 读取

## 第四阶段：性能和稳定性优化

重点优化：

- 批量事务
- block read/write
- 减少往返次数
- 减少日志对时序的影响
- Wi-Fi 固定信道 / 稳定连接策略
- FreeRTOS 任务优先级和缓冲区设计

---

## 8. 我对 Codex 的明确要求

请按以下原则帮我开发：

1. 使用 **ESP-IDF**
2. 目标芯片默认 **ESP32-S3**
3. 默认平台 **Ubuntu**
4. 先做 **双端工程**
5. 不要把 Wi-Fi 当成 GPIO 时序通道
6. A/B 之间传输的是“调试事务”
7. 先实现最小可行版本
8. 所有代码都必须可编译
9. 每个模块都要说明职责、输入输出、线程上下文
10. 不要一上来生成过多耦合代码
11. 先给架构，再给代码
12. 先保证正确性，再做优化

---

## 9. 我希望你先完成的内容

请先完成：

### A 端工程骨架

建议模块：

- `app_main`
- `host_link`
- `wifi_link`
- `transport_proto`
- `session_mgr`
- `dap_frontend`
- `log_utils`

### B 端工程骨架

建议模块：

- `app_main`
- `wifi_link`
- `transport_proto`
- `dap_backend`
- `swd_phy`
- `swd_engine`
- `board_support`
- `log_utils`

### 第一版目标

实现以下能力：

- A/B 通过 Wi-Fi 建立稳定连接
- A 能发送请求给 B
- B 能执行 `PING`
- B 能执行 `SWD_LINE_RESET`
- B 能执行 `READ_DP_IDCODE`
- A 能收到 B 的响应并回传给电脑

---

## 10. 输出要求

请按这个顺序输出：

1. 总体架构
2. A 端与 B 端工程目录结构
3. A/B 私有无线协议设计
4. 第一阶段实现计划
5. 逐文件代码
6. 编译与运行说明
7. 测试方法
8. 下一阶段建议