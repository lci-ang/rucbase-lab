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

---

<!--
## 11. 一步一步完成 Lab1 的实战路线

如果你的目标不是“看懂”，而是“真的把 Lab1 做完”，就按下面顺序走。不要跳步。

### 第 0 步：先确认环境和目录

你先确认三件事：

1. 能正常进入 `build` 目录。
2. `src/storage`、`src/replacer`、`src/record` 这些目录都在。
3. 你知道测试对应哪个模块。

先不用写代码，先建立一个最小地图：

- 磁盘相关看 `src/storage/disk_manager.*`
- 缓冲池相关看 `src/storage/buffer_pool_manager.*`
- LRU 相关看 `src/replacer/lru_replacer.*`
- 记录相关看 `src/record/rm_file_handle.*` 和 `src/record/rm_scan.*`

### 第 1 步：先做 DiskManager

这一步的目标只有一个：让文件和页的读写先正确。

你要先把这些函数做完：

- `write_page`
- `read_page`
- `create_file`
- `destroy_file`
- `open_file`
- `close_file`

然后再做：

- `is_file`
- `allocate_page`

这一阶段你要验证的重点是：

- 页偏移是否算对。
- 文件能否重复打开、重复创建、错误关闭时正确报错。
- 页号是否按文件自增。

完成后先跑 `disk_manager_test`，不要急着去做后面的模块。

### 第 2 步：再做 LRUReplacer

这一步的目标是让“谁该被淘汰”这个规则正确。

你要实现：

- `victim`
- `pin`
- `unpin`
- `Size`

你先只关心一件事：

**一个 frame 被 unpin 之后，能不能按最近最少使用顺序被淘汰。**

这一阶段的检查点是：

- unpin 后是否进入候选集合。
- pin 后是否从候选集合移除。
- victim 是否总是挑最久没用的那个。

完成后跑 `lru_replacer_test`。

### 第 3 步：再做 BufferPoolManager 的骨架

这一步不要一口气全做完，先把“找帧”和“换页”理顺。

先实现这两个辅助函数：

- `find_victim_page`
- `update_page`

它们是 BufferPoolManager 的地基。

这里你要先想明白三个问题：

1. 缓冲池有没有空闲帧。
2. 没空闲帧时能不能从 replacer 拿 victim。
3. victim 是脏页时要不要先刷盘。

先把这一步做对，再做 public 接口。

### 第 4 步：再做 BufferPoolManager 的 public 接口

按这个顺序来写最稳：

1. `fetch_page`
2. `unpin_page`
3. `flush_page`
4. `new_page`
5. `delete_page`
6. `flush_all_pages`

为什么这么排：

- `fetch_page` 最能帮你验证 page table、free list、replacer 这三者是否联动正确。
- `unpin_page` 决定页什么时候能进入淘汰候选集合。
- `flush_page` 能帮你检查脏页落盘逻辑。
- `new_page` 和 `delete_page` 依赖前面的基本链路。
- `flush_all_pages` 是最后的批量收尾逻辑。

这一阶段的检查点是：

- 同一页重复 fetch 时是否命中缓冲池。
- 页被 unpin 到 0 后是否进入 replacer。
- 脏页淘汰前是否真的写回磁盘。
- 删除页时是否只允许 pin_count 为 0 的页被删掉。

完成后跑 `buffer_pool_manager_test`。

### 第 5 步：再做 RmFileHandle

这一步开始进入“页里面怎么放记录”。

建议顺序是：

1. `fetch_page_handle`
2. `create_new_page_handle`
3. `create_page_handle`
4. `release_page_handle`
5. `get_record`
6. `insert_record`
7. `delete_record`
8. `update_record`

为什么这样排：

- 前四个是页级基础设施。
- 后四个才是真正的记录操作。

你要盯住的核心不变量是：

- `bitmap` 反映 slot 是否占用。
- `num_records` 反映当前页里有多少条记录。
- `first_free_page_no` 反映文件里第一个有空位的页。
- `next_free_page_no` 负责把空闲页串起来。

完成后跑 `record_manager_test` 里的记录操作相关部分。

### 第 6 步：最后做 RmScan

扫描器是收尾部分，通常比插入删除简单。

你要实现：

- 构造函数
- `next`
- `is_end`
- `rid`

它的思路很简单：

- 在当前页里找下一个 bit 为 1 的 slot。
- 找不到就切到下一页。
- 一直扫到文件尾。

这一阶段的检查点是：

- 是否能跳过空 slot。
- 是否能跨页继续往后找。
- 是否能在文件末尾正确停止。

### 第 7 步：按测试倒推修 bug

如果测试没过，不要一下子重写一大片，先按下面的顺序查：

1. 先看是不是最底层没写对，比如 DiskManager。
2. 再看 LRU 是否和 pin/unpin 逻辑不一致。
3. 再看 BufferPoolManager 是否把 dirty、page table、free list 三者同步好了。
4. 最后再看记录页的 bitmap 和空闲页链。

这样排查效率最高，因为上层错很多时候其实是下层错了。

### 第 8 步：你可以用一个最小闭环检查自己

如果你想知道自己现在做到哪一步，可以用这个最小闭环判断：

1. 能创建文件。
2. 能在文件里分配页号。
3. 能把页读写进磁盘。
4. 能把页搬进缓冲池。
5. 能淘汰并刷回脏页。
6. 能在页里插入、删除、读取记录。
7. 能顺序扫描所有记录。

只要这 7 件事都通了，Lab1 基本就完成了。

### 第 9 步：推荐的最终完成节奏

如果你想要一个最稳的节奏，就按下面来：

1. 先用 1 天左右把 DiskManager 和 LRUReplacer 做完并跑通。
2. 再用 1 到 2 天把 BufferPoolManager 做完并跑通。
3. 再用 1 到 2 天把 RmFileHandle 做完。
4. 最后用半天到 1 天把 RmScan 补完并回归测试。

如果你卡住了，通常不是因为“整个实验不会做”，而是因为某一步的状态没有想清楚。优先回到这一节，按步骤往回查。

---

-->

## 11. 按步骤完成 Lab1 的实战路线

下面这部分不是理论，而是你可以照着做的完成顺序。每一步都尽量做到“学一点、写一点、测一点”。

### 第 0 步：先把环境和测试跑起来

目标不是写代码，而是确认你知道怎么验证结果。

你先做这几件事：

1. 进入 build 目录。
2. 确认能正常编译。
3. 先跑最底层测试，看看当前哪些是过的，哪些是失败的。

建议顺序：

```bash
cd build
make disk_manager_test
./bin/disk_manager_test
```

这一部的意义是：你先知道测试长什么样，后面每实现完一小块，都能立刻验证。

### 第 1 步：先彻底理解 Page 和 PageId

在动手前，先搞清楚这三个东西：

1. Page 是什么。
2. Frame 是什么。
3. PageId 为什么同时带 fd 和 page_no。

你要想明白一件事：

**数据库里不是只认 page_no，还要知道这个 page_no 属于哪个文件。**

这一步不需要写代码，但你要能回答：

- 一个页面在文件中的偏移怎么计算。
- 为什么缓冲池里要有 pin_count。
- 为什么 dirty 页要先刷盘。

如果这一步没想清楚，后面一定会绕晕。

### 第 2 步：完成 DiskManager

这是 Lab1 最底层，也是最适合先做的部分。

你先实现这些能力：

1. 判断文件是否存在。
2. 创建文件。
3. 打开文件。
4. 关闭文件。
5. 删除文件。
6. 根据 page_no 读写页。
7. 为文件分配新的页号。

做这一步时，你要一直盯着两个状态：

- 这个文件有没有被打开。
- 这个文件已经分配到哪个页号了。

做完以后立刻跑：

```bash
make disk_manager_test
./bin/disk_manager_test
```

通过标准很简单：

- 创建、打开、关闭、删除行为正确。
- 读写页能正确定位到文件偏移。
- 页号分配是连续递增的。

### 第 3 步：完成 Replacer，也就是 LRU

这一部分先不要急着想缓冲池，先把 LRU 本身做对。

你要实现的是：

1. victim：挑一个最久没被用的 frame。
2. pin：把 frame 从可淘汰集合里移出去。
3. unpin：把 frame 放回可淘汰集合。

这一步你一定要想清楚顺序：

- 哪个位置代表“最近使用”。
- 哪个位置代表“最久未使用”。

建议你拿纸画一个 list，模拟几次 pin 和 unpin，这样会特别清楚。

做完以后跑：

```bash
make lru_replacer_test
./bin/lru_replacer_test
```

通过标准是：

- victim 选出来的帧顺序正确。
- pin 之后不会再被 victim。
- unpin 后会重新进入候选集合。

### 第 4 步：完成 BufferPoolManager 的主干

这一部分是 Lab1 的核心，建议你按下面顺序写，而不是一口气全写完。

先写两个辅助函数：

1. find_victim_page：先找 free_list，再找 replacer。
2. update_page：统一更新页表、页元数据、pin 状态。

然后再写 public 函数，顺序建议是：

1. new_page。
2. fetch_page。
3. unpin_page。
4. flush_page。
5. delete_page。
6. flush_all_pages。

为什么这样排？

- new_page 和 fetch_page 是最核心的“拿页”操作。
- unpin_page 是“放页”操作。
- flush 和 delete 是后续维护操作。

你在这一步要反复问自己四个问题：

1. 这个页现在在不在 page_table_ 里。
2. 它的 pin_count 是多少。
3. 它是不是脏页。
4. 没有空帧时该找谁替换。

写完以后跑：

```bash
make buffer_pool_manager_test
./bin/buffer_pool_manager_test
```

如果测试不过，优先检查这几个地方：

- 脏页有没有先刷盘。
- page_table_ 是否和页面内容同步更新。
- pin_count 是否正确增减。
- 淘汰后有没有清理旧页状态。

### 第 5 步：先理解记录文件布局，再做 RmFileHandle

记录管理器最容易卡的地方，不是函数本身，而是你有没有理解页内布局。

你先确保自己能说清楚这三件事：

1. 文件头页存什么。
2. 记录页的页头存什么。
3. bitmap 和 slots 的关系是什么。

然后再开始做 RmFileHandle。

建议的实现顺序是：

1. 构造函数，先把 file_hdr 读进来。
2. create_new_page_handle。
3. fetch_page_handle。
4. create_page_handle。
5. insert_record。
6. delete_record。
7. update_record。
8. get_record。
9. release_page_handle。

这一部分你要特别关注：

- first_free_page_no 怎么维护。
- next_free_page_no 怎么维护。
- bitmap 的每一位什么时候置 1，什么时候置 0。
- 一个页什么时候算满，什么时候算有空位。

做完以后跑：

```bash
make record_manager_test
./bin/record_manager_test
```

### 第 6 步：最后做 RmScan

扫描器通常是最后做，因为它依赖前面的页结构和记录插入逻辑都正确。

它本质上就是一个遍历器，你要做的是：

1. 从第一个有效页开始。
2. 在当前页里找下一个 bitmap 为 1 的 slot。
3. 找不到就跳到下一页。
4. 到文件末尾就结束。

你可以把它当成“按页顺着找第一个有记录的位置”。

### 第 7 步：按测试结果反推问题

如果某个测试失败，不要一上来大改。

你先判断它属于哪一层：

- 文件读写错了，大概率是 DiskManager。
- 替换顺序错了，大概率是 Replacer。
- 页找不到、脏页没刷、pin_count 不对，大概率是 BufferPoolManager。
- 插入删改错位，大概率是 Record Manager。

这个定位方法非常重要，它会帮你少走很多弯路。

### 第 8 步：最后再回头总结一遍

当你把 Lab1 做完，再回头看它，你应该能把整个系统总结成一句话：

**先用 DiskManager 管文件，再用 Replacer 决定淘汰谁，再用 BufferPoolManager 管页的来去，最后用 Record Manager 管一页里的记录。**

如果你能把这句话说顺，Lab1 就算真的理解了。

---

## 12. 各任务实现提示与代码层面指引

这一节不是给你答案，而是在你动手写代码时，帮你少踩坑。每个函数我会告诉你：用什么 API、关键逻辑怎么写、最容易错在哪。

---

### 12.1 DiskManager 实现提示

#### write_page / read_page

核心思路：用 `lseek` 定位到 `page_no * PAGE_SIZE`，再用 `read`/`write` 读写字节。

```cpp
// write_page 伪代码
off_t offset = page_no * PAGE_SIZE;
lseek(fd, offset, SEEK_SET);
ssize_t bytes = write(fd, buffer, num_bytes);
if (bytes != num_bytes) throw InternalError(...);

// read_page 同理，把 write 换成 read
```

易错点：
- `lseek` 的返回值要检查，定位失败时返回 `-1`。
- `write`/`read` 可能返回比 `num_bytes` 小的值（被信号中断等），但本实验中如果不等直接抛异常即可。

#### create_file

```cpp
void DiskManager::create_file(const std::string &path) {
    // 1. 先检查文件是否已存在
    if (is_file(path)) {
        throw FileExistsError(path);
    }
    // 2. 用 O_CREAT | O_EXCL 创建，O_EXCL 确保不覆盖已有文件
    int fd = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) throw UnixError();
    // 3. 立刻关闭，create 只负责创建，不负责打开
    close(fd);
}
```

#### open_file

```cpp
int DiskManager::open_file(const std::string &path) {
    // 1. 检查是否已经打开（查 path2fd_）
    if (path2fd_.count(path)) {
        throw FileNotClosedError(path);  // 文件已打开，不能重复打开
    }
    // 2. 以 O_RDWR 打开
    int fd = open(path.c_str(), O_RDWR);
    if (fd < 0) throw UnixError();
    // 3. 更新两个映射表
    path2fd_[path] = fd;
    fd2path_[fd] = path;
    return fd;
}
```

#### close_file

```cpp
void DiskManager::close_file(int fd) {
    // 1. 检查 fd 是否有效（查 fd2path_）
    if (!fd2path_.count(fd)) {
        throw FileNotOpenError(fd);
    }
    // 2. 调用 close()
    if (close(fd) < 0) throw UnixError();
    // 3. 从两个映射表中删除
    std::string path = fd2path_[fd];
    fd2path_.erase(fd);
    path2fd_.erase(path);
}
```

#### destroy_file

```cpp
void DiskManager::destroy_file(const std::string &path) {
    // 1. 检查文件是否存在
    if (!is_file(path)) {
        throw FileNotFoundError(path);
    }
    // 2. 检查文件是否已打开（已打开的文件不能删除）
    if (path2fd_.count(path)) {
        throw FileNotClosedError(path);
    }
    // 3. 调用 unlink
    if (unlink(path.c_str()) < 0) throw UnixError();
}
```

---

### 12.2 LRUReplacer 实现提示

数据结构已经帮你定义好了：
- `LRUlist_`：`std::list<frame_id_t>`，头部是最近使用的，尾部是最久未使用的。
- `LRUhash_`：`unordered_map<frame_id_t, list::iterator>`，O(1) 定位。

#### victim

```cpp
bool LRUReplacer::victim(frame_id_t* frame_id) {
    std::scoped_lock lock{latch_};
    // 1. 如果 LRUlist_ 为空，返回 false
    if (LRUlist_.empty()) return false;
    // 2. 取尾部元素（最久未使用）
    *frame_id = LRUlist_.back();
    // 3. 从 list 和 hash 中删除
    LRUhash_.erase(*frame_id);
    LRUlist_.pop_back();
    return true;
}
```

#### pin

```cpp
void LRUReplacer::pin(frame_id_t frame_id) {
    std::scoped_lock lock{latch_};
    // 1. 在 hash 中查找
    auto it = LRUhash_.find(frame_id);
    // 2. 如果找到了，从 list 和 hash 中删除
    if (it != LRUhash_.end()) {
        LRUlist_.erase(it->second);
        LRUhash_.erase(it);
    }
    // 3. 如果没找到，什么都不做
}
```

#### unpin

```cpp
void LRUReplacer::unpin(frame_id_t frame_id) {
    std::scoped_lock lock{latch_};
    // 1. 如果已经在 list 中，不重复添加
    if (LRUhash_.count(frame_id)) return;
    // 2. 插入到 list 头部（最近使用）
    LRUlist_.push_front(frame_id);
    // 3. 在 hash 中记录迭代器
    LRUhash_[frame_id] = LRUlist_.begin();
}
```

易错点：
- `unpin` 前要检查是否已存在，否则会重复插入。
- `victim` 返回 `false` 表示没有可淘汰的帧。
- 每个函数都要加锁，用 `std::scoped_lock` 自动管理。

---

### 12.3 BufferPoolManager 实现提示

这是最复杂的部分。先实现两个辅助函数，再实现 public 接口。

#### find_victim_page

```cpp
bool BufferPoolManager::find_victim_page(frame_id_t* frame_id) {
    // 1. 先看 free_list_ 是否有空闲帧
    if (!free_list_.empty()) {
        *frame_id = free_list_.front();
        free_list_.pop_front();
        return true;
    }
    // 2. 没有空闲帧，调用 replacer 的 victim
    return replacer_->victim(frame_id);
}
```

#### update_page

```cpp
void BufferPoolManager::update_page(Page *page, PageId new_page_id, frame_id_t new_frame_id) {
    // 1. 如果是脏页，先写回磁盘
    if (page->is_dirty_) {
        disk_manager_->write_page(page->id_.fd, page->id_.page_no, page->data_, PAGE_SIZE);
        page->is_dirty_ = false;
    }
    // 2. 从 page_table_ 中删除旧映射
    page_table_.erase(page->id_);
    // 3. 重置 page 数据
    page->reset_memory();
    page->id_ = new_page_id;
    page->is_dirty_ = false;
    page->pin_count_ = 0;
    // 4. 添加新映射
    page_table_[new_page_id] = new_frame_id;
}
```

#### fetch_page

```cpp
Page* BufferPoolManager::fetch_page(PageId page_id) {
    std::scoped_lock lock{latch_};
    // 1. 在 page_table_ 中查找
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        // 命中：pin_count++，通知 replacer pin
        frame_id_t frame_id = it->second;
        pages_[frame_id].pin_count_++;
        replacer_->pin(frame_id);
        return &pages_[frame_id];
    }
    // 2. 未命中：找一个可用帧
    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) return nullptr;
    // 3. 更新页（如果旧帧有脏页会先写回）
    update_page(&pages_[frame_id], page_id, frame_id);
    // 4. 从磁盘读取
    disk_manager_->read_page(page_id.fd, page_id.page_no, pages_[frame_id].data_, PAGE_SIZE);
    // 5. 设置 pin_count，通知 replacer pin
    pages_[frame_id].pin_count_ = 1;
    replacer_->pin(frame_id);
    return &pages_[frame_id];
}
```

#### unpin_page

```cpp
bool BufferPoolManager::unpin_page(PageId page_id, bool is_dirty) {
    std::scoped_lock lock{latch_};
    // 1. 查找页面
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return false;
    // 2. 获取 page
    Page* page = &pages_[it->second];
    // 3. pin_count 已经为 0，返回 false
    if (page->pin_count_ <= 0) return false;
    // 4. pin_count--
    page->pin_count_--;
    // 5. 如果 pin_count 降到 0，通知 replacer unpin
    if (page->pin_count_ == 0) {
        replacer_->unpin(it->second);
    }
    // 6. 如果上层说脏了，标记脏
    if (is_dirty) {
        page->is_dirty_ = true;
    }
    return true;
}
```

#### new_page

```cpp
Page* BufferPoolManager::new_page(PageId* page_id) {
    std::scoped_lock lock{latch_};
    // 1. 找一个可用帧
    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) return nullptr;
    // 2. 分配新 page_id（注意要用 page_id 指针传出）
    PageId new_id;
    new_id.fd = ...;  // 需要由调用者指定 fd，但本接口没有 fd 参数
                      // 实际上 new_page 在本实验中由 RmFileHandle 调用，
                      // 调用前会先设置好 fd，这里需要看测试代码确认 fd 来源
    // 注意：仔细看 new_page 的调用方式，page_id 的 fd 通常由调用者预设
    page_id->page_no = disk_manager_->allocate_page(page_id->fd);
    new_id = *page_id;
    // 3. 更新旧帧的页
    update_page(&pages_[frame_id], new_id, frame_id);
    // 4. 初始化新页的数据（已经由 reset_memory 清零）
    // 5. 设置 pin_count，通知 replacer pin
    pages_[frame_id].pin_count_ = 1;
    replacer_->pin(frame_id);
    return &pages_[frame_id];
}
```

**重要**：`new_page` 的 `page_id` 参数是输出参数，但 `fd` 字段需要调用者预先设置。看测试代码和 `RmFileHandle::create_new_page_handle()` 的调用方式即可理解。

#### delete_page

```cpp
bool BufferPoolManager::delete_page(PageId page_id) {
    std::scoped_lock lock{latch_};
    // 1. 查找页面
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return true;  // 不在缓冲池，直接返回 true
    // 2. pin_count 不为 0，不能删除
    if (pages_[it->second].pin_count_ > 0) return false;
    // 3. 从 page_table_ 删除
    page_table_.erase(it);
    // 4. 重置 page 元数据
    pages_[it->second].reset_memory();
    pages_[it->second].id_ = {0, INVALID_PAGE_ID};
    pages_[it->second].is_dirty_ = false;
    pages_[it->second].pin_count_ = 0;
    // 5. 帧放回 free_list_
    free_list_.push_back(it->second);
    return true;
}
```

#### flush_page

```cpp
bool BufferPoolManager::flush_page(PageId page_id) {
    std::scoped_lock lock{latch_};
    // 1. 查找页面
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return false;
    // 2. 无论是否脏，都写回磁盘
    Page* page = &pages_[it->second];
    disk_manager_->write_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
    // 3. 清除脏标志
    page->is_dirty_ = false;
    return true;
}
```

#### flush_all_pages

```cpp
void BufferPoolManager::flush_all_pages(int fd) {
    std::scoped_lock lock{latch_};
    // 遍历 page_table_，把属于该 fd 的页都写回磁盘
    for (auto& [page_id, frame_id] : page_table_) {
        if (page_id.fd == fd) {
            Page* page = &pages_[frame_id];
            disk_manager_->write_page(fd, page_id.page_no, page->data_, PAGE_SIZE);
            page->is_dirty_ = false;
        }
    }
}
```

---

### 12.4 RmFileHandle 实现提示

先理解页面布局：

```
page->data 布局：
[4字节 LSN][RmPageHdr][bitmap][slot0][slot1]...
 ^           ^          ^       ^
 offset=0   offset=4   动态    动态
```

`RmPageHandle` 的构造函数已经帮你算好了指针：
- `page_hdr` 在 `page->data + 4`
- `bitmap` 在 `page_hdr` 之后
- `slots` 在 `bitmap` 之后

#### fetch_page_handle

```cpp
RmPageHandle RmFileHandle::fetch_page_handle(int page_no) const {
    // 1. 用缓冲池获取页面
    PageId page_id = {fd_, page_no};
    Page* page = buffer_pool_manager_->fetch_page(page_id);
    if (page == nullptr) throw PageNotExistError("...", page_no);
    // 2. 包装成 RmPageHandle 返回
    return RmPageHandle(&file_hdr_, page);
}
```

#### create_new_page_handle

```cpp
RmPageHandle RmFileHandle::create_new_page_handle() {
    // 1. 用缓冲池创建新页
    PageId page_id = {fd_, -1};  // fd 预设，page_no 由 new_page 分配
    Page* page = buffer_pool_manager_->new_page(&page_id);
    if (page == nullptr) throw InternalError("...");
    // 2. 初始化 page_hdr
    RmPageHandle page_handle(&file_hdr_, page);
    page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
    page_handle.page_hdr->num_records = 0;
    // 3. 初始化 bitmap（全部清零，表示没有记录）
    Bitmap::init(page_handle.bitmap, file_hdr_.bitmap_size);
    // 4. 更新 file_hdr
    file_hdr_.num_pages++;
    return page_handle;
}
```

#### create_page_handle

```cpp
RmPageHandle RmFileHandle::create_page_handle() {
    // 1. 判断是否有空闲页
    if (file_hdr_.first_free_page_no != RM_NO_PAGE) {
        // 有空闲页，直接获取
        return fetch_page_handle(file_hdr_.first_free_page_no);
    }
    // 2. 没有空闲页，创建新页
    return create_new_page_handle();
}
```

#### release_page_handle

```cpp
void RmFileHandle::release_page_handle(RmPageHandle &page_handle) {
    // 当页面从满变未满时调用
    // 1. 把当前页插入空闲链表头部
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    // 2. 更新 file_hdr 的 first_free_page_no
    file_hdr_.first_free_page_no = page_handle.page->get_page_id().page_no;
}
```

#### get_record

```cpp
std::unique_ptr<RmRecord> RmFileHandle::get_record(const Rid &rid, Context *context) const {
    // 1. 获取 page handle
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    // 2. 检查记录是否存在
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }
    // 3. 拷贝 slot 数据到 RmRecord
    auto record = std::make_unique<RmRecord>(file_hdr_.record_size);
    memcpy(record->data, page_handle.get_slot(rid.slot_no), file_hdr_.record_size);
    return record;
}
```

#### insert_record (无指定位置)

```cpp
Rid RmFileHandle::insert_record(char *buf, Context *context) {
    // 1. 获取一个有空位的 page handle
    RmPageHandle page_handle = create_page_handle();
    // 2. 用 bitmap 找第一个空 slot
    int slot_no = Bitmap::first_bit(false, page_handle.bitmap, file_hdr_.num_records_per_page);
    // 3. 把数据复制到 slot
    memcpy(page_handle.get_slot(slot_no), buf, file_hdr_.record_size);
    // 4. bitmap 对应位置 1
    Bitmap::set(page_handle.bitmap, slot_no);
    // 5. 更新 num_records
    page_handle.page_hdr->num_records++;
    // 6. 如果页面满了，更新 file_hdr 的 first_free_page_no
    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
    }
    // 7. unpin 页面（因为 create_page_handle 会 pin 住页面）
    PageId page_id = {fd_, page_handle.page->get_page_id().page_no};
    buffer_pool_manager_->unpin_page(page_id, true);  // 标记脏
    return Rid{page_handle.page->get_page_id().page_no, slot_no};
}
```

#### delete_record

```cpp
void RmFileHandle::delete_record(const Rid &rid, Context *context) {
    // 1. 获取 page handle
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    // 2. bitmap 对应位清零
    Bitmap::reset(page_handle.bitmap, rid.slot_no);
    // 3. 更新 num_records
    page_handle.page_hdr->num_records--;
    // 4. 如果页面从满变成未满，调用 release_page_handle
    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page - 1) {
        release_page_handle(page_handle);
    }
    // 5. unpin 页面
    PageId page_id = {fd_, rid.page_no};
    buffer_pool_manager_->unpin_page(page_id, true);  // 标记脏
}
```

#### update_record

```cpp
void RmFileHandle::update_record(const Rid &rid, char *buf, Context *context) {
    // 1. 获取 page handle
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    // 2. 直接覆盖 slot 数据
    memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);
    // 3. unpin 页面
    PageId page_id = {fd_, rid.page_no};
    buffer_pool_manager_->unpin_page(page_id, true);  // 标记脏
}
```

---

### 12.5 RmScan 实现提示

#### 构造函数

```cpp
RmScan::RmScan(const RmFileHandle *file_handle) : file_handle_(file_handle) {
    // 从第一个记录页开始找（跳过文件头页 0）
    rid_ = {RM_FIRST_RECORD_PAGE, -1};  // slot_no = -1，next 会从 0 开始找
    // 如果第一页没有记录，需要往后找
    next();
}
```

#### next

```cpp
void RmScan::next() {
    // 1. 在当前页里找下一个 bit 为 1 的 slot
    RmPageHandle page_handle = file_handle_->fetch_page_handle(rid_.page_no);
    rid_.slot_no = Bitmap::next_bit(true, page_handle.bitmap,
                                    file_handle_->file_hdr_.num_records_per_page,
                                    rid_.slot_no);
    // 2. 如果当前页没有更多记录了，跳到下一页
    while (rid_.slot_no >= file_handle_->file_hdr_.num_records_per_page) {
        // unpin 当前页
        PageId page_id = {file_handle_->fd_, rid_.page_no};
        file_handle_->buffer_pool_manager_->unpin_page(page_id, false);
        // 跳到下一页
        rid_.page_no++;
        rid_.slot_no = -1;
        // 如果超过文件总页数，结束
        if (rid_.page_no >= file_handle_->file_hdr_.num_pages) return;
        // 继续在新页里找
        page_handle = file_handle_->fetch_page_handle(rid_.page_no);
        rid_.slot_no = Bitmap::next_bit(true, page_handle.bitmap,
                                        file_handle_->file_hdr_.num_records_per_page,
                                        rid_.slot_no);
    }
}
```

**注意**：上面的 `next` 实现中，`fetch_page_handle` 会 pin 住页面，记得在合适的地方 unpin。实际实现时需要仔细处理 pin/unpin 的配对，避免页面泄漏。

一个更简洁的写法是：每次 `next` 结束时都 unpin 当前页，每次需要读取时再 fetch。

#### is_end

```cpp
bool RmScan::is_end() const {
    return rid_.page_no >= file_handle_->file_hdr_.num_pages;
}
```

---

### 12.6 最容易出错的检查清单

在提交测试前，逐条检查：

**DiskManager**
- [ ] `write_page` / `read_page` 的偏移计算是 `page_no * PAGE_SIZE`
- [ ] `create_file` 检查了文件是否已存在
- [ ] `open_file` 检查了文件是否已打开，并更新了两个映射表
- [ ] `close_file` 检查了 fd 是否有效，并更新了两个映射表
- [ ] `destroy_file` 检查了文件是否已关闭

**LRUReplacer**
- [ ] `victim` 在 list 为空时返回 `false`
- [ ] `pin` 在 frame 不存在时无操作
- [ ] `unpin` 在 frame 已存在时无重复插入
- [ ] 每个函数都加了锁

**BufferPoolManager**
- [ ] `find_victim_page` 先查 free_list_，再查 replacer
- [ ] `update_page` 在脏页时先写回磁盘
- [ ] `fetch_page` 命中时 pin_count++ 并调用 pin
- [ ] `fetch_page` 未命中时调用 find_victim_page + read_page
- [ ] `unpin_page` pin_count 降到 0 时调用 unpin
- [ ] `new_page` 的 page_id.fd 由调用者预设
- [ ] `delete_page` 只允许 pin_count == 0 的页被删
- [ ] `flush_all_pages` 只刷指定 fd 的页

**RmFileHandle**
- [ ] `fetch_page_handle` 用的是 `buffer_pool_manager_->fetch_page`
- [ ] `create_new_page_handle` 初始化了 bitmap 和 page_hdr
- [ ] `insert_record` 在页面满时更新了 `first_free_page_no`
- [ ] `delete_record` 在页面从满变未满时调用了 `release_page_handle`
- [ ] 所有操作完成后都 unpin 了页面

**RmScan**
- [ ] 构造函数跳过了文件头页（page 0）
- [ ] `next` 正确使用了 `Bitmap::next_bit`
- [ ] `is_end` 检查的是 `page_no >= file_hdr_.num_pages`
