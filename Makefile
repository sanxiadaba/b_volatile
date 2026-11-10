# Store Buffering litmus test —— 一页纸构建入口
#
#   make            # 构建 sb
#   make run        # 跑 C 版（三种实现，各 100 万轮）
#   make java       # 编译 Sb.java（需要 JDK，不是 JRE）
#   make run-java   # 跑 Java 版
#   make check      # 构建 + 小样本跑一遍，退出码非 0 就说明有问题
#   make asm        # 生成 sb.s，自己看 relaxed 路径有没有 xchg/lock
#   make clean
#
# Windows 上用的是 MSYS2/xpack 那套 make，它背后是 sh，所以 rm 能直接用。
# 想换编译器：make check CC=clang

CC ?= gcc
# make 内置的 CC 默认是 cc，Windows 上通常没有 cc.exe，这里兜底。
ifeq ($(origin CC),default)
CC := gcc
endif
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pthread
JAVAC ?= javac

ifeq ($(OS),Windows_NT)
EXE := .exe
# 不加 -static 的话，产物依赖 libwinpthread-1.dll，换台机器直接起不来。
STATIC := -static
RUN :=
else
EXE :=
STATIC :=
RUN := ./
endif

.PHONY: all run java run-java check asm clean

all: sb$(EXE)

sb$(EXE): sb.c
	$(CC) $(CFLAGS) $(STATIC) -o $@ $<

run: sb$(EXE)
	$(RUN)sb$(EXE) -n 1000000

java: Sb.class

Sb.class: Sb.java
	$(JAVAC) -encoding UTF-8 Sb.java

run-java: Sb.class
	java Sb -n 1000000

check: sb$(EXE)
	$(RUN)sb$(EXE) -n 200000 -r 2

asm: sb.s

sb.s: sb.c
	$(CC) -std=c11 -O2 -pthread -S -masm=intel -o $@ $<

clean:
	-$(RM) sb$(EXE) sb.s *.o *.class
