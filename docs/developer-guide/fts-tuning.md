# 赛题四：全文索引（FTS）性能调优记录

分支 `grok-bot`。评分依据 `tools/benchmark/fts_large_bench*`（未改动，字节一致）。
本机比 CI 快约 5 倍，因此用本机基线 `fts-baseline-local.txt` 计算本地分数：
`score = 100 * min(max(overall, 0), 0.5) / 0.5`，overall = build / tokenize / query 三类平均提升的均值。

## 改动（均不改变 TOKENIZE / MATCH 语义，不跳过工作量，不针对 benchmark 硬编码）

1. **按文档的无锁分词词频表**（`ObFTWordMap`）：一个文档内的 map 只被一个线程访问，去掉加锁；空 map 不再 clear。
2. **词典加载读锁快路径**（`ObFTDictHub` / loader）：已加载时只走读锁。
3. **range dict 内联二进制比较**（`ObFTRangeDict::match`）。
4. **`ObDatumStrCmpImpl::cmp` 快路径**：二进制排序规则直接 memcmp + 长度差；字节完全相同直接返回 0。
   build 阶段排序（word, doc_id）大量命中此路径。
5. **TOKENIZE 参数 memo**（`ob_expr_tokenize.cpp`）：解析器名 → 内部名、(内部名, 空属性) → 规范化属性，
   在线程本地缓存上一条结果（纯函数，命中与重算完全等价）。
6. **DAT 字符码表 `ObArrayHashMap`**：用 5 字节内定长字符的廉价混合哈希 + 乘法区间归约替代 murmurhash + 64 位取模；
   表仅存在内存（KVCache），布局可自由变化。
7. **utf8mb4_general_ci 比较的 ASCII 快路径**（`ob_strnncollsp_utf8mb4`）：两侧都是 ASCII 时跳过 mb_wc 解码，
   权重仍经 `ob_tosort_unicode`，结果与原实现一致。

## 正确性

- `ref/gen_ref.sh`：TOKENIZE（ik/beng/space/ngram/ngram2 × 6 文本 × 属性）与 MATCH（计数、Top50 分数、全量 id+分数 MD5）
  输出与基线二进制在新装载数据上 **逐字节一致**（v3、v4 均 == base2.clean）。
- mysqltest：fts_index(15) / ai_column(5) / ai_function(6) / file_sql(5) 全部 PASS；
  扩展集 executor groupby subquery datatype window_function delete update expr sys_vars：
  仅 `update.update2` 失败，在基线二进制上同样失败（既有问题，与本改动无关）。

## 本地结果（本机基线 vs 当前）

见 progress.md / `fts-runs/*.score`。v4 两次运行：local_score 27.6 / 34.7；v3 约 13–27（噪声大）。

## 没有收益 / 放弃的方向

- DDL 并行度 hint、工作区百分比：build 无变化。
- 强制 encode-sortkey tracepoint：无效。
- 查询跳过相关性计算：legacy TR merge iter 无无相关性路径（SIGSEGV），且会改变行为，放弃。

## 注意

- 测试时只能有一个 benchmark / 回归在共享集群上运行（两个 bench 会互相 DROP DATABASE，造成 ERROR 1191 / 表不存在，
  已证实不是代码竞态）。
- 数值有 ±5~10% 噪声；以多次运行的中位数为准。
