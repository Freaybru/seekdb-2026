# MiniVectorDB

单进程、单客户端的磁盘型向量数据库。使用 C++20 和 POSIX 文件接口，无第三方运行依赖。程序在 Linux x86_64 上以前台方式从 stdin 读取 SQL，经 stdout 返回结果，数据存入 `/data`。

## 构建与重启

从提交分支的仓库根目录运行。根目录的 Dockerfile 从 `extra/` 读取源码，使用固定摘要的 GCC 13.3.0 构建，最终镜像只包含静态程序和数据目录。

~~~sh
docker build --platform linux/amd64 -t minivectordb .
mkdir -p extra/.demo-data
docker run --rm -i --network none -v "$PWD/extra/.demo-data:/data" minivectordb <<'SQL'
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
docker run --rm -i --network none -v "$PWD/extra/.demo-data:/data" minivectordb <<'SQL'
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

`main.cpp` 中的文本协议用于本地验证。每行发送一条完整 SQL，可带分号。启动后收到 `READY`。成功写入返回 `OK <影响行数>`，错误返回 `ERROR <代码> <消息>`。查询返回 `RESULT <列数> <行数>`、一行列名、指定数量的数据行及 `END`。列以制表符分隔，字符串转义反斜杠、制表符、换行、回车和 NUL。每个响应立即刷新。`MODE exact` 或 `MODE ann` 切换模式，默认 exact。`QUIT` 关闭并返回 `BYE`。遇到 EOF、`SIGTERM`、`SIGINT` 或写入时发现输出读端断开，也结束会话并撤销未提交事务。关闭信号能中断等待输入和等待写入 stdout 的操作。

重复主键、不支持的 SQL 和合法向量字面量的维度不符分别返回公开题包使用的 `duplicate_key`、`unsupported_sql`、`dimension_mismatch`。格式错误、非有限值和 float32 溢出仍返回 `VALUE`。其他错误代码覆盖语法（`SYNTAX`）、表结构（`SCHEMA`）、表或列不存在（`UNKNOWN_TABLE`、`UNKNOWN_COLUMN`）、重名（`TABLE_EXISTS`、`INDEX_EXISTS`）和事务状态（`TRANSACTION`）。存储故障时进程停止，诊断写到 stderr，由下次启动按日志恢复。

## 设计

- **记录与读取**。单个版本化二进制日志保存表定义、变更和提交帧。主键在帧头，行负载按布局存放 float32 向量和标量。内存主键索引保存行版本的位置和长度。查询短日志时可用只读映射。较大日志在相邻位置预读，随机跳转时只读取当前范围。对较宽记录做主键序查询和向量 TopK 时，按长度前缀跳过与过滤无关的正文。TopK 只对满足过滤条件的记录计算距离。
- **事务与恢复**。每个行变更记录上一版本的位置，帧尾的长度用于逆向撤销。更新后行编码相同则复用原记录，影响行数仍按匹配行计。语句和事务分别记录起点，出现语义错误时撤销当前语句并截断对应日志。提交先写完整提交帧并调用 `fdatasync`，再返回成功。启动时按块读取和重放完整帧，撤销最后一个提交帧之后的完整操作并截断尾部。崩溃模型为进程被终止。
- **检索**。精确模式先过滤，再用大小为 K 的堆选邻居。Linux x86_64 可按 CPU 能力使用 AVX2，以 double 计算距离。若两条距离的误差范围重叠，使用存储的 float32 数值精确比较平方和，再按主键打破并列。`CREATE VECTOR INDEX` 为可见向量建立按第一坐标排序的投影索引。ann 模式从最接近查询坐标的位置向两侧扫描，只有能确定尚未访问的坐标距离下界严格大于第 K 名时才停止。索引状态随写入或回滚失效，按需重建。恢复时在就绪前准备索引。投影预算为 4 MiB，超出预算或投影分配失败时使用磁盘精确扫描。高维密集数据的下界可能较松，此时扫描比例接近全表。
- **资源范围**。长字符串排序只保存候选键的日志位置和长度，比较时分块读取内容。主键目录与排序元数据随记录数增长。日志保留已提交的历史版本，持续更新或删除会增加磁盘占用。实现范围限定为题目的 SQL 子集和单客户端单事务。

## 验证

在 `extra` 中运行。

~~~sh
mkdir -p .build
TMPDIR="$PWD/.build" g++ -std=c++20 -O3 -flto -static -Wall -Wextra -Wpedantic \
  sql.cpp log.cpp db.cpp main.cpp -o .build/minivectordb
python3 verify.py .build/minivectordb --benchmark
~~~

验证覆盖 Unicode 和 float32 边界、带过滤条件的 TopK、极端数值下的距离排序、索引维护、语句原子性、提交和回滚、强杀及重复恢复。输出阻塞用例确认写满 stdout 或读端断开时，进程仍能退出并撤销已经写入日志的未提交修改。限内存用例在 14 MiB 进程地址空间内恢复 100,000 条带索引记录，并核对 exact 和 ann 查询。另一组在 18 MiB 地址空间内重启并读取超过 36 MiB 的乱序日志，验证 TopK、长字符串排序、排序并列及事务内的变更。使用 Linux `/proc/<pid>/io` 的 `rchar` 检查 TopK 的进程读取量。可选基准写入 10,000 条 128 维记录、执行 1,000 个 Top10 查询，对返回距离逐条校验，并用独立穷举抽查两组排名。

可以从官方快照提取 `mysqltest/`，在同一目录复跑 20 组、95 条公开 SQL。

~~~sh
git init --bare .build/official.git
git --git-dir=.build/official.git fetch --depth=1 --no-tags \
  https://github.com/oceanbase/seekdb.git 9f6df2ba5acf871376e22b64a3d0724e4b575044
git --git-dir=.build/official.git archive FETCH_HEAD mysqltest | tar -x -C .build
python3 verify.py .build/minivectordb --public-suite .build/mysqltest
~~~

自测为每组创建独立数据目录，执行 `--restart` 时复用该目录，执行 `--mode ann` 后切换检索模式。按清单逐条检查成功状态、错误语义名、影响行数、列名和结果行。也支持早期清单省略文件路径的题包。无序结果按多重集合比较，浮点距离使用绝对和相对容差 `1e-5`。公开包的 JSON 文件描述预期结果。本地测试经上述文本协议执行，正式通信格式以组织方配套驱动为准。[官方公开用例说明](https://github.com/oceanbase/seekdb/tree/9f6df2ba5acf871376e22b64a3d0724e4b575044/mysqltest)

提交的构建入口为仓库根目录的 `Dockerfile`，学习记录为 `agent/conversation.json`。
