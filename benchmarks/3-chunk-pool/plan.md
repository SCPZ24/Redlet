# Redlet 输出缓冲区优化实施计划

> 状态：待实现的设计契约，不是已完成的功能或性能结论。  
> 整理日期：2026-11-09。  
> 配套文档：[问题背景与对照测试方案](./note.md)。两份文件保持在同一目录。  
> 目标：消除输出缓冲前缀搬移，跨连接复用空闲页，并在负载回落后归还物理内存，同时控制尾延迟。

## 0. 工作约定

本轮只改输出路径及其内存管理，保留 RESP2、epoll + LT、网络事件循环线程和现有命令语义。已有后台销毁线程池不参与本内存池，不新增并发访问。

**已确定的方案：**

- 一个 Chunk 对应一个系统基础页；典型 Linux 环境是 4 KiB，运行时读取真实页大小。
- 一个 PageSlab 批量映射多个页，数据页与控制块分离。
- 每连接一个 OutputBuffer，使用侵入式 Chunk 队列和 `sent/written` 游标。
- 一个 Server 共享一个 ChunkPool，空闲块分为 Warm 和 Cold。
- 使用 `sendmsg()` + `iovec` 聚集发送，保留 `MSG_NOSIGNAL`。
- 页级 `MADV_DONTNEED` 回收物理内存；完全空闲的 Slab 再整体 `munmap()`。
- 回收有高低水位、空闲时间、容量上限和单轮预算。

**本轮不做：**小回复 inline buffer、128/256 B 子分配器、伙伴系统、块内环形写入、数据库值的借用式零拷贝、`MSG_ZEROCOPY`、`io_uring`、输入缓冲重写、TCP 参数调优、事件循环无关重构。

小回复仍会占用至少一页，这是本版明确接受、必须测量的成本。后续只有测试证明有必要，才考虑 inline 小缓冲；不要悄悄扩大本轮范围。

## 1. 版本起点与改动边界

核对结果：repo-config、Connection、ReplyWriter。

| 项目 | 该快照的状态 | 本轮安排 |
|---|---|---|
| `OUTPUT_BUFFER_SIZE` | `1024`，限制一次 `send()` 的提交字节数 | 用户先提高，建议 64 KiB，固定为 B0 |
| `MAX_OUTPUT_BYTES` | 4 MiB | 保留每连接待发送协议字节上限 |
| `INPUT_BUFFER_SIZE` | 1 KiB | 不修改 |
| `Connection::outcoming` | `std::string` | 替换为 OutputBuffer |
| 部分发送处理 | 下一次发送前 `erase(0, delivered_pos)` | 只推进 Chunk 游标 |
| `ReplyWriter` | 持有 `std::string&` | 改持有 `OutputBuffer&` |

开始前记录 `git rev-parse HEAD`、`git status --short` 和实际配置。若用户已经提高发送预算，直接采用其版本，不重复修改、不回退。正式基线与比较矩阵见 [note.md](./note.md)。

**发送预算与 Chunk 大小相互独立。** 页大小 4 KiB，不表示每次只发送 4 KiB；64 KiB 预算也不是每连接预分配 64 KiB。

## 2. 内存模型：不能混淆的三个动作

| 动作 | 地址映射 | 数据内容与驻留内存 | 用途 |
|---|---|---|---|
| 归还到 Warm free list | 保留 | 池尚未主动丢弃数据页 | 快速复用 |
| `madvise(page, page_size, MADV_DONTNEED)` 成功 | 保留 | 私有匿名页的旧内容被丢弃，之后按需清零 | 页级物理回收 |
| `munmap(slab.base, slab.bytes)` 成功 | 解除 | 该区域不能再访问 | 整体释放映射 |

以上依赖 **Linux、`MAP_PRIVATE | MAP_ANONYMOUS`、无页锁定** 的数据映射。不能替换为任意 `new char[4096]` 后直接 `madvise`，也不要用语义不同的 `posix_madvise(POSIX_MADV_DONTNEED)` 替代。[mmap][mmap]、[madvise][madvise]

Cold Chunk 的地址仍合法，重新借出不需要再次 `mmap`；第一次实际写页可能触发正常缺页处理，由内核恢复执行。Cold 并不保证访问时一定发生缺页，Warm 也不保证物理页永远驻留。这两个名称是池的逻辑状态，不是 RSS 的别名。

Slab 中一个页仍在使用，不阻止其他空闲页执行 `MADV_DONTNEED`。但它会阻止整个 Slab `munmap`，因此虚拟映射和控制块可能保留。必须同时报告驻留内存、映射容量和元数据，而不是只看 `total_chunks`。

## 3. 架构与所有权

```text
Server
 ├── ChunkPool                         生命周期覆盖全部 Connection
 │    ├── PageSlab A
 │    │    ├── mmap 的 N 个数据页
 │    │    └── N 个稳定地址的 ChunkControlBlock
 │    ├── PageSlab B ...
 │    ├── Warm 空闲链表
 │    └── Cold 空闲链表
 │
 └── Connection
      ├── OutputBuffer                 只借用 Chunk，不拥有底层映射
      │    └── head -> Chunk -> ... -> tail
      └── ReplyWriter -> OutputBuffer
```

### 3.1 数据结构草案

以下为接口设计草案，不是可以直接替换全部头文件的完整实现。沿用仓库现有 C++ 标准，不为本功能强制升级标准或引入新依赖。

```cpp
struct PageSlab;

enum class ChunkState {
    InUse,       // 含 append 临时预留但尚未挂入输出队列的块
    FreeWarm,
    FreeCold,
};

struct ChunkControlBlock {
    std::byte* page_ptr = nullptr;
    std::size_t sent = 0;
    std::size_t written = 0;

    ChunkControlBlock* prev = nullptr;
    ChunkControlBlock* next = nullptr;

    PageSlab* slab = nullptr;
    std::int64_t free_since_ms = 0;
    ChunkState state = ChunkState::FreeCold;
};

struct PageSlab {
    std::byte* base = nullptr;
    std::size_t bytes = 0;
    std::size_t chunk_count = 0;
    std::size_t borrowed_count = 0;
    std::int64_t empty_since_ms = 0;
    std::unique_ptr<ChunkControlBlock[]> controls;

    // 构造完成后地址固定；映射由 RAII 管理。
    // 用额外的 Slab 级链接或维护游标支持有界维护，不复用 Chunk 的链接。
};
```

控制块不能放入可 `MADV_DONTNEED` 的数据页，否则链表和状态也会被丢弃。每个 Slab 的控制块数组分配一次，后续不扩展或搬移数组。

ChunkPool 以 `unique_ptr<PageSlab>` 拥有 Slab，容器扩容不能移动 Slab 对象本身。可用 `std::list<std::unique_ptr<PageSlab>>` 管理 Slab，少量 Slab 级节点分配可以接受；**不能每借还一个 Chunk 就为 `std::list` 新分配/删除一个节点**。

Warm、Cold 和 OutputBuffer 队列都使用侵入式双向链接。同一个块只能处于一个队列；复用 `prev/next` 前先从原队列摘除。临时预留链也遵循这个规则。

不需要每 Chunk 一个 `shared_ptr`。OutputBuffer 不拷贝、不移动；Connection 构造时引用对应池。Server 成员声明中先声明池，再声明 Connection 容器，利用逆序析构保证连接先归还全部块。参照现有 [Server][repo-server] 修改 `try_emplace` 参数。

## 4. 必须一直成立的不变量

### 4.1 Chunk 与输出字节

```text
0 <= sent <= written <= chunk_size

[0, sent)                 已被普通 socket 发送接受
[sent, written)           尚未发送
[written, chunk_size)     尚未写入
```

- 只有队尾追加；头部已发送区域不再写入，也不执行搬移。
- 对外可见的稳定状态中，每个队列节点都有 `sent < written`，不保留空 Chunk。
- 中间块全部填满且未消费，只有头块可能有已发送前缀，只有尾块可能未写满。
- `pending_bytes == sum(written - sent)`。
- `pending_bytes == 0` 等价于 `head == nullptr && tail == nullptr`。
- 空队列不借页；长度为零的追加不借页。
- 尾块恰好写满但没有更多数据时，不提前分配下一块。

**归还条件是 `sent == written`，不是 `sent == chunk_size`。** 一个只写入并发送了 5 B 的块也要立即归还池。

由上述布局可推出，对非空稳定队列：

```text
borrowed_chunks * chunk_size - pending_bytes < 2 * chunk_size
```

该界限不含控制块、池缓存和一次 append 的临时预留页。它不意味着小回复没有浪费：5 B 未发送回复仍借用一页。

### 4.2 Pool 与 Slab

```text
total_chunks = in_use_chunks + warm_free_chunks + cold_free_chunks
mapped_bytes = total_chunks * chunk_size
slab.borrowed_count = 该 Slab 中 InUse 块数
```

`InUse` 包括临时预留。池不得回收或解除包含临时预留块的 Slab。`release` 要校验块属于本池且当前为 InUse，拒绝重复归还。

全局 `mapped_bytes` 上限只约束池数据映射，不等于进程总内存上限；数据库、输入缓冲、线程栈、控制块和内核 socket 内存另算。

## 5. 具体接口契约

### 5.1 ChunkPool

```cpp
class ChunkPool {
public:
    explicit ChunkPool(PoolConfig config);
    ~ChunkPool();

    std::size_t chunk_size() const noexcept;
    ChunkControlBlock* try_acquire();
    void release(ChunkControlBlock* chunk, std::int64_t now_ms) noexcept;

    void maintain(std::int64_t now_ms);
    std::int64_t next_maintenance_ms() const noexcept;
    PoolStats stats() const noexcept;

    ChunkPool(const ChunkPool&) = delete;
    ChunkPool& operator=(const ChunkPool&) = delete;
};
```

| 接口 | 契约 |
|---|---|
| 构造 | 读取并验证页大小、参数和上限；不预触全部数据页；初始化错误明确报错 |
| `try_acquire` | Warm 优先，再 Cold，最后按需增长；返回块已脱链，`sent=written=0`，状态 InUse；容量/映射/元数据分配失败返回空指针并记原因 |
| `release` | 调用方先脱离使用队列；清游标、减借用计数、加入 Warm 队列；只改元数据，不做系统回收 |
| `maintain` | 只在网络事件循环线程执行；按时间、advice 调用数和 Slab 解除数量预算做回收 |
| 析构 | 正常路径要求借用计数为零；释放剩余映射，禁止抛异常 |

运行期可恢复的池分配失败要转换成返回值，不能让 `std::bad_alloc` 或 `mmap` 失败直接越过事件循环终止服务。初始化失败与正常运行耗尽分开处理。`mmap` 成功不保证之后触页永不遭遇系统 OOM，逻辑容量上限也不提供这种保证。

### 5.2 OutputBuffer

```cpp
struct IovBatch {
    std::size_t count = 0;
    std::size_t bytes = 0;
};

class OutputBuffer {
public:
    explicit OutputBuffer(ChunkPool& pool, std::size_t max_pending_bytes);
    ~OutputBuffer();

    bool append(std::string_view bytes);
    bool push_back(char byte);

    std::size_t pending_bytes() const noexcept;
    bool empty() const noexcept;
    bool can_append(std::size_t bytes) const noexcept;

    IovBatch build_iovecs(iovec* dst, std::size_t max_iov,
                         std::size_t max_bytes) const noexcept;
    void consume(std::size_t bytes, std::int64_t now_ms) noexcept;
    void clear(std::int64_t now_ms) noexcept;

    OutputBuffer(const OutputBuffer&) = delete;
    OutputBuffer& operator=(const OutputBuffer&) = delete;
};
```

- `append`：成功则完整复制输入；失败则已有队列的内容、游标、长度不变。输入必须来自有效且不与输出页重叠的区域；不支持自追加。
- `build_iovecs`：只借用区间，不消费，不临时拼接数据，不动态分配。`max_bytes` 为整批上限。
- `consume`：要求 `bytes <= pending_bytes`，按返回字节数推进并归还空块。
- `clear`：连同未发送数据一起全部丢弃并归还，供连接关闭/析构使用。
- `now_ms` 由一次发送或一次清理统一取得，不要每归还一页调用一次时钟。
- `build_iovecs` 返回的引用只用于紧随其后的同步 `sendmsg`，期间不清空、回收或并发修改。
- 测试可提供 `flatten_for_test()` 等快照接口，但不能在生产发送路径重新拼接连续字符串。

### 5.3 ReplyWriter

保留 `raw_str/integer/error/bulk_str/array_header/null_bulk` 的外部语义和 bool 返回类型，只替换输出依赖。

```cpp
class ReplyWriter {
    OutputBuffer& output;
    // 现有格式化逻辑保留；所有 append / push_back 的返回值必须检查。
};
```

检查 `payload + framing` 时避免先相加导致溢出：

```cpp
const auto pending = output.pending_bytes();
if (pending > MAX_OUTPUT_BYTES) return false;
const auto room = MAX_OUTPUT_BYTES - pending;
if (framing > room) return false;
if (payload > room - framing) return false;
```

限额统计协议字节，包括 RESP 长度字段与 CRLF，不按已借用的页容量计费。

## 6. 跨块追加算法与失败边界

例子：尾块 `written=4093`，追加 `hello`。

```text
旧尾块：[原数据 4093 B][h][e][l]   written=4096
新尾块：[l][o]                   written=2
```

不填充、不跳过字节、不修改协议。长度字段、二进制 payload、甚至 CRLF 都允许跨块。最终发送序列必须与连续编码完全一致。

### 6.1 先预留，再修改可见内容

```text
append(data):
    1. 长度为 0 直接成功；检查 max_pending_bytes，使用减法防溢出。
    2. 计算尾块剩余空间，以及确实需要的额外块数。
       ceil(extra / chunk_size) 用商和余数实现，避免 extra+chunk_size-1 溢出。
    3. 先把额外块全部借到临时预留链。
       任意失败：归还已借块，返回 false；原输出队列保持不变。
    4. 反复复制 min(剩余输入, 当前尾块剩余空间)。
       仅在确实还需写入且当前尾块满时，挂入一个预留块。
    5. 更新 written 和 pending_bytes；预留链必须耗尽。
```

提交阶段只有已验证的指针操作、计数和 `memcpy`，不再分配内存。使用小型 RAII reservation guard 防止失败路径遗失预留链；无需为每个页创建堆上的智能指针对象。

这里的失败原子性针对 **OutputBuffer 的逻辑内容**。池可能已成功映射一个新 Slab 并把页退回缓存；不要求失败后进程映射容量也完全恢复原值。

### 6.2 回复级错误策略

一条 RESP 回复可能包含多个 `append`，单次 append 原子性不等于整条回复事务。

第一版沿用连接级失败策略：任意写入失败立即向上传递，`dispatch` 返回失败，关闭该连接并清空其输出；不继续发送已经构造了一部分的失败回复。当前 Server 在 `read_buffer` 返回非零时删除连接，可接入这条路径。[Connection][repo-connection]、[Server][repo-server]

不新增“已执行命令的数据库回滚”；连接断开时客户端必须把未确认命令视为结果不确定。不要输出半个 RESP 后再追加错误字符串冒充完整回复。

## 7. 聚集发送与 Connection 接入

### 7.1 系统 API

```cpp
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

long system_iov_max = ::sysconf(_SC_IOV_MAX);  // 启动时读取与校验

iovec vec[32]{};                             // 初始实现固定栈数组
const std::size_t limit = /* min(32, 已验证的 system_iov_max) */;
const auto batch = output.build_iovecs(vec, limit, write_budget);

msghdr msg{};
msg.msg_iov = vec;
msg.msg_iovlen = batch.count;
// batch.bytes > 0 时才调用。
ssize_t n = ::sendmsg(fd, &msg, MSG_NOSIGNAL);
```

`sendmsg` 使用 `msg_iov`，`MSG_NOSIGNAL` 抑制 SIGPIPE 但仍需处理错误返回；`writev` 可聚集写入，但本项目选择能保留发送 flags 的 `sendmsg`。[sendmsg][sendmsg]、[iovec/writev][readv]

`sysconf` 返回无效值时不能直接转换为无符号大数；使用经平台确认的保守值或明确初始化失败。无需为支持上述接口改用 C++20 `span`。[sysconf][sysconf]

64 KiB 发送预算在头块只剩 1 B 时可能跨 17 个 4 KiB 块，不能假定 16 个 iovec 总能覆盖预算。32 是起始数组容量，仍须遵守系统上限；预算过大时允许只提交能描述的前缀。

### 7.2 build_iovecs

从 head 顺序遍历，每项：

```cpp
iov_base = chunk.page_ptr + chunk.sent;
iov_len = min(chunk.written - chunk.sent, remaining_budget);
```

遇到以下任一条件停止：队列结束、字节预算耗尽、iovec 容量耗尽。禁止提交 `[written, chunk_size)` 的未写区域。

### 7.3 发送结果与公平性

| 返回 | 操作 |
|---|---|
| `n > 0` | 只 `consume(n)`；可能跨多块，也可能停在块中间 |
| `EINTR` | 重试当前调用，未确认发送前不能消费 |
| `EAGAIN/EWOULDBLOCK` | 保留数据与 EPOLLOUT 关注，等待后续就绪 |
| 其他发送错误 | 返回连接级错误，由 Server 关闭并归还所有页，不终止整个服务器 |
| 非空提交却返回 0 | 视为无进展，不自旋、不消费，记录诊断 |

第一版沿用当前每次 `write_buffer` 一次有效发送尝试的节奏，EINTR 重试除外；不要顺带加入不受限的发送循环。若以后改为多次调用，每次事件的累计成功发送字节仍不得超过同一预算。

例如提交 `4096 + 4096 + 2` B，返回 6000：归还第一块，第二块 `sent += 1904`，第三块不动，`pending_bytes -= 6000`。

清空后取消 EPOLLOUT；仍有数据则保留。对端半关闭输入后，继续发送已有回复，输出清空再按现有语义关闭。

本版为普通拷贝式发送，成功发送的部分可以复用其用户态存储，不必等 ACK；成功返回不代表客户端已收齐。不得启用需要额外完成通知才能复用缓冲的 `MSG_ZEROCOPY`。[Linux 零拷贝说明][zerocopy]

## 8. PageSlab 映射与按需扩容

```cpp
#include <sys/mman.h>
#include <unistd.h>

const long raw_page_size = ::sysconf(_SC_PAGESIZE);
// 验证 raw_page_size > 0，并校验下列乘法不会溢出。
const std::size_t page_size = static_cast<std::size_t>(raw_page_size);
const std::size_t bytes = page_size * pages_per_slab;

void* base = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
if (base == MAP_FAILED) {
    // 保存 errno，清理本次尚未发布的元数据，向调用方返回失败。
}
```

使用系统选择的页对齐地址。新 Slab 只初始化控制块，不 `memset` 整个映射，不启用 `MAP_POPULATE`、`MAP_HUGETLB` 或锁页。[mmap][mmap]

`try_acquire` 顺序：

1. 从 Warm 取最近归还的页；Warm 队列另一端保留最老页供回收。
2. 没有 Warm 时取 Cold 页。
3. 两者都没有时，检查容量上限，再增加一个 Slab；原 Slab 不移动、不重映射。

新页加入 Cold，直到真正借出和写入。已借出时增加池与 Slab 借用数；Slab 从完全空闲变成活跃时，移出整体回收候选队列。

增长必须检查 `slab_bytes <= max_mapped_bytes - mapped_bytes`，先验证 `mapped_bytes <= max_mapped_bytes`。建议配置上限为 Slab 大小的整数倍；非法或小于一个 Slab 的配置在启动时拒绝。

## 9. 回收策略：同时管理 RSS 与映射规模

### 9.1 初始参数

这些数值是待 benchmark 调整的起始值，不是最佳参数或性能保证。

| 参数 | 初始值 | 说明 |
|---|---:|---|
| `pages_per_slab` | 256 | 4 KiB 页时为 1 MiB |
| `max_mapped_bytes` | 256 MiB | 可配置；只约束池数据映射 |
| `warm_high_chunks` | 128 | 触发空闲 Warm 页回收 |
| `warm_target_chunks` | 64 | 进入回收后降到此值才停止 |
| `warm_min_idle_ms` | 2000 | 避免刚归还就产生下一次缺页 |
| `maintenance_interval_ms` | 100 | 空闲时也能推进回收 |
| `max_advice_calls_per_tick` | 32 | 第一版逐页回收，同时限制页数与系统调用次数 |
| `maintenance_budget_us` | 200 | 两次工作项之间检查的软时间预算 |
| `empty_slab_idle_ms` | 10000 | 整个 Slab 持续无借用后的解除映射等待 |
| `keep_empty_slabs` | 最多 1 | 备用地址映射，不代表预留全部物理页 |
| `max_unmap_slabs_per_tick` | 1 | 限制整块解除映射及元数据销毁 |

在不同页大小下，以页数表示的阈值对应字节数不同，启动日志和测试必须记录。时间预算不能中断一个正在内核中执行的调用，不能承诺严格的 200 μs 上界。

### 9.2 借还快路径

`release`：从输出/预留链脱离后，清游标，更新计数与空闲时间，加入 Warm 最近端。这里不 `madvise`、不 `munmap`、不申请链表节点。

Slab 借用数降到 0 时记录 `empty_since_ms`，加入整体空闲候选结构；又被借用时取消。首次创建但从未借用的空 Slab 也必须能进入这一状态。

### 9.3 Warm → Cold：物理回收

Warm 数量超过高水位后设置 `reclaim_active=true`，**保留这个状态直到 Warm 降到目标值**。不能每轮只检查是否还超过高水位，否则回收到 127 就停了。

从最老 Warm 页开始，选择空闲时间达标的页。队列按归还时间排序，最老页未到期时结束本轮页回收。单次 advice 失败也结束本轮页回收，保留节点及原时间顺序。每轮检查次数受调用预算及一次到期检查约束。

```cpp
if (::madvise(chunk->page_ptr, page_size, MADV_DONTNEED) == 0) {
    // 从 Warm 摘除，改为 FreeCold，加入 Cold，更新统计。
} else {
    // 记录 errno，逻辑上仍为 Warm；停止本轮页回收。
}
```

调用前再次确认 `FreeWarm`、地址对齐、范围完全属于无人使用的数据页。成功后不为“确认清零”读写该页，避免主动重新触页。[madvise][madvise]

第一版逐页回收，每次 advice 处理一页，因此成功丢弃页数由调用次数减失败次数得到。以后若合并连续页，再增加独立页数预算及成功页数统计。合并需要检查页的地址连续性及空闲状态，范围必须避开 InUse 页。

失败时保持保守逻辑账目。统计“成功提交丢弃的字节数”不等于精确测得的 RSS 降幅；以 OS 指标复核。

### 9.4 整个 Slab 空闲：解除映射

仅当 `borrowed_count == 0`、空闲时间达标且超出备用数量时执行。处理流程是一个不与借用交错的维护工作项：

1. 确认没有 OutputBuffer 或 reservation 引用该 Slab。
2. 执行整段 `munmap`；失败则保留映射及所有队列关系，不销毁控制块。
3. 成功后，依靠独立元数据摘除该 Slab 的全部 Warm/Cold 节点，更新统计。
4. 将映射句柄标记为已解除，销毁控制块和 Slab，避免 RAII 二次解除映射。

维护始终由事件循环单线程执行，因此成功解除到元数据摘链之间不能调用分配/发送或用户回调。这个提交阶段不得再出现可能抛异常的分配。

不要从 Slab 中间逐页 `munmap` 来替代 Cold 状态，那会打洞并增加映射管理复杂度。[munmap][mmap]

Slab 级候选维护不能每 100 ms 全量遍历所有 Chunk。采用独立空闲 Slab 队列，或保存可继续的有界扫描游标。空闲 Slab 的 256 个控制块摘除也应计入维护耗时统计。

### 9.5 事件循环接入

由 Server 在原有循环中调用：

```text
wait_timeout = min(现有等待上限,
                   max(0, pool.next_maintenance_ms() - now))

epoll_wait(wait_timeout)
处理现有网络事件和定时工作
若维护已到期：pool.maintain(now)
```

不要新增后台回收线程，也不需要专门注册 timerfd。已有 TTL/连接超时行为保留。维护执行后推进下一次截止时间；不要为了“追上错过的所有周期”在一次循环内连续执行多轮重回收。

## 10. 容量压力、所有权与安全边界

- 每连接按 `pending_bytes` 限制 4 MiB；全局同时限制池映射容量。
- 慢客户端的未发送数据属于 InUse，不能为了缩容丢弃、复用或 `madvise`。
- 达到上限且无空闲块时，分配失败并按连接级策略处理；不在事件循环里等待，不从其他活跃连接偷页。
- 关闭连接必须清空全部借用块；池不能先于 Connection 析构。
- ReplyWriter 把数据库返回的数据复制进 Chunk。不能把数据库 `string_view` 挂到跨事件循环的发送队列；后续覆盖、删除和 TTL 都可能结束原对象生命周期。
- 归还 Warm 不清零整页；安全性由 `written` 限定发送范围保证。不得发送上一连接留下的字节。
- 保留已有线程池但不让它接触 OutputBuffer 或 ChunkPool；未来多线程方案需要另立同步与生命周期设计。

## 11. 可观测性

快路径用简单计数器，不能逐页打印日志。详细测量与正式压测分开，以减少观测扰动。

| 类别 | 最低计数项 |
|---|---|
| 池容量 | Slab 数、mapped bytes、控制块数量/估算字节、InUse/Warm/Cold 数量 |
| 借还 | Warm/Cold 命中次数、扩容次数、分配失败次数及原因、借用峰值 |
| 输出 | 全局 pending bytes、每连接借页数、payload 追加复制字节、发送成功字节 |
| 回收 | advice/munmap 次数与失败数、由调用结果推导的成功丢弃页数、维护耗时、预算耗尽次数 |
| 发送 | sendmsg 调用数、短写、EAGAIN、每次 iovec 数量与提交字节 |

不把 `Cold * page_size` 称为“已节省 RSS”。OS 测量方法及低扰动采样安排在 [note.md](./note.md)。

## 12. 分阶段实施与测试

| 阶段 | 实施范围 | 通过条件 |
|---|---|---|
| P0 | 记录当前版本；单独提高发送预算，固定 B0 | 原有测试通过，基线配置和二进制可复现 |
| P1 | PageSlab RAII、稳定控制块、Pool 借还与上限 | 对齐、计数、析构、映射失败、重复归还防护测试通过 |
| P2 | OutputBuffer 跨块 append、consume、clear、预留回滚 | 边界与随机差分测试通过，无多借空块 |
| P3 | ReplyWriter 与 Connection 接入；sendmsg | 协议字节完全相同，短写/EAGAIN/半关闭/断连通过 |
| P4 | Warm/Cold、页级与整 Slab 回收、维护预算 | 部分活跃 Slab 可回收空闲页，回收后复用正确 |
| P5 | B0/O 对比与 O-no-reclaim 消融 | 数据与失败率完整，按 note.md 给出收益及代价 |

建议新增 `src/buffer/chunk_pool.hpp`、`src/buffer/output_buffer.hpp`，是否拆分 `page_slab.hpp` 由复杂度决定；保持与当前仓库组织一致。修改 `config.hpp`、`reply_writer.hpp`、`connection.hpp`、`server.hpp` 及相应测试，避免无关格式化。

### 正确性测试清单

- [ ] 追加 0、1、4095、4096、4097 B；在非 4 KiB 页环境下使用对应 `page_size±1`。
- [ ] `4093 + 5`、RESP 长度字段/CRLF 跨页、二进制 NUL、空 bulk、嵌套数组输出顺序。
- [ ] 至少 30,000 次固定随机种子的 append/consume，与连续字节串模型差分；每步检查不变量。
- [ ] 尾块只写 5 B，全部消费后归还；恰好填满不多申请下一页。
- [ ] consume 跨页、部分 head + full middle + partial tail；容量浪费界限成立。
- [ ] 模拟在第 k 次借块失败；已有输出不变、临时页全部归还、池账目一致。
- [ ] `MAX_OUTPUT_BYTES` 边界包含 framing；超大长度不溢出；失败不会继续发送残缺回复。
- [ ] 模拟短写、EINTR、EAGAIN、EPIPE；另用非阻塞 socket 做真实集成测试。
- [ ] iovec 项数/字节预算边界，64 KiB 跨 17 页，以及系统项数不足时的正确前缀。
- [ ] 部分输出时断连、半关闭输入、Server 析构；没有悬挂引用和借用泄漏。
- [ ] madvise 失败仍保守留在 Warm；munmap 失败不销毁控制块。
- [ ] 每个 Slab 保留一个活页，其他页仍能回收；活页内容不变。
- [ ] Cold 重新借用后只发送新数据，不依赖旧内容，不需要重新 mmap。
- [ ] 高低水位滞回跨轮持续，空闲不足或 advice 失败时停止本轮页回收；调用数、Slab 解除数量和时间预算都有效。
- [ ] 完全空闲 Slab 解除映射后，空闲链表和计数没有悬挂节点；持续扩缩多轮。
- [ ] 回归原有协议、TTL、Sorted Set、异步销毁及连接测试。
- [ ] ASan/UBSan 正确性测试与无 sanitizer 的 Release 性能测试分开。

本轮完成不以“QPS 必须上升”定义，而以正确性、可解释的内存回落、受控的尾延迟和可信的对照实验定义。项目结论写回 [note.md](./note.md)，不得把期望值填成实测值。

## 参考依据

源码链接固定到文档核对的提交；Codex 实现时须重新检查本地实际版本。系统接口依据 Linux man-pages 与内核文档。
[mmap]: https://man7.org/linux/man-pages/man2/mmap.2.html
[madvise]: https://man7.org/linux/man-pages/man2/madvise.2.html
[sendmsg]: https://man7.org/linux/man-pages/man2/sendmsg.2.html
[readv]: https://man7.org/linux/man-pages/man2/readv.2.html
[sysconf]: https://man7.org/linux/man-pages/man3/sysconf.3.html
[zerocopy]: https://docs.kernel.org/networking/msg_zerocopy.html
