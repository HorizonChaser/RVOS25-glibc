---
title: 搭建 RISC-V glibc 的调试环境并向量化 memcpy
author: 侯文轩
pubDate: 2025-05-28
categories:
  - 2025 年第一期
description: 基于 rvv-env 修改了可用于 RISC-V glibc 的调试环境; 尝试向量化优化了 memcpy; 与 glibc 进行了整合并进行了简单的测试
---
# 搭建 RISC-V glibc 的调试环境并尝试使用 RVV 优化 memcpy

> TL;DR: 目前已完成的工作总结
> 
> 1. 基于 rvv-env 修改了可用于 RISC-V glibc 的调试环境
> 2. 阅读了 RVV 1.0 SPEC
> 3. 对 glibc 中的 `memcpy` 进行了向量化与测试
> 4. 将优化后的 `memcpy` 整合到 glibc 中生成 patch
> 5. 利用 glibc 的框架对优化后的 `memcpy` 进行测试

> 本文的原始文件与 glibc 优化相关的代码与 patch 可从 [RVOS25-glibc 仓库](https://github.com/HorizonChaser/RVOS25-glibc) 获取。
> 修改后的 rvv-env 可从 [rvv-env 仓库](https://github.com/HorizonChaser/rvv-env) 获取。

这篇文章尝试使用 [`rvv-env`](https://gitlab.com/riseproject/rvv-env) 作为 RISC-V 下的交叉编译与调试环境，并在其基础上进行自定义，增加必须的工具与符号等，以用来调试 RISC-V glibc。

## `rvv-env` 环境搭建

[原始版本的 `rvv-env`](https://gitlab.com/riseproject/rvv-env) 提供了容器化的 RISC-V 编译与调试环境，使用 `qemu-user` 在一个容器中（称作 `target`）进行模拟，并在另一个容器（称作 `host`）中通过 GDB 进行远程调试。我对该项目进行了一些修改，**在正确放置所有文件后**，拉取镜像后应当可以直接调试 glibc.

### 常规的使用方法及存在的问题

通常来说，不考虑 glibc 及其调试符号的情况下，我们只需要按照 README 中的顺序，安装 `qemu-user-binfmt` 并拉取镜像即可，也就是

1. `sudo apt install qemu-user-binfmt` ，注意如果你在使用 Ubuntu 24.04，你可能需要使用 `qemu-user` 包替换前者。
2. `git clone https://gitlab.com/riseproject/rvv-env.git`
3. `source env.sh`
4. `./oci-pull.sh` 拉取预构建的镜像

之后，通过 `target-run gcc -o test-out -g test.c` 来编译可执行文件，然后首先 `target-gdb test-out` 启动 QEMU 并附加 gdb，之后再 `host-gdb` 连接到远程的 gdb 进行调试。

一般来说这种方法已经足够，但为了调试 glibc，我们还需要两个东西：**对应当前版本的** glibc 的调试符号，以及**对应当前版本的** glibc 源码。这两种方式都可以通过获取对应的包后提取相应的文件，并在容器启动时挂载到对应的位置上来解决。

> 其实最开始我是通过直接在构建镜像时安装这些包解决的，但这完全没有必要 QAQ
> 构建镜像时会联网拉取预构建的 RISC-V 工具链，很慢，所以我修改了 Dockerfile, 现在它会使用本地的文件。当然，这需要你先下载一次并放到 `rvv-env/container/`

### 获取相关的文件

> tldr: `work/`下所有需要的文件都已经被打包至仓库的 release 中，下载 `work.tar.xz` 并解压即可，注意路径**不要**出现 `rvv-env/work/work/...`, 即多层 `work/`嵌套。

为了简便起见，我的 host 与 target 的基础镜像都使用 Ubuntu 24.04 Noble, 对应的 `libc6` 的包的版本如下，其中 `libc6-dbg` 是调试符号：

```bash
(py) horizon@horizon-VMware20-1:~/project/rvv-env$ IMAGE_VARIANT_TARGET=target-ubuntu target-run bash
horizon@target-ubuntu:/rvv-env$ apt list | grep libc6

WARNING: apt does not have a stable CLI interface. Use with caution in scripts.

libc6-dbg-riscv64-cross/now 2.39-0ubuntu8cross1 all [installed,local]
libc6-dbg/now 2.39-0ubuntu8.4 riscv64 [installed,local]
libc6-dev/now 2.39-0ubuntu8.4 riscv64 [installed,local]
libc6-riscv64-cross/now 2.39-0ubuntu8cross1 all [installed,local]
libc6/now 2.39-0ubuntu8.4 riscv64 [installed,local]
```

首先，从 [我的 GitHub 仓库](https://github.com/HorizonChaser/rvv-env) 获取修改后的 `rvv-env`.

之后，所需的包可以从 [https://packages.ubuntu.com](https://packages.ubuntu.com) 搜索，点击对应架构的文件列表可以看到包安装后的各个文件的位置。[下载 `libc6-dbg`](https://code.launchpad.net/ubuntu/noble/riscv64/libc6-dbg/2.39-0ubuntu8) 后，将其中的 `usr/` 提取出，放置在 `rvv-env/work/` 下。[下载 `glibc 源码`](http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/glibc_2.39.orig.tar.xz) 后，将 `glibc-2.39/` 同样放置在`rvv-env/work/` 下。[下载 `pwndbg portable`](https://github.com/pwndbg/pwndbg/releases/download/2025.04.18/pwndbg_2025.04.18_riscv64-portable.tar.xz) 后，将 `pwndbg/` 同样放置在`rvv-env/work/` 下。此时你的 `rvv-env/work/` 看起来应该是这样的：

```bash
(py) horizon@horizon-VMware20-1:~/project/rvv-env$ tree -L 2 work
work
├── glibc-2.39
│   ├── abi-tags
│   ├── aclocal.m4
│   ├── ADVISORIES
│   ├── argp
│   ├── assert
│   ├── benchtests
│   ├── bits
│   ├── catgets
│   ├── ChangeLog.old
│   ├── config.h.in
│   ├── config.make.in
│   ├── configure
│   ├── configure.ac
│   ├── conform
│   ├── CONTRIBUTED-BY
│   ├── COPYING
│   ├── ... (omitted)
│   ├── wcsmbs
│   └── wctype
├── pwndbg
│   ├── bin
│   ├── exe
│   ├── lib
│   └── share
└── usr
    ├── lib
    └── share

71 directories, 31 files
```


### 拉取镜像

安装 Docker（建议使用 [rootless mode](https://docs.docker.com/engine/security/rootless/)) 后，在 `rvv-env` 下 `source env.sh`，再拉取镜像，注意这里拉取的是 `target-ubuntu`：

```bash
(py) horizon@horizon-VMware20-1:~/project/rvv-env$ IMAGE_VARIANT_TARGET=target-ubuntu ./oci-pull.sh
```

### 对 `rvv-env` 进行的修改

1. 修改了 `env.sh`, 现在 `source env.sh` 是幂等的，同时会将 `${WORK_DIR}` 指向 `rvv-env/work/`
2. 修改了 `oci-run.sh`, 额外增加了一个 `host-pwndbg` 的命令，用于使用 pwndbg 进行调试
3. 修改了 `oci-run.sh`, 默认设置好符号和源码的搜索路径 (目前仅对 `host-gdb` 生效; 由于 `pwndbg`本质上是 gdb 的插件，在加载之后就会覆盖配置，难以通过命令行参数指定)
4. 修改了 `container/variants/target-*.env`, 现在在构建镜像时会额外增加 glibc 相关的包，不过因为不再需要手动构建镜像所以用不到了

## 使用 `rvv-env`

基本的使用方法按照 [[#常规的使用方法及存在的问题]] 即可，修改后的 `rvv-env` 默认会将 `rvv-env/work/` 挂载到容器的 `/work/` 下，同时额外增加了一个 `host-pwndbg` 的命令，用于使用 pwndbg 进行调试，并默认启用 `compact-reg` 来缩小寄存器显示的空间。

右侧是通过 `host-pwndbg` 运行的 `pwndbg`, 同时设置了源码与符号的路径，正在观察调试 `memcpy`.

![[Pasted image 20250520160136.png]]

## 向量化 `memcpy`

> 在完成了之后，才发现戴成荣老师在 25 年 1 月份 [提交了 `memcpy` 的向量化补丁](https://gcc.gnu.org/pipermail/libc-alpha/2025-January/164045.html), 不过 `glibc 2.41.0` 并未包含了向量化的 `memcpy`, 而是增加了支持快速未对齐访问的版本...

### 对常规版本的 `memcpy`的分析

常规版本的 `memcpy`(`glibc-2.39/string/memcpy.c`) 实现原理比较简单，代码如下：

```c
void *
MEMCPY (void *dstpp, const void *srcpp, size_t len)
{
  unsigned long int dstp = (long int) dstpp;
  unsigned long int srcp = (long int) srcpp;

  /* Copy from the beginning to the end.  */

  /* If there not too few bytes to copy, use word copy.  */
  if (len >= OP_T_THRES)
    {
      /* Copy just a few bytes to make DSTP aligned.  */
      len -= (-dstp) % OPSIZ;
      BYTE_COPY_FWD (dstp, srcp, (-dstp) % OPSIZ);

      /* Copy whole pages from SRCP to DSTP by virtual address manipulation,
	 as much as possible.  */

      PAGE_COPY_FWD_MAYBE (dstp, srcp, len, len);

      /* Copy from SRCP to DSTP taking advantage of the known alignment of
	 DSTP.  Number of bytes remaining is put in the third argument,
	 i.e. in LEN.  This number may vary from machine to machine.  */

      WORD_COPY_FWD (dstp, srcp, len, len);

      /* Fall out and copy the tail.  */
    }

  /* There are just a few bytes to copy.  Use byte memory operations.  */
  BYTE_COPY_FWD (dstp, srcp, len);

  return dstpp;
}
```

具体来说，当复制的长度足够 (>= 16 bytes) 时，首先复制前 `(-dstp) % OPSIZ` 个字节，使得 src 和 dst 与 16B 对齐，之后尝试按照整页复制 (不过 RISC-V 上由于 `PAGE_COPY_THRESHOLD` 被定义为 `0`, 所以这里并未生效). 最后，逐字节拷贝直到完成。

### RVV 下的实现

基于对原始 `memcpy` 的分析，我们可以使用向量加速复制的过程，即

1. 计算这次向量操作的长度，即 `vsetl` 指令
2. 按块读取内存，保存到向量寄存器，即 `vle` 指令
3. 将向量寄存器的值保存到指定地址，即 `vse` 指令

不过在这之前和之后，我们需要手动处理对齐和尾部剩余的问题，就像原始版本的 `memcpy` 做的那样。核心部分的代码如下，具体的细节参见注释。

> 完整的代码参见 [GitHub 上的仓库](https://github.com/HorizonChaser/RVOS25-glibc)

```C
void* memcpy_rvv_internal(void* dst, const void* src, size_t len) {
    void* original_dst = dst;
    uint8_t* dst_u8 = (uint8_t*)dst;
    const uint8_t* src_u8 = (const uint8_t*)src;
    size_t vlenb;

    // 获取硬件支持的单个向量寄存器的长度, 单位字节
    asm volatile("csrr %0, vlenb" : "=r"(vlenb));

    // 如果长度小于16字节，直接使用字节拷贝, 类似原始版本的 memcpy
    if (len < 16) {
        for (size_t i = 0; i < len; ++i)
            dst_u8[i] = src_u8[i];
        return original_dst;
    }

    // 确保两个指针都对齐到 8 字节
    size_t head_len = 0;
    if (((uintptr_t)dst_u8 % sizeof(unsigned long)) != 0) {
        head_len = sizeof(unsigned long) - ((uintptr_t)dst_u8 % sizeof(unsigned long));
        if (head_len > len)
            head_len = len;

        // 处理头部未对齐的字节
        for (size_t i = 0; i < head_len; ++i)
            dst_u8[i] = src_u8[i];
        dst_u8 += head_len;
        src_u8 += head_len;
        len -= head_len;
    }

    // 向量化, 启动
    if (len >= sizeof(unsigned long) && (((uintptr_t)src_u8 % sizeof(unsigned long)) == 0)) {
        unsigned long* dst_ul = (unsigned long*)dst_u8;
        const unsigned long* src_ul = (const unsigned long*)src_u8;
        size_t num_ul = len / sizeof(unsigned long);
        size_t current_vl_ul;
        if (num_ul > 0) {
            // 64-bit
            if (sizeof(unsigned long) == 8) {
                asm volatile(
                    "1:\n" //循环开始
                    "vsetvli %[vl], %[len_in_elements], e64, m8, ta, ma\n" //设置vl
                    "beqz %[vl], 2f\n" //如果 vl == 0, 跳到 2, 也就是结束
                    "vle64.v v0, (%[src_ptr])\n" // 从 src 读入数据到 v0
                    "vse64.v v0, (%[dst_ptr])\n" // 从 v0 写入数据到 dst
                    // 计算处理的字节数
                    "slli %[processed_bytes], %[vl], 3\n"
                    "add %[src_ptr], %[src_ptr], %[processed_bytes]\n"
                    "add %[dst_ptr], %[dst_ptr], %[processed_bytes]\n"
                    "sub %[len_in_elements], %[len_in_elements], %[vl]\n"
                    "j 1b\n"
                    "2:\n"
                    : [vl] "=&r"(current_vl_ul), [len_in_elements] "+&r"(num_ul),
                    [src_ptr] "+&r"(src_ul), [dst_ptr] "+&r"(dst_ul)
                    : [processed_bytes] "r"(0)
                    : "t0", "t1", "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7");
            } else {
                asm volatile(
                    "1:\n"
                    "vsetvli %[vl], %[len_in_elements], e32, m8, ta, ma\n"
                    "beqz %[vl], 2f\n"
                    "vle32.v v0, (%[src_ptr])\n"
                    "vse32.v v0, (%[dst_ptr])\n"
                    "slli %[processed_bytes], %[vl], 2\n"
                    "add %[src_ptr], %[src_ptr], %[processed_bytes]\n"
                    "add %[dst_ptr], %[dst_ptr], %[processed_bytes]\n"
                    "sub %[len_in_elements], %[len_in_elements], %[vl]\n"
                    "j 1b\n"
                    "2:\n"
                    : [vl] "=&r"(current_vl_ul), [len_in_elements] "+&r"(num_ul),
                    [src_ptr] "+&r"(src_ul), [dst_ptr] "+&r"(dst_ul)
                    : [processed_bytes] "r"(0)
                    : "t0", "t1", "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7");
            }
            size_t bytes_processed_ul = (len / sizeof(unsigned long) - num_ul) * sizeof(unsigned long);
            dst_u8 = (uint8_t*)dst_ul;
            src_u8 = (const uint8_t*)src_ul;
            len -= bytes_processed_ul;
        }
    }
    // 如果还有剩余的字节，使用字节向量化拷贝
    if (len > 0) {
        size_t current_vl_u8;
        uint8_t* dst_u8_asm = dst_u8;
        const uint8_t* src_u8_asm = src_u8;
        size_t len_asm = len;
        asm volatile(
            "1:\n"
            "vsetvli %[vl], %[rem_len], e8, m8, ta, ma\n"
            "beqz %[vl], 2f\n"
            "vle8.v v0, (%[src_ptr])\n"
            "vse8.v v0, (%[dst_ptr])\n"
            "add %[src_ptr], %[src_ptr], %[vl]\n"
            "add %[dst_ptr], %[dst_ptr], %[vl]\n"
            "sub %[rem_len], %[rem_len], %[vl]\n"
            "j 1b\n"
            "2:\n"
            : [vl] "=&r"(current_vl_u8), [rem_len] "+&r"(len_asm),
            [src_ptr] "+&r"(src_u8_asm), [dst_ptr] "+&r"(dst_u8_asm)
            :
            : "t0", "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7");
    }
    return original_dst;
}
```

整体的思路和原始版本的 `memcpy` 基本一致。

### 对向量化后的正确性与性能分析

对两种不同的 `memcpy`, 在各种不同的复制大小下进行测试，并使用 `memcmp` 保证正确性，结果如下：

> 编译参数：`-march=rv64gcv -mabi=lp64d -O2`
> 在 `target-run-rvv` (即 qemu) 中运行

> 完整代码参见我的 [GitHub 仓库](https://github.com/HorizonChaser/RVOS25-glibc)

![[Pasted image 20250521170515.png]]

结果表明，在 qemu 下，向量化对于 `memcpy` 的性能提升并不显著，甚至具有负面作用。使用每次只搬运一字节的向量化版本 (即 `e8`) 进行对比，结果如下：

![[Pasted image 20250521171216.png]]

可以发现更大的元素宽度对于向量化后的性能是有提升的，这说明向量化后的性能降低可能与 qemu 实现中 `vsetvl`, `vle` 和 `vse` 指令的实现性能有关 -- 模拟后的这些指令的开销可能过大。

## 整合到 glibc

### glibc 的目录结构

参考 [glibc 文档附录 D -- 维护](https://www.gnu.org/software/libc/manual/html_mono/libc.html#Maintenance), glibc 的结构基本按照功能分类，例如 `string/` 下是所有的字符串函数，`math/` 下是所有的数学函数，以此类推。每个子目录下有一个简单的 `Makefile` 记录了这个文件夹下的所有需要构建的内容。

> 通用的 `memcpy` 实现就在 `string/memcpy.c`, 不知道为什么会放在这里。[一种说法](https://stackoverflow.com/questions/9782126/why-memory-functions-such-as-memset-memchr-are-in-string-h-but-not-in-stdli) 是，`memcpy` 这种并非特定于操作系统的 "内存" 操作对于类似嵌入式系统的场景同样需要 -- 它们不一定有 `malloc` 这些内存分配方法，但确实需要用到 `memcpy` 这种单纯的操作。[另一种说法](https://www.quora.com/Why-is-memcpy-located-in-string-h) 是，`memcpy` 最早其实是用来复制 Pascal 中的定长字符串的，而 `strcpy` 用来复制 (不定长的) `\0` 结尾的字符串，因此 `memcpy` 某种程度上也算作字符串操作函数。 
> 
> 总之，这看上去是历史原因，毕竟现在的 `memcpy` 和字符串已经没什么关系了，不过你当然还是可以拿它复制定长的字符串。

与特定架构相关的内容放在 `sysdeps/` 下，例如 `sysdeps/riscv/` 下就是与 RISC-V (但与特定的系统无关) 相关的东西了。`sysdeps/unix/sysv/linux/` 下则是与 Linux 相关的内容，类似地，`sysdeps/unix/sysv/linux/riscv/` 下就是与 Linux 上的 RISC-V 相关的内容了。

### 整合

参考 [戴成荣老师提交的 patch](https://patchwork.sourceware.org/project/glibc/list/?series=44338), 要利用 `IFUNC` 机制实现自动选择向量化版本的 `memcpy`, 我们大致需要做两件事情：

1. 在 `sysdeps/riscv/` 下实现对 RVV 支持的检测
2. 在 `sysdeps/unix/sysv/linux/riscv/multiarch/` 下实现调用向量化版本的 `memcpy` 的逻辑

glibc 的构建逻辑大致如下：首先，`autoconf` 根据 `configure.ac` 模板生成 `configure` 脚本，根据 `config.h.in` 生成 `config.h`, 之后运行 `configure` 脚本，它根据提供的命令行选项生成最终用于编译的 `Makefile`, 最后就是 `make` 根据前面生成的 `Makefile` 完成编译构建，测试与安装流程了。

> 以下的内容同样参考 [戴程容老师提交的 patch](https://patchwork.sourceware.org/project/glibc/list/?series=44338), 使用 unified-diff 格式。 
> 
> 基于 glibc-2.41.

**首先**, 修改 `./config.h.in`, 在 RISC-V 相关的部分增加一个宏 `HAVE_RISCV_ASM_VECTOR_SUPPORT`:

```diff
diff --color -u -r -p -N '--exclude=autom4te.cache' '--exclude=.vscode' glibc-2.41/config.h.in glibc-2.41-patched/config.h.in
--- glibc-2.41/config.h.in	2025-01-29 01:31:33.000000000 +0800
+++ glibc-2.41-patched/config.h.in	2025-05-27 23:47:18.775395328 +0800
@@ -139,6 +139,10 @@
 /* RISC-V floating-point ABI for ld.so.  */
 #undef RISCV_ABI_FLEN
 
+/* NEW */
+/* Define if assembler supports vector instructions on RISC-V.  */
+#undef HAVE_RISCV_ASM_VECTOR_SUPPORT
+
 /* LOONGARCH integer ABI for ld.so.  */
 #undef LOONGARCH_ABI_GRLEN
```

**之后**, 修改 `sysdeps/riscv/configure.ac`:

```diff
diff --color -u -r -p -N '--exclude=autom4te.cache' '--exclude=.vscode' glibc-2.41/sysdeps/riscv/configure.ac glibc-2.41-patched/sysdeps/riscv/configure.ac
--- glibc-2.41/sysdeps/riscv/configure.ac	2025-01-29 01:31:33.000000000 +0800
+++ glibc-2.41-patched/sysdeps/riscv/configure.ac	2025-05-27 23:47:21.135442533 +0800
@@ -40,6 +40,29 @@ EOF
   fi
   rm -rf conftest* ])
 
+AC_CACHE_CHECK([for gcc attribute riscv vector support],
+libc_cv_gcc_rvv, [dnl
+cat > conftest.S <<EOF
+foo:
+.option push
+.option arch, +v
+vsetivli t0, 8, e8, m8, ta, ma
+.option pop
+ret
+EOF
+libc_cv_gcc_rvv=no
+if ${CC-asm} -c conftest.S -o conftest.o 1>&AS_MESSAGE_LOG_FD \
+2>&AS_MESSAGE_LOG_FD ; then
+libc_cv_gcc_rvv=yes
+fi
+rm -f conftest*])
+
+if test x"$libc_cv_gcc_rvv" = xyes; then
+AC_DEFINE(HAVE_RISCV_ASM_VECTOR_SUPPORT)
+fi
+
+LIBC_CONFIG_VAR([have-gcc-riscv-rvv], [$libc_cv_gcc_rvv])
+
 if test "$libc_cv_static_pie_on_riscv" = yes; then
   AC_DEFINE(SUPPORT_STATIC_PIE)
 fi
```

这里使用了 `AC_CACHE_CHECK` 宏，它有三个参数：描述，结果变量名，以及检测代码。判断逻辑也很直观：将一段使用了 `vsetivli` 指令的汇编程序写到 `conftest.S` 中并尝试编译执行，如果能运行，就说明目标系统支持 RVV, 把结果缓存到 `libc_cv_gcc_rvv` 中，并定义 `HAVE_RISCV_ASM_VECTOR_SUPPORT` 宏。

**之后**, 定义我们新的向量化 `memcpy`, 位于 `sysdeps/riscv/multiarch/memcpy_vector.c`:

```diff
diff --color -u -r -p -N '--exclude=autom4te.cache' '--exclude=.vscode' glibc-2.41/sysdeps/riscv/multiarch/memcpy_vector.c glibc-2.41-patched/sysdeps/riscv/multiarch/memcpy_vector.c
--- glibc-2.41/sysdeps/riscv/multiarch/memcpy_vector.c	1970-01-01 08:00:00.000000000 +0800
+++ glibc-2.41-patched/sysdeps/riscv/multiarch/memcpy_vector.c	2025-05-27 23:47:21.155442932 +0800
@@ -0,0 +1,108 @@
+#include<stdint.h>
+#include<stddef.h>
+
+void* __memcpy_vector(void* dst, const void* src, size_t len) {
+    void* original_dst = dst;
+    uint8_t* dst_u8 = (uint8_t*)dst;
+    const uint8_t* src_u8 = (const uint8_t*)src;
+    size_t vlenb;
+
+    // 获取硬件支持的单个向量寄存器的长度, 单位字节
+    asm volatile("csrr %0, vlenb" : "=r"(vlenb));
+
+    // 如果长度小于16字节，直接使用字节拷贝, 类似原始版本的 memcpy
+    if (len < 16) {
+        for (size_t i = 0; i < len; ++i)
+            dst_u8[i] = src_u8[i];
+        return original_dst;
+    }
+
+    // 确保两个指针都对齐到 8 字节
+    size_t head_len = 0;
+    if (((uintptr_t)dst_u8 % sizeof(unsigned long)) != 0) {
+        head_len = sizeof(unsigned long) - ((uintptr_t)dst_u8 % sizeof(unsigned long));
+        if (head_len > len)
+            head_len = len;
+
+        // 处理头部未对齐的字节
+        for (size_t i = 0; i < head_len; ++i)
+            dst_u8[i] = src_u8[i];
+        dst_u8 += head_len;
+        src_u8 += head_len;
+        len -= head_len;
+    }
+
+    // 向量化, 启动
+    if (len >= sizeof(unsigned long) && (((uintptr_t)src_u8 % sizeof(unsigned long)) == 0)) {
+        unsigned long* dst_ul = (unsigned long*)dst_u8;
+        const unsigned long* src_ul = (const unsigned long*)src_u8;
+        size_t num_ul = len / sizeof(unsigned long);
+        size_t current_vl_ul;
+        if (num_ul > 0) {
+            // 64-bit
+            if (sizeof(unsigned long) == 8) {
+                asm volatile(
+                    "1:\n" //循环开始
+                    "vsetvli %[vl], %[len_in_elements], e64, m8, ta, ma\n" //设置vl
+                    "beqz %[vl], 2f\n" //如果 vl == 0, 跳到 2, 也就是结束
+                    "vle64.v v0, (%[src_ptr])\n" // 从 src 读入数据到 v0
+                    "vse64.v v0, (%[dst_ptr])\n" // 从 v0 写入数据到 dst
+                    // 计算处理的字节数
+                    "slli %[processed_bytes], %[vl], 3\n"
+                    "add %[src_ptr], %[src_ptr], %[processed_bytes]\n"
+                    "add %[dst_ptr], %[dst_ptr], %[processed_bytes]\n"
+                    "sub %[len_in_elements], %[len_in_elements], %[vl]\n"
+                    "j 1b\n"
+                    "2:\n"
+                    : [vl] "=&r"(current_vl_ul), [len_in_elements] "+&r"(num_ul),
+                    [src_ptr] "+&r"(src_ul), [dst_ptr] "+&r"(dst_ul)
+                    : [processed_bytes] "r"(0)
+                    : "t0", "t1", "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7");
+            } else {
+                asm volatile(
+                    "1:\n"
+                    "vsetvli %[vl], %[len_in_elements], e32, m8, ta, ma\n"
+                    "beqz %[vl], 2f\n"
+                    "vle32.v v0, (%[src_ptr])\n"
+                    "vse32.v v0, (%[dst_ptr])\n"
+                    "slli %[processed_bytes], %[vl], 2\n"
+                    "add %[src_ptr], %[src_ptr], %[processed_bytes]\n"
+                    "add %[dst_ptr], %[dst_ptr], %[processed_bytes]\n"
+                    "sub %[len_in_elements], %[len_in_elements], %[vl]\n"
+                    "j 1b\n"
+                    "2:\n"
+                    : [vl] "=&r"(current_vl_ul), [len_in_elements] "+&r"(num_ul),
+                    [src_ptr] "+&r"(src_ul), [dst_ptr] "+&r"(dst_ul)
+                    : [processed_bytes] "r"(0)
+                    : "t0", "t1", "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7");
+            }
+            size_t bytes_processed_ul = (len / sizeof(unsigned long) - num_ul) * sizeof(unsigned long);
+            dst_u8 = (uint8_t*)dst_ul;
+            src_u8 = (const uint8_t*)src_ul;
+            len -= bytes_processed_ul;
+        }
+    }
+    // 如果还有剩余的字节，使用字节向量化拷贝
+    if (len > 0) {
+        size_t current_vl_u8;
+        uint8_t* dst_u8_asm = dst_u8;
+        const uint8_t* src_u8_asm = src_u8;
+        size_t len_asm = len;
+        asm volatile(
+            "1:\n"
+            "vsetvli %[vl], %[rem_len], e8, m8, ta, ma\n"
+            "beqz %[vl], 2f\n"
+            "vle8.v v0, (%[src_ptr])\n"
+            "vse8.v v0, (%[dst_ptr])\n"
+            "add %[src_ptr], %[src_ptr], %[vl]\n"
+            "add %[dst_ptr], %[dst_ptr], %[vl]\n"
+            "sub %[rem_len], %[rem_len], %[vl]\n"
+            "j 1b\n"
+            "2:\n"
+            : [vl] "=&r"(current_vl_u8), [rem_len] "+&r"(len_asm),
+            [src_ptr] "+&r"(src_u8_asm), [dst_ptr] "+&r"(dst_u8_asm)
+            :
+            : "t0", "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7");
+    }
+    return original_dst;
+}
\ No newline at end of file
```

这里的代码与前述没有区别; 向量化 `memcpy` 函数的入口为 **`__memcpy_vector`**, 记住它。

**之后**, 修改 `sysdeps/unix/sysv/linux/riscv/multiarch/ifunc-impl-list.c`, 如其名字所示，它包含了 `IFUNC` 的实现函数列表：

```diff
diff --color -u -r -p -N '--exclude=autom4te.cache' '--exclude=.vscode' glibc-2.41/sysdeps/unix/sysv/linux/riscv/multiarch/ifunc-impl-list.c glibc-2.41-patched/sysdeps/unix/sysv/linux/riscv/multiarch/ifunc-impl-list.c
--- glibc-2.41/sysdeps/unix/sysv/linux/riscv/multiarch/ifunc-impl-list.c	2025-01-29 01:31:33.000000000 +0800
+++ glibc-2.41-patched/sysdeps/unix/sysv/linux/riscv/multiarch/ifunc-impl-list.c	2025-05-27 23:47:20.415428131 +0800
@@ -20,6 +20,9 @@
 #include <string.h>
 #include <sys/hwprobe.h>
 
+#include <ldsodefs.h>
+#include <asm/hwcap.h>
+
 size_t
 __libc_ifunc_impl_list (const char *name, struct libc_ifunc_impl *array,
 			size_t max)
@@ -28,6 +31,15 @@ __libc_ifunc_impl_list (const char *name
 
   bool fast_unaligned = false;
 
+  #if defined(HAVE_RISCV_ASM_VECTOR_SUPPORT)
+    bool rvv_ext = false;
+  #endif
+
+  #if defined(HAVE_RISCV_ASM_VECTOR_SUPPORT)
+    if (GLRO(dl_hwcap) & COMPAT_HWCAP_ISA_V)
+      rvv_ext = true;
+  #endif
+
   struct riscv_hwprobe pair = { .key = RISCV_HWPROBE_KEY_CPUPERF_0 };
   if (__riscv_hwprobe (&pair, 1, 0, NULL, 0) == 0
       && (pair.value & RISCV_HWPROBE_MISALIGNED_MASK)
@@ -35,6 +47,10 @@ __libc_ifunc_impl_list (const char *name
     fast_unaligned = true;
 
   IFUNC_IMPL (i, name, memcpy,
+    #if defined(HAVE_RISCV_ASM_VECTOR_SUPPORT)
+        IFUNC_IMPL_ADD(array, i, memcpy, rvv_ext,
+          __memcpy_vector)
+    #endif
 	      IFUNC_IMPL_ADD (array, i, memcpy, fast_unaligned,
 			      __memcpy_noalignment)
 	      IFUNC_IMPL_ADD (array, i, memcpy, 1, __memcpy_generic))
```

新增的代码主要判断 `HAVE_RISCV_ASM_VECTOR_SUPPORT` 宏是否定义，如果定义，则通过 `IFUNC_IMPL_ADD` 宏增加 **`__memcpy_vector`** 作为 `memcpy` 的一个具体实现。

**之后**, 修改 `sysdeps/unix/sysv/linux/riscv/multiarch/Makefile`, 让它把我们定义的新的 `memcpy_vector.c` 加入构建：

```diff
diff --color -u -r -p -N '--exclude=autom4te.cache' '--exclude=.vscode' glibc-2.41/sysdeps/unix/sysv/linux/riscv/multiarch/Makefile glibc-2.41-patched/sysdeps/unix/sysv/linux/riscv/multiarch/Makefile
--- glibc-2.41/sysdeps/unix/sysv/linux/riscv/multiarch/Makefile	2025-01-29 01:31:33.000000000 +0800
+++ glibc-2.41-patched/sysdeps/unix/sysv/linux/riscv/multiarch/Makefile	2025-05-27 23:47:20.415428131 +0800
@@ -5,5 +5,11 @@ sysdep_routines += \
   memcpy_noalignment \
   # sysdep_routines
 
+ifeq ($(have-gcc-riscv-rvv),yes)
+sysdep_routines += \
+  memcpy_vector \
+  # rvv sysdep_routines
+endif
+
 CFLAGS-memcpy_noalignment.c += -mno-strict-align
 endif
```

**最后**, 修改 `sysdeps/unix/sysv/linux/riscv/multiarch/memcpy.c`, 在其中的 `IFUNC` 选择器中增加调用我们的 `__memcpy_vector` 的逻辑：

```diff
diff --color -u -r -p -N '--exclude=autom4te.cache' '--exclude=.vscode' glibc-2.41/sysdeps/unix/sysv/linux/riscv/multiarch/memcpy.c glibc-2.41-patched/sysdeps/unix/sysv/linux/riscv/multiarch/memcpy.c
--- glibc-2.41/sysdeps/unix/sysv/linux/riscv/multiarch/memcpy.c	2025-01-29 01:31:33.000000000 +0800
+++ glibc-2.41-patched/sysdeps/unix/sysv/linux/riscv/multiarch/memcpy.c	2025-05-27 23:47:20.415428131 +0800
@@ -27,16 +27,25 @@
 # include <ifunc-init.h>
 # include <riscv-ifunc.h>
 # include <sys/hwprobe.h>
+# include <asm/hwcap.h>
 
 extern __typeof (__redirect_memcpy) __libc_memcpy;
 
 extern __typeof (__redirect_memcpy) __memcpy_generic attribute_hidden;
 extern __typeof (__redirect_memcpy) __memcpy_noalignment attribute_hidden;
 
+extern __typeof(__redirect_memcpy) __memcpy_vector attribute_hidden;
+
 static inline __typeof (__redirect_memcpy) *
 select_memcpy_ifunc (uint64_t dl_hwcap, __riscv_hwprobe_t hwprobe_func)
 {
   unsigned long long int v;
+
+  #if defined(HAVE_RISCV_ASM_VECTOR_SUPPORT)
+    if (dl_hwcap & COMPAT_HWCAP_ISA_V)
+      return __memcpy_vector;
+  #endif
+
   if (__riscv_hwprobe_one (hwprobe_func, RISCV_HWPROBE_KEY_CPUPERF_0, &v) == 0
       && (v & RISCV_HWPROBE_MISALIGNED_MASK) == RISCV_HWPROBE_MISALIGNED_FAST)
     return __memcpy_noalignment;
```

### 最终补丁

位于 [RVOS25-glibc 仓库的 `patch/` 下](https://github.com/HorizonChaser/RVOS25-glibc/), 你可以通过如下方式打 patch:

1. 将 `memcpy_rvv.patch` 放到 `glibc-2.41/` 的 **同级目录**
2. 进入 `glibc-2.41/`
3. `patch -p1 --verbose --dry-run < ../memcpy_rvv.patch`, 此时 `patch` 应当会尝试执行，能够找到正确的文件 (如果要你输入文件名，就说明层级错了)
4. `patch -p1 --verbose < ../memcpy_rvv.patch`

### 交叉编译并使用 glibc 的框架进行测试

#### 编译

编译机的物理硬件如下：

```text
Host: PowerEdge T640
Kernel: 5.15.0-67-generic
Uptime: 15 days, 1 hour, 53 mins
Packages: 1955 (dpkg), 11 (snap)
Shell: zsh 5.8
CPU: Intel Xeon Gold 6248R (96) @ 4.000GHz
GPU: NVIDIA d9:00.0 NVIDIA Corporation Device 20b5
GPU: NVIDIA 3b:00.0 NVIDIA Corporation Device 20b5
GPU: NVIDIA b1:00.0 NVIDIA Corporation Device 20b5
Memory: 23135MiB / 1031433MiB
```

编译在 Ubuntu x64 容器中进行，版本为 `25.10 (Questing Quokka)`.

从 [riscv-gnu-toolchain](https://github.com/riscv-collab/riscv-gnu-toolchain/releases/download/2025.05.22/riscv64-glibc-ubuntu-24.04-gcc-nightly-2025.05.22-nightly.tar.xz) 获取适用于 Ubuntu 的 riscv **交叉**编译器 (x64 -> RV64), 并解压到容器的 `/opt/` 下。

在**合并 patch 后**，**首先**, 在 `glibc-2.41/` 下，执行 `touch ./sysdeps/riscv/configure.ac; autoconf -I. ./sysdeps/riscv/configure.ac > ./sysdeps/riscv/configure` 以更新相关的 `configure` 脚本。注意该过程需要你的 `autoconf` 版本为 2.72.

**之后**, 在 `glibc-2.41/` 的 **同级目录** 创建 `build/` 并进入，执行下述命令进行配置：

```bash
../glibc-2.41/configure \
--prefix=/home/ubuntu/glibc-ins \
--host=riscv64-unknown-linux-gnu \
--build=x86_64-pc-linux-gnu \
--with-headers=/opt/riscv/sysroot/usr/include/   \
CFLAGS="-O2 -march=rv64gcv_zicsr_zifencei_zba_zbb_zbc_zbs -mabi=lp64d"   \
CXXFLAGS="-O2 -march=rv64gcv_zicsr_zifencei_zba_zbb_zbc_zbs -mabi=lp64d"  \
--disable-werror
```

可以**根据你的实际情况修改** `--prefix` (安装时的目录) 与 `--with-headers` (RISC-V 64 版本的 Linux 内核头文件，在 riscv-gnu-toolchain 中提供).

**最后**, 进行交叉编译：

```bash
make -j$(nproc)
```

> 交叉编译如果快的话大概只需一分钟，比 qemu 下模拟本机编译快很多
> 
> ![[Pasted image 20250528181334.png]]

将编译的 glibc 安装到 `prefix` 指定的位置：

```bash
make install
```

构建测试，这里我们只构建 `string` 相关的测试，`memcpy` 相关的测试也在它下面：

```bash
make bench BENCHSET="string-benchset"
```

#### 测试

配置好 `qemu-binfmt` 环境，**或者** 直接安装 `qemu-riscv64` 包，然后在 `build/` 下运行

```bash
qemu-riscv64 ./benchtests/bench-mempcpy
```

它应当会输出一大段 json, 前几行应该类似

```json
{
 "timing_type": "hp_timing",
 "functions": {
  "memcpy": {
   "bench-variant": "default",
   "ifuncs": ["generic_memcpy", "__memcpy_vector", "__memcpy_noalignment", "__memcpy_generic"],
   "results": [
    {
     "length": 1,
     "align1": 0,
     "align2": 0,
     "dst > src": 0,
     "timings": [171.911, 142.239, 114.986, 102.326]
    },
    ... //omitted
    ]
  }
}
```

可以看到我们的 `__memcpy_vector` 以及其时间。正确性也已经在前述的 [[#对向量化后的正确性与性能分析]] 中检测。

## 总结与结论

本文在 `rvv-env` 的基础上，增加了对 glibc 调试的支持，包括源码与调试符号等，同时增加了 pwndbg 作为调试工具，尽可能做到开箱即用。

同时，本文对 `memcpy` 进行了一定程度的向量化，使用 `vse` 与 `vle` 组合实现内存的向量化复制，利用 glibc 的 `IFUNC` 机制将其整合，同时测试了向量化后的性能。尽管由于条件所限在 qemu 下向量化后的性能没有提升，但保证了结果的正确性。

> Q: 为什么 qemu 下的 RVV 这么慢？ 
> A: 这很可能确实是一个 qemu 的实现问题 ([Issue on GitLab](https://gitlab.com/qemu-project/qemu/-/issues/2137), [QEMU Patch](https://lists.gnu.org/archive/html/qemu-riscv/2024-07/msg00496.html))。按照个人理解，qemu 的 `virt` 设备并不对应任何一种现实的物理设备，它应当只被用于程序的正确性检测，也不能表现程序的真实性能。

### 下一步的计划

下一步计划继续围绕 glibc 的其他函数及 `IFUNC` 机制展开。由于时间所限，本次只对 `memcpy` 进行了优化，同时对 `IFUNC` 的具体实现机制也没有很深入的了解。

因此，下一步计划：

1. 继续阅读 RVV Spec 与 `IFUNC` 相关的文档
2. 阅读 glibc 源码，深入理解其编译流程
3. 了解 `IFUNC` 的具体实现机制
4. 继续尝试优化其他 glibc 库函数
