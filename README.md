# Store Buffering 与 volatile 语义实验

这个项目用经典的 Store Buffering litmus test 比较 C 和 Java 的并发内存语义：

```text
初始状态：a = 0, b = 0

线程 1                 线程 2
a = 1                  b = 1
x = b                  y = a
```

在严格顺序一致性（sequential consistency）下，`x = 0, y = 0` 不可能出现；在较弱的内存序或没有正确同步的程序中，它可能出现。

## 结论边界

观察到 `0/0` 只能说明两个线程都读到了旧值，不能单凭这个结果断言编译器交换了指令。相同结果还可能来自 CPU Store Buffer、缓存可见性或语言内存模型允许的行为。

判断编译器是否重排，需要检查生成的汇编；研究 CPU 与语言内存序，应使用语言定义的原子操作和对照实验。旧版 C 示例使用 `volatile` 变量产生 data race，按 C 标准属于未定义行为，因此不能作为严谨证明。

## 当前实验

| 语言 | 实验组 | 对照组 | `0/0` 预期 |
|---|---|---|---|
| C11 | `memory_order_relaxed` | `memory_order_seq_cst` | relaxed 允许但不保证出现；seq_cst 禁止 |
| Java | 普通 `a/b` 字段 | `volatile a/b` 字段 | 普通字段允许但不保证出现；volatile 禁止 |

C 版本使用 C11 原子变量消除未定义行为。Java 版本把 `volatile` 放在真正在线程间竞争的 `a/b` 上；结果变量 `x/y` 由完成屏障安全发布给主线程。

两个版本都：

- 只创建并复用两个工作线程；
- 使用 `pthread_barrier_t` 或 `CyclicBarrier` 对齐每轮实验；
- 在屏障后使用原子计数会合两个工作线程，不依赖机器相关的延时；
- 使用固定迭代次数，不再无限运行；
- 只输出四种结果的汇总统计，不逐轮打印；
- 检查线程和屏障相关错误。

屏障只放在每轮关键读写的前后，用来完成初始化和结果发布。原子会合点只确认两个工作线程均已就绪；它不会在随后两个线程的关键读写之间建立顺序。

## 构建与运行

### C11

```bash
gcc test.c -std=c11 -O2 -pthread -Wall -Wextra -Werror -o test
./test 100000
```

### Java

```bash
javac Test.java
java Test 100000
```

迭代次数参数可省略，默认是 `100000`。弱内存结果具有概率性；某次没有观察到 `0/0`，不代表该内存模型禁止它。

## 如何检查编译器输出

例如生成 C 汇编：

```bash
gcc test.c -std=c11 -O2 -pthread -S -masm=intel -o test.s
```

汇编只能说明编译器生成了什么指令；仍需结合目标 CPU 架构和语言内存模型解释并发结果。

## 历史资料

- B 站视频：[98%开发者不了解的 volatile 关键字](https://www.bilibili.com/video/BV1SY4y1d7sv/)
- `jsr133.pdf`：Java 内存模型相关资料
- `ansi-iso-9899-1990-1.pdf`：C89 标准资料
