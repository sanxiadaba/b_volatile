# volatile 实验
#
#   make            # 编译 C 版
#   make run        # 跑 C 版（三种写法，各 100 万轮，约 2 秒）
#   make java       # 编译 Java 版（需要 JDK，不是 JRE）
#   make run-java   # 跑 Java 版
#   make asm        # 生成 volatile.s，看编译出来长什么样
#   make clean

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

all: volatile$(EXE)

volatile$(EXE): volatile.c
	$(CC) $(CFLAGS) $(STATIC) -o $@ $<

run: volatile$(EXE)
	$(RUN)volatile$(EXE)

java: Volatile.class

Volatile.class: Volatile.java
	$(JAVAC) -encoding UTF-8 Volatile.java

run-java: Volatile.class
	java Volatile

asm: volatile.s

volatile.s: volatile.c
	$(CC) -std=c11 -O2 -pthread -S -masm=intel -o $@ $<

clean:
	-$(RM) volatile$(EXE) volatile.s *.o *.class
