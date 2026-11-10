# Store Buffering 与 volatile 语义实验

用经典的 Store Buffering litmus test 对照三种内存语义：C11 原子操作、C 的
`volatile`、Java 的普通字段与 `volatile` 字段。

```text
初始状态：a = 0, b = 0

线程 1                 线程 2
a = 1                  b = 1
x = b                  y = a
```

顺序一致（SC）下 `(x, y) = (0, 0)` **不可能**出现：它要求
`w(a) < r(b)`、`w(b) < r(a)`（程序顺序）同时 `r(b) < w(b)`、`r(a) < w(a)`，
四条边构成一个环。relaxed/普通字段没有这个约束，`(0, 0)` 允许出现。

## 三个程序

| 文件 | 主题 | `(0,0)` 允许吗 | 说明 |
|---|---|---|---|
| `test.c` | C11 `memory_order_relaxed` vs `memory_order_seq_cst` | relaxed 允许 / seq_cst 禁止 | 严谨对照，无数据竞争 |
| `volatile_demo.c` | C 的 `volatile` | **没有保证** | 反面教材：程序本身是 UB，只用来演示"volatile 不是同步原语" |
| `Test.java` | Java 普通字段 vs `volatile` 字段 | plain 允许 / volatile 禁止 | 结果变量由 `CyclicBarrier` 发布 |

三者都复用两个工作线程、用屏障对齐每一轮、固定迭代次数，每次运行打印一行汇总，
可以 `--repeat` 多轮；退出码在出现"本该禁止的 `(0,0)`"时非零。

## 怎么读结果（先看这一节）

**1. 观察到 `0/0` 能说明什么**

只说明"两个线程都读到了旧值"。它可能来自 CPU 的 store buffer、编译器的
重排、或者语言内存模型本来就允许。**不能**凭它断言"编译器交换了指令"——
要判断那个，得看汇编。

**2. 两组数字不能横向比较**

`seq_cst` 的 store 在 x86 上编译成 `xchg`（隐含 lock，全栅栏），它会把 store
buffer 强制排空；`relaxed` 的 store 是裸 `mov`，store 与 load 紧挨着。
**两组实验的"时序"本身就不同**，所以 `(0,0)` 出现率的差异里既有内存模型的
成分，也有指令间距的成分。

`(1,1)` 那一列反而更有意思：它要求"对端的 store 在本次 load 之前已经可见"，
只有 store 被排空才容易发生。本机实测 relaxed 是 0.19%，seq_cst 是 4.4% ——
`(1,1)` 几乎就是"这个 store 有没有被 fence 排空"的指纹。

**3. 单次运行不可信**

同一份代码，Java plain 组的 `(0,0)` 在三次 10 万轮里分别是 4.2%、6.4%、2.0%。
所以默认跑 3 轮（`--repeat`），并且**交替**两组的先后顺序：如果永远先跑
relaxed，缓存预热、CPU 频率、JIT 状态都会系统性地偏向其中一组。

**4. 屏障会让两个线程"同时起跑"**

主线程是屏障的第三个参与者，每轮两次 `barrier_wait`。这让两个工作线程几乎
同时进入关键区，也意味着整个循环的开销大头其实是屏障 —— 这是有意的：
没有它就没法逐轮统计。

**5. 缓存行布局会改变结果**

`a`、`b` 默认各占一条 64 字节缓存行。用 `-DFALSE_SHARING=1` 把它们塞进同一行，
`(1,1)` 从 0.19% 掉到 0.01%（都基于本机 20 万轮）。这是实验设计层面的对照，
不是内存模型的差异。

## 平台支持

| 平台 | 状态 | 说明 |
|---|---|---|
| Linux / glibc | ✅ | 用 `-std=c11` 即可：`test.c` 自己在源码里定义了 `_GNU_SOURCE` |
| Windows / MinGW-w64 | ✅（实测） | 建议加 `-static`，否则产物依赖 `libwinpthread-1.dll` |
| Windows / MSVC | ❌ | 没有 `pthread.h` |
| macOS | ❌ | 没有 `pthread_barrier_t`（POSIX 可选特性） |
| 任意平台 + JDK 8/17/21 | ✅ | 本机在 17.0.21 实测；CI 配置覆盖 8/17/21 三档 |

C 版本依赖 `pthread_barrier_t`，所以只能跑在有 POSIX 屏障的系统上。想在 macOS 上玩，
得把屏障换成条件变量自实现——本仓库没做。

## 目录结构

```text
test.c                    C11 原子对照实验（主实验）
volatile_demo.c           volatile 反面教材（UB，仅演示）
Test.java                 Java 普通字段 vs volatile 字段
Makefile                  一页纸构建入口
.github/workflows/ci.yml  Linux(gcc/clang) + Java(8/17/21) + Windows(MinGW) 矩阵
LICENSE                   MIT（PDF 除外）
README.md
ansi-iso-9899-1990-1.pdf  C89 标准资料（第三方版权，见 LICENSE 末尾）
jsr133.pdf                JSR-133 / Java 内存模型资料（第三方版权）
```

## 构建与运行

### 最快路径：make

```bash
make            # 构建 test / test_fs / volatile_demo
make run        # C11 对照实验，10 万轮 × 3 次
make run-fs     # 同一实验，但 a/b 共享缓存行（对照）
make run-demo   # volatile 反面教材
make java       # 编译 Test.java
make run-java   # 跑 Java 对照实验
make asm        # 生成 test.s
make clean
```

Windows 上用 MinGW 的 make（`mingw32-make` 也行）。

### 手写命令

**C（Linux / glibc）**

```bash
gcc test.c -std=c11 -O2 -pthread -Wall -Wextra -Werror -o test
./test --iterations 100000 --repeat 3
```

**C（Windows / MinGW-w64）**

```bash
gcc test.c -std=c11 -O2 -pthread -Wall -Wextra -Werror -static -o test.exe
test.exe --iterations 100000 --repeat 3
```

`-static` 不是可选项：不加的话 `objdump -p test.exe` 会看到
`DLL Name: libwinpthread-1.dll`，换台机器直接起不来。

**Java（需要 JDK，不是 JRE）**

```bash
javac -encoding UTF-8 Test.java
java Test --iterations 100000 --repeat 3
```

`-encoding UTF-8` 也是必需项：源码里有中文注释，而 Windows 中文版
`javac` 默认按 GBK 读文件，会报"编码 GBK 的不可映射字符"。

### 命令行参数

`test` 与 `java Test` 参数一致：

| 参数 | 含义 | 默认 |
|---|---|---|
| `-n`, `--iterations N` | 每次运行的关键读写轮数 | 100000 |
| `-r`, `--repeat R` | 每种模式独立运行几次 | 3 |
| `-m`, `--mode M` | `all` / `relaxed` / `seq_cst`（Java：`all`/`plain`/`volatile`） | all |
| `--csv` | stdout 输出 CSV，环境信息改走 stderr | 关 |
| `-h`, `--help` | 帮助 | |
| 裸数字 | 等价于 `--iterations`，兼容旧用法 `./test 100000` | |

退出码：`0` 正常，`1` 参数/系统错误，`2` 出现了本该禁止的 `(0,0)`。

### 实测输出

C（i5-10400F，gcc 4.9.4 32 位 MinGW-w64，10 万轮 × 3 次）：

```text
# windows | gcc 4.9.4 | 12 logical CPUs | 32-bit pointers | C11
# cpu: Intel(R) Core(TM) i5-10400F CPU @ 2.90GHz
# layout: a/b do not share one cache line | pinning off
# iterations per run: 100000 | runs per mode: 3
relaxed  run 1/3  n=100000  (0,0)=10418 ( 10.4%)  (0,1)=43415  (1,0)=45927  (1,1)=240
seq_cst  run 1/3  n=100000  (0,0)=0 (  0.0%)  (0,1)=46768  (1,0)=49342  (1,1)=3890
seq_cst  run 2/3  n=100000  (0,0)=0 (  0.0%)  (0,1)=45801  (1,0)=49477  (1,1)=4722
relaxed  run 2/3  n=100000  (0,0)=11156 ( 11.2%)  (0,1)=43926  (1,0)=44699  (1,1)=219
relaxed  run 3/3  n=100000  (0,0)=12667 ( 12.7%)  (0,1)=41708  (1,0)=45508  (1,1)=117
seq_cst  run 3/3  n=100000  (0,0)=0 (  0.0%)  (0,1)=44612  (1,0)=50714  (1,1)=4674
relaxed  total  n=300000  (0,0)=34241 ( 11.4%)  (0,1)=129049  (1,0)=136134  (1,1)=576
seq_cst  total  n=300000  (0,0)=0 (  0.0%)  (0,1)=137181  (1,0)=149533  (1,1)=13286
OK: seq_cst never produced (0,0) in these runs.
```

Java（Temurin 17.0.21，10 万轮 × 3 次，耗时约 8.7 s）：

```text
# Windows 10 amd64 | OpenJDK 64-Bit Server VM 17.0.21 | 12 logical CPUs
# iterations per run: 100,000 | runs per mode: 3
plain    run 1/3  n=100000  (0,0)=4,202 (  4.2%)  (0,1)=47,154  (1,0)=48,639  (1,1)=5
volatile run 1/3  n=100000  (0,0)=0 (  0.0%)  (0,1)=49,332  (1,0)=49,418  (1,1)=1,250
...
plain    total  n=300,000  (0,0)=12,612 (  4.2%)  (0,1)=144,674  (1,0)=142,709  (1,1)=5
volatile total  n=300,000  (0,0)=0 (  0.0%)  (0,1)=148,223  (1,0)=147,881  (1,1)=3,896
OK: plain fields produced (0,0), volatile fields never did.
```

`volatile_demo`（10 万轮 × 2 次）：`(0,0)` 合计 43497 次，`(1,1)` 只有 4 次。
这正是"volatile 不是同步原语"的直观证据——但它同时是一个 UB 程序。

### CSV

```bash
./test --iterations 5000 --repeat 2 --csv 2>/dev/null
```

```text
mode,run,iterations,zero_zero,zero_one,one_zero,one_one
relaxed,1,5000,300,2336,2360,4
seq_cst,1,5000,0,2487,2352,161
seq_cst,2,5000,0,2530,2245,225
relaxed,2,5000,451,2245,2298,6
```

（`java Test` 的 CSV 列完全相同。）

## 看汇编：到底有没有重排

```bash
gcc test.c -std=c11 -O2 -pthread -S -masm=intel -o test.s
```

关键读写只有两条指令，区别一目了然：

```asm
; relaxed —— 程序顺序，store 在前，没有任何 fence
mov	DWORD PTR [esi+64], 1      ; b = 1
mov	eax, DWORD PTR [esi]       ; 读 a
mov	DWORD PTR [esi+192], eax   ; y = 读到的东西

; seq_cst —— store 变成全栅栏
mov	eax, 1
xchg	eax, DWORD PTR [esi+64]    ; b = 1（隐含 lock，排空 store buffer）
mov	eax, DWORD PTR [esi]       ; 读 a
mov	DWORD PTR [esi+192], eax
```

`volatile_demo.c` 里同样两条访存，加上 `volatile` 之后**依然没有** fence：

```asm
mov	DWORD PTR [edi+4], 1   ; b = 1（volatile：保证不被优化掉）
mov	eax, DWORD PTR [edi]   ; 读 a（volatile：保证真的从内存读）
```

访问没被删掉，这就已经够了 `volatile` 的全部作用；它不提供原子性，也不建立
happens-before。

**踩过的坑**：内存序写成函数参数（`atomic_store_explicit(p, 1, order)`，
`order` 是运行时变量）时，gcc 4.9 `-O2` 无法常量折叠，会保守地生成 `xchg`——
relaxed 组因此表现得和 seq_cst 一模一样（`(0,0)` 恒为 0）。所以
`test.c` 里两个 `critical_section_*` 函数的函数体几乎相同，是**故意**保留的。
改完之后如果 `test.s` 里 relaxed 路径出现 `xchg`/`lock`，说明又退化了。

## 降噪与对照实验

| 做法 | 命令 | 作用 |
|---|---|---|
| 多轮 + 交替顺序 | `-r 5`（默认已交替） | 抵消预热/频率漂移 |
| 缓存行对照 | `-DFALSE_SHARING=1` | `a`/`b` 共享缓存行，看 false sharing 影响 |
| 线程绑核 | `-DPIN_THREADS=1` | 两个工作线程绑到不同 CPU，降低调度抖动 |
| 降低自旋开销 | 内置，自旋 1000 次后 `sched_yield()` | 单核/高负载下不烧 CPU |
| 固定 CPU 频率 | 系统设置 | 跨轮次对比时最有效 |

## 常见问题

**Q：Linux 上 `pthread_barrier_t` 未定义？**
glibc 把屏障声明放在 `#ifdef __USE_XOPEN2K` 之后，而 `-std=c11` 会定义
`__STRICT_ANSI__`、让 glibc 不再默认打开 `_POSIX_C_SOURCE`。`test.c` 顶部已经
`#define _GNU_SOURCE 1` 解决；如果你把代码搬走，记得一起搬。

**Q：Windows 上双击 exe 提示缺 `libwinpthread-1.dll`？**
加 `-static` 重新链接。

**Q：`javac` 报"编码 GBK 的不可映射字符"？**
加 `-encoding UTF-8`。

**Q：为什么 `seq_cst` 的 `(1,1)` 那么高？**
因为 `xchg` 会等 store buffer 排空，对端的 store 来得及在本次 load 之前可见。
它恰恰说明"两组时序不同"。

**Q：为什么某次 `(0,0)` 是 0？**
概率性结果 + 线程时序高度依赖机器负载。多跑几轮、看 `total` 行，别只看单轮。

**Q：macOS 上能跑吗？**
C 版本不能（没有 `pthread_barrier_t`）；Java 版本可以。

## 实测数据汇总（本机）

环境：Intel i5-10400F（12 逻辑核）、Windows 10、gcc 4.9.4（32 位 MinGW-w64）、
Temurin 17.0.21。

| 实验 | 轮数 | `(0,0)` | `(1,1)` |
|---|---|---|---|
| C relaxed（缓存行隔离） | 30 万 | 11.4% | 0.19% |
| C seq_cst（缓存行隔离） | 30 万 | 0 | 4.43% |
| C relaxed（`-DFALSE_SHARING=1`） | 20 万 | 13.7% | 0.01% |
| C `volatile`（UB） | 20 万 | 21.7% | 0.002% |
| Java plain | 30 万 | 4.2% | 0.002% |
| Java volatile | 30 万 | 0 | 1.3% |

数字只对本机有效，换 CPU/编译器/优化级别都会变——这也正是本仓库要反复强调的。

## 与旧版的差异

| 项 | 旧版 | 现在 |
|---|---|---|
| `test.c` | 四个 `volatile int`，每轮 `pthread_create` 新建线程，用空循环凑时序 | C11 原子操作，relaxed / seq_cst 对照 |
| `volatile` 演示 | 就是 `test.c` 本身 | 独立成 `volatile_demo.c`，并在文件头标明是 UB |
| 结果变量 `x`/`y` | 普通 `int`（靠屏障发布，形式上是 data race） | `_Atomic int` |
| 线程 | 每轮新建两个线程 | 两个线程复用，屏障对齐 |
| 循环 | 无限循环到出现 `0/0` 为止 | 固定轮数，默认 10 万轮 × 3 次 |
| 输出 | 逐轮打印 | 每轮一行 + 汇总，可 `--csv` |

副作用：旧的 `./test 100000` 现在会跑 3 遍（`--repeat 1` 可恢复旧行为）。

## 历史资料与许可

- B 站视频：[98%开发者不了解的 volatile 关键字](https://www.bilibili.com/video/BV1SY4y1d7sv/) ——
  视频里的代码对应 `volatile_demo.c`（旧版直接叫 `test.c`，现在拆开了）。
- `jsr133.pdf`：JSR-133 / Java 内存模型资料。
- `ansi-iso-9899-1990-1.pdf`：C89 标准资料。

代码与文档以 MIT 许可发布，见 [LICENSE](LICENSE)。仓库里的两个 PDF 是第三方
出版的标准文档，版权归各自权利人所有，**不在** MIT 许可范围内。
