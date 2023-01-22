# volatile细节知多少
- B站视频：

- 参考资料：

1. jsr133文档
2. ansi-iso-9899-1990 （C89标准）

- 参考链接：

1. [指令重排序、内存屏障很难？看完这篇你就懂了！ - 掘金 (juejin.cn)](https://juejin.cn/post/6901283327160877063)
2. [谈谈 C/C++ 中的 volatile - 知乎 (zhihu.com)](https://zhuanlan.zhihu.com/p/33074506)

- 代码环境：

1. C语言：gcc (x86_64-win32-seh-rev0, Built by MinGW-W64 project) 8.1.0
    Copyright (C) 2018 Free Software Foundation, Inc.
    This is free software; see the source for copying conditions.  There is NO
    warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
2. Java： openjdk version "1.8.0_333"
    OpenJDK Runtime Environment (build 1.8.0_333-b02)
    OpenJDK 64-Bit Server VM (build 25.333-b02, mixed mode)

- 其它注意说明：C语言线程一代码里的空循环是为了“等待”线程二，在测试时，有时我几百次就能观察到指令重排，有时候几十万次才能观察到，很大一部分原因是因为线程一先启动，线程二后启动，所以要调节这里空循环的次数，让两个线程尽量做到同时启动，还有，运行代码时最好关闭优化，因为编译器可能会将这行空循环代码给优化掉，最后，运行C语言文件时候别忘了动态链接 -lpthread

截图说明：

1. JSR文档对volatile语义增强

![JSR文档对volatile语义增强](https://sanxiadaba-pic.oss-cn-hangzhou.aliyuncs.com/img/msedge_oSK1GJ5lTJ.png)

 2. C语言文档对volatile解释

    ![](https://sanxiadaba-pic.oss-cn-hangzhou.aliyuncs.com/img/image-20230122154923326.png)

3. C语言多线程观察到的指令重排

    ![consolepauser_1ARdn0W5p9](https://sanxiadaba-pic.oss-cn-hangzhou.aliyuncs.com/img/consolepauser_1ARdn0W5p9.png)

4. Java多线程观察到的指令重排

    ![idea64_5HV1IWx7tw](https://sanxiadaba-pic.oss-cn-hangzhou.aliyuncs.com/img/idea64_5HV1IWx7tw.png)
