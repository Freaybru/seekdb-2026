# MiniVectorDB

单进程、单客户端的磁盘型向量数据库。使用 C++20 和 POSIX 文件接口，无第三方运行依赖。程序在 Linux x86_64 上以前台方式从 stdin 读取 SQL，经 stdout 返回结果，数据存入 `/data`。

## 构建与重启

从提交分支的仓库根目录运行。根目录的 Dockerfile 从 `extra/` 读取源码，使用固定摘要的 GCC 13.3.0 构建，最终镜像只包含静态程序和数据目录。镜像默认使用官方 JSONL 协议。下面的手动 SQL 示例显式选择逐行文本模式。

~~~sh
docker build --platform linux/amd64 -t minivectordb .
mkdir -p extra/.demo-data
docker run --rm -i --network none -v "$PWD/extra/.demo-data:/data" minivectordb --input=line <<'SQL'
CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(16), embedding VECTOR(2));
INSERT INTO docs VALUES (1,'db','[1,0]'),(2,'db','[0,1]'),(3,'other','[0,0]');
SELECT id, category FROM docs WHERE id >= 2 ORDER BY id;
CREATE VECTOR INDEX docs_vec ON docs(embedding);
SELECT id, L2_DISTANCE(embedding,'[0,0]') AS d FROM docs WHERE category='db' ORDER BY d LIMIT 2;
BEGIN;
UPDATE docs SET embedding='[0,0]' WHERE id=2;
SELECT id FROM docs WHERE category='db' ORDER BY L2_DISTANCE(embedding,'[0,0]') LIMIT 1;
ROLLBACK;
BEGIN;
DELETE FROM docs WHERE id=3;
COMMIT;
QUIT
SQL
docker run --rm -i --network none -v "$PWD/extra/.demo-data:/data" minivectordb --input=line <<'SQL'
SELECT * FROM docs ORDER BY id;
QUIT
SQL
~~~

事务内最近的一行是 `id=2`。回滚后它恢复为 `[0,1]`，提交的删除在重启后仍生效，最终保留 `id=1` 和 `id=2`。

## SQL 与本地交互

- 支持 `CREATE TABLE`，含一个 `BIGINT` 主键、`VARCHAR(n)` 和至多一个 `VECTOR(n)`。主键可在列定义内声明，也可用表级 `PRIMARY KEY(id)` 声明。
- 支持单行和多行 `INSERT`、`UPDATE`、`DELETE`。列清单形式的 `INSERT` 须提供全部列。主键唯一，禁止更新主键。
- 支持 `SELECT *` 或列投影、`WHERE` 中的 `= < <= > >=` 与 `AND`、`ORDER BY`、`LIMIT`。同一排序键值下按主键升序；省略排序时按主键升序扫描。
- `VARCHAR(n)` 按 Unicode 码点计长和比较。向量常量可写成 `'[1,2]'`，入库时转换为有限的 float32。`L2_DISTANCE` 以存储值计算欧氏距离，TopK 在满足过滤条件的行内计算。
- 支持 `BEGIN`、`COMMIT`、`ROLLBACK`。写语句默认自动提交，显式事务内的成功修改立即可见，失败语句只撤回本条语句。

默认 JSONL 模式在恢复后输出 `{"type":"ready"}`。每行接收一个含 `id` 和 `op` 的 JSON 对象，执行请求还含 `sql`，可通过 `search_mode` 指定 exact 或 ann。响应保留请求 ID 的类型和值，返回布尔 `ok` 和对应的查询结果、影响行数或错误。BIGINT 输出为 JSON 整数，向量输出为数组。收到 shutdown 时撤销未提交事务，发送一次成功响应后退出。每条响应换行并立即刷新，诊断信息写入 stderr。[官方 stdin/stdout 协议](https://github.com/oceanbase/seekdb/blob/f4d4e26653af823a7601668802f80e374ad6643c/STDIO_PROTOCOL.md)

本地可以用 `--input=line` 每行发送一条完整 SQL。启动后收到 `READY`。成功写入返回 `OK <影响行数>`，错误返回 `ERROR <代码> <消息>`，查询返回 `RESULT <列数> <行数>`、以制表符分隔的列名和数据行及 `END`。`MODE exact` 或 `MODE ann` 切换模式，`QUIT` 关闭并返回 `BYE`。用 `--input=sql` 可以执行多行脚本，分号位于字符串、向量常量、反引号标识符和注释之外时结束请求。启动参数接受 `--data-dir /data` 和 `--data-dir=/data`。两类输入遇到 EOF、关闭信号或 stdout 读端断开时均会结束会话并撤销未提交事务。

重复主键、不支持的 SQL 和合法向量字面量的维度不符分别返回公开题包使用的 `duplicate_key`、`unsupported_sql`、`dimension_mismatch`。格式错误、非有限值和 float32 溢出仍返回 `VALUE`。其他错误代码覆盖语法（`SYNTAX`）、表结构（`SCHEMA`）、表或列不存在（`UNKNOWN_TABLE`、`UNKNOWN_COLUMN`）、重名（`TABLE_EXISTS`、`INDEX_EXISTS`）和事务状态（`TRANSACTION`）。存储故障时进程停止，诊断写到 stderr，由下次启动按日志恢复。

## 设计

- **记录与读取**。单个版本化二进制日志保存表定义、变更和提交帧。主键在帧头，行负载按布局存放 float32 向量和标量。主键目录在内存中按键排序分组，每组连续保存多条行位置，插入和删除可分裂或合并分组。查询和构建索引时可使用最多 256 MiB 的只读文件映射；存在进程地址空间上限时，映射预算最多为该上限的四分之一。开始修改和事务操作前释放映射，避免占用后续分配空间。超出预算或映射失败时，在相邻位置预读，随机跳转时只读取当前范围。对较宽记录做主键序查询和向量 TopK 时，按长度前缀跳过与过滤无关的正文。TopK 只对满足过滤条件的候选计算原向量的距离。
- **事务与恢复**。每个行变更记录上一版本的位置，帧尾的长度用于逆向撤销。更新后行编码相同则复用原记录，影响行数仍按匹配行计。语句和事务分别记录起点，出现语义错误时撤销当前语句并截断对应日志。提交先写完整提交帧并调用 `fdatasync`，再返回成功。启动扫描日志，仅在读到完整提交帧后重放对应变更。启动缓冲最多保留 128 个帧头，较长的已提交事务按需再次读取剩余帧。无提交帧的尾部直接截断。日志格式保持兼容，崩溃模型为进程被终止。
- **检索**。大小为 K 的堆只接收符合过滤条件的候选。支持 AVX2 时，事务外的中高维精确 TopK 可以使用最多 2 MiB 的共享候选缓存，按表保存向量的 8 位量化码及行位置。查询使用三角不等式，将量化误差加入当前第 K 名的半径；只有整数距离严格超过向上取整的界限才跳过该行。保留的候选仍读取磁盘原向量。修改或回滚会使缓存失效，预算不足、查询超出量化范围或处于事务中时直接扫描。对于 32 至 4096 维、升序且有 LIMIT 的扫描，另有 float32 平方和的保守下界，溢出或接近边界时用 double 计算。若两条 double 距离的误差范围重叠，使用存储的 float32 数值精确比较平方和，再按主键打破并列。返回距离仍来自 double 平方和。`CREATE VECTOR INDEX` 为可见向量建立按第一坐标排序的投影索引。ann 模式从最接近查询坐标的位置向两侧扫描，只有能确定尚未访问的坐标距离下界严格大于第 K 名时才停止。索引状态随写入或回滚失效，按需重建。恢复时在就绪前准备索引。投影预算为 4 MiB，超出预算或投影分配失败时使用磁盘精确扫描。
- **输入解析**。JSON 与 SQL 的无转义字符串按连续字节复制；遇到转义字符仍逐项解码。向量的十进制分量直接转换为 float32，保留次正规数、溢出和非法字面量的检查。写入仍以同样的日志格式提交和同步。
- **资源范围**。长字符串排序只保存候选键的日志位置和长度，比较时分块读取内容。主键目录与排序元数据随记录数增长。日志保留已提交的历史版本，持续更新或删除会增加磁盘占用。实现范围限定为题目的 SQL 子集和单客户端单事务。

精确 TopK 遇到单列等值过滤时，可在同一缓存预算内为一个列建立候选分组。分组最多保留 32 个值的指纹和对应的行序号；候选仍检查原谓词，哈希碰撞不会改变结果。高基数列、宽行或预算不足时继续使用原扫描。建立向量索引时先映射已提交的日志前缀，再写入索引定义，减少乱序主键引起的零散读取。

## 验证

在 `extra` 中运行。

~~~sh
mkdir -p .build
TMPDIR="$PWD/.build" g++ -std=c++20 -O3 -flto -static -Wall -Wextra -Wpedantic \
  sql.cpp log.cpp db.cpp main.cpp -o .build/minivectordb
python3 verify.py .build/minivectordb --benchmark
~~~

验证覆盖 Unicode 和 float32 边界、带过滤条件的 TopK、极端数值下的距离排序、索引维护、语句原子性、提交和回滚、强杀及重复恢复。两组剪枝测试用 Python 整数精确平方和校验排名，覆盖 31 至 4097 维、溢出、下溢、并列、量化范围外的查询，以及更新、失败写入、回滚和重启后的缓存失效。输出阻塞用例确认写满 stdout 或读端断开时，进程仍能退出并撤销已经写入日志的未提交修改。限内存用例在 14 MiB 进程地址空间内恢复 100,000 条带索引记录，并核对 exact 和 ann 查询。另一组在 18 MiB 地址空间内重启并读取超过 36 MiB 的乱序日志，验证 TopK、长字符串排序、排序并列及事务内的变更。使用 Linux `/proc/<pid>/io` 的 `rchar` 检查 TopK 的进程读取量。可选基准写入 10,000 条 128 维记录、执行 1,000 个 Top10 查询，对返回距离逐条校验，并用独立穷举抽查两组排名。

等值过滤回归额外检查 Unicode 分组、主键范围内无匹配记录、高基数回退，以及更改过滤列后旧分组失效。

加上 `--stress` 会运行两组大数据恢复测试。在 26 MiB 地址空间内恢复 600,000 条窄行，核对主键范围与两种检索模式。在 9 MiB 地址空间内重启两次，检查约 16 MB 的未提交日志尾部已截断且已提交数据完整。常规自测还覆盖多行输入、批量删除造成的分组合并及跨分组回滚。

可以从官方快照提取 `mysqltest/`，在同一目录复跑 20 组、95 条公开 SQL。

~~~sh
git init --bare .build/official.git
git --git-dir=.build/official.git fetch --depth=1 --no-tags \
  https://github.com/oceanbase/seekdb.git f4d4e26653af823a7601668802f80e374ad6643c
git --git-dir=.build/official.git archive FETCH_HEAD mysqltest protocol_check STDIO_PROTOCOL.md | tar -x -C .build
python3 verify.py .build/minivectordb --public-suite .build/mysqltest
TMPDIR="$PWD/.build" python3 .build/protocol_check/run.py --image minivectordb \
  --output .build/official-protocol-results
~~~

自测为每组创建独立数据目录，执行 `--restart` 时复用该目录，执行 `--mode ann` 后切换检索模式。按清单检查成功状态、错误语义名、影响行数、列名和结果行。也支持早期清单省略文件路径的题包。无序结果按多重集合比较，浮点距离使用绝对和相对容差 `1e-5`。官方检查器使用镜像默认的 JSONL 协议，输出目录须为新目录。[官方公开用例说明](https://github.com/oceanbase/seekdb/tree/f4d4e26653af823a7601668802f80e374ad6643c/mysqltest)

提交的构建入口为仓库根目录的 `Dockerfile`，学习记录为 `agent/conversation.json`。
