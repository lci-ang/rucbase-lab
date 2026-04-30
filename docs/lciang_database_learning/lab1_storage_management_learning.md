# RucBase Lab1 学习笔记：存储管理

这份笔记不是实验答案，而是一份“看懂 Lab1 的路线图”。如果你现在看文档还是有点乱，先记住一句话：

**Lab1 的核心，是把磁盘上的页安全、高效地搬到内存里，再在一页内部把记录管理好。**

你可以把它理解成两层问题：

1. 页怎么从磁盘进出内存。
2. 一页里面的记录怎么放、怎么找、怎么删、怎么遍历。

---

## 1. 先建立整体心智模型

### 1.1 四个角色分别做什么

- DiskManager：只负责磁盘文件和页的读写、分配、删除。
- Replacer：只负责“内存不够时先淘汰谁”。
- BufferPoolManager：负责把磁盘页和内存页串起来。
- Record Manager：负责一页里面的记录布局和记录操作。

如果把数据库存储系统比作一个图书馆：

- DiskManager 是书库管理员，知道书放在哪个柜子里。
- BufferPoolManager 是你的桌面，常用书先放桌上。
- Replacer 是桌面不够时的收纳规则。
- Record Manager 是一本书里每一页怎么排版。

### 1.2 为什么要有缓冲池

磁盘慢，内存快。数据库不可能每次都直接读磁盘，否则性能会很差。

所以系统会把磁盘上的页缓存在内存里：

- 经常访问的页留在缓冲池。
- 不常用的页在缓冲池满时被淘汰。
- 修改过的页在被踢出去之前要先写回磁盘。

这就是 Lab1 的大方向。

### 1.3 关键概念先记住

- Page：磁盘和内存交互的基本单位，大小固定，通常是 4KB。
- Frame：缓冲池里的一块内存空间，大小和 Page 一样。
- PageId：页面的唯一标识，里面包含 fd 和 page_no。
- pin_count：当前页被“固定”住的次数，只要大于 0，就不能被淘汰。
- dirty：脏页，表示这页在内存里被修改过，必须写回磁盘。
- bitmap：用 bit 表示某个 slot 有没有放记录。
- RID：一条记录的位置，通常由 page_no + slot_no 组成。

---

## 2. Lab1 的数据流

你可以把一次记录操作想成下面这条链路：

```text
SQL/上层操作
    -> Record Manager
    -> BufferPoolManager
    -> DiskManager
    -> 磁盘文件
```

更细一点看：

- 读记录时，先看看目标页是否已经在缓冲池里。
- 如果在，就直接返回内存里的页。
- 如果不在，就找一个空帧或者淘汰一个旧页，再从磁盘读进来。
- 写记录时，先改内存页，再把 dirty 标记起来。
- 需要刷盘时，再由 BufferPoolManager 调用 DiskManager 写回磁盘。

所以 Lab1 不是在做“把数据写进磁盘”这么简单，而是在做“如何管理磁盘页和内存页之间的生命周期”。

---

## 3. 任务 1.1：DiskManager

### 3.1 它的作用是什么

DiskManager 负责最底层的磁盘 I/O：

- 读页
- 写页
- 分配页号
- 创建、打开、关闭、删除文件

它不关心一页里存的是什么，只关心“从哪个文件、哪个页号、哪个偏移开始读写多少字节”。

### 3.2 为什么要这样设计

把磁盘操作单独封装起来有两个好处：

- 上层不用直接碰系统调用，逻辑更清楚。
- 后面无论是缓冲池还是记录管理，都可以统一通过它访问文件。

### 3.3 页偏移怎么算

一个页在文件中的偏移通常是：

```text
offset = page_no * PAGE_SIZE
```

举个例子：

- page_no = 3
- PAGE_SIZE = 4096
- offset = 12288

意思就是：第 3 号页从文件的第 12288 个字节开始。

### 3.4 读写页面时要注意什么

Lab1 里的读写接口支持按字节数读写，不一定非得整页都读完：

- 读整页：常见于 fetch page。
- 只读页头：常见于读取文件头。
- 只写一部分：也可能用于局部更新。

所以实现时不要把 num_bytes 写死成 PAGE_SIZE，而要尊重上层传进来的长度。

### 3.5 文件操作的核心状态

从头文件可以看出，DiskManager 维护了两类映射：

- path -> fd：一个路径是否已经打开。
- fd -> path：一个文件句柄对应哪个路径。

还有一个很重要的状态：

- fd2pageno_[fd]：这个文件已经分配到哪个页号了。

这个变量很关键，因为 allocate_page 不是“随机找一个号”，而是简单地按文件内的当前分配数递增。

### 3.6 这一部分最容易错的点

- 没有检查文件是否已经存在就重复创建。
- 没有检查文件是否已经打开就重复打开。
- 关闭后没有更新打开列表。
- 删除文件时文件还开着。
- 读写页时没有正确定位到页偏移。

### 3.7 你可以先这样理解实现

```text
write_page:
  先把 fd 对应的文件偏移定位到 page_no 的起始位置
  再写入 num_bytes 字节

read_page:
  先定位到 page_no 的起始位置
  再读出 num_bytes 字节

allocate_page:
  取出当前页号，然后自增
```

---

## 4. 任务 1.2：Replacer 和 LRU

### 4.1 它解决什么问题

缓冲池里的帧是有限的。如果没有空帧，必须决定淘汰哪个页。

Replacer 就是做这个决定的。

### 4.2 为什么用 LRU

LRU 的核心假设是：

**最近被访问过的页，接下来很可能还会被访问。**

这叫局部性原理。

数据库里这种现象很常见：

- 事务会反复访问某些热页。
- 索引遍历和记录扫描会在局部区域集中访问。

所以 LRU 很适合作为教学实验中的替换策略。

### 4.3 pin 和 unpin 到底在干什么

这个地方是很多人最容易混淆的。

- pin：表示这页正在被使用，不能淘汰。
- unpin：表示这页不再被使用了，可以进入候选淘汰集合。

可以把 pin_count 理解成“正在拿着这本书的人数”：

- 人数大于 0，不能把书收回去。
- 人数变成 0，才允许 Replacer 考虑它。

### 4.4 LRU 一般怎么实现

从代码结构看，LRUReplacer 通常会用两种结构配合：

- 一个 list 记录帧的访问顺序。
- 一个 hash 表快速定位某个 frame 是否在 list 里。

这样做的原因很直接：

- list 方便在头尾插删。
- hash 表方便 O(1) 找到一个帧并删除。

### 4.5 你要特别记住的顺序

结合头文件里的注释，通常可以这样理解：

- 最近被 unpin 的帧放在 LRUlist 的前面。
- 最久没被用的帧在后面。
- victim 时从后面淘汰。

这个顺序本质上就是“谁最久没被用，谁先走”。

### 4.6 为什么 Replacer 要加锁

缓冲池是共享资源，多个线程可能同时 pin、unpin、victim。

如果不加锁，可能出现：

- 一个线程刚把帧移出 list，另一个线程又来访问它。
- 两个线程同时淘汰同一帧。

所以每个接口都需要原子化处理。

---

## 5. 任务 1.3：BufferPoolManager

### 5.1 它是整个 Lab1 的核心

如果只学一个类，最该学的就是 BufferPoolManager。

因为它把前两部分串起来了：

- DiskManager 负责和磁盘打交道。
- Replacer 负责挑淘汰对象。
- BufferPoolManager 负责协调两者。

### 5.2 它维护了什么

从头文件里可以看到它维护了几个关键结构：

- pages_：缓冲池里的 Page 数组。
- page_table_：PageId 到 frame_id 的映射。
- free_list_：空闲帧列表。
- replacer_：页满时的淘汰策略。
- latch_：并发控制锁。

你可以把它理解成：

- page_table_ 负责“找页”。
- free_list_ 负责“先用空位”。
- replacer_ 负责“实在没空位时怎么赶人”。

### 5.3 new_page 的思路

new_page 做的是“创建一页并放进缓冲池”。

典型流程是：

1. 先找空闲帧。
2. 如果没有空闲帧，就从 Replacer 里找 victim。
3. 如果被淘汰页是脏页，先写回磁盘。
4. 从 page_table_ 中删掉旧映射。
5. 向 DiskManager 申请新的 page_no。
6. 把新页放进这个 frame。
7. 更新 page_table_、pin_count、dirty 状态等元数据。

注意：新页一旦被创建，就默认被 pin 住了，因为调用者马上就要用它。

### 5.4 fetch_page 的思路

fetch_page 做的是“把已有页取进缓冲池并返回”。

典型流程是：

1. 先查 page_table_，看页是否已经在内存里。
2. 如果在，直接增加 pin_count，必要时让 Replacer pin 掉它。
3. 如果不在，先找可用帧。
4. 没空帧就找 victim，必要时写回脏页。
5. 从磁盘读出目标页。
6. 更新 page_table_ 和页元数据。

### 5.5 unpin_page 的思路

unpin_page 是“我用完了这页，放回去一点”。

它最重要的规则是：

- 一次 unpin 只减少一次 pin_count。
- 只有 pin_count 降到 0，才可以把帧交给 Replacer。

如果上层修改了页面，还要顺便把 dirty 位置为 true。

### 5.6 delete_page 的思路

删除页时要确认两件事：

- 这页在不在缓冲池里。
- 它的 pin_count 是否为 0。

只有没人在用它时才允许删除。删除之后，帧可以回到 free_list_。

### 5.7 flush_page 和 flush_all_pages

flush_page：强制把某个页写回磁盘，不管它是不是脏页，也不管它是否还被 pin 住。

flush_all_pages：把指定文件中当前在缓冲池里的所有页都刷回去。

这两个接口的意义是：

- 保证修改不会一直只停留在内存里。
- 在文件关闭、测试结束或需要持久化时很有用。

### 5.8 这里最容易错的点

- 忘了先写回脏页就直接淘汰。
- page_table_ 删除和页元数据更新不同步。
- pin_count 没有正确增减。
- 只把页从 Replacer 里删了，却没从 page_table_ 里删。
- 只更新了内存页，没更新 dirty。

### 5.9 一个非常实用的理解方式

你可以把 BufferPoolManager 的行为记成两条主线：

```text
命中：
  找到页 -> pin_count++ -> 返回

未命中：
  找空帧 / 找 victim -> 必要时刷脏页 -> 读磁盘页 -> 建立映射 -> 返回
```

只要这两条主线想明白，代码会清晰很多。

---

## 6. 任务 2：记录管理器

任务 2 建立在任务 1 的基础上。

你可以把它理解成：

- 任务 1 解决“页怎么搬运”。
- 任务 2 解决“页里面怎么放记录”。

### 6.1 文件头和页面头分别管什么

记录文件里有两个层次的元数据：

#### 文件头 RmFileHdr

- record_size：每条记录多大。
- num_pages：这个文件总共分配了多少页。
- num_records_per_page：每页最多能放多少条记录。
- first_free_page_no：当前第一个还有空位的页。
- bitmap_size：每页位图占多少字节。

#### 页面头 RmPageHdr

- next_free_page_no：当前页满了以后，下一个有空位的页是谁。
- num_records：当前页里已经放了多少条记录。

### 6.2 一页内部的布局

每个记录页的组织方式大致是：

```text
[ RmPageHdr ][ bitmap ][ slots ]
```

其中：

- bitmap 的每一位表示一个 slot 是否被占用。
- slots 区域按定长记录顺序摆放。

为什么用 bitmap？

因为定长记录最怕“这个位置有没有放东西”不好判断。bitmap 可以让你很快知道：

- 哪些 slot 空着。
- 哪些 slot 已经有记录了。

### 6.3 为什么文件头页是 page 0

在这个实验里，文件头页通常固定为 0 号页，记录页从 1 号页开始。

这样做的好处是：

- 文件元信息和数据页分开。
- 管理时更清晰。
- 扫描记录时可以直接跳过 0 号页。

### 6.4 RmFileHandle 的职责

RmFileHandle 是“一个记录文件”的操作入口。

它主要负责：

- 从磁盘读出文件头。
- 创建新页。
- 找到某一页对应的 page handle。
- 插入、删除、更新、读取记录。

### 6.5 构造函数为什么要先读文件头

因为后面的所有操作都依赖文件头里的信息，尤其是：

- record_size
- num_records_per_page
- bitmap_size
- first_free_page_no

如果不知道这些，后面根本没法正确计算 slot 偏移和空闲页链。

### 6.6 插入记录时的思路

插入一条记录时，最重要的是找一个有空位的页。

典型流程是：

1. 先看 file_hdr.first_free_page_no。
2. 如果有空闲页，就直接拿它。
3. 如果没有，就新建一个页。
4. 在页内通过 bitmap 找一个空 slot。
5. 把记录复制进去。
6. 把对应 bit 置 1。
7. 更新 page_hdr.num_records。
8. 如果页被插满了，就把它从空闲页链里移出去。

这里有一个很重要的思想：

**记录不是“挂”在页上，而是塞进页内的某个 slot 里。**

### 6.7 删除记录时的思路

删除记录本质上就是把 slot 标记成空。

典型流程是：

1. 根据 RID 找到对应页。
2. 检查 bitmap，确认这条记录确实存在。
3. 把对应 bit 置 0。
4. 更新记录个数。
5. 如果这页原来是满的，删完后变成有空位，就要重新放回空闲页链。

### 6.8 更新记录时的思路

更新通常最简单：

- 先定位 RID。
- 然后直接覆盖 slot 中的数据。

因为是定长记录，所以不需要像变长记录那样搬移一大片数据。

### 6.9 记录扫描器 RmScan

RmScan 的作用是顺序遍历文件里所有存在的记录。

它做的事情并不复杂：

1. 从当前 RID 开始。
2. 在当前页里找下一个 bit 为 1 的 slot。
3. 如果当前页没有了，就跳到下一页。
4. 如果已经到最后一页还没找到，就结束。

所以扫描器真正依赖的是 bitmap。没有 bitmap，就不知道哪些 slot 有记录。

### 6.10 这一部分最容易错的点

- 忘了更新 bitmap。
- 忘了更新 first_free_page_no。
- 忘了更新 next_free_page_no。
- 扫描时把文件头页 0 当成记录页。
- 只改了页内数据，但没更新页头里的记录数。

---

## 7. 推荐的学习顺序

如果你现在还是有点乱，建议按下面顺序看和写：

1. 先理解 Page、Frame、PageId、pin_count、dirty。
2. 先做 DiskManager，搞清楚页号和文件偏移的关系。
3. 再做 Replacer，理解 LRU 的“谁最久没用谁先走”。
4. 然后做 BufferPoolManager，把前两者串起来。
5. 再做 RmFileHandle，理解“页内部如何放记录”。
6. 最后做 RmScan，理解顺序遍历如何依赖 bitmap。

这个顺序非常重要，因为后面的内容都建立在前面的概念之上。

---

## 8. 建议你边写边验证的测试顺序

Lab1 的测试顺序也建议按依赖来：

```bash
cd build
make disk_manager_test
./bin/disk_manager_test

make lru_replacer_test
./bin/lru_replacer_test

make buffer_pool_manager_test
./bin/buffer_pool_manager_test

make record_manager_test
./bin/record_manager_test
```

这个顺序不是随便排的，而是因为：

- DiskManager 是最底层。
- Replacer 依赖页的固定管理。
- BufferPoolManager 依赖前两者。
- Record Manager 依赖 BufferPoolManager。

如果最底层没过，后面会一串都错，排查会非常痛苦。

---

## 9. 一张“速记卡”

如果你只想临时记住最关键的几句话，可以背下面这 8 句：

1. DiskManager 只管文件和页的读写，不管页里存什么。
2. BufferPoolManager 只管页在内存和磁盘之间怎么搬。
3. Replacer 只管缓冲池满了以后淘汰谁。
4. pin_count 大于 0 的页不能淘汰。
5. dirty 页被淘汰前必须先写回磁盘。
6. Record Manager 只管一页里如何组织记录。
7. bitmap 决定哪个 slot 有记录。
8. 扫描器靠 bitmap 和 RID 一页一页往后找。

---

## 10. 你接下来最适合怎么学

如果你愿意继续往下学，我建议你下一步不要直接闷头写代码，而是先做这三件事：

1. 画一张 Lab1 的数据流图，把 DiskManager、Replacer、BufferPoolManager、RmFileHandle 画出来。
2. 自己手写一个“插入记录”的流程图，标出 page header、bitmap、slot 的变化。
3. 先把 disk_manager_test 和 lru_replacer_test 跑通，再进入 buffer_pool_manager_test。

这样你会更容易知道自己到底是在修“磁盘文件”、还是在修“缓存逻辑”、还是在修“页内布局”。
