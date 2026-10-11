# SLE Learning TX/RX Demo

基于 **BearPi-Pico H3863（Hi3863 / RISC-V）** 的星闪 **SLE**（NearLink）双板收发示例，
用于学习 SLE 建链流程、SSAP 数据通路与 UART 参数调试。

板 A 编译为 Server，板 B 编译为 Client。

这不是一个"跑通即止"的示例：文档记录了调试过程中发现的 **9 个缺陷**
（6 个已修复、1 个仅作文档说明、2 个记录为未修），包括中断上下文做重活、
RX 回调触发条件与 FIFO 水线搭配错误、应用层缺少消息边界、日志口与数据口同源等。
每个缺陷都附有**根因溯源、修复方式与实测验证**，结论均标注来源
（**实测** / **代码溯源** / **代码审查推断**），不把推断当结论。

## 仓库结构

```
README.md                                        本文档（含全部实验记录与缺陷分析）
LICENSE                                          Apache License 2.0
application/samples/products/
    sle_learning_demo/                           demo 自身的 3 个文件
        CMakeLists.txt
        Kconfig
        sle_learning_demo.c
patches/
    0001-sle-uart-server-adv-use-chip-address.patch
```

## 依赖与集成

本仓库**不包含 SDK**（约 300 MB，且绝大部分不是本项目的工作）。请自行获取
`bearpi-pico_h3863-master` SDK，然后：

```bash
# 1) 放入 demo
cp -r application/samples/products/sle_learning_demo <SDK>/application/samples/products/

# 2) 应用补丁（修复通告地址被写死为 01:02:03:04:05:06 的问题）
cd <SDK> && git apply patches/0001-sle-uart-server-adv-use-chip-address.patch
```

补丁不应用也能编译，但多块 Server 板同时上电会出现**地址冲突**（见「问题 6」）。
编译与烧录步骤见下文「编译与烧录要点」。

## 许可证与归属

本仓库以 **Apache License 2.0** 发布，与所用 SDK 一致。
`patches/` 中的补丁针对 SDK 示例文件
`application/samples/products/sle_uart/sle_uart_server/sle_uart_server_adv.c`（版权归原作者所有），
仅以 diff 形式说明修改内容，不分发其完整源码。

## 硬件与串口连接（实测确认）

- 每块板只需一根 USB-C 线，无需额外接线。
- **日志口和数据口是同一个 UART**：`osal_printk` 的日志与业务数据都走 UART **bus 0**，波特率 **115200**。
- 原因：配置中 `CONFIG_DYNAMIC_UART_ID_BINDDING=y` 时，日志口的总线号在运行时从 NV（`NV_ID_HSO_UART_BUS_ID`）读取，板上取值为 0，与 `CONFIG_SLE_LEARNING_UART_BUS` 重合；应用层启动时又对同一 bus 执行 `uapi_uart_deinit/init` 并按 115200 重建，因此**编译期的 `CONFIG_LOG_UART_BAUDRATE=921600` 不生效**。
- 判读习惯：日志行带 `[SLE-LEARN]` / `[sle uart ...]` / `APP|` 前缀；**不带前缀的裸文本才是对端发来的业务数据**。
- 建议发送数据时自带固定前缀（如 `#DATA:`），便于在日志流中快速识别。

## 编译与烧录要点

- 两块板共用同一份配置（`build/config/target_config/ws63/menuconfig/acore/ws63_liteos_app.config`，由 Kconfig 工具自动生成，**不可手工修改**）：
  必须「编 Server → **备份固件** → 切换角色 → 编 Client」，否则前一块板的固件会被覆盖。
- 改参数必须走 menuconfig（改 Kconfig 的 `default` 无效，因为 `.config` 中已有该符号的取值）：
  `python build.py menuconfig ws63-liteos-app`
  路径：`Application` → `Enable the Sample of products.` → `Support SLE learning TX/RX demo.` → `SLE Learning TX/RX Demo Configuration`
- 烧录文件：`output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_all.fwpkg`

## 必背流程

Client 到 Server：

`UART RX(中断) -> 环形缓冲 -> 任务组帧('\n') -> ssapc_write_req -> SSAP write callback -> UART TX`

Server 到 Client：

`UART RX(中断) -> 环形缓冲 -> 任务组帧('\n') -> ssaps_notify_indicate -> notification callback -> UART TX`

建链流程：

`Server announce + Client seek -> connect -> pair -> MTU exchange -> property discovery -> data transfer`

分帧策略：**分隔符优先 + 空闲超时兜底**

- 收到 `'\n'` → **立即**提交一帧（边界精确；QCOM 勾选 `Send With Enter` 即可）
- `LEARN_FRAME_IDLE_FLUSH_MS`（50ms）内没有新字节 → 把已收到的数据**当作一帧提交**

⚠️ 因此**不加换行也能发出去**，但会有约 50ms 延迟；
且连续输入不会与下一次输入粘成一帧（超时兜底会先把前一段提交掉）。
`'\n'` 仍是推荐方式——它让帧边界**不依赖时序**。

## 建议实验顺序

1. 保持全部默认值，分别编译 Server 和 Client，先跑通双向收发。**（已完成 → 实验 0）**
2. 只修改 `SLE_LEARNING_MAX_PAYLOAD`，发送小于、等于、大于限制的数据。**（已完成 → 实验 1a）**
2b. 两端设置不同的 `MAX_PAYLOAD`，观察 Server 侧的 `[DROP]` 是否被触发。**（已完成 → 实验 1b / 1c）**
2c. 修改 RX 回调的触发条件，修复「小包被困」缺陷并验证实时性。**（已完成 → 实验 1d）**
3. 只修改 `SLE_LEARNING_UART_BAUDRATE`；两端串口工具参数不一致时观察乱码或无有效输出。
4. 修改 UART bus/pin 前先核对板卡复用关系，错误配置通常导致 UART 无收发，而不是 SLE 建链失败。
5. 修改 task stack 后观察任务是否能创建、长时间运行是否稳定；不要一开始就追求最小值。
6. 修改 task priority 后用高负载场景观察调度影响；它通常不改变协议含义，但会影响任务获得 CPU 的时机。

## 日志阶段

- `[SERVER][RX-WRITE]`：Client 写入到达 Server。
- `[SERVER][TX-NOTIFY]`：Server 准备向 Client 发通知。
- `[CLIENT][TX-WRITE]`：Client 准备写 Server 属性。
- `[CLIENT][RX-NOTIFY]`：Client 收到 Server 通知。
- `[DROP] frame len=N exceeds max=M`：**一条完整帧**超过 `MAX_PAYLOAD`，已丢弃。
- `[DROP] uart ring overflow, lost=N bytes`：环形缓冲溢出而丢弃的字节数。
- `[UART-RX-DROP] error events=N`：UART 接收回调报错次数（在任务中汇总打印）。
- `[TX-FAIL]`：SDK 发送调用失败，结合状态码和连接阶段定位。

> 说明：`[DROP]` / `[UART-RX-DROP]` 现在由**任务上下文汇总打印**，
> 因为中断回调里只允许做字节搬运，不允许日志输出。

## 实验记录

### 实验 0：基线 —— 双向收发跑通

| 项 | 内容 |
|---|---|
| 配置 | 全默认：`MAX_PAYLOAD=128`、`UART_BAUDRATE=115200`，板 A = Server / 板 B = Client |
| 连接 | 每板一根 USB-C；串口工具 115200 / 8N1 / 无流控 |
| 结果 | 双向收发正常 |

数据阶段关键日志：

```
Client -> Server : [CLIENT][TX-WRITE] conn=0 handle=2 len=4
                   [SERVER][RX-WRITE] conn=0 handle=2 len=4 status=0x0
Server -> Client : [SERVER][TX-NOTIFY] len=6
                   [CLIENT][RX-NOTIFY] conn=0 len=6 status=0x0
写确认回执        : ssapc write rsp handle:2
                   [sle_write_req_complete_cbk]conn_id:0, err_code:0
                   [sle uart client] ... write_cfm_cb, conn_id:0 ... status:0 handle:02 type:00
```

结论：

- 两条通路（Write / Notify）均已闭环；写确认回执 `status:0` 证明使用的是**需要确认的 Write Request**（而非 Write Command）。
- 连接号为 `conn=0` 时收发仍正常 —— 说明 `g_sle_pair_hdl = conn_id + 1` 的哨兵设计是必要的：若直接保存 `conn_id`，`sle_uart_client_is_connected()` 会返回 0 而被误判为"未配对"，Server → Client 方向将失效。
- Client 服务发现得到的句柄为 `handle=2`，与 Server 侧 `[RX-WRITE] handle=2` 一致，验证**句柄由协议栈运行时分配、Client 必须靠服务发现获取**。
- 传输对应用层是**字节透明**的：ASCII 数据长度与内容完全一致。发送中文时对端显示乱码，原因是两端串口工具**编码不一致**（GBK 发送 / 另一编码解析），与星闪协议无关。
- 内存水位基线（供后续栈实验对比）：Client `mem: used ≈ 92256`，Server `mem: used ≈ 96028`。

### 实验 1a：`SLE_LEARNING_MAX_PAYLOAD` 128 → 32

| 项 | 内容 |
|---|---|
| 只改的参数 | 两块板 `SLE_LEARNING_MAX_PAYLOAD`：128 → 32 |
| 操作 | 从 Client 侧串口依次发送纯数字串（避开多字节编码干扰） |

| 组 | 发送长度 | Client `[TX-WRITE]` | Server `[RX-WRITE]` | `[DROP]` | 现象 |
|---|---|---|---|---|---|
| A | 8 | `len=8` ×1 | `len=8` ×1 | 无 | 完整透传 |
| B | 32 | `len=32` ×1 | `len=32` ×1 | 无 | 完整透传（正好等于上限） |
| C | 64 | `len=32` ×2 | `len=32` ×2 | 无 | **分段，但不丢字节** |

C 组证据：原文为 `12345678901234567890123456789012` + `34567890123456789012345678901234`；
Server 侧两次 `[RX-WRITE] len=32` 分别收到这两段，拼接后与原文完全一致；每次写都有各自独立的确认回执。

结论：

- 超长数据表现为**分段**，而不是丢弃或截断。
- 分段粒度为 `MAX_PAYLOAD`（等于 `rx_buffer_size`）：缓冲区收满触发一次回调（`UART_RX_CONDITION_FULL_OR_IDLE` 的 FULL 条件），剩余数据再次触发回调。
- 三组均**未出现 `[DROP]`**，与该分支不可达的判断一致（见问题 1）。

### 实验 1b：两端 `MAX_PAYLOAD` 不一致（Client 256 / Server 32）

| 项 | 内容 |
|---|---|
| 配置 | Client `MAX_PAYLOAD=256`，Server `MAX_PAYLOAD=32` |
| 操作 | 从 Client 侧发送 32 字节字符串；另做一次 4 字节对照 |

实测结果：

| 发送长度 | Client `[TX-WRITE]` | Server `[DROP]` | Server `[RX-WRITE]` | Client `write_cfm_cb` |
|---|---|---|---|---|
| 32（连点 8 次） | `len=256`（一次） | `len=256 exceeds max=32` | **无** | **`status:0`（成功）** |
| 4（点 1 次） | `len=4` | 无 | `len=4`，数据 `1234` | `status:0` |

两个重要结论：

- **静默失败已确认**：Server 应用层丢弃了数据（无 `[RX-WRITE]`、串口无输出），但 Client 侧写确认回执仍是 `status:0`。
  **协议层写入成功 ≠ 对端应用层处理成功**，发送方对此完全无感知。
- **`[DROP]` 确实可达**：只要写入长度超过本板 `MAX_PAYLOAD` 就触发，验证了问题 1 的修正结论。
  （本次结论受「问题 3」影响：实际写入长度被放大到了缓冲上限，而非应用发送的 32 字节。）

现象解读：Client 发送 32 字节需要**连点 8 次**（`8 × 32 = 256`）才会发出，一次写出 256 字节。
根因见「问题 3」。

### 实验 1c：边界确认（Client 33 / Server 32）

| 项 | 内容 |
|---|---|
| 配置 | Client `MAX_PAYLOAD=33`，Server `MAX_PAYLOAD=32` |
| 操作 | 从 Client 侧发送**一次** 33 字节字符串 |

> 说明：因问题 3，数据只在缓冲写满时才交付；把客户端缓冲设为 33，可让「输入长度 = 缓冲大小」，
> 从而在一次点击后立即产生一次 33 字节的写入（反向利用该缺陷来完成本次边界测试）。

实测结果：

```
Client: [CLIENT][TX-WRITE] conn=0 handle=2 len=33          （一次写出 33 字节）
        ssapc write rsp handle:2
        ... write_cfm_cb, conn_id:0 ... status:0           （协议层"成功"）
Server: [SERVER][DROP] len=33 exceeds max=32               （恰好越界 1 字节）
        （无 [RX-WRITE]，串口无数据）
```

结论：

- `MAX_PAYLOAD` 语义为**闭区间上界**：`== 上限` 通过，`> 上限` 拒绝。
  - 32 字节 → 通过（实验 1a B 组）
  - 33 字节 → 丢弃（本次）
- 第三次确认**静默失败**：应用层丢弃数据，发送方收到的写确认仍为 `status:0`。

附带发现（MTU）：

```
ssaps_mtu_changed_cbk ... mtu_size:208, status:0
```

- 双方均按 520 配置（Client `SLE_MTU_SIZE_DEFAULT` / Server `ssaps_set_info`），
  但**实际协商值为 208** —— 又一个「编译期/配置值 ≠ 运行时实际值」的例证，待查其原因与上限来源。
- 由实验 1b 可反推：Client 一次写入 256 字节（> 208），Server 侧回调仍收到完整的 `len=256`，
  说明 SSAP 层在协议栈内部**自动完成分片与重组**，对应用层透明。

### 实验 1d：修复验证（RX 回调条件 + 组帧 + 中断减负）

| 项 | 内容 |
|---|---|
| 配置 | 两端 `SLE_LEARNING_MAX_PAYLOAD=32`、`TASK_STACK_SIZE=0x2000`（修复后代码） |
| 操作 | Client 发 32 字节；Server 发 `12345` 与 32 字节；两端各发一次 33 字节 |

实测日志：

```
SERVER: [SERVER][RX-WRITE] conn=0 handle=2 len=32 status=0     <- Client 发来的 32 字节
        12345678901234567890123456789012
SERVER: [SERVER][TX-NOTIFY] len=5
SERVER: [SERVER][TX-NOTIFY] len=32
CLIENT: [CLIENT][RX-NOTIFY] conn=0 len=5 status=0
        12345
CLIENT: [CLIENT][RX-NOTIFY] conn=0 len=32 status=0
        12345678901234567890123456789012
```

验证结论：

| 验证点 | 修复前 | 修复后（本次实测） |
|---|---|---|
| 32 字节一次点发送 | 需**连点 8 次**，最后写成 `len=256` | ✅ 一次 `len=32`（问题 3 已修） |
| 帧是否被拆开 | 64 字节被拆成 `2 × 32`（实验 1a C 组） | ✅ 32 / 33 字节均作为**一帧**交付 |
| 双向收发 | 正常 | ✅ 正常（5 字节、32 字节双向往返） |
| 超长帧 | `[DROP]` 形同虚设（问题 1） | ✅ `len=33` 被**本地**拦下（问题 1 已修） |
| 不加换行是否还挂起 | 挂起并与后续数据粘帧（问题 7） | ✅ 用户确认：连发 3 次 `1234` 得到三次独立的 `len=4` |

> ⚠️ 留证说明：「32 字节只需点一次」的判定依据是「一次发送只产生一条 `[TX-WRITE]`」，
> 本次截图未显示点击次数与时间戳。若需严格留证，请勾选 QCOM 的 `Show Time` 并做单次点击对照。
> 同理，「不勾 `Send With Enter` 连发 3 次 `1234`」一项为用户口头确认，未留日志。

补充：该次日志中出现的 `updata sle send report handle: ...` 一行**不属于本工程源码**
（`application` / `middleware` / `protocol` / `drivers` 均无此字符串），
来自 SLE 协议栈预编译库内部，出现在 `[SERVER][TX-NOTIFY]` 之后，可忽略。

### 实验 1e：MTU 协商值 208 的来源【已实测】

| 项 | 内容 |
|---|---|
| 现象 | Client 请求 520、Server 声明 520，实际协商结果却是 **208** |
| 本次只改的东西 | `sle_uart_server.c`：把 MTU 声明**提前**到连接建立回调（只改时序，数值不变） |
| 需要重烧的板 | **只有 Server 板**（改动文件仅参与 Server 角色编译） |

**第一步：先分清「MTU 约束什么」（由实验 1b 反推，无需新实验）**

Client 一次写入 256 字节（> MTU 208），Server 仍然收到完整的 `len=256`。因此：

- MTU **不约束应用层的单次写入长度** —— 超出部分由 SSAP 层在协议栈内部自动分片 / 重组，对应用层透明；
- MTU 只决定「一个空口包能装多少」：写 1024 字节会被切成 `⌈1024 / 208⌉ ≈ 5` 个包。

> **MTU 是效率参数，不是能力参数。** MTU 小不会导致发不出去，只会让分片变多、吞吐下降。

**第二步：代码溯源（全工程各只有一处调用）**

| 侧 | 位置 | 行为 |
|---|---|---|
| Client | `sle_uart_client.c` → `sle_uart_client_sample_pair_complete_cbk()` | 收到配对完成事件后**立刻**发出 `ssapc_exchange_info_req(mtu=520)` |
| Server | `sle_uart_server.c` → `sle_pair_complete_cbk()` | 收到配对完成事件后调用 `ssaps_set_info(mtu=520)` |

协商规则是**取双方较小值**（Client 发请求，Server 用本端上限应答）。
最初据此推断：「两端都声明 520 却得到 208，说明应答发生时 Server 的上限还不是 520」。
该推断已被**第一轮实测推翻**（见下）—— 所以下面这段保留为"曾经的假设"，不要当作结论。

**第三步：三个候选假设**

| # | 假设 | 含义 |
|---|---|---|
| H1 | **时序竞争** | Client 的请求比 Server 的 `set_info` 先到，Server 用**协议栈默认值 208** 应答 |
| H2 | **协议栈硬上限** | 520 被协议栈截断到 208（SLE 实现是预编译库，仓库内无源码，查不到常量） |
| H3 | **声明未被采纳** | `ssaps_set_info` 参数或调用时机不当，520 根本没生效 |

**本次改动（B 组）**

新增 `sle_uart_server_set_mtu_info(tag)`：在 `sle_connect_state_changed_cbk()` 的
`SLE_ACB_STATE_CONNECTED` 分支里**提前**调用一次相同的声明，并**打印返回值**；
原有的 `pair_complete` 调用**保留**，使「时序」成为唯一变量。

**第一轮实测结果（声明值 520 + 提前声明）**

```
Server: [sle uart server] set mtu info at connect, mtu_size:520, ret:0x0
        [sle uart server] set mtu info at pair_complete, mtu_size:520, ret:0x0
        [sle uart server] ssaps ssaps_mtu_changed_cbk ... mtu_size:208, status:0
Client: [sle uart client] exchange mtu, mtu size: 520, version: 1.
```

**重要发现（与原有结论冲突）**

1. 两次声明**都成功**（`ret:0x0`）、入参都是 **520**，但服务器侧 `mtu_changed_cbk` 仍报 **208**
   → **208 不等于本端声明值**，它来自协议栈内部，与 `ssaps_set_info()` 的入参无关。
2. 客户端回调报 **520**、服务器回调报 **208** → **两端看到的不是同一个数**。
3. 因此实验 1c 中「实际协商值为 208」的表述**不准确**：208 是**服务器侧回调报出的值**，
   不能直接等同于"协商结果"。H1（时序竞争）**未被这次改动证实**。

**待确定的关键问题**：客户端回调里的 `mtu_size` 究竟代表什么？

| 可能性 | 含义 |
|---|---|
| (i) 对端（Server）的声明值 | 那么 208 就只能是服务器内部上限，而非协商结果 |
| (ii) 客户端自身请求值 520 的回显 | 该回调不能用于判断协商结果，1c 的相关推论需整体重做 |

**第二轮（A/B 变值实验）**：把服务器的声明值从 520 改成 **100**，提前声明保留（时序不变，只变数值）。
（下表为**事先预测**，保留用于与实测对照）

| Client 报告的 `exchange mtu` | 结论 |
|---|---|
| **100** | ✅ 客户端回调收到的是**对端声明值** → 声明 520 时客户端就应看到 520 → 1c 出现的 208 只能源于「声明未及时生效」→ **H1 成立** |
| 仍 **520** | ❌ 该回调只是**回显自身请求**，不能用于判断协商结果，需换手段（只看 `ssaps_mtu_changed_cbk`） |
| 仍 **208** | ⚠️ 是与声明值无关的固定值 → 指向 **H2（协议栈内部上限）** |

同时观察服务器侧 `ssaps_mtu_changed_cbk` 是否也随声明值变化，可交叉验证。

**第二轮实测结果（声明值 100，提前声明保留）**

```
Server: [sle uart server] set mtu info at connect, mtu_size:100, ret:0x0
        [sle uart server] set mtu info at pair_complete, mtu_size:100, ret:0x0
        [sle uart server] ssaps ssaps_mtu_changed_cbk ... mtu_size:208, status:0
Client: 客户端窗口的 exchange mtu 行待补
```

**已确定的结论（服务器侧）**

声明值 **520 → 100**（两次调用均 `ret:0x0`），服务器回调**仍然报 208**。
两次实测得到同一个数，说明：

- **208 与 `ssaps_set_info()` 的入参完全无关** —— 它是服务器侧协议栈内部的固定值（**H2 成立**）；
- 因此 `ssaps_mtu_changed_cbk` 报的既不是"声明值"，也不是"协商结果"，
  而是**协议栈认定的本地上限**；
- 结合下面的客户端结果：把声明值从 520 改成 100 后，**两端回调都没有任何变化**，
  因此 `ssaps_set_info()` 在本工程中的**可观测效果未能确认** ——
  不能断言它"改变了应答中携带的值"，这一点区别于原假设。

**客户端实测结果（服务器声明 100）**

```
Client: [sle uart client] exchange mtu, mtu size: 520, version: 1.
```

服务器声明降到 **100**，客户端**仍报 520**（= 客户端自身的请求值）→ 客户端回调**不随对端声明变化**。

**实验 1e 最终结论**

| 观测对象 | 值 | 是否随配置变化 |
|---|---|---|
| 客户端请求（`SLE_MTU_SIZE_DEFAULT`） | 520 | 是（代码常量） |
| 服务器声明（`ssaps_set_info` 入参） | 520 → 100 | 是（日志已见） |
| 客户端回调 `exchange_info_cbk` | **520** | ❌ 不随对端声明变化 |
| 服务器回调 `mtu_changed_cbk` | **208** | ❌ 不随本端声明变化 |

三条可直接复述的结论：

1. **日志里的 208 不是"协商结果"**，而是服务器侧协议栈内部的固定值。
   实验 1c 把它记为"实际协商值为 208"属于**误读**（已在上文标注）。
2. **`ssaps_set_info()` 入参 / `exchange_info_cbk` / `mtu_changed_cbk` 是三件不同的东西**，
   两个回调都**不能**当作"协商后 MTU 是多少"的判断依据（本工程实测）。
3. **MTU 是效率参数，不是能力参数**：实验 1b 中 Client 一次写入 256 字节（> 208）仍完整送达；
   本次把声明值降到 100，双向收发同样正常 —— 超长写入由 SSAP 层自动分片/重组，对应用层透明。

**关于"提前声明"改动的处置：已回退**

实测证明它**没有任何可观测效果**（客户端数字不变），故不作为修复保留，
`sle_uart_server.c` 已恢复原状 —— 共享示例文件不应留下无依据的改动。
归因记录：`ssaps_set_info()` 的**调用时机**在本次实验中**未被证明有影响**。

**遗留（非阻塞）**：客户端回调 `mtu_size` 的确切语义无法由头文件注释确定（只写了"mtu大小"）。
可选判定：把客户端请求值改为 **600**（> 520）后重测 ——
报 **600** 即"回显自身请求"；报 **520** 即"与对端应答取 min"。


> 注：本实验对 `sle_uart_server.c` 的改动属于**实验性改动**，`sle_uart` 示例当前未参与编译，
> 但该文件为共享文件，实验结束后需按结论决定「保留（作为修复）」还是「回退」。

### 实验 1f：组帧后 `MAX_PAYLOAD` 的语义（帧长边界）

| 项 | 内容 |
|---|---|
| 配置 | 两端 `MAX_PAYLOAD=32` |
| 操作 | 两端各发送一行 **33** 字节文本（`123456789012345678901234567890123`） |

实测结果（两端各出现一次，且**不带角色前缀**）：

```
[SLE-LEARN] [DROP] frame len=33 exceeds max=32
```

结论：

- 组帧后 `MAX_PAYLOAD` 的语义是「**单帧最大字节数**」，判定为**闭区间上界**：
  - 32 → 通过（实验 1a B 组、实验 1d）
  - 33 → 丢弃（本次）
- 33 字节只产生**一条** `[DROP]`，说明它被当作**一整帧**处理，没有被拆成两条 —— 组帧逻辑正确。
- `[DROP]` 之前没有 `[RX-WRITE]`、对端也没有任何记录 → 该帧**根本没有发出去**。

#### 两条 `[DROP]` 打印的区别（易混淆，面试可讲）

| 打印形式 | 代码位置 | 检查对象 | 语义 |
|---|---|---|---|
| `[SLE-LEARN] [DROP] frame len=N exceeds max=M` | `learning_frame_commit()` | 本机串口收到并**组装完成**的帧 | **发送端守门**：拒绝发出超长帧，对端完全无感知 |
| `[SLE-LEARN] [SERVER][DROP] len=N exceeds max=M` | `learning_server_write_cb()` | **对端** SLE 写入的长度 | **接收端守门**：拒绝接收超长写入 |

一眼分辨方法：**看有没有角色前缀**（`[SERVER]`）。
带前缀＝「别人发给我、我不收」；不带前缀＝「我自己要发、我不发」。

### 实验 2：串口波特率（`SLE_LEARNING_UART_BAUDRATE`）【已实测】

| 项 | 内容 |
|---|---|
| 只改的参数 | **仅 Client 板** `SLE_LEARNING_UART_BAUDRATE`：115200 → **9600** |
| 配置路径 | `Application → Enable the Sample of products. → Support SLE learning TX/RX demo. → SLE Learning TX/RX Demo Configuration → UART baud rate` |
| 需要重烧的板 | **只有 Client 板**；Server 板保持 115200，作为对照 |
| 要验证的结论 | ① 改数据口波特率会**连日志一起改**（问题 5：日志口 = 数据口）<br>② 串口波特率是**本地链路参数**，与空口无关 |

**A 组实测（Client 窗口仍设 115200）**

```
Client 窗口 @115200：
  ... rf cali OK. time cost:22, ret:0
  [SLE-LEA                       ← 只读出这 8 个字符，其后全是乱码
  （乱码）

Server 窗口 @115200：
  [sle uart server] pair complete ...
  [sle uart server] ssaps_mtu_changed_cbk ... mtu_size:208, status:0
  [SLE-LEARN] [SERVER] [RX-WRITE] conn=0 handle=2 len=1 status=0   ← Server 侧一切正常
```

✅ **结论 ① 成立**：把数据口波特率改成 9600，**日志口跟着一起变**。

最有力的证据是 `[SLE-LEARN] role=CLIENT: ...` 这一行**只读出前缀 `[SLE-LEA`，后半截已成乱码**。

> 该行是在 `learning_uart_init()` **之前**打印的，本应完全可读。最佳解释：
> `osal_printk` 只是把数据**排队**，实际发送由日志任务**异步**完成 ——
> 这一行在 `uapi_uart_deinit(0)` + `uapi_uart_init(0, 9600)` 改波特率时**正处于发送中**，
> 于是被切成两种波特率。
> （另一种可能是 `uapi_uart_deinit()` 直接丢弃了未发完的部分。）
> 两种解释都指向同一结论：**日志口就是数据口**，且**波特率切换会破坏正在输出的日志**。

**B 组实测（Client 窗口改成 9600）**

```
Client 窗口 @9600：
  （启动段由 115200 发出 → 此处全是乱码）
  [SLE-LEARN] role=CLIENT: enable -> seek -> connect -> discover
  [SLE-LEARN] UART ready: bus=0 baud=9600 tx=17 rx=18 max_payload=32
  [adv_report] event_type: 0x03, addr_type: 0x0000, addr: 35:**:**:**:4d:b1
  [adv_report] data_length: 6, data: 0x01 0x02 0x01 0x02 0x02 0x00
  [adv_report] data_length: 20, data: 0x0c 0x02 0x0a 0x10 0x0b 0x73 ...
  [sle uart client] sle uart scan data : □
  sle_uart_server
```

✅ **结论 ③ 成立**：启动段日志（115200 发出）在 9600 窗口下全是乱码；
而 9600 段在 115200 窗口下同样读不出来 —— **单个串口窗口读不全启动日志**。

✅ **结论 ② 成立**（用户确认，未留日志截图）：波特率对齐后，从 Client 侧发送的数据
由 Server（仍是 115200）**完整正确**收到；反方向亦然。
→ **串口波特率只是"板与 PC 之间那条线"的参数，SLE 空口完全不知道它的存在。**

**顺带解释一条容易误判的日志**

启动日志里必然出现「两次 `try enable` + 一次 `sle enbale fail !`」，**这不是故障**，
原因见 问题 9。

**两个附加发现**

| # | 现象 | 说明 |
|---|---|---|
| 1 | A 组中 Server 收到 `len=1` 的**垃圾数据** | PC→Client 方向同样波特率错配，字节被**错误采样**；Client 把解出的 1 个字节经空闲超时组帧后发给了 Server |
| 2 | Client 扫到的地址为 `35:**:**:**:4d:b1`，Server 自报 `0x38 ... 4d:b1` | 末两字节相同、高位不同 → 疑似**随机地址**机制（未深究，留作观察） |

**由结论 ③ 引出的调试痛点**

日志口与数据口同源、且启动过程中波特率会变化 → **单个串口窗口无法完整读取启动日志**。
这正是正式产品应把日志口与数据口**物理分开**的现实理由。

### 实验 4：任务栈大小（`SLE_LEARNING_TASK_STACK_SIZE`）—— 计划中（未执行实测）

**为什么这个实验值得做**

本次代码修复把「组帧 + 发送」从**中断上下文**搬到了**任务上下文**（见问题 4），
这个任务的栈需求因此显著上升 —— 栈上要承载 `osal_printk` 与 SLE 协议栈调用的开销。
所以「`0x2000` 够不够、最小能用多少」是一个**由我们自己的改动直接引出**的问题，
而不是照抄默认值。

| 项 | 内容 |
|---|---|
| 只改的参数 | `SLE_LEARNING_TASK_STACK_SIZE`（menuconfig 中为 hex 项） |
| 实验板 | **只动 Client 板**；Server 板保持 `0x2000` 作为对照 |
| 建议档位 | `0x2000 → 0x1800 → 0x1000 → 0x800 → 0x600 → 0x400 → 0x200` |
| 观察点 | ① `task creation failed`（`osal_kthread_create` 返回 NULL）<br>② 创建成功但运行时**跑飞 / 重启 / 日志错乱**（栈溢出踩到相邻内存）<br>③ 收发是否仍然正确 |

**必须"带负载"测**

空跑不崩不代表栈够用。每一档都要**实际收发数据**（两端互发几十字节、含 32 字节整帧），
因为栈消耗主要发生在打印与协议栈调用路径上。

**预期与要记录的内容**

| 现象 | 含义 |
|---|---|
| 出现 `[SLE-LEARN] task creation failed` | 栈太小，**创建阶段**就失败（最"干净"的失败） |
| 创建成功但板子随机重启 / 日志残缺 / 收发异常 | **栈溢出**踩到相邻内存 —— 比创建失败更危险，因为**不报错** |
| 一切正常 | 该档位可用，继续下调 |

**结论模板**："最小可用值 = `0x____`；低于它表现为 ____；选 `0x2000` 的理由是 ____。"

## 代码修复记录

修复主线：**让「中断只搬运、任务做重活」，并让帧边界由应用层显式定义。**

| # | 修复项 | 改动位置 | 说明 |
|---|---|---|---|
| 1 | RX 回调触发条件 | `sle_learning_demo.c`（Server / Client 各一处） | `UART_RX_CONDITION_FULL_OR_IDLE` → `UART_RX_CONDITION_FULL_OR_SUFFICIENT_DATA_OR_IDLE`，使 `size` 参数生效（见问题 3） |
| 2 | 中断回调减负 | `learning_uart_rx_isr()` | 回调只把字节搬进环形缓冲，不再打印、不再调用协议栈 API（见问题 4） |
| 3 | 新增任务消费循环 | `learning_role_task()` | Server 改为「带超时轮询断连队列 + 排空环形缓冲」；Client 原来初始化完就 `return`，现改为常驻循环 |
| 4 | 应用层组帧 | `learning_frame_feed()` / `learning_drain_rx_ring()` | 以 `'\n'` 为帧结束符，一条完整帧才发一次（见问题 2） |
| 5 | 长度校验对象改变 | `learning_frame_feed()` | 由「驱动交付块」改为「一条完整帧」，`[DROP]` 重新变得有意义（见问题 1） |
| 6 | 通告地址不再写死 | `../sle_uart/sle_uart_server/sle_uart_server_adv.c` | 改用 `sle_get_local_addr()`，读取失败再回退固定地址（见问题 6） |
| 7 | 分帧策略补强 | `learning_drain_rx_ring()` | 在 `'\n'` 之外增加**空闲超时兜底**（50ms）：避免未加换行时数据挂起或与后续数据粘帧（见问题 7） |

⚠️ 修复引入两个**使用上的变化**，测试前务必注意：

1. **发送必须以换行结尾**（`'\n'` 是帧结束符），见「必背流程」。
2. **发送动作从中断上下文移到了任务上下文**，任务栈需求上升。
   建议把 `SLE_LEARNING_TASK_STACK_SIZE` 调到 **`0x2000`**（menuconfig 中修改）。

## 已发现的问题

### 问题 1：Client 侧的 `[DROP]` 校验不可达【已实测 · 已修复】

`sle_learning_demo.c` 中，同一块板上「接收缓冲区大小」与「应用层长度上限」取的是**同一个**宏：

```c
static uint8_t g_uart_rx_buffer[CONFIG_SLE_LEARNING_MAX_PAYLOAD];   // 缓冲区大小 = 上限
...
.rx_buffer_size = CONFIG_SLE_LEARNING_MAX_PAYLOAD,
...
if (length > CONFIG_SLE_LEARNING_MAX_PAYLOAD) { ... [DROP] ... }    // Client 侧：恒不成立
```

UART 驱动不会交付超过缓冲区大小的数据，因此 **Client 侧**的 `length > MAX_PAYLOAD` 永远不成立，该分支不可达。
实验 1a（两端均设为 32）的 A/B/C 三组均未触发 `[DROP]`，与此一致。

⚠️ 但 **Server 侧**的校验（`learning_server_write_cb` 中判断 `request->length`）**不是死代码**：
它检查的是 SLE 写入长度，由**对端**的配置决定，不受本板缓冲区约束；
两端 `MAX_PAYLOAD` 不一致时（如 Client 256 / Server 32）即可触发 —— 见「待做实验 1b」。

影响：真正生效的限长来自缓冲区大小，超长部分被**静默分段**，调用方无感知；
"看似有保护、实际未生效"比"没有保护"更危险。

**修复后实测（实验 1f）**：把校验对象改为「组装完成的一帧」后，该分支已可达 ——
`[DROP] frame len=33 exceeds max=32`，且发生在**发送之前**（对端无任何记录）。

### 问题 2：应用层缺少消息边界（framing）【已实测 · 已修复】

一条 64 字节的输入被拆成**两次独立的 SLE 写**（各 32 字节），对端收到两次 `[RX-WRITE]`。

- 分组边界由**物理层缓冲区大小 + 发送节奏**隐式决定，而非协议显式定义；
- 改变 `MAX_PAYLOAD` 或发送时序，分组方式随之变化；
- 若上层需要按"一条完整消息"处理，当前实现会导致解析失败。

建议修法：加入**长度前缀**（推荐）或**结束符**（如 `\r\n`），由应用层完成组帧/拆帧。

### 问题 3：RX 回调「触发条件」与「FIFO 水线」搭配错误，小包会被困住【已实测 + 代码溯源 · 已修复】（最严重）

`uapi_uart_register_rx_callback(bus, UART_RX_CONDITION_FULL_OR_IDLE, 1, cb)` 用的条件是 `FULL | IDLE`，
**不含 `SUFFICIENT_DATA` 位**，因此第 3 个参数 `size=1` 形同虚设。

驱动内部有两条 ISR（`drivers/drivers/driver/uart/uart.c`）：

| ISR | 触发源 | 行为 |
|---|---|---|
| `uart_rx_isr` | FIFO 达到**水线** | 把 FIFO 数据搬进软件缓冲，**但不调用回调**（其回调条件要求 `SUFFICIENT_DATA` 位） |
| `uart_idle_isr` | 线路**空闲**（硬件 `CHAR_TIMEOUT`） | 搬进缓冲**并调用回调** |

而硬件 `CHAR_TIMEOUT` 的前提是 **RX FIFO 非空**。于是形成死结：

```
数据到达 → FIFO 攒到水线 → uart_rx_isr 把 FIFO 抽干搬进软件缓冲
         → FIFO 变空 → CHAR_TIMEOUT 无法再触发
         → 数据被困在软件缓冲，直到缓冲写满（FULL 条件）才回调
```

水线 = 16 字节：`extra_attr = NULL` 时 HAL 取默认 `HAL_UART_RX_FIFO_AVAILABLE_LEVEL_1_4`，
且 `CONFIG_UART_FIFO_DEPTH = 64` → `64 >> 2 = 16`。

实测对照（`MAX_PAYLOAD=256`）：

| 发送长度 | 是否达到水线 | 结果 |
|---|---|---|
| 4 字节 | 否 | FIFO 保持非空 → 空闲中断正常触发 → 立即交付 `len=4` |
| 32 字节 | 是 | FIFO 被抽干 → 空闲中断失效 → 需连点 **8 次**攒满 256 才交付 `len=256` |

说明：实验 1a（`MAX_PAYLOAD=32`）之所以看起来正常，只是因为软件缓冲大小与发送长度凑巧相等，
掩盖了该缺陷 —— **参数凑巧掩盖 bug**。

修复（改条件即可，让 `size` 生效）：

```c
uapi_uart_register_rx_callback(CONFIG_SLE_LEARNING_UART_BUS,
    UART_RX_CONDITION_FULL_OR_SUFFICIENT_DATA_OR_IDLE, 1, cb);
```

影响：实时性不可控（小包可能长时间不发出）；且 `length` 会被放大到缓冲上限，
导致实际发送长度与用户输入长度**完全不符**。

### 问题 4：UART 接收回调在中断上下文执行重活【代码审查发现 · 已修复】

`include/driver/uart.h` 明确注明接收回调 **in an interrupt context**，且"回调处理期间收到的数据会被丢弃"。

修复前的实现（`sle_learning_demo.c` 中已删除的 `learning_server_uart_rx` / `learning_client_uart_rx`）在该回调中直接执行：

- `osal_printk`（日志输出本身较慢，且与数据**共用同一个 UART**）
- `ssapc_write_req` / `ssaps_notify_indicate`（协议栈发送 API）

风险：数据率提高、或协议栈发送阻塞时，回调变慢会导致后半段数据被驱动丢弃。
实验 1a 未丢数据，只能说明当前速率与回调耗时尚未越过临界点。
（另注：由于问题 3 导致回调触发频率远低于预期，反而降低了本问题的暴露概率；
修复问题 3 后，本问题的风险会随之上升，应一并处理。）

建议修法：回调内只把数据拷入环形缓冲/消息队列，由任务异步发送（Server 侧处理"断连"事件已采用此思路，数据路径尚未采用）。

### 问题 5：日志口与数据口共用同一 UART，文档未说明【已实测】

按编译配置推断日志口应为独立 UART（`CONFIG_LOG_UART=1` @ 921600），实测却是与数据口相同的 bus 0 @ 115200。
详见开头「硬件与串口连接」小节。

影响：

- 修改 `SLE_LEARNING_UART_BAUDRATE` 会**同时影响日志与数据**；
- 日志与业务数据在串口上互相穿插，必须靠前缀区分；
- 做 UART 引脚类实验时，配错会**连日志一起丢失**，导致无法定位问题（风险高于预期）。

**实验 2 的实测证据**：把 `SLE_LEARNING_UART_BAUDRATE` 从 115200 改成 9600 后，日志输出随之变乱码，
**连正在输出的那一行都被切成两种波特率** —— 这直接证明日志与数据是**同一个 UART**。
该实验同时暴露一个新的调试痛点：**单个串口窗口读不全启动日志**（详见实验 2）。

### 问题 6：通告地址写死【代码审查发现 · 已修复 · 已实测确认】

`../sle_uart/sle_uart_server/sle_uart_server_adv.c` 中 `local_addr` 硬编码为 `{0x01,0x02,0x03,0x04,0x05,0x06}`。

影响：多块 Server 板同时上电会出现**地址冲突**；正式产品应从 efuse 或随机地址生成。

**修复后实测确认**（Server 启动日志）：

```
[sle uart server] sle_uart_local addr: 0x38:**:**:**:4d:b1
[ACore] sle set announce param, own addr:0x38:**:**:**:4d:b1
```

通告参数里携带的是**芯片真实地址**，不再是写死的 `01~06`，问题已消除。

### 问题 7：分隔符组帧下，未终止数据会挂起并与后续数据粘帧【已实测 · 已修复】

实测现象：不勾选 `Send With Enter`，连续发送 3 次 `1234`，对端只收到**一次** `len=12`，内容为 `123412341234`。

原因：分隔符组帧的定义是「**一帧 = 两个分隔符之间的所有字节**」。
前两次没有分隔符，在协议意义上只是"同一帧的前半部分"，必然与第三次合并 —— 行为符合定义。

但它暴露了三个真实缺陷：

| # | 缺陷 | 后果 |
|---|---|---|
| 1 | **没有任何反馈** | 发送方不知道数据在等分隔符，看起来像"丢了" |
| 2 | **可无限期挂起** | 若始终不来 `'\n'`，该帧永远不发；未终止数据还持续占用缓冲，超出部分被静默丢弃 |
| 3 | **语义融合** | 多组独立输入被拼成一条消息 |

修复：在 `'\n'` 之外增加**空闲超时兜底**（50ms 无新字节则提交一帧）。
`'\n'` 仍是首选手段（边界精确、不依赖时序），超时**只作为兜底**。

**修复后实测（实验 1d，用户确认）**：不勾选 `Send With Enter` 连发 3 次 `1234`，
对端收到**三次独立的 `len=4`**，不再粘成一次 `len=12`。

代价（如实记录）：**超时分帧依赖时序，本质是脆弱机制**。
本方案不把它当作主要分帧手段，仅用于兜底，因此该缺点被限制在很小的范围内。
另一个实际影响：手打输入过慢（>50ms/字节）会被拆帧；用 QCOM 整串发送不受影响。

### 问题 8：`MAX_PAYLOAD` 长度校验两端不对称【代码审查发现 · 未修 · 轻微】

| 侧 | 收到对端数据的入口 | 是否校验 `MAX_PAYLOAD` |
|---|---|---|
| Server | `learning_server_write_cb()` | ✅ 校验 `request->length` |
| Client | `sle_uart_notification_cb()` | ❌ 只判空，未校验 `data->data_len` |

后果：对端若按更大的 `MAX_PAYLOAD` 配置运行，Client 会照单全收并转发到串口。
因数据直接写入 UART、**没有缓冲区拷贝**，所以没有内存安全问题，属于**校验不对称**而非越界风险。

修法（可选）：在 `sle_uart_notification_cb()` 中补一条与 Server 对称的长度判断。
本次未改，避免影响已通过的实验基线。

### 问题 9：Client 初始化被重复调用（回调中重入 + 回调内长时间 sleep）【代码审查发现 · 未修 · 轻微】

`sle_uart_client.c`：

```c
void sle_uart_client_init(...)
{
    (void)osal_msleep(5000);                    /* 睡眠 5s，且可能运行在回调里 */
    osal_printk("[SLE Client] try enable.\r\n");
    ...
    if (enable_sle() != ERRCODE_SUCC) {
        osal_printk("[SLE Client] sle enbale fail !\r\n");
    }
}

static void sle_uart_client_sample_sle_enable_cbk(errcode_t status)
{
    osal_printk("sle enable: %d.\r\n", status);
    sle_uart_client_init(...);                  /* ← 在自己的使能回调里又初始化一次 */
    sle_uart_start_scan();
}
```

于是启动日志必然是"两次 `try enable` + 一次 `fail`"：

```
[SLE Client] try enable.          <- 应用任务里的第一次
[ACore] sle enable cbk in, result:0
sle enable: 0.                    <- 使能成功回调
[SLE Client] try enable.          <- 回调里又初始化了一次
[SLE Client] sle enbale fail !    <- 第二次 enable_sle() 必然返回非成功
```

影响：

- 日志里的 `sle enbale fail !` **不是真故障**，是重复使能的返回值，极易误判为建链失败
  （实验 2 的 B 组日志里就出现了）；
- 使能回调内 `osal_msleep(5000)` —— 头文件明确要求回调**不得阻塞或长时间等待**，
  这里在 SLE 服务上下文里睡了 5 秒；
- 回调被**重复注册**。

修法（可选）：把 `sle_uart_client_init()` 从使能回调里移除，只保留 `sle_uart_start_scan()`。
本次未改，避免影响已通过的实验基线。

## 待做实验

| 实验 | 只改的参数 | 修改前 → 修改后 | 预期影响 |
|---|---|---|---|
| 1b | `MAX_PAYLOAD`（两端不同） | 128/128 → 256/32 | **（已完成）** Server 侧触发 `[DROP]`；并发现静默失败与问题 3 |
| 1c | `MAX_PAYLOAD`（Client 33 / Server 32） | 33 / 32 | **（已完成）** 边界确认：32 通过、33 丢弃；附带发现 MTU 实际协商为 208 |
| 1d | RX 回调触发条件（**改代码**） | `FULL_OR_IDLE` → `FULL_OR_SUFFICIENT_DATA_OR_IDLE` | **（已完成）** 32 字节点一次即发出；帧不再被拆开 |
| 1e | MTU 协商值 | 配置 520 | **（已完成）** 已证实 208 是服务器侧协议栈内部值，≠ 协商结果；MTU 不约束应用层写入长度 |
| 1f | 组帧后 `MAX_PAYLOAD` 语义 | 现在为「单帧最大字节数」 | **（已完成）** 33 字节触发 `[DROP] frame len=33 exceeds max=32`，且发生在发送之前 |
| 2 | `UART_BAUDRATE`（仅 Client 板） | 115200 → 9600 | **（已完成）** 改数据口波特率会连日志一起改（日志行被波特率切换劈开）；Server 侧收发不受影响 |
| 3 | `UART_TX_PIN` | 17 → 另一已知可用引脚 | UART 无输出（日志一并消失），SLE 建链仍正常 |
| 4 | `TASK_STACK_SIZE` | **先设为 0x2000** 再逐步下调 | 修复后发送在任务上下文执行，栈需求上升；下调到某值后会出现创建失败或跑飞 |
| 5 | `TASK_PRIORITY` | 28 → 10 | 高负载下调度时机变化，协议语义不变 |
| 6 | `SLE_CONN_INTV_MIN/MAX_DEFAULT` | 0x64 → 0x1F4 | 连接间隔由 12.5ms 变为 62.5ms，时延/吞吐变化 |
| 7 | 本地设备名（仅 Server） | `sle_uart_server` → 其他 | 两端名称不一致时 Client 无法建链 |


