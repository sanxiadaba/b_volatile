# volatile 到底保证了什么

用经典的 Store Buffering litmus test，对照 C11 的 `relaxed`、`seq_cst`、C 的
`volatile`，以及 Java 的普通字段和 `volatile` 字段。

初始 `a = 0, b = 0`：

```text
线程 1          线程 2
a = 1           b = 1
x = b           y = a
```

顺序一致（SC）下 `(x, y) = (0, 0)` 不可能出现。它要求 `w(a) < r(b)`、
`w(b) < r(a)`（程序顺序），同时 `r(b) < w(b)`、`r(a) < w(a)`（`(0,0)` 的含义），
四条边闭合成环，与 SC 的全序冲突。`relaxed` 和普通字段没有这个约束，所以允许。

## 三段实现

`sb.c` 里三个函数上下相邻，每个只有两条分支、各两条语句，差别一眼可见：

| 函数 | 两个访存 | `(0,0)` |
|---|---|---|
| `run_relaxed` | `memory_order_relaxed` | 允许 |
| `run_seq_cst` | `memory_order_seq_cst` | 禁止 |
| `run_volatile` | `volatile int` 普通读写 | 会出现，但整段程序是 data race（UB） |

`Sb.java` 是同一个实验的 Java 版。注意 C 的 `volatile` 和 Java 的 `volatile`
是两回事：C 的那个只保证"别把访存优化掉"，Java 的才真的带内存语义（禁止重排、
建立 happens-before）。这是全仓库最值得记住的一条。

那 `volatile` 该用在哪：MMIO 寄存器、信号处理里的 flag、`setjmp` 之后的局部变量
—— 都是"别被优化掉"，不是"线程间同步"。

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
# 注: volatile 模式整段程序是 data race（UB），它的数字只作演示，没有任何保证
relaxed  (0,0)=7778       0.8%   (0,1)=455026   (1,0)=460662   (1,1)=76534
seq_cst  (0,0)=0          0.0%   (0,1)=406423   (1,0)=451313   (1,1)=142264
volatile (0,0)=8242       0.8%   (0,1)=424193   (1,0)=470442   (1,1)=97123

seq_cst 从未出现 (0,0)，符合顺序一致。
relaxed / volatile 出现了 (0,0)：两者都不拦 CPU 的 store buffer。
```

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

- **别拿两种实现的百分比横比。** 数字随机器、编译器、负载大幅变化，`(0,0)` 在
  不同机器上从 0.8% 到 11% 都见过。能确定的只有"`seq_cst` 一次都没出现"这一条。
  Java 的 `plain` 比例甚至每次运行都不同，取决于 JIT 当次怎么编译。
- **内存序必须是编译期常量。** 把它做成函数参数，编译器无法常量折叠，会保守地
  生成 `xchg`，`relaxed` 就退化成 `seq_cst`。`make asm` 里 relaxed 路径一旦出现
  `xchg`，就是退化了。

## 资料

- B 站视频：[98%开发者不了解的 volatile 关键字](https://www.bilibili.com/video/BV1SY4y1d7sv/)
- `jsr133.pdf`：JSR-133 / Java 内存模型
- `ansi-iso-9899-1990-1.pdf`：C89 标准
- 代码与文档 MIT，见 [LICENSE](LICENSE)；两个 PDF 是第三方出版的标准文档，不在 MIT 范围内
