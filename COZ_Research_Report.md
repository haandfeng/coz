# COZ Profiler — C++/Linux 技术研究报告

---

## 一、项目概述

COZ 是一款**因果性能分析器（Causal Profiler）**，由 Emery Berger 等人开发。与传统热点分析不同，COZ 不寻找"哪里慢"，而是量化"优化哪段代码能带来多少整体加速"。核心机制是**虚拟加速（Virtual Speedup）**。

---

## 二、核心原理：虚拟加速机制

### 基本思想

**数学等价关系**：将某段代码加速 X%，等价于将**其他所有线程**减慢相同比例。

COZ 不实际优化代码，而是：
1. 对选定的代码行进行采样
2. 每次命中，向其他线程注入延迟（`nanosleep`）
3. 观察进度点（Progress Point）的吞吐量变化
4. 绘制"虚拟加速 vs 实际性能提升"曲线

### 关键公式

```
延迟时间 d，采样周期 P = 1ms
虚拟加速比 = d / P

例：插入 250μs 延迟 = 模拟 25% 加速
```

实验结果解读：
```
Impact = 1 - (p_speedup / p_baseline)
```
- `p_baseline`：0% 虚拟加速时进度点的访问周期
- `p_speedup`：虚拟加速时的访问周期
- 正值表示此处优化有效

---

## 三、Linux 专属采样机制：perf_event

### 3.1 系统调用接口

`libcoz/perf.cpp`

```cpp
// 系统调用封装
long perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
                     int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}
```

### 3.2 采样器配置

`libcoz/profiler.cpp`（约第 646 行）

```cpp
struct perf_event_attr pe;
pe.type        = PERF_TYPE_SOFTWARE;
pe.config      = PERF_COUNT_SW_TASK_CLOCK;   // CPU 时钟驱动采样
pe.sample_type = PERF_SAMPLE_IP              // 指令指针
               | PERF_SAMPLE_CALLCHAIN;       // 调用链
pe.sample_period  = 1000000;                 // 1ms 采样间隔
pe.wakeup_events  = 10;                      // 每 10 个样本触发一次信号
pe.exclude_idle   = 1;                       // 排除 idle
pe.exclude_kernel = 1;                       // 只采用户态
```

### 3.3 环形缓冲区（Ring Buffer）

```cpp
// 将内核 perf_event ring buffer 映射到用户态
void* ring_buffer = mmap(NULL, MmapSize, PROT_READ | PROT_WRITE,
                         MAP_SHARED, _fd, 0);

enum {
    DataPages = 2,
    PageSize  = 0x1000,            // 4KB
    DataSize  = 2 * 4096,          // 8KB 数据页
    MmapSize  = DataSize + PageSize // 12KB 总计（含元数据页）
};
```

### 3.4 异步信号投递

```cpp
// 设置 SIGPROF 发送给当前线程（而非进程）
fcntl(_fd, F_SETFL, O_ASYNC);
fcntl(_fd, F_SETSIG, SIGPROF);
fcntl(_fd, F_SETOWN, gettid());   // 线程级，而非进程级
```

**关键点**：`F_SETOWN` 传入 `gettid()` 而非 `getpid()`，实现了**线程级精确投递**。

---

## 四、延迟注入与线程协调

### 4.1 延迟计数器设计

`libcoz/profiler.h`

```cpp
// 全局延迟计数（纳秒累计）
std::atomic<size_t> _global_delay;

// 每线程本地延迟计数（thread_state 中）
std::atomic<size_t> local_delay;
```

**协调逻辑**：
- `global_delay` 只增不减，是"所有线程应插入的总延迟"
- 每线程的 `local_delay` 表示"该线程已插入了多少延迟"
- `local_delay < global_delay` → 该线程需要暂停
- `local_delay > global_delay` → 提升 global（加速线程提前）

### 4.2 延迟应用函数

`libcoz/profiler.cpp`（约第 724 行）

```cpp
void profiler::add_delays(thread_state* state) {
    size_t global = _global_delay.load();
    size_t local  = state->local_delay.load();

    if(local > global) {
        // 当前线程"超前"：提升全局延迟（该线程是被加速的目标）
        _global_delay.fetch_add(local - global);
    } else if(local < global) {
        // 当前线程"落后"：需要暂停
        size_t needed = global - local;
        state->sampler.stop();
        nanosleep(needed);              // 实际暂停
        state->local_delay.fetch_add(needed);
        state->sampler.start();
    }
}
```

**设计优势**：
- 不需要跨线程信号
- 线程自我调节，在进度点处主动检查
- 批量处理每 10ms 一次，降低开销

### 4.3 虚拟加速触发时机

每次采样命中**选中行**时：

```cpp
// 命中选中行 → 为当前线程增加本地延迟（等于"加速"这个线程）
// 相当于：其他线程需要等待，目标线程先跑
state->local_delay.fetch_add(_delay_size.load());

// 跟踪调用链（Linux 独有功能）
string chain = build_callchain_string(r);
state->_hit_callchains[chain]++;
```

---

## 五、阻塞操作处理：pre/post_block

### 问题

线程在 `mutex_lock`/`cond_wait` 期间阻塞时，若其他线程插入了延迟，被阻塞线程醒来后不应再"补偿"这些延迟。

### 解决方案

`libcoz/libcoz.cpp`（约第 382 行）

```cpp
// 在阻塞前：快照当前全局延迟
void profiler::pre_block() {
    state->is_blocked = true;
    state->pre_block_time = _global_delay.load();
}

// 阻塞后：跳过期间插入的延迟
void profiler::post_block(bool skip_delays) {
    if(skip_delays) {
        // 将阻塞期间增加的全局延迟计入本地（即"跳过"）
        size_t inserted_while_blocked = _global_delay.load() - state->pre_block_time;
        state->local_delay.fetch_add(inserted_while_blocked);
    }
    state->is_blocked = false;
}

// 解锁前确保自己的延迟已执行完（catch_up）
void profiler::catch_up() {
    process_samples(state);  // 使延迟计数器同步
}
```

### 拦截的 POSIX 接口

通过 `LD_PRELOAD` 拦截以下系统调用：

| 函数 | 处理方式 |
|------|---------|
| `pthread_create` | 子线程继承父线程 `local_delay` |
| `pthread_join` | `pre_block` → `post_block(true)` |
| `pthread_mutex_lock` | `pre_block` |
| `pthread_mutex_unlock` | `catch_up` → `post_block` |
| `pthread_cond_wait` | `pre_block` → `post_block(skip)` |
| `pthread_cond_signal` | `catch_up`（唤醒前同步延迟） |
| `pthread_barrier_wait` | 同上 |
| `sigwait` / `pthread_kill` | 信号相关协调 |

---

## 六、C++ 用户态 API

### 6.1 宏展开机制

`include/coz.h`

```cpp
// 进度点宏
#define COZ_PROGRESS \
    COZ_INCREMENT_COUNTER(COZ_COUNTER_TYPE_THROUGHPUT, __FILE__ ":" STR(__LINE__))

// 延迟检查宏（每次进度点触发）
#define _COZ_CHECK_DELAYS \
    if(_counter->backoff == 0) { \
        _call_coz_add_delays(); \
    }
```

**核心计数器增量**：

```cpp
#define COZ_INCREMENT_COUNTER(type, name) \
  if(1) { \
    static coz_counter_t* _counter = 0; \
    static unsigned char _initialized = 0; \
    if(!_initialized) { \
      _counter = _call_coz_get_counter(type, name);  // 动态 dlsym 查找 \
      _initialized = 1; \
    } \
    if(_counter) { \
      __atomic_add_fetch(&_counter->count, 1, __ATOMIC_RELAXED); \
      _COZ_CHECK_DELAYS; \
    } \
  }
```

**静态局部变量 + `dlsym`**：通过 `RTLD_DEFAULT` 懒初始化，不需要库链接时静态绑定，支持 `LD_PRELOAD` 模式。

### 6.2 进度点类型

```cpp
// 计数器结构体（共享内存友好）
typedef struct {
    size_t count;    // 原子计数
    size_t backoff;  // 批处理提示（暂未使用）
} coz_counter_t;

// 吞吐量点（Throughput）：统计某操作完成次数
class throughput_point {
    coz_counter_t _counter;
    const std::string _name;
};

// 延迟点（Latency）：利用 Little's Law 统计平均延迟
// W = L/λ，无需追踪单个事务
class latency_point {
    coz_counter_t _begin_counter;
    coz_counter_t _end_counter;
};
```

### 6.3 自定义阻塞支持

```cpp
// 用于自定义同步原语（非 pthread）
#define COZ_PRE_BLOCK  _call_coz_pre_block()
#define COZ_CATCH_UP   _call_coz_add_delays()
#define COZ_POST_BLOCK(skip_delays) _call_coz_post_block(skip_delays)

// 使用示例：
COZ_PRE_BLOCK;
my_custom_lock_acquire(&lock);
COZ_POST_BLOCK(1);  // skip=1 表示被其他线程唤醒

// 唤醒其他线程前：
COZ_CATCH_UP;
my_custom_lock_release(&lock);
```

---

## 七、DWARF 调试信息解析

### 7.1 地址到源码行的映射

`libcoz/inspect.cpp`

```cpp
// 地址区间作为 key，支持 O(log n) PC 查找
std::map<interval, std::shared_ptr<line>> _ranges;

class interval {
    uintptr_t _base;    // 起始地址
    uintptr_t _limit;   // 结束地址（不含）
    // 重载 < 使重叠区间视为相等（用于 map 查找）
};
```

### 7.2 Linux 下的内存布局发现

```cpp
// 解析 /proc/self/maps 获取可执行段映射
void get_loaded_files() {
    // 读取 /proc/self/maps
    // 筛选 r-xp 段（可读可执行）
    // 提取文件路径和加载基址
}
```

### 7.3 DWARF 版本兼容

- DWARF 2-5 通过 `libelfin` 解析
- DWARF 5 修复：`file_index` 从 0 开始（非 1）
- 支持 ELF split debug info

### 7.4 作用域过滤

```bash
# 环境变量配置
COZ_BINARY_SCOPE="MAIN\t%mylib%"  # Tab 分隔的二进制名通配符
COZ_SOURCE_SCOPE="%src/%"          # 源文件路径通配符
COZ_FILTER_SYSTEM=1                # 自动排除 /usr/include, /usr/lib
```

---

## 八、信号处理与安全保护

### 8.1 信号处理器

`libcoz/profiler.cpp`（约第 936 行）

```cpp
// Linux：每线程独立处理（通过 F_SETOWN=gettid() 定向投递）
void profiler::samples_ready(int signum, siginfo_t* info, void* p) {
    thread_state* state = get_instance().get_thread_state();
    if(!state || state->check_in_use()) return;  // 防重入

    state->set_in_use(true);
    process_samples(state);   // 读取 ring buffer，更新延迟
    state->set_in_use(false);
}
```

### 8.2 保护 COZ 信号不被用户代码屏蔽

`libcoz/libcoz.cpp`（约第 545 行）

```cpp
// 拦截 sigaction：阻止用户覆盖 SIGPROF
int sigaction(int signum, ...) {
    if(is_coz_signal(signum)) return 0;  // 静默忽略
    return real::sigaction(signum, ...);
}

// 拦截 sigprocmask：确保 SIGPROF 永不被屏蔽
int sigprocmask(int how, const sigset_t* set, ...) {
    sigset_t myset = *set;
    remove_coz_signals(&myset);           // 从屏蔽集中移除 COZ 信号
    return real::sigprocmask(how, &myset, oldset);
}
```

---

## 九、LD_PRELOAD 注入机制

### 9.1 main 函数包装

`libcoz/libcoz.cpp`（约第 307 行）

```cpp
// 覆盖 __libc_start_main，在 main 启动前初始化
extern "C" int coz_libc_start_main(main_fn_t main_fn, int argc, char** argv, ...) {
    auto real_start = (decltype(__libc_start_main)*)dlsym(RTLD_NEXT, "__libc_start_main");
    real_main = main_fn;
    // 传入 wrapped_main 替代真实 main
    return real_start(wrapped_main, argc, argv, init, fini, rtld_fini, stack_end);
}
```

### 9.2 初始化流程

```
LD_PRELOAD=libcoz.so ./target
    │
    ▼
__libc_start_main 被拦截
    │
    ▼
init_coz()
    ├─ 解析 /proc/self/maps（获取已加载二进制）
    ├─ 解析 DWARF 调试信息（构建地址→源码行映射）
    └─ profiler::startup()
           ├─ 启动 profiler 后台线程
           ├─ 设置 SIGPROF 处理器
           └─ 初始化实验参数
    │
    ▼
真实 main() 开始执行
```

### 9.3 导出符号

```cpp
extern "C" coz_counter_t* _coz_get_counter(progress_point_type t, const char* name);
extern "C" void _coz_add_delays();
extern "C" void _coz_pre_block();
extern "C" void _coz_post_block(int skip_delays);
```

用户宏通过 `dlsym(RTLD_DEFAULT, "_coz_get_counter")` 懒查找，实现零依赖可选链接。

---

## 十、实验参数配置

| 参数 | 值 | 含义 |
|------|----|------|
| `SamplePeriod` | 1,000,000 ns | 采样间隔 1ms |
| `SampleBatchSize` | 10 | 每批 10 个样本触发一次信号 |
| `SpeedupDivisions` | 20 | 虚拟加速步长 5%（0~100%）|
| `ZeroSpeedupWeight` | 7 | ~25% 概率做 0% 加速基线实验 |
| `ExperimentMinTime` | 500ms | 最短实验时长 |
| `ExperimentCoolOffTime` | 10ms | 实验间冷却时间 |
| `ExperimentTargetDelta` | 5 | 最少进度点访问次数 |

**随机加速选择**：

```cpp
size_t r = uniform_dist(0, 27);
// r in [0,7]  → delay_size = 0        （基线，约 25% 概率）
// r in [8,27] → delay_size = (r-7) * SamplePeriod / SpeedupDivisions
//               即 50k, 100k, ..., 1000k ns（5%, 10%, ..., 100%）
```

---

## 十一、输出格式

### JSON Lines（默认）

```json
{"type":"startup","time":1234567890}
{"type":"samples","location":"src/compute.cpp:156","count":1523}
{"type":"experiment","selected":"file.cpp:42","speedup":0.25,
 "duration":500000000,"selected_samples":1234,"hit_callchains":[...]}
{"type":"throughput_point","name":"process_request","delta":42}
{"type":"latency_point","name":"txn","arrivals":100,"departures":95}
{"type":"runtime","time":30000000000}
```

---

## 十二、性能开销

| 来源 | 开销 |
|------|------|
| 延迟注入机制本身 | ~10.2% |
| 采样与记账 | ~7.4% |
| **总均值** | **~17.6%** |

**实际应用效果**：
- Memcached：识别出 9% 优化潜力
- SQLite：识别出 25% 优化潜力
- PARSEC benchmarks：最高 68% 加速潜力

---

## 十三、关键文件索引

| 文件 | 核心职责 |
|------|---------|
| `include/coz.h` | 用户 API 宏、`coz_counter_t` 定义 |
| `libcoz/profiler.h` | 实验控制、常量定义、线程管理接口 |
| `libcoz/profiler.cpp` | 主循环、采样处理、延迟注入 |
| `libcoz/perf.h/.cpp` | Linux `perf_event` 封装、ring buffer |
| `libcoz/inspect.h/.cpp` | DWARF 解析、地址→源码行映射 |
| `libcoz/libcoz.cpp` | LD_PRELOAD 入口、POSIX 函数拦截 |
| `libcoz/thread_state.h` | 每线程延迟计数器 |
| `libcoz/perf_macos.h/.cpp` | macOS kperf 采样（对比参考） |

---

## 十四、技术亮点总结

1. **非侵入式 Linux perf_event 采样**
   利用 `PERF_COUNT_SW_TASK_CLOCK` + `F_SETOWN(gettid())` 实现线程级精准采样，完全在用户态读取 ring buffer，无额外系统调用开销。

2. **无跨线程信号的延迟协调**
   通过全局/本地原子计数器对比实现延迟注入，线程在进度点处自我调节，避免了高频信号的性能代价。

3. **透明 LD_PRELOAD 拦截**
   拦截 `__libc_start_main` 及所有 pthreads 同步原语，对目标程序零修改（只需标注进度点宏）。

4. **因果推断的数学严格性**
   虚拟加速在数学上等价于实际加速，能定量预测每段代码的优化潜力，解决了传统 profiler 无法回答"优化这里有多少价值"的根本问题。

5. **阻塞感知的延迟传播**
   通过 `pre_block`/`post_block` 机制，正确处理线程在同步原语上阻塞时的延迟计账，保证因果推断的准确性。
