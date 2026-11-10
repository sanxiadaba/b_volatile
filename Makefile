# Store Buffering litmus test
#
#   make            # 构建 sb
#   make run        # 跑 C 版（三种实现，各 100 万轮，约 2 秒）
#   make java       # 编译 Sb.java（需要 JDK，不是 JRE）
#   make run-java   # 跑 Java 版
#   make asm        # 生成 sb.s，自己看三个实现的机器码差别
#   make clean
#
# Windows 上用的是 MSYS2/xpack 那套 make，背后是 sh，所以 rm 能直接用。

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

.PHONY: all run java run-java asm clean

all: sb$(EXE)

sb$(EXE): sb.c
	$(CC) $(CFLAGS) $(STATIC) -o $@ $<

run: sb$(EXE)
	$(RUN)sb$(EXE)

java: Sb.class

Sb.class: Sb.java
	$(JAVAC) -encoding UTF-8 Sb.java

run-java: Sb.class
	java Sb

asm: sb.s

sb.s: sb.c
	$(CC) -std=c11 -O2 -pthread -S -masm=intel -o $@ $<

clean:
	-$(RM) sb$(EXE) sb.s *.o *.class
