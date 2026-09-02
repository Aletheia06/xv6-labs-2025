// ================================
// ELF 可执行文件头
// ================================

// ELF 文件最开始的 4 个字节：
// 0x7F 'E' 'L' 'F'
// 用来判断这个文件是不是 ELF 格式。
#define ELF_MAGIC 0x464C457FU


// ========================================
// ELF Header
// 描述整个 ELF 文件的总体信息
// ========================================
struct elfhdr {

  // ELF 魔数
  // 必须等于 ELF_MAGIC，否则说明不是合法 ELF 文件。
  uint magic;

  // ELF 标识信息，共 12 字节。
  // 与标准 ELF 的 e_ident[4..15] 对应。
  //
  // 主要包含：
  //   ELF 是 32 位还是 64 位
  //   大端还是小端
  //   ELF 版本
  //   OS ABI
  //   ABI 版本
  // 等信息。
  uchar elf[12];

  // ELF 文件类型：
  //   可执行文件
  //   可重定位目标文件
  //   共享库
  //   Core Dump
  // 等。
  ushort type;

  // 目标 CPU 架构。
  // xv6-riscv 使用的是 RISC-V。
  ushort machine;

  // ELF 格式版本。
  // 一般为当前版本 1。
  uint version;

  // 程序入口点。
  // 表示程序装载完成之后，
  // CPU 应该从哪个“虚拟地址”开始执行。
  //
  // xv6 exec() 最终会把它设置到：
  // p->trapframe->epc
  uint64 entry;

  // Program Header Table 在 ELF 文件中的偏移。
  //
  // 注意：
  // 这是“文件偏移”，不是虚拟地址。
  //
  // 内核通过它找到所有 proghdr。
  uint64 phoff;

  // Section Header Table 在 ELF 文件中的偏移。
  //
  // Section 主要用于链接器、调试器等。
  // xv6 加载程序时基本不依赖它。
  uint64 shoff;

  // 处理器相关的标志位。
  uint flags;

  // ELF Header 自身的大小。
  //
  // 对 ELF64 来说通常是 64 bytes。
  ushort ehsize;

  // 每一个 Program Header 的大小。
  //
  // xv6 的 struct proghdr 通常为 56 bytes。
  ushort phentsize;

  // Program Header 的数量。
  //
  // 例如 phnum = 2：
  //
  // Program Header 0
  // Program Header 1
  ushort phnum;

  // 每一个 Section Header 的大小。
  ushort shentsize;

  // Section Header 的数量。
  ushort shnum;

  // Section Header String Table 所在的 Section
  // 在 Section Header Table 中的索引。
  //
  // 主要用于找到 .text、.data 等 section 的名字。
  ushort shstrndx;
};


// ========================================
// Program Header
// 描述“一个需要怎样加载到内存中的 segment”
// ========================================
struct proghdr {

  // Segment 类型。
  //
  // 最重要的是：
  // ELF_PROG_LOAD = 1
  //
  // 表示这一段需要被加载到内存。
  uint32 type;

  // Segment 的访问权限。
  //
  // bit 0 = EXEC
  // bit 1 = WRITE
  // bit 2 = READ
  //
  // 例如：
  //   5 = 101 = READ + EXEC
  //   6 = 110 = READ + WRITE
  uint32 flags;

  // 这一段数据在 ELF 文件中的起始偏移。
  //
  // 例如：
  // off = 0x1000
  //
  // 表示：
  // 从 ELF 文件第 0x1000 字节开始，
  // 就是这一段的数据。
  uint64 off;

  // 这一段加载到进程地址空间后的
  // 起始“虚拟地址”。
  //
  // off 是文件中的位置，
  // vaddr 是内存中的位置。
  uint64 vaddr;

  // 物理地址。
  //
  // 现代操作系统通常不依赖这个字段。
  // xv6 加载 ELF 时也基本忽略它。
  uint64 paddr;

  // 这一段在 ELF 文件中实际存在的数据大小。
  //
  // 即：
  // 从 off 开始，从文件中需要读取多少字节。
  uint64 filesz;

  // 这一段装载到内存后最终需要占用的大小。
  //
  // memsz >= filesz
  //
  // 如果：
  // memsz > filesz
  //
  // 那么多出来的部分不在 ELF 文件中，
  // 加载器需要自己分配内存并清零。
  //
  // 这通常对应 .bss 等内容。
  uint64 memsz;

  // 这一段的内存对齐要求。
  //
  // 常见的是 4096，也就是 4 KiB 页对齐。
  uint64 align;
};


// ========================================
// Program Header 的 type
// ========================================

// PT_LOAD
// 表示这个 segment 是需要加载到内存中的。
#define ELF_PROG_LOAD 1


// ========================================
// Program Header 的 flags
// ========================================

// 可执行
#define ELF_PROG_FLAG_EXEC  1

// 可写
#define ELF_PROG_FLAG_WRITE 2

// 可读
#define ELF_PROG_FLAG_READ  4