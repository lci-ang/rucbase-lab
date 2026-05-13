# Lab1 存储管理：完整学习与实现指南

> 这份文档的目标：让你**完全理解** Lab1 的每一个 TODO，然后**自己动手**把代码写进源文件。
>
> 文档结构：先教你原理 → 再告诉你该改哪个文件的哪几行 → 给你逻辑思路但不直接给完整代码 → 最后给你验证方法。

---

## 目录

1. [你需要先搞懂的基础知识](#1-你需要先搞懂的基础知识)
2. [DiskManager：6 个 TODO](#2-diskmanager6-个-todo)
3. [LRUReplacer：3 个 TODO](#3-lrureplacer3-个-todo)
4. [BufferPoolManager：8 个 TODO](#4-bufferpoolmanager8-个-todo)
5. [RmFileHandle：9 个 TODO](#5-rmfilehandle9-个-todo)
6. [RmScan：3 个 TODO](#6-rmscan3-个-todo)
7. [完整测试流程](#7-完整测试流程)
8. [调试技巧](#8-调试技巧)

---

## 1. 你需要先搞懂的基础知识

### 1.1 文件描述符 (fd)

Linux 中，`open()` 返回一个整数，叫**文件描述符**。之后所有对这个文件的操作（read、write、close）都用这个整数来标识文件。

```cpp
int fd = open("test.db", O_RDWR);  // 打开文件，拿到 fd
// 之后用 fd 读写
close(fd);  // 关闭
```

### 1.2 lseek 定位

`lseek(fd, offset, SEEK_SET)` 把文件的"读写指针"移到 `offset` 位置。之后的 read/write 从这个位置开始。

```cpp
lseek(fd, 0, SEEK_SET);       // 移到文件开头
lseek(fd, 4096, SEEK_SET);    // 移到第 4096 字节处
```

### 1.3 Page（页）和 PageId

- **Page**：磁盘和内存之间数据交换的基本单位，本项目中固定 4096 字节（4KB）。
- **PageId**：一个页的唯一标识，由 `{fd, page_no}` 组成。`fd` 标识哪个文件，`page_no` 标识文件中的第几页。
- 页在文件中的偏移 = `page_no * PAGE_SIZE`（即 `page_no * 4096`）。

### 1.4 Frame（帧）

缓冲池中的一块内存空间，大小和 Page 一样。Frame 是"槽位"，Page 是"数据"。一个 Frame 最多同时存放一个 Page。

### 1.5 pin_count 和 dirty

- **pin_count**：有多少人在用这个页。大于 0 时不能被淘汰。
- **dirty**：这个页在内存中被修改过，被淘汰前必须写回磁盘。

### 1.6 Bitmap（位图）

用一个 bit 表示一个 slot 是否有记录。`1` = 有记录，`0` = 空闲。

Bitmap API（已提供在 `src/record/bitmap.h`）：
- `Bitmap::init(bm, size)` — 全部清零
- `Bitmap::set(bm, pos)` — 第 pos 位置 1
- `Bitmap::reset(bm, pos)` — 第 pos 位置 0
- `Bitmap::is_set(bm, pos)` — 第 pos 位是否为 1
- `Bitmap::first_bit(bit, bm, max_n)` — 找第一个为 bit 的位
- `Bitmap::next_bit(bit, bm, max_n, curr)` — 从 curr+1 开始找下一个为 bit 的位

### 1.7 记录文件的页面布局

```
文件第 0 页（文件头页）：存 RmFileHdr（record_size, num_pages, num_records_per_page, first_free_page_no, bitmap_size）
文件第 1 页起（数据页）：每页布局 [RmPageHdr][bitmap][slot0][slot1][slot2]...
```

- `RmPageHdr`：`{next_free_page_no, num_records}`
- 空闲页链表：通过 `first_free_page_no`（文件头）和 `next_free_page_no`（页头）串起来

### 1.8 RmPageHandle 的作用

`RmPageHandle` 把一个 Page 的原始字节数据解释成三个区域：

```cpp
struct RmPageHandle {
    const RmFileHdr *file_hdr;  // 文件头指针
    Page *page;                 // 原始页面
    RmPageHdr *page_hdr;        // 指向 page->data + 4
    char *bitmap;               // 指向 page_hdr 之后
    char *slots;                // 指向 bitmap 之后
};
```

它的构造函数已经算好了指针位置，你只需要用 `page_handle.page_hdr->xxx`、`page_handle.bitmap`、`page_handle.get_slot(slot_no)` 就行。

---

## 2. DiskManager：6 个 TODO

**文件**：`src/storage/disk_manager.cpp`

DiskManager 是最底层，只负责磁盘文件的读写。它不关心页里存了什么，只关心"从哪个文件、哪个位置、读写多少字节"。

它内部维护两个映射：
- `path2fd_`：文件路径 → fd
- `fd2path_`：fd → 文件路径

---

### TODO 2.1：write_page

**位置**：`disk_manager.cpp` 第 29-35 行

**做什么**：把内存中的数据写入磁盘文件的指定页面。

**你需要理解的**：
- 页在文件中的偏移 = `page_no * PAGE_SIZE`
- 用 `lseek` 定位到这个偏移
- 用 `write` 写入 `num_bytes` 字节
- 如果写入的字节数不等于 `num_bytes`，抛 `InternalError`

**提示**：
```cpp
void DiskManager::write_page(int fd, page_id_t page_no, const char *offset, int num_bytes) {
    // 第一步：计算偏移
    // page_no * PAGE_SIZE 就是这一页在文件中的起始位置

    // 第二步：lseek 定位
    // lseek(fd, 偏移, SEEK_SET)

    // 第三步：write 写入
    // write(fd, offset, num_bytes)

    // 第四步：检查返回值
    // 如果写入的字节数 != num_bytes，throw InternalError("DiskManager::write_page Error");
}
```

**关键点**：
- `lseek` 的返回值是 `off_t` 类型，定位失败返回 `-1`
- `write` 的返回值是 `ssize_t`，表示实际写入的字节数
- 参数 `offset` 是 `const char*`，是**要写入的数据的指针**，不是文件偏移量（名字容易混淆）

---

### TODO 2.2：read_page

**位置**：`disk_manager.cpp` 第 44-50 行

**做什么**：从磁盘文件的指定页面读取数据到内存。

**和 write_page 完全对称**，只是把 `write` 换成 `read`。

**提示**：
```cpp
void DiskManager::read_page(int fd, page_id_t page_no, char *offset, int num_bytes) {
    // 和 write_page 一样的逻辑，把 write 换成 read
    // 注意：read 的返回值 != num_bytes 时也要抛异常
}
```

---

### TODO 2.3：create_file

**位置**：`disk_manager.cpp` 第 101-105 行

**做什么**：创建一个新文件。

**你需要理解的**：
- `open(path, O_CREAT | O_WRONLY, 0644)` 会创建文件
- 但如果文件已存在，`O_CREAT` 不会报错，会直接打开。所以要先用 `is_file()` 检查
- 创建后要立刻 `close()`，因为 create 只负责创建，不负责保持打开状态

**提示**：
```cpp
void DiskManager::create_file(const std::string &path) {
    // 1. 检查文件是否已存在：if (is_file(path)) throw FileExistsError(path);
    // 2. 创建文件：int fd = open(path.c_str(), O_CREAT | O_WRONLY, 0644);
    //    如果 fd < 0，throw UnixError()
    // 3. 关闭：close(fd);
}
```

---

### TODO 2.4：open_file

**位置**：`disk_manager.cpp` 第 124-129 行

**做什么**：打开一个已存在的文件，返回 fd。

**你需要理解的**：
- 打开后要更新 `path2fd_` 和 `fd2path_` 两个映射表
- 不能重复打开同一个文件

**提示**：
```cpp
int DiskManager::open_file(const std::string &path) {
    // 1. 检查是否已经打开：if (path2fd_.count(path)) throw FileNotClosedError(path);
    // 2. 打开：int fd = open(path.c_str(), O_RDWR);
    //    如果 fd < 0，throw UnixError()
    // 3. 更新映射：path2fd_[path] = fd; fd2path_[fd] = path;
    // 4. return fd;
}
```

---

### TODO 2.5：close_file

**位置**：`disk_manager.cpp` 第 135-140 行

**做什么**：关闭一个已打开的文件。

**提示**：
```cpp
void DiskManager::close_file(int fd) {
    // 1. 检查 fd 是否有效：if (!fd2path_.count(fd)) throw FileNotOpenError(fd);
    // 2. 先取出路径：std::string path = fd2path_[fd];
    // 3. 关闭：close(fd);（失败 throw UnixError()）
    // 4. 从两个映射表中删除：fd2path_.erase(fd); path2fd_.erase(path);
}
```

**注意**：要在 close 之前取出 path，否则 close 之后就找不到了。

---

### TODO 2.6：destroy_file

**位置**：`disk_manager.cpp` 第 111-116 行

**做什么**：删除一个文件。

**提示**：
```cpp
void DiskManager::destroy_file(const std::string &path) {
    // 1. 检查文件是否存在：if (!is_file(path)) throw FileNotFoundError(path);
    // 2. 检查文件是否已打开（已打开的不能删）：if (path2fd_.count(path)) throw FileNotClosedError(path);
    // 3. 删除：unlink(path.c_str());（失败 throw UnixError()）
}
```

---

### DiskManager 验证

写完后在 `build/` 目录下运行：
```bash
make disk_manager_test && ./bin/disk_manager_test
```

---

## 3. LRUReplacer：3 个 TODO

**文件**：`src/replacer/lru_replacer.cpp`

LRU（最近最少使用）策略：当缓冲池满了，淘汰最久没被访问的帧。

数据结构（已定义在头文件中）：
- `LRUlist_`：`std::list<frame_id_t>`，头部 = 最近使用，尾部 = 最久未使用
- `LRUhash_`：`unordered_map<frame_id_t, list::iterator>`，O(1) 快速定位

---

### TODO 3.1：victim

**位置**：`lru_replacer.cpp` 第 22-32 行

**做什么**：淘汰最久未使用的帧，返回它的 frame_id。

**逻辑**：
1. 加锁（已有 `std::scoped_lock lock{latch_}`）
2. 如果 `LRUlist_` 为空，返回 `false`（没有可淘汰的）
3. 取尾部元素（最久未使用），赋值给 `*frame_id`
4. 从 `LRUhash_` 和 `LRUlist_` 中删除
5. 返回 `true`

**你需要写的部分**（在 `std::scoped_lock lock{latch_}` 之后）：
```cpp
// 检查是否为空
if (LRUlist_.empty()) return false;
// 取尾部
*frame_id = LRUlist_.back();
// 从 hash 中删除
LRUhash_.erase(*frame_id);
// 从 list 中删除
LRUlist_.pop_back();
return true;
```

---

### TODO 3.2：pin

**位置**：`lru_replacer.cpp` 第 38-43 行

**做什么**：固定一个帧，让它不能被淘汰（从 LRU 候选集合中移除）。

**逻辑**：
1. 加锁
2. 在 `LRUhash_` 中查找 `frame_id`
3. 如果找到，从 `LRUlist_` 和 `LRUhash_` 中删除
4. 如果没找到，什么都不做

**你需要写的部分**：
```cpp
std::scoped_lock lock{latch_};
auto it = LRUhash_.find(frame_id);
if (it != LRUhash_.end()) {
    LRUlist_.erase(it->second);  // it->second 是 list 的迭代器
    LRUhash_.erase(it);
}
```

**关键点**：`LRUhash_` 的 value 类型是 `std::list<frame_id_t>::iterator`，可以直接传给 `LRUlist_.erase()`。

---

### TODO 3.3：unpin

**位置**：`lru_replacer.cpp` 第 49-53 行

**做什么**：取消固定一个帧，让它可以被淘汰（加入 LRU 候选集合）。

**逻辑**：
1. 加锁（注意：这个函数的 TODO 提示说要"支持并发锁"，你需要自己加 `std::scoped_lock`）
2. 如果 `frame_id` 已经在 `LRUhash_` 中，不重复添加
3. 插入到 `LRUlist_` 头部（最近使用的位置）
4. 在 `LRUhash_` 中记录迭代器

**你需要写的部分**：
```cpp
std::scoped_lock lock{latch_};
// 防止重复插入
if (LRUhash_.count(frame_id)) return;
// 插入头部
LRUlist_.push_front(frame_id);
// 记录迭代器
LRUhash_[frame_id] = LRUlist_.begin();
```

**关键点**：`unpin` 是三个函数中唯一没有预置 `std::scoped_lock` 的，你需要自己加锁。

---

### LRUReplacer 验证

```bash
make lru_replacer_test && ./bin/lru_replacer_test
```

---

## 4. BufferPoolManager：8 个 TODO

**文件**：`src/storage/buffer_pool_manager.cpp`

这是 Lab1 的核心。它管理一个固定大小的 `pages_` 数组，通过 `page_table_`（PageId → frame_id 的映射）和 `free_list_`（空闲帧列表）来协调磁盘和内存。

**关键数据结构**（已定义在头文件中）：
- `pages_`：`Page` 数组，大小为 `pool_size_`
- `page_table_`：`unordered_map<PageId, frame_id_t, PageIdHash>`
- `free_list_`：`list<frame_id_t>`，存放空闲帧的编号
- `replacer_`：LRU 替换器
- `disk_manager_`：磁盘管理器

---

### TODO 4.1：find_victim_page（辅助函数）

**位置**：`buffer_pool_manager.cpp` 第 18-25 行

**做什么**：找一个可用的帧。优先从 `free_list_` 取，取不到再用 `replacer_` 淘汰。

**逻辑**：
1. 如果 `free_list_` 不为空，取头部，从 `free_list_` 中删除，返回 `true`
2. 否则调用 `replacer_->victim(frame_id)` 返回

**你需要写的部分**：
```cpp
if (!free_list_.empty()) {
    *frame_id = free_list_.front();
    free_list_.pop_front();
    return true;
}
return replacer_->victim(frame_id);
```

---

### TODO 4.2：update_page（辅助函数）

**位置**：`buffer_pool_manager.cpp` 第 33-39 行

**做什么**：把一个帧从"存旧页"变成"存新页"。如果旧页是脏页，先写回磁盘。

**逻辑**：
1. 如果 `page->is_dirty_` 为 true，调用 `disk_manager_->write_page()` 写回磁盘，然后 `is_dirty_ = false`
2. 从 `page_table_` 中删除旧的 PageId 映射
3. 重置 page 的数据（`reset_memory()` 清零）
4. 更新 page 的 `id_` 为新的 `new_page_id`
5. `is_dirty_ = false`，`pin_count_ = 0`
6. 在 `page_table_` 中添加新映射：`page_table_[new_page_id] = new_frame_id`

**你需要写的部分**：
```cpp
// 1. 脏页写回
if (page->is_dirty_) {
    disk_manager_->write_page(page->id_.fd, page->id_.page_no, page->data_, PAGE_SIZE);
    page->is_dirty_ = false;
}
// 2. 删除旧映射
page_table_.erase(page->id_);
// 3. 重置
page->reset_memory();
page->id_ = new_page_id;
page->is_dirty_ = false;
page->pin_count_ = 0;
// 4. 添加新映射
page_table_[new_page_id] = new_frame_id;
```

**关键点**：`page->id_` 存的是旧的 PageId，要在删除旧映射之前用它，或者先保存再删除。

---

### TODO 4.3：fetch_page

**位置**：`buffer_pool_manager.cpp` 第 48-58 行

**做什么**：获取缓冲池中的指定页面。如果在内存中就直接返回，如果不在就从磁盘读入。

**这是最重要的函数，你要彻底理解它。**

**逻辑**：
1. 加锁
2. 在 `page_table_` 中查找 `page_id`
3. **命中**：增加 `pin_count_`，调用 `replacer_->pin(frame_id)`，返回页面
4. **未命中**：
   a. 调用 `find_victim_page` 找一个可用帧
   b. 调用 `update_page` 把旧页换出（如果是脏页会写回磁盘）
   c. 调用 `disk_manager_->read_page` 从磁盘读取目标页到帧中
   d. 设置 `pin_count_ = 1`
   e. 调用 `replacer_->pin(frame_id)` 固定帧
   f. 返回页面

**你需要写的部分**：
```cpp
std::scoped_lock lock{latch_};
// 1. 查找
auto it = page_table_.find(page_id);
if (it != page_table_.end()) {
    // 命中
    frame_id_t frame_id = it->second;
    pages_[frame_id].pin_count_++;
    replacer_->pin(frame_id);
    return &pages_[frame_id];
}
// 2. 未命中
frame_id_t frame_id;
if (!find_victim_page(&frame_id)) return nullptr;
// 3. 换出旧页
update_page(&pages_[frame_id], page_id, frame_id);
// 4. 从磁盘读入
disk_manager_->read_page(page_id.fd, page_id.page_no, pages_[frame_id].data_, PAGE_SIZE);
// 5. 设置 pin
pages_[frame_id].pin_count_ = 1;
replacer_->pin(frame_id);
return &pages_[frame_id];
```

---

### TODO 4.4：unpin_page

**位置**：`buffer_pool_manager.cpp` 第 66-77 行

**做什么**：使用完页面后取消固定。一次 unpin 只减少一次 pin_count，降到 0 时通知 replacer。

**逻辑**：
1. 加锁
2. 在 `page_table_` 中查找，找不到返回 `false`
3. 如果 `pin_count_ <= 0`，返回 `false`
4. `pin_count_--`
5. 如果 `pin_count_` 降到 0，调用 `replacer_->unpin(frame_id)`
6. 根据参数 `is_dirty` 设置脏标志

**你需要写的部分**：
```cpp
std::scoped_lock lock{latch_};
auto it = page_table_.find(page_id);
if (it == page_table_.end()) return false;
Page* page = &pages_[it->second];
if (page->pin_count_ <= 0) return false;
page->pin_count_--;
if (page->pin_count_ == 0) {
    replacer_->unpin(it->second);
}
if (is_dirty) {
    page->is_dirty_ = true;
}
return true;
```

---

### TODO 4.5：new_page

**位置**：`buffer_pool_manager.cpp` 第 100-107 行

**做什么**：在缓冲池中创建一个新页面。

**重要细节**：`page_id` 是输出参数，但 `page_id->fd` 需要由调用者预先设置。看 `RmFileHandle::create_new_page_handle()` 就知道，调用者会先设置 `page_id.fd`。

**逻辑**：
1. 加锁
2. 找可用帧
3. 用 `disk_manager_->allocate_page(page_id->fd)` 分配新的 page_no，写入 `page_id->page_no`
4. 调用 `update_page` 换出旧页
5. 设置 `pin_count_ = 1`，调用 `replacer_->pin`
6. 返回页面

**你需要写的部分**：
```cpp
std::scoped_lock lock{latch_};
frame_id_t frame_id;
if (!find_victim_page(&frame_id)) return nullptr;
// 分配 page_no
page_id->page_no = disk_manager_->allocate_page(page_id->fd);
// 换出旧页
update_page(&pages_[frame_id], *page_id, frame_id);
// 设置 pin
pages_[frame_id].pin_count_ = 1;
replacer_->pin(frame_id);
return &pages_[frame_id];
```

---

### TODO 4.6：delete_page

**位置**：`buffer_pool_manager.cpp` 第 114-120 行

**做什么**：从缓冲池中删除一个页面。只有 pin_count == 0 的页才能被删除。

**逻辑**：
1. 加锁
2. 在 `page_table_` 中查找，找不到返回 `true`（本来就不在）
3. 如果 `pin_count_ > 0`，返回 `false`
4. 从 `page_table_` 删除
5. 重置 page 元数据（`reset_memory()`，`id_` 设为无效，`is_dirty_` 和 `pin_count_` 清零）
6. 帧放回 `free_list_`
7. 返回 `true`

**你需要写的部分**：
```cpp
std::scoped_lock lock{latch_};
auto it = page_table_.find(page_id);
if (it == page_table_.end()) return true;
if (pages_[it->second].pin_count_ > 0) return false;
page_table_.erase(it);
pages_[it->second].reset_memory();
pages_[it->second].id_ = {0, INVALID_PAGE_ID};
pages_[it->second].is_dirty_ = false;
pages_[it->second].pin_count_ = 0;
free_list_.push_back(it->second);
return true;
```

**注意**：`page_table_.erase(it)` 之后 `it->second` 就失效了！所以要先保存 `frame_id`，或者在 erase 之前完成所有用到 `it->second` 的操作。上面的写法中，`erase` 之后的 `it->second` 是未定义行为，你需要先保存 frame_id：

```cpp
frame_id_t frame_id = it->second;
page_table_.erase(it);
pages_[frame_id].reset_memory();
// ...
free_list_.push_back(frame_id);
```

---

### TODO 4.7：flush_page

**位置**：`buffer_pool_manager.cpp` 第 84-93 行

**做什么**：强制把指定页面写回磁盘，不管是否脏、是否被 pin。

**逻辑**：
1. 加锁
2. 查找页面，找不到返回 `false`
3. 调用 `disk_manager_->write_page` 写回
4. `is_dirty_ = false`
5. 返回 `true`

**你需要写的部分**：
```cpp
std::scoped_lock lock{latch_};
auto it = page_table_.find(page_id);
if (it == page_table_.end()) return false;
Page* page = &pages_[it->second];
disk_manager_->write_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
page->is_dirty_ = false;
return true;
```

---

### TODO 4.8：flush_all_pages

**位置**：`buffer_pool_manager.cpp` 第 126-128 行

**做什么**：把指定文件（通过 fd 标识）在缓冲池中的所有页面都写回磁盘。

**逻辑**：
1. 加锁
2. 遍历 `page_table_`，找到所有 `page_id.fd == fd` 的页面
3. 对每个匹配的页面，调用 `disk_manager_->write_page` 写回，`is_dirty_ = false`

**你需要写的部分**：
```cpp
std::scoped_lock lock{latch_};
for (auto& [pid, frame_id] : page_table_) {
    if (pid.fd == fd) {
        Page* page = &pages_[frame_id];
        disk_manager_->write_page(fd, pid.page_no, page->data_, PAGE_SIZE);
        page->is_dirty_ = false;
    }
}
```

---

### BufferPoolManager 验证

```bash
make buffer_pool_manager_test && ./bin/buffer_pool_manager_test
```

---

## 5. RmFileHandle：9 个 TODO

**文件**：`src/record/rm_file_handle.cpp`

从这里开始进入"页内部怎么放记录"。每个 `RmFileHandle` 对应一个记录文件。

---

### TODO 5.1：fetch_page_handle

**位置**：`rm_file_handle.cpp` 第 87-93 行

**做什么**：根据 page_no 获取对应的 RmPageHandle。

**逻辑**：
1. 用 `buffer_pool_manager_->fetch_page()` 获取 Page
2. 检查返回是否为 nullptr
3. 用 `RmPageHandle(&file_hdr_, page)` 包装

**你需要写的部分**：
```cpp
PageId page_id = {fd_, page_no};
Page* page = buffer_pool_manager_->fetch_page(page_id);
if (page == nullptr) {
    throw PageNotExistError("fetch_page_handle", page_no);
}
return RmPageHandle(&file_hdr_, page);
```

---

### TODO 5.2：create_new_page_handle

**位置**：`rm_file_handle.cpp` 第 99-106 行

**做什么**：创建一个全新的数据页，并初始化它的页头和 bitmap。

**逻辑**：
1. 用 `buffer_pool_manager_->new_page()` 创建新页（注意：要先设置 `page_id.fd = fd_`）
2. 检查返回是否为 nullptr
3. 构造 `RmPageHandle`
4. 初始化 `page_hdr`：`next_free_page_no = RM_NO_PAGE`，`num_records = 0`
5. 初始化 bitmap：`Bitmap::init(page_handle.bitmap, file_hdr_.bitmap_size)`
6. 更新 `file_hdr_.num_pages++`

**你需要写的部分**：
```cpp
PageId page_id;
page_id.fd = fd_;
Page* page = buffer_pool_manager_->new_page(&page_id);
if (page == nullptr) {
    throw InternalError("Failed to create new page");
}
RmPageHandle page_handle(&file_hdr_, page);
page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
page_handle.page_hdr->num_records = 0;
Bitmap::init(page_handle.bitmap, file_hdr_.bitmap_size);
file_hdr_.num_pages++;
return page_handle;
```

---

### TODO 5.3：create_page_handle

**位置**：`rm_file_handle.cpp` 第 114-122 行

**做什么**：获取一个有空闲 slot 的页面。如果有空闲页就用它，没有就创建新页。

**逻辑**：
1. 检查 `file_hdr_.first_free_page_no` 是否为 `RM_NO_PAGE`
2. 如果不是，说明有空闲页，用 `fetch_page_handle` 获取
3. 如果是，调用 `create_new_page_handle` 创建新页

**你需要写的部分**：
```cpp
if (file_hdr_.first_free_page_no != RM_NO_PAGE) {
    return fetch_page_handle(file_hdr_.first_free_page_no);
} else {
    return create_new_page_handle();
}
```

---

### TODO 5.4：release_page_handle

**位置**：`rm_file_handle.cpp` 第 127-133 行

**做什么**：当一个页面从"满"变成"未满"时，把它重新加入空闲页链表。

**逻辑**：
1. 把当前页的 `next_free_page_no` 设为原来的 `first_free_page_no`
2. 把 `file_hdr_.first_free_page_no` 更新为当前页的 page_no

**你需要写的部分**：
```cpp
page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
file_hdr_.first_free_page_no = page_handle.page->get_page_id().page_no;
```

**这就是一个头插法**：把当前页插到空闲链表的头部。

---

### TODO 5.5：get_record

**位置**：`rm_file_handle.cpp` 第 19-25 行

**做什么**：根据 Rid 获取一条记录。

**逻辑**：
1. 调用 `fetch_page_handle(rid.page_no)` 获取页面
2. 检查 bitmap 中对应 slot 是否有记录：`Bitmap::is_set(page_handle.bitmap, rid.slot_no)`
3. 如果没有，抛 `RecordNotFoundError`
4. 创建一个 `RmRecord`，把 slot 中的数据复制进去
5. 返回

**你需要写的部分**：
```cpp
RmPageHandle page_handle = fetch_page_handle(rid.page_no);
if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
    throw RecordNotFoundError(rid.page_no, rid.slot_no);
}
auto record = std::make_unique<RmRecord>(file_hdr_.record_size);
memcpy(record->data, page_handle.get_slot(rid.slot_no), file_hdr_.record_size);
return record;
```

**注意**：`fetch_page_handle` 会 pin 住页面，但 `get_record` 是 `const` 函数，你不能在这里 unpin。实际上测试代码会处理 unpin，或者你需要在其他地方处理。看测试代码的行为来决定。

---

### TODO 5.6：insert_record（无指定位置）

**位置**：`rm_file_handle.cpp` 第 33-42 行

**做什么**：在文件中找一个空闲 slot 插入记录。

**逻辑**：
1. 调用 `create_page_handle()` 获取一个有空位的页面
2. 用 `Bitmap::first_bit(false, ...)` 找第一个空 slot
3. 用 `memcpy` 把 `buf` 复制到 slot 中
4. 用 `Bitmap::set` 把对应 bit 置 1
5. `num_records++`
6. 如果页面满了（`num_records == num_records_per_page`），更新 `file_hdr_.first_free_page_no`
7. `unpin` 页面，标记脏
8. 返回 `Rid{page_no, slot_no}`

**你需要写的部分**：
```cpp
RmPageHandle page_handle = create_page_handle();
int slot_no = Bitmap::first_bit(false, page_handle.bitmap, file_hdr_.num_records_per_page);
memcpy(page_handle.get_slot(slot_no), buf, file_hdr_.record_size);
Bitmap::set(page_handle.bitmap, slot_no);
page_handle.page_hdr->num_records++;
if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
    file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
}
PageId page_id = {fd_, page_handle.page->get_page_id().page_no};
buffer_pool_manager_->unpin_page(page_id, true);
return Rid{page_handle.page->get_page_id().page_no, slot_no};
```

**关键点**：
- `create_page_handle()` 会 pin 住页面，所以用完后要 unpin
- `unpin_page` 的第二个参数 `true` 表示标记为脏页
- 页面满时要更新 `first_free_page_no`，把它从空闲链表中"摘掉"

---

### TODO 5.7：delete_record

**位置**：`rm_file_handle.cpp` 第 58-63 行

**做什么**：删除指定位置的记录。

**逻辑**：
1. 调用 `fetch_page_handle(rid.page_no)` 获取页面
2. 用 `Bitmap::reset` 把对应 bit 清零
3. `num_records--`
4. 如果页面从满变成未满（`num_records == num_records_per_page - 1`），调用 `release_page_handle`
5. `unpin` 页面，标记脏

**你需要写的部分**：
```cpp
RmPageHandle page_handle = fetch_page_handle(rid.page_no);
Bitmap::reset(page_handle.bitmap, rid.slot_no);
page_handle.page_hdr->num_records--;
if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page - 1) {
    release_page_handle(page_handle);
}
PageId page_id = {fd_, rid.page_no};
buffer_pool_manager_->unpin_page(page_id, true);
```

**关键点**：只有当页面**刚好**从满变成未满时才调用 `release_page_handle`。如果本来就未满，不需要调用。

---

### TODO 5.8：update_record

**位置**：`rm_file_handle.cpp` 第 72-77 行

**做什么**：更新指定位置的记录数据。

**逻辑**：
1. 调用 `fetch_page_handle(rid.page_no)` 获取页面
2. 用 `memcpy` 把 `buf` 复制到 slot 中
3. `unpin` 页面，标记脏

**你需要写的部分**：
```cpp
RmPageHandle page_handle = fetch_page_handle(rid.page_no);
memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);
PageId page_id = {fd_, rid.page_no};
buffer_pool_manager_->unpin_page(page_id, true);
```

这是最简单的，因为定长记录不需要移动其他数据。

---

### TODO 5.9：insert_record（指定位置）

**位置**：`rm_file_handle.cpp` 第 49-51 行

**做什么**：在指定的 Rid 位置插入记录（这个函数在 Lab1 测试中可能不直接调用，但为了完整性也实现它）。

**逻辑**：和无指定位置的 insert 类似，但不需要找空 slot，直接用 `rid` 指定的位置。

**你需要写的部分**：
```cpp
RmPageHandle page_handle = fetch_page_handle(rid.page_no);
memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);
Bitmap::set(page_handle.bitmap, rid.slot_no);
page_handle.page_hdr->num_records++;
PageId page_id = {fd_, rid.page_no};
buffer_pool_manager_->unpin_page(page_id, true);
```

---

### RmFileHandle 验证

```bash
make record_manager_test && ./bin/record_manager_test
```

---

## 6. RmScan：3 个 TODO

**文件**：`src/record/rm_scan.cpp`

RmScan 是一个迭代器，用于遍历文件中所有存在的记录。

---

### TODO 6.1：构造函数

**位置**：`rm_scan.cpp` 第 18-22 行

**做什么**：初始化 `file_handle_` 和 `rid_`，找到第一个存放了记录的位置。

**逻辑**：
1. 设置 `rid_` 为 `{RM_FIRST_RECORD_PAGE, -1}`（从第 1 页、slot -1 开始）
2. 调用 `next()` 找到第一个有记录的位置

**你需要写的部分**：
```cpp
rid_ = {RM_FIRST_RECORD_PAGE, -1};
next();
```

---

### TODO 6.2：next

**位置**：`rm_scan.cpp` 第 27-31 行

**做什么**：移动到下一个有记录的位置。

**逻辑**：
1. 获取当前页的 page_handle
2. 在当前页中用 `Bitmap::next_bit(true, ...)` 找下一个 bit 为 1 的 slot
3. 如果当前页没有了（`slot_no >= num_records_per_page`），跳到下一页
4. 如果下一页也超出了文件总页数，到达末尾

**你需要写的部分**：
```cpp
// 在当前页找下一个有记录的 slot
RmPageHandle page_handle = file_handle_->fetch_page_handle(rid_.page_no);
rid_.slot_no = Bitmap::next_bit(true, page_handle.bitmap,
                                file_handle_->file_hdr_.num_records_per_page,
                                rid_.slot_no);
// 如果当前页没有更多记录，跳到下一页
while (rid_.slot_no >= file_handle_->file_hdr_.num_records_per_page) {
    // unpin 当前页
    PageId page_id = {file_handle_->fd_, rid_.page_no};
    file_handle_->buffer_pool_manager_->unpin_page(page_id, false);
    // 跳到下一页
    rid_.page_no++;
    rid_.slot_no = -1;
    // 检查是否到达文件末尾
    if (is_end()) return;
    // 继续在新页中找
    page_handle = file_handle_->fetch_page_handle(rid_.page_no);
    rid_.slot_no = Bitmap::next_bit(true, page_handle.bitmap,
                                    file_handle_->file_hdr_.num_records_per_page,
                                    rid_.slot_no);
}
```

**关键点**：
- `fetch_page_handle` 会 pin 住页面，记得在跳页时 unpin
- `is_end()` 检查 `page_no >= num_pages`，如果到了末尾就直接返回
- `Bitmap::next_bit` 的第三个参数是 `curr`，它从 `curr+1` 开始找

---

### TODO 6.3：is_end

**位置**：`rm_scan.cpp` 第 36-40 行

**做什么**：判断是否到达文件末尾。

**你需要写的部分**：
```cpp
return rid_.page_no >= file_handle_->file_hdr_.num_pages;
```

---

### RmScan 验证

RmScan 的测试包含在 `record_manager_test` 中，不需要单独运行。

---

## 7. 完整测试流程

按顺序运行，每一步都通过后再做下一步：

```bash
cd build

# 第 1 步：DiskManager
make disk_manager_test && ./bin/disk_manager_test

# 第 2 步：LRU Replacer
make lru_replacer_test && ./bin/lru_replacer_test

# 第 3 步：Buffer Pool Manager
make buffer_pool_manager_test && ./bin/buffer_pool_manager_test

# 第 4 步：Record Manager（包含 RmFileHandle 和 RmScan）
make record_manager_test && ./bin/record_manager_test
```

如果某一步失败了，不要急着改后面的代码，先修好当前这一步。

---

## 8. 调试技巧

### 8.1 编译错误

- 如果报 `undeclared identifier`，检查是否包含了正确的头文件
- 如果报 `no matching function`，检查函数签名是否和声明一致
- 如果报 `undefined reference`，检查是否在 `.cpp` 文件中实现了所有函数

### 8.2 运行时错误

- **Segmentation fault**：最常见的是空指针访问。检查 `fetch_page`、`new_page` 返回的是否为 nullptr
- **Assertion failed**：检查 pin_count 是否正确增减
- **死锁**：检查是否在已经持锁的情况下调用了另一个也会加锁的函数

### 8.3 逻辑错误

- **脏页没写回**：检查 `update_page` 中是否在脏页时调用了 `write_page`
- **page_table_ 不同步**：检查 `update_page` 中是否正确删除旧映射、添加新映射
- **pin_count 不对**：检查 `fetch_page` 是否 +1，`unpin_page` 是否 -1
- **bitmap 不对**：检查 insert 时是否 set，delete 时是否 reset

### 8.4 调试命令

```bash
# 用 gdb 调试
gdb ./bin/buffer_pool_manager_test
(gdb) break buffer_pool_manager.cpp:48   # 在 fetch_page 入口打断点
(gdb) run
(gdb) print page_table_                  # 打印页表
(gdb) print pages_[0]                    # 打印第 0 帧

# 用 valgrind 检查内存泄漏
valgrind --leak-check=full ./bin/record_manager_test
```

---

## 附录：各函数速查表

| 模块 | 函数 | 文件位置 | 核心操作 |
|------|------|----------|----------|
| DiskManager | write_page | disk_manager.cpp:29 | lseek + write |
| DiskManager | read_page | disk_manager.cpp:44 | lseek + read |
| DiskManager | create_file | disk_manager.cpp:101 | open(O_CREAT) + close |
| DiskManager | open_file | disk_manager.cpp:124 | open(O_RDWR) + 更新映射 |
| DiskManager | close_file | disk_manager.cpp:135 | close + 更新映射 |
| DiskManager | destroy_file | disk_manager.cpp:111 | unlink |
| LRUReplacer | victim | lru_replacer.cpp:22 | 取 list 尾部 |
| LRUReplacer | pin | lru_replacer.cpp:38 | 从 list/hash 删除 |
| LRUReplacer | unpin | lru_replacer.cpp:49 | 插入 list 头部 |
| BPM | find_victim_page | buffer_pool_manager.cpp:18 | free_list 或 replacer |
| BPM | update_page | buffer_pool_manager.cpp:33 | 写脏页 + 更新页表 |
| BPM | fetch_page | buffer_pool_manager.cpp:48 | 查页表 → pin 或读磁盘 |
| BPM | unpin_page | buffer_pool_manager.cpp:66 | pin_count-- → unpin |
| BPM | new_page | buffer_pool_manager.cpp:100 | allocate_page + update_page |
| BPM | delete_page | buffer_pool_manager.cpp:114 | 从页表删除 + 回收帧 |
| BPM | flush_page | buffer_pool_manager.cpp:84 | 强制写回磁盘 |
| BPM | flush_all_pages | buffer_pool_manager.cpp:126 | 遍历写回指定 fd 的页 |
| RM | fetch_page_handle | rm_file_handle.cpp:87 | fetch_page + 包装 |
| RM | create_new_page_handle | rm_file_handle.cpp:99 | new_page + 初始化 |
| RM | create_page_handle | rm_file_handle.cpp:114 | 有空闲页用空闲页，否则新建 |
| RM | release_page_handle | rm_file_handle.cpp:127 | 头插法加入空闲链表 |
| RM | get_record | rm_file_handle.cpp:19 | fetch + bitmap 检查 + memcpy |
| RM | insert_record | rm_file_handle.cpp:33 | 找空 slot + memcpy + bitmap set |
| RM | delete_record | rm_file_handle.cpp:58 | bitmap reset + 可能 release |
| RM | update_record | rm_file_handle.cpp:72 | fetch + memcpy 覆盖 |
| RmScan | 构造函数 | rm_scan.cpp:18 | 从 page 1 开始 + next |
| RmScan | next | rm_scan.cpp:27 | bitmap next_bit + 跨页 |
| RmScan | is_end | rm_scan.cpp:36 | page_no >= num_pages |
