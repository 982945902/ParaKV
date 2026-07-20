# ParaKV io_uring 读写异步改造方案

> 目标:把 ParaKV 的 SSD 数据面从同步 `pread`/`pwrite`(+ 每条 fsync 的 WAL)改造为
> 基于 io_uring 的异步、批量、可控持久性的 IO 引擎。面向 LLM KVCache 场景:
> value 为 MB 级、访问是大块批量、写为非关键路径。

## 0. 现状事实(改造的出发点,均已核对代码)

| 位置 | 现状 | 问题 |
|---|---|---|
| `segment_base.h:59-72` | `Read/Insert/BatchInsert/Delete` 全同步返回 `Status` | 提交-完成无法分离,占用调用线程(brpc bthread)干等磁盘 |
| `segment_file.cc` `PRead/PWrite` | 同步循环 syscall | 一次 BatchRead N 个 chunk = N 次独立 syscall,吃不满 NVMe 队列深度 |
| `segment_base.cc:44` | `slot_size_ = key_size + value_size`(默认 528B) | 非 4K 对齐,O_DIRECT 不可用 |
| `segment_file.cc:126-138` | `Insert` = `PWrite(key)`→`PWrite(value)`→`FlushBitmap`,均无 fsync | 数据/bitmap 顺序靠代码顺序,crash 后可能 bitmap 说 live 但数据是垃圾 |
| `wal.cc:245` | `Append` 每条 `pwrite` + `fsync`,一把全局 mutex | 每个写阻塞在同步 fsync,串行,写延迟高(对 cache 语义过重) |

## 1. 领域特性给出的三个简化(为什么 ParaKV 比通用 DB 好做)

1. **chunk 不可变、append-only**:slot 数据写彼此**无依赖**,可完全乱序并发提交;只有"数据 → bitmap"一步有序。
2. **cache 语义**:丢数据只掉性能不掉正确性。⇒ 持久性可放宽为"异步组提交",崩溃丢最近窗口即可(那些 chunk 下次 miss 重算)。
3. **写是非关键路径**:吞吐优先、延迟不敏感 ⇒ 可用大攒批窗口、可容忍 SQPOLL 的批控制粒度。

## 2. io_uring 使用的三条基本律(约束本方案的正确性)

1. **不保证执行顺序**:同批 SQE 可能乱序/并行。要顺序用 `IOSQE_IO_LINK`(链)或应用层排队。
2. **write 返回 ≠ 落盘**:需 `IORING_OP_FSYNC`(`IORING_FSYNC_DATASYNC` = fdatasync)或 FUA。
3. **buffered write 掉 io-wq**:page cache 写常不能 nowait,内核丢给 io-wq 线程池,几十 µs 抖动。延迟敏感写要 O_DIRECT。
4. **link 语义陷阱**:`IO_LINK` 是"前一个**完成**"非"成功";short write / 出错会把后续链节点 `-ECANCELED`。⇒ **必须检查 write 那节 CQE 的 `res == 预期长度`,不能只看 fsync 的 CQE**。

## 3. 核心组件:IoUringEngine(新增)

一个进程级(或 per-NUMA)的 io_uring 引擎,封装 SQ/CQ + 收割线程 + completion 唤醒。
brpc/bthread 的收割与唤醒接入由使用方提供(团队已有成熟方案),本引擎只暴露提交接口。

```cpp
// parakv/core/io/io_uring_engine.h  (新增)
struct IoReq {
  int      fd;
  void*    buf;
  uint64_t offset;
  uint32_t len;
  enum { kRead, kWrite, kFsync } op;
};

// 一组请求作为一个「提交单元」;done 在这一组全部 CQE 收割后触发一次。
// 组内可选顺序约束(用于 数据→bitmap 的 IO_LINK)。
using Completion = std::function<void(Status /*agg*/, std::vector<int32_t> /*res*/)>;

class IoUringEngine {
 public:
  // reqs 内无序并发提交;若 link_last=true,最后一个 req 用 IO_LINK 挂在前面之后。
  void Submit(std::vector<IoReq> reqs, bool link_last, Completion done);
  // 便捷:一组独立读,全部完成回调。
  void SubmitReads(std::vector<IoReq> reads, Completion done);
};
```

要点:
- **`res` 逐项回传**,调用方按基本律④检查每项 `res == len`。
- completion 的唤醒机制(eventfd / bthread resume)在引擎内留接入点,不在本方案范围。
- fixed file / fixed buffer(`IOSQE_FIXED_FILE` / `write_fixed`)为可选优化,第二阶段再上(也为 RDMA MR 注册铺路)。

### 3.1 线程模型:ring 钉在 brpc user_pthread(定稿)

**IoUringEngine 跑在固定的 brpc user_pthread(`bthread::usercode_in_pthread`)上,每线程一个 ring 混读写;ring 的提交/收割全在其 owner user_pthread,brpc bthread 仅通过队列投递 IoReq 并挂起等唤醒,不触碰 ring。**

为什么走 user_pthread 而不是 bthread:
- io_uring 要求**线程稳定**(SINGLE_ISSUER 要求同一 ring 只由同一线程提交 SQE;SQPOLL、`register_files`/`register_buffers` 都有线程归属假设)。
- bthread 是 M:N 可迁移的,work-stealing 会让挂起-唤醒后落到不同 pthread ⇒ 在错误线程碰 ring,轻则性能塌,重则违反 SINGLE_ISSUER 出错。
- user_pthread 把执行钉死在固定 pthread,ring↔线程绑定稳定,上述亲和假设天然满足。

形态:
```
brpc worker bthread (handler)                user_pthread io 线程 (钉住, 持有 ring)
  │ BatchRead handler                          ┌─ 提交 SQE(SINGLE_ISSUER 安全)
  │ 组装 IoReq[] ──── 无锁队列/eventfd ───────→│  收割 CQE
  │ 挂起等 completion                          │  同线程再提交后续(link/fsync)
  │        ←──────── 唤醒(bthread resume) ─────┤
  ▼                                            └─ 全程线程稳定
```
- 跨线程只传 **IoReq 描述符 + completion 句柄**(轻量),bthread 绝不碰 ring 操作。
- 提交只发生在 owner user_pthread ⇒ SINGLE_ISSUER "提交必须回 owner 线程" 的约束自动满足。
- io 线程数量固定(如 per-NUMA 几个),不需要 bthread 的海量并发——ParaKV 的 IO 提交/收割本就是少数固定线程。

### 3.2 ring 归属:每 io 线程一个 ring 混用 + 两个演化钩子

业界主流:**ring 的归属轴是线程,不是操作类型**;读/Segment 写/WAL 混在同一 ring 是常态(Seastar/ScyllaDB、TigerBeetle 皆如此)。拆 ring 的理由从不是"读写语义不同",而是 **flags 不兼容** 或 **故障域/预算隔离**。

P1/P2 阶段:**每个 io 线程一个 ring,读、Segment 写、WAL group commit 全混用**。共享一次 `submit_and_wait` 的摊薄收益。

留两个演化钩子(`Options` 里加 flag,触发任一才拆成双 ring,仍由同一 io 线程驱动):
1. **`sqpoll`(更可能先撞)**:读要低延迟、写/WAL 要高吞吐省提交 syscall,两个 profile 分化时。SQPOLL 与非 SQPOLL 不能共存于一个 ring ⇒ 拆:读 ring(普通)+ 写/WAL ring(SQPOLL)。user_pthread 的线程稳定性正好给 SQPOLL 配一个稳定提交线程。
2. **`iopoll`(后撞)**:读要榨极限延迟。IOPOLL ring 只能放 O_DIRECT 可轮询读,fsync/buffered/timeout 全不支持 ⇒ 拆:读 IOPOLL ring + 写/fsync 普通 ring。
拆开后两个 ring 仍在同一 user_pthread 里轮流 peek,架构不变。

## 4. 读路径改造

### 4.1 分界线(异步推到哪一层)

**阶段一(推荐先做)——异步只到 SegmentBase,上层包成同步:**

给 `SegmentBase` 加批量异步读虚接口:
```cpp
struct ReadReq { uint32_t slot_id; void* key_buf; void* val_buf; };
virtual void SubmitReads(const std::vector<ReadReq>& reqs, Completion done) = 0;
```
`SegmentFile::SubmitReads`:对每个 slot 算 `GetSlotOffset(sid)`,生成 read IoReq(key、value 可合并为一个连续读,因为盘上 `[key][value]` 相邻),一把提交给 IoUringEngine,CQE 全回来后 `done`。

上层 `Index` / backend 暂时"提交后等 completion"包成同步:`index.cc` / `backend.cc` **签名不变**。
- **拿到**:一次 syscall 提交整个 BatchRead、NVMe 深队列并行(读吞吐的大头)。
- **暂不拿到**:释放 bthread(仍阻塞等待,但等的是"一批"而非"逐个")。

**阶段二(可选)——异步推到 Index/backend:**
`Index::BatchGet` 返回 future;backend 的 `BatchRead` handler 挂起 bthread,CQE 唤醒后回填 response。拿到 bthread 不被磁盘阻塞。回归面广,量出阶段一收益后再决定。

### 4.2 新增聚合入口

`Index` 加 `BatchGet(const std::vector<KeyT>& keys, ...)`:
1. 对每个 key 走 map lookup 拿 encoded offset(纯内存,快);
2. 按 (segment_id) 分组;
3. 每组一次 `SegmentFile::SubmitReads`;
4. 全部组 completion 后回填。

backend 的 `BatchRead` handler 从"循环单个 `Get`"改为一次 `BatchGet`。

## 5. 写路径改造

写实际是**两条独立盘上写**,分别处理。

### 5.1 Segment slot 写:用 IO_LINK 表达"数据先于 bitmap"

顺序约束:**slot 数据必须先落,bitmap 后落**(否则 crash 后 bitmap 标记 live 但数据是垃圾 → 脏读)。

- `BatchInsert` 的 N 个 slot 数据:**N 个独立 write SQE,乱序并发**(append-only 无依赖)。
- 最后一次 bitmap 写:`IOSQE_IO_LINK` 挂在数据写之后,保证顺序。
- 是否 fsync:见 §5.3。

```
[write slot_0][write slot_1]...[write slot_{n-1}]   ← 乱序并发
                                                     ← 全部完成后(link)
[write bitmap]                                       ← 最后落 bitmap
```

**bitmap 覆写的注意**:`FlushBitmap` 现在每次 `PWrite` 整个 bitmap 区(一个对齐块反复原地改写)。O_DIRECT 下这是高频对齐块覆写,是写路径 O_DIRECT 化最麻烦处 → 阶段一保持 buffered,O_DIRECT 留阶段二。

### 5.2 WAL 写:group commit(即之前选的"方案 B"的 io_uring 实现)

攒一个事件循环 tick(或大小/时间双阈值)内到达的 WAL 记录 → 合并成一次大 append write + 一次 `IORING_FSYNC_DATASYNC`,用 `IO_LINK` 串:
```
[write batch_buf @ wal_tail]  (IO_LINK)
[fsync DATASYNC]
  → fsync CQE 到达后,统一 resume 这批等 commit 的写方
```
- 一批 N 条摊一次 fsync(fsync 是百 µs~ms 级大头,摊薄是数量级收益)。
- `IO_LINK` 保证 fsync 在 write 之后,免两轮往返。
- **检查 write 节 CQE 的 res**(基本律④),再认 fsync 成功。

### 5.3 持久性语义(cache 场景放宽)

- **WAL**:group commit 异步 fsync。可见性建议"写入内存 map 后立即可见,后台批 fsync"——崩溃丢最近一窗口索引项(重算)。符合 cache 语义,写延迟最低。
  - (若要保守:入队后阻塞等所属组 fsync 完成再返回 + 才装进 map;保留原耐久契约,延迟 = 组提交周期。)
- **Segment**:数据→bitmap 用 IO_LINK 保序;fsync 可**周期性**下发(非每批),崩溃丢最近未 fsync 的 slot(bitmap 也未持久 → 重建时视为未写,一致)。

## 6. O_DIRECT 与对齐(阶段二,连锁 §3.2)

阶段一用 **io_uring + buffered IO**:不碰 slot 布局,现有 528B slot 直接提交,先拿 batching + 深队列。

阶段二上 O_DIRECT,必须:
- `CalculateLayout`(`segment_base.cc:44`)改为 `slot_size_ = align_up(key_size + value_size, 4096)`,或分离 key/value 区、value 单独 4K 对齐(= §3.2 layer-major)。
- 所有 buffer `posix_memalign(4096)`(也为 RDMA MR 注册铺路)。
- **破坏磁盘格式兼容**(旧 528B 文件读不了)⇒ 必须与 §3.2 布局重构一起做,单独立项。
- 权衡:O_DIRECT slot 定长 4K,小 value 浪费大 ⇒ 仅对大 value(MB 级 chunk)划算,正是目标场景。

## 7. 分阶段落地

| 阶段 | 内容 | 布局改动 | 接口签名改动 | 风险 |
|---|---|---|---|---|
| **P1** | IoUringEngine + `SegmentBase::SubmitReads` + `Index::BatchGet`;读路径 batch(buffered) | 无 | 无(上层包同步) | 低 |
| **P2** | Segment 写走 io_uring:数据 IO_LINK bitmap;WAL group commit | 无 | 无 | 中(顺序/持久性正确性) |
| **P3** | 异步推到 Index/backend,释放 bthread | 无 | `Index::BatchGet` future 化 + handler | 中高(回归面) |
| **P4** | O_DIRECT + slot 4K 对齐(合并 §3.2 layer-major)+ fixed buf | **是(破坏兼容)** | 无 | 高 |

## 8. 建议起点

**P1(读路径 batch,buffered,同步包装)**:零布局、零签名改动,拿到 io_uring 读吞吐大头,风险最低。
其次 **P2 的 Segment 数据→bitmap IO_LINK 写**:顺序约束明确、契合 append-only、且修复当前"数据/bitmap 无 fsync 保序"的正确性短板,收益比 WAL 更实。
