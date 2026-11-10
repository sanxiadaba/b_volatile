# Store Buffering / volatile 实验 —— 一页纸构建入口
#
#   make            # 构建 C 实验（test、test_fs、volatile_demo）
#   make run        # 跑 C11 原子对照实验（100000 轮 × 3 次）
#   make run-fs     # 同一实验，但 a/b 共享缓存行（对照）
#   make asm        # 生成 test.s，自己看有没有 fence
#   make java       # 编译 Test.java（需要 JDK）
#   make run-java   # 跑 Java 对照实验
#   make clean
#
# Windows 上用 MinGW 的 make（mingw32-make 也可以）；Linux/macOS 用系统 make。

CC ?= gcc
# make 内置的 CC 默认是 cc，而 Windows 上通常没有 cc.exe，这里做一次兜底。
ifeq ($(origin CC),default)
CC := gcc
endif
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
PTHREAD := -pthread

ifeq ($(OS),Windows_NT)
EXE := .exe
STATIC := -static
RUN :=
JAVAC_ENC := -encoding UTF-8
RM_CMD := del /Q
else
EXE :=
STATIC :=
RUN := ./
JAVAC_ENC := -encoding UTF-8
RM_CMD := rm -f
endif

BINARIES := test$(EXE) test_fs$(EXE) volatile_demo$(EXE)

.PHONY: all run run-fs run-demo asm java run-java clean

all: test$(EXE) test_fs$(EXE) volatile_demo$(EXE)

test$(EXE): test.c
	$(CC) $(CFLAGS) $(PTHREAD) $(STATIC) -o $@ $<

# 同一条代码路径，但 a/b 落在同一条缓存行上：用来观察 false sharing 对结果的影响。
test_fs$(EXE): test.c
	$(CC) $(CFLAGS) $(PTHREAD) $(STATIC) -DFALSE_SHARING=1 -o $@ $<

volatile_demo$(EXE): volatile_demo.c
	$(CC) $(CFLAGS) $(PTHREAD) $(STATIC) -o $@ $<

test.s: test.c
	$(CC) -std=c11 -O2 $(PTHREAD) -S -masm=intel -o $@ $<

asm: test.s

run: test$(EXE)
	$(RUN)test$(EXE) --iterations 100000 --repeat 3

run-fs: test_fs$(EXE)
	$(RUN)test_fs$(EXE) --iterations 100000 --repeat 3

run-demo: volatile_demo$(EXE)
	$(RUN)volatile_demo$(EXE) --iterations 100000

java: Test.class

Test.class: Test.java
	javac $(JAVAC_ENC) Test.java

run-java: Test.class
	java Test --iterations 100000 --repeat 3

clean:
	-$(RM_CMD) $(BINARIES) *.o *.class test.s vd.s
