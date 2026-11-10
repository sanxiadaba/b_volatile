# volatile 到底保证了什么

两个线程，各自把一个变量置 1，再去读对方那个变量，读到的值存进 x 和 y：

```text
初始   a = 0   b = 0

线程 1          线程 2
a = 1           b = 1
x = b           y = a
```

问题只有一句：**(x, y) 会不会两个都是 0？**

把 `(0,0)` 展开成四条"先后"关系，就看出它为什么自相矛盾：

```text
a = 1  先于  x = b    线程 1 的程序顺序
x = b  先于  b = 1    x 读到 0，说明读的时候 b = 1 还没生效
b = 1  先于  y = a    线程 2 的程序顺序
y = a  先于  a = 1    y 读到 0，同理
```

推下去得到"`a = 1` 先于 `a = 1`"。顺序一致（SC）要求所有线程看到同一个全局顺序，
不允许这种环，所以 `(0,0)` 不可能。

弱内存模型不要求全局同一个顺序，这四条可以同时成立 —— 于是 `(0,0)` 会出现，而且
真能测出来。这个实验叫 Store Buffering。

## 一轮跑的是什么

1. 主线程把 `a`、`b` 清零
2. 主线程和两个 worker 在闸门 1 对齐 —— 保证清零对两个线程可见
3. 线程 1 做 `a = 1; x = b;`，线程 2 做 `b = 1; y = a;`
4. 三方在闸门 2 对齐 —— 保证两个线程都做完了
5. 主线程看 `x`、`y` 是 0 还是 1，记一笔

少了任何一次闸门，读到的都是上一轮的残留。

跑 100 万轮，四种结果各自记账：

| 结果 | 含义 |
|---|---|
| `(0,0)` | 两个线程都在对方写生效之前读完了 —— **只有弱内存模型允许** |
| `(0,1)` | 线程 1 读到旧值，线程 2 读到新值 |
| `(1,0)` | 线程 1 读到新值，线程 2 读到旧值 |
| `(1,1)` | 两个都读到新值 |

## 三段实现

`sb.c` 里三个函数上下相邻，每个两条分支、各两条语句，差别就在那两个内存序上：

| 函数 | 两个访存 | `(0,0)` |
|---|---|---|
| `run_relaxed` | `memory_order_relaxed` | 允许 |
| `run_seq_cst` | `memory_order_seq_cst` | 禁止 |
| `run_volatile` | `volatile int` 普通读写 | 会出现，但整段程序是 data race（UB） |

`Sb.java` 是同一个实验的 Java 版。C 的 `volatile` 和 Java 的 `volatile` 是两回事：
C 的那个只保证"别把访存优化掉"，Java 的才真的带内存语义。这是全仓库最值得记住的
一条。

那 `volatile` 该用在哪：MMIO 寄存器、信号处理里的 flag、`setjmp` 之后的局部变量 ——
都是"别被优化掉"，不是"线程间同步"。

## 跑

```bash
make run          # C 版，三种实现各 100 万轮（约 2 秒）
make run-java     # Java 版，需要 JDK
make check        # 小样本跑一遍，退出码非 0 就说明有问题
make asm          # 生成 sb.s
```

单跑一种、改轮数、换编译器：

```bash
./sb -m seq_cst -n 100000
java Sb -m plain -n 100000
make check CC=clang
```

## 输出

```text
# windows | gcc 4.9.4 | 8 cpus | 32-bit | 1000000 iterations
# (x,y): x = 线程1读到的 b，y = 线程2读到的 a
# 注: volatile 模式整段程序是 data race（UB），它的数字只作演示，没有任何保证
relaxed  (0,0)=9403       0.9%   (0,1)=414530   (1,0)=471991   (1,1)=104076
seq_cst  (0,0)=0          0.0%   (0,1)=447639   (1,0)=418756   (1,1)=133605
volatile (0,0)=9030       0.9%   (0,1)=439073   (1,0)=461843   (1,1)=90054

seq_cst 从未出现 (0,0)，符合顺序一致。
relaxed / volatile 出现了 (0,0)：两者都不拦 CPU 的 store buffer。
```

看 `relaxed` 那一行：100 万轮里有 9403 轮，两个线程都读到了旧值。`seq_cst` 一轮都没有。

## 看汇编

`make asm`，`gcc -O2 -masm=intel` 的结果：

```asm
; relaxed —— 一条 mov 存、一条 mov 读，没有任何 fence
mov  DWORD PTR 64[rdi], 1      ; b = 1
mov  eax, DWORD PTR [rdi]      ; 读 a

; seq_cst —— store 变成 xchg，隐含 lock，全栅栏
mov  eax, 1
xchg eax, DWORD PTR 64[rdi]    ; b = 1
mov  eax, DWORD PTR [rdi]      ; 读 a

; volatile —— 和 relaxed 逐字相同
mov  DWORD PTR 64[rdi], 1      ; b = 1
mov  eax, DWORD PTR [rdi]      ; 读 a
```

`volatile` 那版生成的机器码和 `relaxed` 完全相同：访问没被优化掉，这就已经是它的
全部作用；它不提供原子性，也不建立 happens-before。

## 两个坑

- **别拿两种实现的百分比横比。** 数字随机器、编译器、负载大幅变化，`(0,0)` 在不同
  机器上从 0.8% 到 11% 都见过。能确定的只有"`seq_cst` 一次都没出现"这一条。
  Java 的 `plain` 比例甚至每次运行都不同，取决于 JIT 当次怎么编译。
- **内存序必须是编译期常量。** 把它做成函数参数，编译器无法常量折叠，会保守地生成
  `xchg`，`relaxed` 就退化成 `seq_cst`。`make asm` 里 relaxed 路径一旦出现 `xchg`，
  就是退化了。

## 资料

- B 站视频：[98%开发者不了解的 volatile 关键字](https://www.bilibili.com/video/BV1SY4y1d7sv/)
- `jsr133.pdf`：JSR-133 / Java 内存模型
- `ansi-iso-9899-1990-1.pdf`：C89 标准
- 代码与文档 MIT，见 [LICENSE](LICENSE)；两个 PDF 是第三方出版的标准文档，不在 MIT 范围内
