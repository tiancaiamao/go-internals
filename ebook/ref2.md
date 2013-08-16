# 附录B Go的源代码目录结构
下载Go源码后，根目录结构如下：

	|– AUTHORS — 文件，官方 Go语言作者列表
	|– CONTRIBUTORS — 文件，第三方贡献者列表
	|– LICENSE — 文件，Go语言发布授权协议
	|– PATENTS — 文件，专利
	|– README — 文件，README文件，大家懂的。提一下，经常有人说：Go官网打不开啊，怎么办？其实，在README中说到了这个。该文件还提到，如果通过二进制安装，需要设置GOROOT环境变量；如果你将Go放在了/usr/local/go中，则可以不设置该环境变量（Windows下是C:\go）。当然，建议不管什么时候都设置GOROOT。另外，确保$GOROOT/bin在PATH目录中。
	|– VERSION — 文件，当前Go版本
	|– api — 目录，包含所有API列表，方便IDE使用
	|– doc — 目录，Go语言的各种文档，官网上有的，这里基本会有，这也就是为什么说可以本地搭建”官网”。这里面有不少其他资源，比如gopher图标之类的。
	|– favicon.ico — 文件，官网logo
	|– include — 目录，Go 基本工具依赖的库的头文件
	|– lib — 目录，文档模板
	|– misc — 目录，其他的一些工具，相当于大杂烩，大部分是各种编辑器的Go语言支持，还有cgo的例子等
	|– robots.txt — 文件，搜索引擎robots文件
	|– src — 目录，Go语言源码：基本工具（编译器等）、标准库
	`– test — 目录，包含很多测试程序（并非_test.go方式的单元测试，而是包含main包的测试），包括一些fixbug测试。可以通过这个学到一些特性的使用。

下面详细介绍一些目录（可能分功能介绍）

## 一、api目录
|– README
|– go1.txt
`– next.txt

通过阅读README知道，go1.txt可以通过go tool api命令生成。而通过go1.txt可以做成编辑器的api自动提示，比如Vim：VimForGo
next.txt是一些将来可能加入的API

## 二、Go基本工具（cmd）

### 1、include目录
该目录包含以下文件（文件夹）

ar.h bio.h bootexec.h fmt.h libc.h mach.h plan9 u.h ureg_amd64.h ureg_arm.h ureg_x86.h utf.h

其中，plan9目录是针对Plan 9操作系统的，从里面的文件名知道，跟include跟目录下的是一个意思。

386 libc.h mach.h ureg_amd64.h ureg_arm.h ureg_x86.h （386目录下就只有一个u.h头文件）

1）u.h
根据Rob Pike在How to Use the Plan 9 C Compiler上的介绍以及文件的源码，知道u.h文件定义了一些依赖架构（architecture-dependent）的类型（这样使得该类型独立于架构，不过依赖于编译器），如用于setjmp系统调用的jmp_buf，以及类型int8、uint8等。

该文件直接来源于plan9。地址：http://code.swtch.com/plan9port/src/tip/include/u.h。所有plan9 C程序必须在开始出包含该头文件（因为其他文件引用了该文件中的类型定义）

2）ureg.h
包括：ureg_amd64.h ureg_arm.h ureg_x86.h

三种架构的定义。该文件来源于Inferno操作系统，相应的源码分别是：http://code.google.com/p/inferno-os/source/browse/utils/libmach/ureg5/6/8.h。

该文件定义了一个类型（struct）：Ureg。定义了在系统栈上寄存器的布局

在ureg_x86.h中对各个字段有注释：

```c
	struct Ureg
	{
	    uint32 di; /* general registers */
	    uint32 si; /* ... */
	    uint32 bp; /* ... */
	    uint32 nsp;
	    uint32 bx; /* ... */
	    uint32 dx; /* ... */
	    uint32 cx; /* ... */
	    uint32 ax; /* ... */
	    uint32 gs; /* data segments */
	    uint32 fs; /* ... */
	    uint32 es; /* ... */
	    uint32 ds; /* ... */
	    uint32 trap; /* trap type */
	    uint32 ecode; /* error code (or zero) */
	    uint32 pc; /* pc */
	    uint32 cs; /* old context */
	    uint32 flags; /* old flags */
	    union {
		uint32 usp;
		uint32 sp;
	    };
	    uint32 ss; /* old stack segment */
	};
```

3）libc.h/utf.h/fmt.h
在严格的标准C中，头文件按相关功能分组在一个单独文件中：一个头文件用于字符串处理，一个头文件用于内存管理，一个头文件用于I/O处理，没有头文件是用于系统调用的。plan9采用了不同的方式，一个C库由strings函数、内存操作函数、一些格式化IO程序，加上所有和这些相关的系统调用。为了使用这些功能，需要包含libc.h头文件。该文件从Inferno和Plan9中提取出来的。

Inferno：http://code.google.com/p/inferno-os/source/browse/include/kern.h
Plan 9：http://code.swtch.com/plan9port/src/tip/include/libc.h

该文件开头有几行注释：

	/*
	* Lib9 is miscellany from the Plan 9 C library that doesn’t
	* fit into libutf or into libfmt, but is still missing from traditional
	* Unix C libraries.
	*/

在该文件中包含了utf.h和fmt.h

在Plan 9中使用nil表示指针的零值，这也就是为什么Go中采用nil了。nil的定义在libc.h中：

	#ifndef nil
	#define nil ((void*)0)
	#endif

对于solaris，nil在u.h中有定义

另外，libc中声明了很多系统调用

包含了该文件之后，可以直接使用print、fprint之类的，而不需要包含标准IO库，这是因为libc.h中包含了fmt.h，而fmt.h中提供了这些print函数。当然，如果需要使用printf，得导入stdio.h

utf.h是实际上引用了src/lib9/utf/utf.h，提供了对UNICODE字符集相关操作。

题外话：

Plan 9支持的每一种CPU架构都给其一个单个字母或数字的名称：k表示SPARC，q表示Motorola Power PC 630和640，v表示MIPS，0表示little-endian MIPS，1表示Motorola 68000，2表示Motorola 68020和68040，5表示Acorn ARM 7500，6表示AMD64,7表示DEC Alpha，8表示Intel 386，9表示AMD 2900。可以看出，Go中5/6/8的由来了。
对于为什么取这样的名字，How to Use the Plan 9 C Compiler 中Heterogeneity有解释。

注意：在看源码过程中可能会看到

	ARGBEGIN{
	}ARGEND

这是在libc.h中定义的宏。这是一些处理命令行参数的宏。其他宏还有：

	ARGF()
	EARGF(x)
	ARGC()
	
4）bio.h
上面提到，libc.h中包含了print等，这些IO是没有buffer的。而bio.h提供了buffer I/O，这是推荐使用的方式。这个和ANSI标准I/O，stdio.h类似。

根据官方说法，Bio更小、更高效，特别是buffer-at-a-time或line-at-a-time I/O，即使character-at-a-time I/O也比stdio更快。

和其他系统明显不同的是，Plan 9中I/O的接口的文本不是ASCII编码，而是UTF（ISO叫做UTF-8）编码。一个字符在Plan 9中称为rune，也叫做Code-point。（Go中沿用了该叫法）

看一下utf.h中的一个枚举类型

	{
	    UTFmax = 4, /* maximum bytes per rune */
	    Runesync = 0x80, /* cannot represent part of a UTF sequence ( Runeself = 0x80, /* rune and UTF sequences are the same ( Runeerror = 0xFFFD, /* decoding error in UTF */
	    Runemax = 0x10FFFF, /* maximum rune value */
	};

引用一段解释：

	The library defines several symbols relevant to the representation of characters. Any byte with unsigned value less than Runesync will not appear in any multi-byte encoding of a character. Utfrune compares the character being searched against Runesync to see if it is sufficient to call strchr or if the byte stream must be interpreted. Any byte with unsigned value less than Runeself is represented by a single byte with the same value. Finally, when errors are encountered converting to runes from a byte stream, the library returns the rune value Runeerror and advances a single byte. This permits programs to find runes embedded in binary data.

关于UTF8的操作在utf.h文件中声明了

该文件来源于Inferno操作系统
http://code.google.com/p/inferno-os/source/browse/include/bio.h

5）ar.h
该文件来源于Inferno操作系统
http://code.google.com/p/inferno-os/source/browse/utils/include/ar.h

iar是一个压缩命令，该压缩的文件格式通过ar.h头文件描述。

关于该文件的具体说明，可以查看： http://www.vitanuova.com/inferno/man/10/ar.html

6）bootexec.h/mach.h
bootexec.h 文件来源于Inferno操作系统
http://code.google.com/p/inferno-os/source/browse/utils/libmach/bootexec.h
但注释掉了一些东西

该文件定义了一些架构私有的引导执行程序的文件头格式（引导程序）。这是Plan 9（Inferno的祖先）操作系统的说明：The header format of a bootable executable is defined by each manufacturer. Header file /sys/include/bootexec.h contains structures describing the headers currently supported.

mach.h文件来源于Inferno操作系统：
http://code.google.com/p/inferno-os/source/browse/utils/libmach/a.out.h
http://code.google.com/p/inferno-os/source/browse/utils/libmach/mach.h

该文件定义了一些特定架构的应用程序数据。目前支持的架构：

	/*
	 * Supported architectures:
	 * mips,
	 * 68020,
	 * i386,
	 * amd64,
	 * sparc,
	 * sparc64,
	 * mips2 (R4000)
	 * arm
	 * powerpc,
	 * powerpc64
	 * alpha
	 */

该文件中列出了详细的支持的架构类型（可执行文件）

bootexec.h是针对引导程序的；mach.h是针对应用程序的。

### 2、src下的lib9/libbio/libmach
由include目录中文件的名字知道，这三个目录分别是libc.h、bio.h和mach.h三个头文件的实现。具体代码有兴趣可以看看。

这些都是Plan 9或Inferno操作系统的库

### 3、src/cmd 包含了各种工具的源码
目录如下：

5a 5c 5g 5l 6a 6c 6g 6l 8a 8c 8g 8l addr2line api cc cgo dist fix gc go godoc gofmt ld nm objdump pack vet yacc

一个目录对应一个工具
除了go/godoc/gofmt/dist，其他工具的使用方式：
go tool 工具名 xxx

注：这些工具基本来自Plan 9上已有的工具。工具帮助文档：http://plan9.bell-labs.com/sys/man/1/INDEX.html

经过前面的介绍，看到这些名字，应该大概猜到是啥了。

我们看一下Plan 9中文件后缀的问题

前面我们知道，AMD64上，标示是6，我们以这个为例。

根据Plan 9命名规则，AMD64上的C编译器是6c，汇编器是6a，链接器或装载器是6l。c文件编译后生成的对象文件后缀是.6，链接后默认的可执行文件名是6.out。

5/6/8这一序列中，跟Plan 9是一致的，另外，新增了一个g，表示Go编译器。通过这些工具编译Go文件生成的中间文件对象的后缀和C文件编译后是一样的，以.5/6/8结尾。

说到这里提醒一下，目前Go编译不建议直接通过5g/6g/8g这样的进行，而是使用go这个工具（网上很多Go1正式版发布之前的文章用的是6g这样的工具）

5/6/8这一序列中，每个目录下的都有一个doc.go文件，这个文件大概说明了该工具的作用。这一序列工具具体的源码，感兴趣的可以阅读。

1）cc/gc/ld分别是C编译器、Go编译器和链接器
这三个可以看成是对5/6/8序列的抽象（不依赖具体架构）

2）api 可以生产所有Go包的API列表。
GOROOT/api中的go1.txt就可以通过这个工具生产

3）cgo 允许通过调用C代码创建Go包
4）fix 找到用旧API写的Go程序，然后用新API重写他们。
这个可用于Go升级了，处理用之前版本Go写的应用程序。

5）go 管理Go源代码的工具，很好用很重要的一个工具。
应该总是使用go这个工具，而不是使用6g这样的工具。当然，如果需要生产中间对象，可以使用6g这样的工具。

6）godoc 提取并生产Go程序文档（包括Go本身）
7）gofmt 格式化Go程序代码
8）nm 是Plan 9中的nm工具
。详细说明：http://plan9.bell-labs.com/magic/man2html/1/nm 。查看符号表用的

9）pack 是Plan 9中的ar工具，这个用来归档目标文件。
pkg中的.a文件就是pack生成的。详细说明：http://plan9.bell-labs.com/magic/man2html/1/ar

10）vet 用于检查并报告Go源码中可疑的结构。
比如调用Printf，它的参数和格式化字符串提供的不一致，如：fmt.Printf(“%s is %s”, name)，这样会被检查出来。

11）yacc Go版本的yacc。
http://plan9.bell-labs.com/magic/man2html/1/yacc。这是一个经典的生成语法分析器的工具。更多详细说明，可以查阅相关资料。Yacc 与 Lex 快速入门

以上工具目录中都有doc.go文件，用于生成文档。http://golang.org/cmd/可以查看。

12）addr2line linux下有这个命令。
这是一个addr2line的模拟器，只是为了使pprof能够在mac上工作。关于addr2line，可以查看linux的man手册，也可以看addr2line探秘

13）objdump linux下有这个命令。
这是一个objdump的模拟器，只是为了使pprof能够在mac上工作。关于objdump，可以查看linux的man手册

14）dist 这是一个重要的工具。它是一个引导程序，负责构建Go其他基本工具。通过源码安装Go时，会先安装该工具。
注：安装完之后，pkg/tool/$GOOS_$GOARCH下面的pprof工具是从misc下面copy过来的

## 四、安装脚本
通过源码安装Go相当简单（安装速度也很快），因为它提供了方便的脚本。脚本在src目录下

all.bash/all.bat — 会执行make脚本和run脚本
make.bash/make.bat — 安装Go
run.bash/run.bat — 测试标准库

所以，通过源码安装Go，一般cd到src目录执行./all.bash。如果不想测试标准库，可以直接./make.bash，这样会比较快。

Make.dist 被其他Makefile文件引用，比如cmd下面的很多工具中的Makefile文件。这个文件的作用是：运行go tool dist去安装命令，同时在安装过程中会打印出执行了该文件的目录名。可见，在源码安装Go的过程中，打印出的大部分信息就是这个文件的作用。

## 五、src/pkg Go标准库源码

1) runtime目录

	|── alg.c 	Type结构体中的alg，类型操作。
	|── append_test.go
	|── arch_386.h
	|── arch_amd64.h
	|── arch_arm.h
	|── asm_386.s 	
	|── asm_amd64.s
	|── asm_arm.s
	|── atomic_386.c
	|── atomic_amd64.c
	|── atomic_arm.c
	|── callback_windows_386.c
	|── callback_windows_amd64.c
	|── cgo
	|   ├── asm_386.s
	|   ├── asm_amd64.s
	|   ├── asm_arm.s
	|   ├── callbacks.c
	|   ├── cgo.go
	|   ├── cgo_arm.c
	|   ├── freebsd.c
	|   ├── gcc_386.S
	|   ├── gcc_amd64.S
	|   ├── gcc_arm.S
	|   ├── gcc_darwin_386.c
	|   ├── gcc_darwin_amd64.c
	|   ├── gcc_freebsd_386.c
	|   ├── gcc_freebsd_amd64.c
	|   ├── gcc_freebsd_arm.c
	|   ├── gcc_linux_386.c
	|   ├── gcc_linux_amd64.c
	|   ├── gcc_linux_arm.c
	|   ├── gcc_netbsd_386.c
	|   ├── gcc_netbsd_amd64.c
	|   ├── gcc_netbsd_arm.c
	|   ├── gcc_openbsd_386.c
	|   ├── gcc_openbsd_amd64.c
	|   ├── gcc_setenv.c
	|   ├── gcc_util.c
	|   ├── gcc_windows_386.c
	|   ├── gcc_windows_amd64.c
	|   ├── iscgo.c
	|   ├── libcgo.h
	|   ├── netbsd.c
	|   ├── openbsd.c
	|   └── setenv.c
	|── cgocall.c
	|── cgocall.h
	|── chan.c 	通道的实现
	|── compiler.go
	|── complex.c
	|── cpuprof.c 	pprof相关
	|── debug
	|   ├── debug.c
	|   ├── garbage.go
	|   ├── garbage_test.go
	|   ├── stack.go
	|   └── stack_test.go
	|── debug.go
	|── defs1_linux.go
	|── defs2_linux.go
	|── defs_arm_linux.go
	|── defs_darwin.go
	|── defs_darwin_386.h
	|── defs_darwin_amd64.h
	|── defs_freebsd.go
	|── defs_freebsd_386.h
	|── defs_freebsd_amd64.h
	|── defs_freebsd_arm.h
	|── defs_linux.go
	|── defs_linux_386.h
	|── defs_linux_amd64.h
	|── defs_linux_arm.h
	|── defs_netbsd.go
	|── defs_netbsd_386.go
	|── defs_netbsd_386.h
	|── defs_netbsd_amd64.go
	|── defs_netbsd_amd64.h
	|── defs_netbsd_arm.go
	|── defs_netbsd_arm.h
	|── defs_openbsd.go
	|── defs_openbsd_386.h
	|── defs_openbsd_amd64.h
	|── defs_plan9_386.h
	|── defs_plan9_amd64.h
	|── defs_windows.go
	|── defs_windows_386.h
	|── defs_windows_amd64.h
	|── env_plan9.c
	|── env_posix.c
	|── error.go
	|── extern.go
	|── float.c
	|── gc_test.go
	|── hashmap.c 	map容器的底层实现
	|── hashmap.h
	|── hashmap_fast.c
	|── iface.c 	interface的底层实现
	|── lfstack.c 	垃圾回收中用到了这个文件，lock free stack的缩写。垃圾回收中，利用来实现PtrBuffer的并发安全性
	|── lock_futex.c
	|── lock_sema.c
	|── malloc.goc 	malloc相关的封装，提供了runtime.mallocgc
	|── malloc.h
	|── malloc1.go
	|── mallocrand.go
	|── mallocrep.go
	|── mallocrep1.go
	|── mcache.c	内存管理实现相关，MCache层次的数据结构和操作
	|── mcentral.c 	内存管理实现相关，MCentral层次的数据结构和操作
	|── mem.go 	内存信息统计
	|── mem_darwin.c
	|── mem_freebsd.c
	|── mem_linux.c 	内存分配相关，依赖于系统部分的分配函数。最下层接口，这里提供SysAlloc分配大块内存，提供runtime内存池使用
	|── mem_netbsd.c
	|── mem_openbsd.c
	|── mem_plan9.c
	|── mem_windows.c
	|── memclr_arm.s
	|── memmove_386.s
	|── memmove_amd64.s
	|── memmove_arm.s
	|── memmove_linux_amd64_test.go
	|── mfinal.c
	|── mfinal_test.go
	|── mfixalloc.c
	|── mgc0.c 	垃圾回收最核心的部分都是在这一个文件里实现的
	|── mgc0.go
	|── mgc0.h
	|── mheap.c 	内存管理实现相关，MHeap层次的数据结构和操作
	|── mkversion.c
	|── mprof.goc 	pprof相关
	|── msize.c
	|── netpoll.goc
	|── netpoll_epoll.c 	封装了依赖到系统的epoll，对Go提供runtime.netpoll相关的函数
	|── netpoll_kqueue.c
	|── netpoll_stub.c
	|── os_darwin.c
	|── os_darwin.h
	|── os_freebsd.c
	|── os_freebsd.h
	|── os_freebsd_arm.c
	|── os_linux.c 	goroutine相关，依赖于系统的部分。runtime.newosproc，runtime.minit等函数都在这里实现
	|── os_linux.h
	|── os_linux_386.c
	|── os_linux_arm.c
	|── os_netbsd.c
	|── os_netbsd.h
	|── os_netbsd_386.c
	|── os_netbsd_amd64.c
	|── os_netbsd_arm.c
	|── os_openbsd.c
	|── os_openbsd.h
	|── os_plan9.c
	|── os_plan9.h
	|── os_plan9_386.c
	|── os_plan9_amd64.c
	|── os_windows.c
	|── os_windows.h
	|── os_windows_386.c
	|── os_windows_amd64.c
	|── panic.c
	|── parfor.c 	垃圾回收相关，提供了并行方面的支持
	|── pprof
	|   ├── pprof.go
	|   └── pprof_test.go
	|── print.c
	|── proc.c 		goroutine调度器
	|── proc.p
	|── proc_test.go
	|── race
	|   ├── README
	|   ├── doc.go
	|   ├── race.go
	|   ├── race_darwin_amd64.syso
	|   ├── race_linux_amd64.syso
	|   ├── race_test.go
	|   ├── race_windows_amd64.syso
	|   └── testdata
	|       ├── atomic_test.go
	|       ├── cgo_test.go
	|       ├── cgo_test_main.go
	|       ├── chan_test.go
	|       ├── comp_test.go
	|       ├── finalizer_test.go
	|       ├── io_test.go
	|       ├── map_test.go
	|       ├── mop_test.go
	|       ├── mutex_test.go
	|       ├── regression_test.go
	|       ├── rwmutex_test.go
	|       ├── select_test.go
	|       ├── slice_test.go
	|       ├── sync_test.go
	|       └── waitgroup_test.go
	|── race.c
	|── race.go
	|── race.h
	|── race0.c
	|── race_amd64.s
	|── rt0_darwin_386.s
	|── rt0_darwin_amd64.s
	|── rt0_freebsd_386.s
	|── rt0_freebsd_amd64.s
	|── rt0_freebsd_arm.s
	|── rt0_linux_386.s
	|── rt0_linux_amd64.s 	生成可执行文件的入口，包括main函数等
	|── rt0_linux_arm.s
	|── rt0_netbsd_386.s
	|── rt0_netbsd_amd64.s
	|── rt0_netbsd_arm.s
	|── rt0_openbsd_386.s
	|── rt0_openbsd_amd64.s
	|── rt0_plan9_386.s
	|── rt0_plan9_amd64.s
	|── rt0_windows_386.s
	|── rt0_windows_amd64.s
	|── rune.c
	|── runtime-gdb.py
	|── runtime.c
	|── runtime.h 	几乎大部分的数据结构都是在这个文件中定义的
	|── runtime1.goc
	|── runtime_linux_test.go
	|── runtime_test.go
	|── sema.goc
	|── signal_386.c
	|── signal_amd64.c
	|── signal_arm.c
	|── signal_darwin_386.h
	|── signal_darwin_amd64.h
	|── signal_freebsd_386.h
	|── signal_freebsd_amd64.h
	|── signal_freebsd_arm.h
	|── signal_linux_386.h
	|── signal_linux_amd64.h
	|── signal_linux_arm.h
	|── signal_netbsd_386.h
	|── signal_netbsd_amd64.h
	|── signal_netbsd_arm.h
	|── signal_openbsd_386.h
	|── signal_openbsd_amd64.h
	|── signal_unix.c
	|── signal_unix.h
	|── signals_darwin.h
	|── signals_freebsd.h
	|── signals_linux.h
	|── signals_netbsd.h
	|── signals_openbsd.h
	|── signals_plan9.h
	|── signals_windows.h
	|── sigqueue.goc
	|── slice.c 	slice的底层实现
	|── softfloat64.go
	|── softfloat64_test.go
	|── softfloat_arm.c
	|── stack.c 	runtime.newstack函数是这里实现的
	|── stack.h 	这个文件里的注释很重要，讲了Go里使用的栈结构
	|── string.goc
	|── string_test.go
	|── symtab.c
	|── symtab_test.go
	|── sys_darwin_386.s
	|── sys_darwin_amd64.s
	|── sys_freebsd_386.s
	|── sys_freebsd_amd64.s
	|── sys_freebsd_arm.s
	|── sys_linux_386.s
	|── sys_linux_amd64.s 	封装了重要的系统调用，提供runtime.xx的函数，比如runtime.read
	|── sys_linux_arm.s
	|── sys_netbsd_386.s
	|── sys_netbsd_amd64.s
	|── sys_netbsd_arm.s
	|── sys_openbsd_386.s
	|── sys_openbsd_amd64.s
	|── sys_plan9_386.s
	|── sys_plan9_amd64.s
	|── sys_windows_386.s
	|── sys_windows_amd64.s
	|── syscall_windows.goc
	|── syscall_windows_test.go
	|── time.goc
	|── time_plan9_386.c
	|── traceback_arm.c
	|── traceback_x86.c
	|── type.go
	|── type.h 		类型系统，里面有Type结构体的定义。interface以及reflect也依赖于这里一些东西
	|── typekind.h 	类型系统，提供了Type中的kind编号
	|── vdso_linux_amd64.c
	|── vlop_386.s
	|── vlop_arm.s
	|── vlop_arm_test.go
	|── vlrt_386.c
	|── vlrt_arm.c
	|── zasm_darwin_amd64.h
	|── zgoarch_amd64.go
	|── zgoos_darwin.go
	|── zmalloc_darwin_amd64.c
	|── zmprof_darwin_amd64.c
	|── znetpoll_darwin_amd64.c
	|── zruntime1_darwin_amd64.c
	|── zruntime_defs_darwin_amd64.go
	|── zsema_darwin_amd64.c
	|── zsigqueue_darwin_amd64.c
	|── zstring_darwin_amd64.c
	|── ztime_darwin_amd64.c
	|── zversion.go

