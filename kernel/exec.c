#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

static int loadseg(pde_t *, uint64, struct inode *, uint, uint);

// map ELF permissions to PTE permission bits.
int flags2perm(int flags)
{
    int perm = 0;
    if(flags & 0x1)
      perm = PTE_X;
    if(flags & 0x2)
      perm |= PTE_W;
    return perm;
}

//
// the implementation of the exec() system call
//
int
kexec(char *path, char **argv)
{
  char *s, *last;

  // i：遍历 Program Header Table 时使用的下标
  // off：当前 Program Header 在 ELF 文件中的偏移
  int i, off;

  // argc：参数个数
  // sz：当前已经分配到用户地址空间的最高地址
  // sp：用户栈指针（stack pointer）
  // ustack[]：临时保存 argv[i] 字符串在“用户虚拟地址空间”中的地址
  // stackbase：用户栈的最低地址，用来检查栈是否越界
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;

  // ELF 文件头
  struct elfhdr elf;

  // 可执行文件对应的 inode
  struct inode *ip;

  // 当前 Program Header
  // 一个 ELF 文件通常有多个 Program Header，
  // 每一个描述一个需要如何加载到内存中的 segment。
  struct proghdr ph;

  // 新进程的页表
  pagetable_t pagetable = 0;

  // 进程原来的页表，最后成功提交新用户地址空间时释放
  pagetable_t oldpagetable;

  // 获取当前进程
  struct proc *p = myproc();


  // ============================================================
  // 一、打开 ELF 可执行文件
  // ============================================================

  // 开启文件系统操作事务。
  // xv6 的文件系统操作通常需要 begin_op()/end_op() 包围。
  begin_op();

  // 根据路径找到对应的 inode。
  //
  // 例如：
  // path = "/bin/sh"
  //
  // namei() 会找到这个文件对应的 inode。
  if((ip = namei(path)) == 0){
    // 找不到文件
    end_op();
    return -1;
  }

  // 给 inode 加锁，后面要读取文件内容。
  ilock(ip);


  // ============================================================
  // 二、读取并检查 ELF Header
  // ============================================================

  // 从文件 offset = 0 的位置开始，
  // 读取 sizeof(elf) 个字节到内存中的 elf 结构体。
  //
  // ELF 文件最开始就是 ELF Header。
  //
  // readi(ip, 0, ...)
  //   第一个 0：表示从 inode 的文件内容读取
  //   第二个 0：表示文件偏移量 offset = 0
  //
  // 读取成功后：
  //     elf.magic
  //     elf.entry
  //     elf.phoff
  //     elf.phnum
  //     ...
  // 都已经被填充。
  if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;

  // 检查 ELF 魔数。
  //
  // ELF 文件最开始必须是：
  //     0x7F 'E' 'L' 'F'
  //
  // 如果不是，则这个文件不是合法 ELF。
  if(elf.magic != ELF_MAGIC)
    goto bad;


  // ============================================================
  // 三、创建一个新的用户页表
  // ============================================================

  // 为当前进程创建一张“新的用户页表”。
  //
  // 注意：
  // 此时 p->pagetable 还是旧页表。
  //
  // 我们先把新程序加载到新页表 pagetable 中，
  // 等全部加载成功后，最后才：
  //
  //     p->pagetable = pagetable;
  //
  // 这样可以避免加载失败时破坏原来的进程地址空间。
  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;


  // ============================================================
  // 四、加载 ELF 中的 Program Header
  // ============================================================

  // elf.phoff：
  //     Program Header Table 在 ELF 文件中的起始偏移
  //
  // elf.phnum：
  //     Program Header 一共有多少个
  //
  // sizeof(ph)：
  //     一个 Program Header 的大小
  //
  // 因此：
  //
  //     第 0 个 Program Header：
  //     off = elf.phoff
  //
  //     第 1 个 Program Header：
  //     off = elf.phoff + sizeof(ph)
  //
  //     第 2 个：
  //     off = elf.phoff + 2*sizeof(ph)
  //
  // ...
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){

    // 从 ELF 文件中读取当前 Program Header。
    //
    // off 表示：
    // 当前 Program Header 在 ELF 文件中的位置。
    if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;


    // ------------------------------------------------------------
    // 不是 PT_LOAD 就跳过
    // ------------------------------------------------------------

    // ELF 中并不是每个 Program Header 都表示需要加载的程序数据。
    //
    // PT_LOAD（ELF_PROG_LOAD）表示：
    //
    //     “这一段数据需要被加载到进程内存中”
    //
    // xv6 真正关心的主要就是这些。
    if(ph.type != ELF_PROG_LOAD)
      continue;


    // ------------------------------------------------------------
    // 检查 filesz 和 memsz
    // ------------------------------------------------------------

    // filesz：
    //     这一段在 ELF 文件中实际存在的数据大小
    //
    // memsz：
    //     这一段加载到内存以后最终需要占用的大小
    //
    // 正常情况必须满足：
    //
    //     memsz >= filesz
    //
    // 因为：
    //
    //     文件中的数据
    //          +
    //     文件中不存在、需要额外分配并清零的部分
    //          =
    //     内存中的完整 segment
    //
    // 后面多出来的部分典型就是 .bss。
    if(ph.memsz < ph.filesz)
      goto bad;


    // ------------------------------------------------------------
    // 检查地址计算是否发生整数溢出
    // ------------------------------------------------------------

    // 需要把 segment 放到：
    //
    //     [ph.vaddr, ph.vaddr + ph.memsz)
    //
    // 如果加法发生 uint64 溢出：
    //
    //     ph.vaddr + ph.memsz < ph.vaddr
    //
    // 就说明地址已经绕回去了，这是非法 ELF。
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;


    // ------------------------------------------------------------
    // 要求 segment 起始虚拟地址页对齐
    // ------------------------------------------------------------

    // xv6 页面大小：
    //
    //     PGSIZE = 4096
    //
    // 因此正常情况下 segment 的起始虚拟地址
    // 必须按照页边界对齐。
    //
    // 例如：
    //
    //     0x1000  √
    //     0x2000  √
    //     0x3000  √
    //
    //     0x1234  ×
    //
    if(ph.vaddr % PGSIZE != 0)
      goto bad;


    // ============================================================
    // 为这个 segment 分配虚拟内存
    // ============================================================

    uint64 sz1;

    // uvmalloc()：
    //
    //     从当前 sz 开始，
    //     一直分配到 ph.vaddr + ph.memsz
    //
    // 注意这里的核心思想：
    //
    //     ELF 的 vaddr 决定“这段程序应该出现在用户虚拟地址空间的哪里”
    //
    // 例如：
    //
    //     ph.vaddr  = 0x1000
    //     ph.memsz  = 0x2000
    //
    // 那么就需要保证：
    //
    //     [0x1000, 0x3000)
    //
    // 这一段用户虚拟地址已经存在。
    //
    // flags2perm(ph.flags)：
    //     把 ELF 的 R/W/X 权限
    //     转换成 xv6 页表需要的 PTE_R/PTE_W/PTE_X 权限。
    //
    // 例如：
    //
    //     ELF: READ + EXEC
    //          ↓
    //     PTE_R | PTE_X
    //
    if((sz1 = uvmalloc(
            pagetable,
            sz,
            ph.vaddr + ph.memsz,
            flags2perm(ph.flags))) == 0)
      goto bad;

    // uvmalloc() 成功以后，
    // sz1 表示当前用户地址空间已经扩展到哪里。
    //
    // 更新 sz。
    sz = sz1;


    // ============================================================
    // 把 ELF 文件中的实际数据复制到刚才分配的内存中
    // ============================================================

    // loadseg() 的核心任务：
    //
    //     从 ELF 文件读取 ph.filesz 个字节
    //     从 ph.off 开始读取
    //     写入用户虚拟地址 ph.vaddr
    //
    // 所以这里体现了两个非常重要的映射：
    //
    //     ph.off
    //         ↓
    //     ELF 文件中的位置
    //
    //     ph.vaddr
    //         ↓
    //     用户地址空间中的位置
    //
    // 即：
    //
    //     文件中的数据
    //          ↓ loadseg()
    //     用户虚拟内存
    //
    // 注意：
    //
    //     loadseg() 只需要读取 ph.filesz
    //     而 memsz - filesz 的部分不来自文件，
    //     由 uvmalloc() 分配后保持为 0。
    if(loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }


  // ============================================================
  // 五、ELF 的代码和数据已经全部加载完成
  // ============================================================

  // Program Header 已经全部处理完，
  // 不再需要 inode 锁。
  iunlockput(ip);

  // 结束文件系统操作事务。
  end_op();

  // 防止后面的错误处理再次操作已经释放的 inode。
  ip = 0;


  // ============================================================
  // 六、准备用户栈
  // ============================================================

  // 再次取得当前进程。
  //
  // 这里实际上 p 本来还是当前进程，
  // 重新 myproc() 主要是为了明确当前进程对象。
  p = myproc();


  // 保存旧用户地址空间的大小。
  //
  // 注意：
  // p->pagetable 仍然是“旧页表”，
  // p->sz 也仍然是旧程序的大小。
  //
  // 后面成功提交新地址空间时：
  //
  //     p->pagetable = pagetable;
  //     p->sz = sz;
  //
  // 然后：
  //
  //     proc_freepagetable(oldpagetable, oldsz);
  //
  // 释放旧程序。
  uint64 oldsz = p->sz;


  // ------------------------------------------------------------
  // 为用户栈分配空间
  // ------------------------------------------------------------

  // 把 sz 向上取整到下一个页边界。
  //
  // 例如：
  //
  //     sz = 0x2345
  //
  // 变成：
  //
  //     sz = 0x3000
  //
  // 这样用户栈从完整的页边界开始。
  sz = PGROUNDUP(sz);


  uint64 sz1;

  // 一次分配：
  //
  //     (USERSTACK + 1) 个页面
  //
  // 其中：
  //
  //     最下面 1 页：
  //     Stack Guard Page
  //
  //     上面的 USERSTACK 页：
  //     真正的 User Stack
  //
  // 例如：
  //
  //          高地址
  //     ┌──────────────────┐
  //     │     User Stack   │
  //     │                  │
  //     ├──────────────────┤
  //     │     User Stack   │
  //     ├──────────────────┤
  //     │     User Stack   │
  //     ├──────────────────┤
  //     │   Guard Page     │ ← 不允许访问
  //     └──────────────────┘
  //          低地址
  //
  // PTE_W：
  //     栈需要可写。
  if((sz1 = uvmalloc(
          pagetable,
          sz,
          sz + (USERSTACK+1)*PGSIZE,
          PTE_W)) == 0)
    goto bad;

  // 更新整个用户地址空间的大小。
  sz = sz1;


  // ------------------------------------------------------------
  // 把第一张栈页设置为不可访问
  // ------------------------------------------------------------

  // uvmclear()：
  //     清除这个页面的用户访问权限。
  //
  // 它就是 Stack Guard Page。
  //
  // 作用：
  //     如果用户栈向下增长越过正常边界，
  //     就会访问到这一页，
  //     从而触发页错误，而不是悄悄覆盖其他内存。
  uvmclear(pagetable, sz-(USERSTACK+1)*PGSIZE);


  // ------------------------------------------------------------
  // 初始化栈指针
  // ------------------------------------------------------------

  // sp 指向用户栈最高地址。
  //
  // RISC-V 中栈通常向低地址增长：
  //
  //     push 数据
  //         ↓
  //     sp -= ...
  //
  sp = sz;

  // stackbase 表示真正用户栈区域的最低地址。
  //
  // 注意：
  // 最下面还有一页 guard page，
  // 所以这里从：
  //
  //     sz - USERSTACK * PGSIZE
  //
  // 开始才是真正可以使用的栈。
  stackbase = sp - USERSTACK*PGSIZE;


  // ============================================================
  // 七、把 argv[] 中的字符串复制到用户栈
  // ============================================================

  // argv 是内核传入的：
  //
  //     char **argv
  //
  // 例如：
  //
  //     argv[0] = "echo"
  //     argv[1] = "hello"
  //     argv[2] = "world"
  //     argv[3] = 0
  //
  // 这里要把这些字符串真正复制到：
  //
  //     新程序的用户栈
  //
  // 中。
  for(argc = 0; argv[argc]; argc++) {

    // xv6 限制最多 MAXARG 个参数。
    if(argc >= MAXARG)
      goto bad;


    // ------------------------------------------------------------
    // 给字符串分配栈空间
    // ------------------------------------------------------------

    // strlen(argv[argc]) + 1：
    //
    // +1 是为了保存字符串结尾的 '\0'。
    //
    // 栈向低地址增长，所以：
    //
    //     sp -= 字符串大小
    //
    // 相当于在栈上“开辟空间”。
    sp -= strlen(argv[argc]) + 1;


    // ------------------------------------------------------------
    // RISC-V 栈要求 16 字节对齐
    // ------------------------------------------------------------

    // sp -= sp % 16
    //
    // 把 sp 向下调整到 16 字节对齐的位置。
    //
    // 例如：
    //
    //     sp = 0x7ff8
    //
    //     sp % 16 = 8
    //
    // 调整之后：
    //
    //     sp = 0x7ff0
    //
    sp -= sp % 16;


    // 如果已经低于 stackbase，
    // 说明用户栈空间不够。
    if(sp < stackbase)
      goto bad;


    // ------------------------------------------------------------
    // 把字符串从内核地址复制到用户地址
    // ------------------------------------------------------------

    // argv[argc] 是当前进程内核中的字符串地址。
    //
    // sp 是新用户地址空间中的虚拟地址。
    //
    // copyout()：
    //
    //     kernel memory
    //          ↓
    //     pagetable 翻译
    //          ↓
    //     user virtual memory
    //
    // 最后用户程序看到的就是这个字符串。
    if(copyout(
          pagetable,
          sp,
          argv[argc],
          strlen(argv[argc]) + 1) < 0)
      goto bad;


    // 保存这个字符串在“用户空间”的地址。
    //
    // 注意：
    //
    //     ustack[argc] 存的不是内核地址，
    //     而是用户程序未来能访问到的虚拟地址。
    //
    // 例如：
    //
    //     argv[0] = "echo"
    //
    // 最终可能：
    //
    //     ustack[0] = 0x7ffffff0
    //
    ustack[argc] = sp;
  }


  // argv[] 最后必须有 NULL：
  //
  //     argv[argc] = 0
  //
  // 这样用户程序才能知道参数数组在哪里结束。
  ustack[argc] = 0;


  // ============================================================
  // 八、把 argv[] 指针数组本身也放进用户栈
  // ============================================================

  // 前面只是把：
  //
  //     "echo"
  //     "hello"
  //     "world"
  //
  // 这些字符串复制到了用户栈。
  //
  // 现在还需要构造：
  //
  //     argv
  //
  // 也就是一个“指向这些字符串的指针数组”。
  //
  // 逻辑上最终应该变成：
  //
  //     argv
  //       ↓
  //     ┌────────────┐
  //     │ 0x....     │ ─────→ "echo"
  //     │ 0x....     │ ─────→ "hello"
  //     │ 0x....     │ ─────→ "world"
  //     │ 0          │
  //     └────────────┘
  //
  // 每个指针占 sizeof(uint64) = 8 字节。
  //
  // argc+1：
  //     +1 是为了最后的 NULL 指针。
  sp -= (argc+1) * sizeof(uint64);


  // 同样进行 16 字节对齐。
  sp -= sp % 16;


  // 检查栈是否越界。
  if(sp < stackbase)
    goto bad;


  // 把内核中的 ustack[] 数组复制到新用户空间的 sp。
  //
  // 注意：
  // 此时复制进去的是“一堆用户虚拟地址”。
  //
  // 用户程序以后看到的：
  //
  //     argv[0]
  //     argv[1]
  //     ...
  //
  // 就是这些地址。
  if(copyout(
        pagetable,
        sp,
        (char *)ustack,
        (argc+1)*sizeof(uint64)) < 0)
    goto bad;


  // ============================================================
  // 九、设置用户程序启动时的寄存器
  // ============================================================

  // RISC-V ABI 中：
  //
  //     a0 = 第一个参数
  //     a1 = 第二个参数
  //
  // 用户程序最终希望调用：
  //
  //     main(argc, argv)
  //
  // 因此：
  //
  //     a0 = argc
  //     a1 = argv
  //
  // 但是这里没有直接：
  //
  //     p->trapframe->a0 = argc
  //
  // 因为 kexec() 本身是通过系统调用 exec 进入内核的。
  //
  // kexec() 的返回值：
  //
  //     return argc;
  //
  // 最后会通过系统调用机制自动放进：
  //
  //     a0
  //
  // 所以这里手动设置的只有：
  //
  //     a1 = argv
  //
  p->trapframe->a1 = sp;


  // ============================================================
  // 十、保存程序名字
  // ============================================================

  // path 可能是：
  //
  //     "/bin/echo"
  //
  // 这里希望保存：
  //
  //     "echo"
  //
  // 而不是：
  //
  //     "/bin/echo"
  //
  // last 最终指向最后一个 '/' 后面的字符。
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;

  // 把程序名复制到：
  //
  //     p->name
  //
  // 主要用于调试、进程列表显示等。
  safestrcpy(p->name, last, sizeof(p->name));


  // ============================================================
  // 十一、正式提交新的用户地址空间
  // ============================================================

  // 保存旧页表。
  //
  // 因为下面马上要让：
  //
  //     p->pagetable
  //
  // 指向新页表。
  oldpagetable = p->pagetable;


  // ------------------------------------------------------------
  // 从这里开始，正式切换到新程序
  // ------------------------------------------------------------

  // 当前进程开始使用刚才构造好的新页表。
  p->pagetable = pagetable;

  // 更新进程用户地址空间大小。
  p->sz = sz;


  // ------------------------------------------------------------
  // 设置程序入口点
  // ------------------------------------------------------------

  // ELF Header 中的 elf.entry：
  //
  //     程序入口虚拟地址
  //
  // 例如：
  //
  //     elf.entry = 0x1000
  //
  // 那么 CPU 恢复到用户态以后：
  //
  //     PC = 0x1000
  //
  // 就从这个地址开始执行程序。
  //
  // 这里用 trapframe 保存，是因为从内核返回用户态时，
  // CPU 会根据 trapframe 恢复用户寄存器。
  p->trapframe->epc = elf.entry;


  // ------------------------------------------------------------
  // 设置用户栈指针
  // ------------------------------------------------------------

  // sp 是我们刚才构造好的 argv 数组所在位置。
  //
  // 用户程序开始运行以后：
  //
  //     SP = sp
  //
  // 所以用户程序可以从栈上继续执行。
  p->trapframe->sp = sp;


  // ------------------------------------------------------------
  // 新地址空间已经完全准备好
  // ------------------------------------------------------------

  // 旧程序已经没有用了。
  //
  // 现在释放旧页表以及旧用户内存。
  proc_freepagetable(oldpagetable, oldsz);


  // ============================================================
  // 十二、成功
  // ============================================================

  // kexec() 最终通过系统调用返回值把 argc 放进 a0。
  //
  // 因此用户态最终看到：
  //
  //     a0 = argc
  //     a1 = argv
  //
  // 然后程序入口代码就可以调用：
  //
  //     main(argc, argv)
  //
  return argc;


  // ============================================================
  // 加载失败
  // ============================================================
bad:

  // 如果已经创建了新的页表，
  // 就释放这个“不完整的新用户地址空间”。
  //
  // 注意：
  // 这里释放的是 pagetable，
  // 不是 p->pagetable。
  //
  // 因为在成功提交之前：
  //
  //     p->pagetable
  //
  // 仍然是旧页表。
  if(pagetable)
    proc_freepagetable(pagetable, sz);


  // 如果 inode 还没有被释放，
  // 就解锁并释放它，然后结束文件系统事务。
  if(ip){
    iunlockput(ip);
    end_op();
  }


  // exec 失败
  return -1;
}

// Load an ELF program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
static int
loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz)
{
  uint i, n;
  uint64 pa;

  for(i = 0; i < sz; i += PGSIZE){
    pa = walkaddr(pagetable, va + i);
    if(pa == 0)
      panic("loadseg: address should exist");
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, 0, (uint64)pa, offset+i, n) != n)
      return -1;
  }
  
  return 0;
}
