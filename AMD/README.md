# AMD CAGRA 构建与查询优化

本分支以比赛 V5 冻结提交 `634888d9b037153fa143d2c913ba4b25503e1d62` 为直接祖先。
实现覆盖先导入数据、再执行 `CREATE VECTOR INDEX ... lib=cuvs` 的离线 DDL 路径。
题目及评分规则以[比赛说明](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/seebnbaoxot1b0tb)为准。

## 实现

生产改动仅在向量适配层和 hipVS bridge：

- GPU NN-descent 使用 5 次迭代、中间图度数 128、最终图度数 64，减少近邻细化、图裁剪和持久化工作量。
- 查询始终调用平台原生 `cuvsCagraSearch`，使用 `MULTI_CTA`、`itopk_size=max(4096,4*k)`、`search_width=4`。
  更宽的 GPU 搜索补足图构建预算减少后的召回。没有替换或修改 hipVS 搜索实现。
- 每个索引复用搜索参数和查询工作区，最多保留 8 MiB；较大的批次临时分配并及时释放。
  同一资源流完成输入复制、搜索、输出复制及同步，错误路径也等待未完成的 GPU 工作。
- 复用一个 32 MiB 栈的 GPU 工作线程，避免每条 SQL 创建和销毁 pthread/HIP 线程状态。
  作业同步返回，沿用索引注册表、生命周期和新鲜度保护。

所有配置对正常建图、首查、后续查询和重启重载一致生效，不按数据集、查询内容或评测阶段切换实现。
FP32 输入、L2 距离、外部 ID、SQL 参数和 `OBCUVS02` 快照格式保持兼容。
复用的是同一存活索引的查询工作区；每轮测量都从无索引的新数据库开始，不复用上一轮图或查询结果。
`lib=vsag` 继续走原来的 CPU 路径。

## 验证与性能报告

前一版提交 `4784cc2eb586813f08c07cd990e6965f2bfa6162` 的正式平台结果为 **18.166 / 20**，
`build=2.598x, query=4.582x, gates=pass`（2026-11-04 13:11）。
本次减少中间图与迭代预算，保留相同的宽搜索配置。以下是最终候选的本地验证，生成报告时尚未提测本次候选。

实测日期：2026-11-04。AMD `gfx1100`、约 48 GiB 显存、ROCm 7.2.4、CMake 3.31.10、Rust 1.97.1。
使用预装 hipVS 和冻结第三方依赖，按题目规定命令完成源码编译，32 路并发、Cargo 离线运行。
测试服务限制为 24 个 CPU 槽、16 GiB 配置内存；显存以同一设备 sysfs 计数器采样，进程内存采样 `VmHWM`，间隔 50 ms。

数据包含公开 SIFT1M 的前 250,000 条 128 维向量及 4,096 条独立查询，
以及独立生成的 250,000 × 256 FP32 高斯向量和 4,096 条查询，种子 `20261104`。
高斯数据是压力测试，**不是 Hidden250K**。另用 250K × 128 高斯数据检查维度变化后的召回。
真值针对实际选取的 250K 向量重新穷举计算，采用 FP32 平方 L2；GPU 路径关闭 reduced-precision matmul。

### 本次候选与前一版配对 SQL

这里的 reference 是前一版已提测源码，**不是 V5 冻结基线**，表中加速比不能直接换算为正式分数。
配对使用相同源码编译的服务程序，分别加载两个 bridge，核对实际动态链接库及哈希。
最终候选源码重新按固定流程编译、重链，并另外完成 64 条查询的功能、并发与重启验证。

每次测量均从无索引的新数据库开始。数据准备、导入、启动和编译不计入构建时间。
构建从固定 DDL 开始计时，直到第一条近似查询返回 10 个合法且互不重复的 ID。
Top-10/100 分别执行 4,096 条单行近似 SQL；查询加速是两组 QPS 比值的几何平均。
SIFT 按 reference → candidate、Gaussian 按 candidate → reference 运行。
外部主键非连续且包含负数，检查真实 SQL ID 映射。

| 数据 | 前一版构建含首查 | 本次构建含首查 | 相对前一版构建加速 | 查询比值 | 本次 Recall@10 / @100 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 公开 SIFT250K × 128 | 7.132 s | 5.387 s | **1.324×** | 0.998× | 0.999927 / 0.999963 |
| 自生成 Gaussian250K × 256 | 9.242 s | 7.072 s | **1.307×** | 0.986× | 0.985962 / 0.971660 |

全部主测查询返回完整、合法、互不重复的 ID。两组相对前一版的召回变化均在 0.005 以内。
另外对三组数据运行原生接口的 4,096 条单次查询；本次召回均不低于同轮 V5 bridge 召回减 0.005。
最终正式的 Hidden250K 召回、同轮 V5 SQL 加速比和组织方 GPU 路径抽检，以平台评测为准。

| 数据 | 前一版采样显存峰值 | 本次采样显存峰值 | 本次进程内存峰值 |
| --- | ---: | ---: | ---: |
| 公开 SIFT250K × 128 | 634.99 MiB | 634.79 MiB | 2060.80 MiB |
| 自生成 Gaussian250K × 256 | 757.71 MiB | 757.50 MiB | 2936.46 MiB |

同一公开 SIFT250K 上，CPU `lib=vsag` 构建含首查为 **80.819 s**，
GPU 为 **5.387 s**，约 **15.001×**。
CPU 同样完成真实 SQL 建索引、合法 ID 检查及重启重载；其 64 条查询仅作功能诊断，不计入 GPU 查询性能对比。

GPU 回归通过工作区扩容、超出缓存上限的批次、4 线程查询、FP32 距离、非法参数、快照重载及 256/512/1024 行小索引。
本地 profiler 记录到 1,524 次 kernel dispatch，其中 64 次 NN-descent、
334 次 multi-CTA search，另有 140 次内存复制。
每组配对 SQL 在计时结束后另用 4 客户端并发执行 128 条查询，结果一致；日志确认 GPU 快照生成与重载，未观察到 CPU fallback。

精简指标、输入哈希、二进制/库哈希、正式前一版结果和验证范围见 [results.json](results.json)。

## 从源码构建

在本分支仓库根目录执行。`deps/3rd` 和离线 Rust vendor 按比赛编译环境预置，不引入新的生产依赖。

```bash
mkdir -p build_release/amd/bridge
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  tools/hipvs_bridge/build.sh "$PWD/build_release/amd/bridge/libseekdb_cuvs_bridge.so"

cmake -S . -B build_release -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOB_ENABLE_UNITY=ON \
  -DOB_USE_LLD=ON \
  -DOB_BUILD_CUVS=ON \
  -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB="$PWD/build_release/amd/bridge/libseekdb_cuvs_bridge.so"
CMAKE_BUILD_PARALLEL_LEVEL=32 cmake --build build_release --target seekdb
```

所有下面的运行产物均放入已被 Git 忽略的 `build_release/amd`。不要把数据、运行目录、日志、profile 原始文件或二进制提交到分支。

## 准备测试数据

仅测试脚本需要 NumPy、PyMySQL；这些包不参与生产源码构建。
实测使用 NumPy 2.4.6、PyMySQL 1.2.3。可在准备阶段安装到隔离目录：

```bash
python3 -m pip install --target build_release/amd/python-deps numpy==2.4.6 PyMySQL==1.2.3
export PYTHONPATH="$PWD/build_release/amd/python-deps${PYTHONPATH:+:$PYTHONPATH}"
mkdir -p build_release/amd/data
```

SIFT 原始数据参见 [TEXMEX](http://corpus-texmex.irisa.fr/) 和[公开镜像](https://huggingface.co/datasets/qbo-odp/sift1m)。
若原始站点无法连接，可从镜像取所需前缀：

```bash
curl -L --fail --range 0-128999999 \
  'https://hf-mirror.com/datasets/qbo-odp/sift1m/resolve/main/sift_base.fvecs?download=true' \
  -o build_release/amd/data/sift_base.fvecs
curl -L --fail --range 0-2113535 \
  'https://hf-mirror.com/datasets/qbo-odp/sift1m/resolve/main/sift_query.fvecs?download=true' \
  -o build_release/amd/data/sift_query.fvecs

python3 AMD/prepare_data.py --output build_release/amd/data/sift250k \
  --sift-base build_release/amd/data/sift_base.fvecs \
  --sift-queries build_release/amd/data/sift_query.fvecs
python3 AMD/prepare_data.py --output build_release/amd/data/gaussian256 \
  --kind gaussian --rows 250000 --dim 256 --nq 4096 --seed 20261104
```

默认用 CPU 穷举真值。若准备环境已有 ROCm PyTorch，可加 `--truth-device cuda`；实测大规模数据使用此路径。
`cuda` 是 PyTorch 的设备 API 名称，在该平台上实际由 ROCm 执行。
准备脚本拒绝覆盖既有输出目录，并生成 `metadata.json` 记录维度、规模和文件哈希。

## SQL 配对测试

先在另一个隔离源码目录检出 V5 冻结 commit，用上面的相同命令构建 reference。
以下用环境变量指向两个真实源码构建产物；`REFERENCE_BIN`、`REFERENCE_BRIDGE` 由运行者设置。
不要用模板旧版预编译 seekdb 代替 V5 reference。

```bash
export CANDIDATE_BIN="$PWD/build_release/src/observer/seekdb"
export CANDIDATE_BRIDGE="$PWD/build_release/amd/bridge/libseekdb_cuvs_bridge.so"
python3 AMD/benchmark_sql.py --binary "$REFERENCE_BIN" --bridge "$REFERENCE_BRIDGE" \
  --data build_release/amd/data/sift250k --work build_release/amd/runs/sift-baseline \
  --output build_release/amd/results/sift-baseline.json --port 2981 --nq 4096 --restart
python3 AMD/benchmark_sql.py --binary "$CANDIDATE_BIN" --bridge "$CANDIDATE_BRIDGE" \
  --data build_release/amd/data/sift250k --work build_release/amd/runs/sift-candidate \
  --output build_release/amd/results/sift-candidate.json --port 2981 --nq 4096 --restart --concurrent 4
python3 AMD/benchmark_sql.py --binary "$CANDIDATE_BIN" --bridge "$CANDIDATE_BRIDGE" \
  --data build_release/amd/data/gaussian256 --work build_release/amd/runs/gaussian-candidate \
  --output build_release/amd/results/gaussian-candidate.json --port 2981 --nq 4096 --restart
python3 AMD/benchmark_sql.py --binary "$REFERENCE_BIN" --bridge "$REFERENCE_BRIDGE" \
  --data build_release/amd/data/gaussian256 --work build_release/amd/runs/gaussian-baseline \
  --output build_release/amd/results/gaussian-baseline.json --port 2981 --nq 4096 --restart
python3 AMD/benchmark_sql.py --binary "$CANDIDATE_BIN" --bridge "$CANDIDATE_BRIDGE" \
  --data build_release/amd/data/sift250k --work build_release/amd/runs/cpu-sift \
  --output build_release/amd/results/cpu-sift.json --backend vsag --port 2981 --nq 64 --restart
```

运行时需要 Linux 的 `taskset`、`rocm-smi` 和可访问的 GPU sysfs 计数器。
脚本只启动和停止自己创建的进程，拒绝覆盖旧运行目录。
并发回归在 QPS 计时之后运行，不混入顺序查询 QPS。
资源峰值包含该次运行的启动、建索引、查询和重启阶段。

## GPU 回归与 profiler

```bash
g++ -O2 -std=c++17 -pthread -I . \
  unittest/oblib/lib/vector/test_cuvs_bridge_workspace.cpp \
  -L build_release/amd/bridge -Wl,-rpath,"$PWD/build_release/amd/bridge" \
  -lseekdb_cuvs_bridge -o build_release/amd/test-workspace
build_release/amd/test-workspace build_release/amd/workspace.snapshot
rocprofv3 --kernel-trace --memory-copy-trace --output-format csv \
  -d build_release/amd/profile -o cagra -- \
  build_release/amd/test-workspace build_release/amd/profile.snapshot
```

回归检查工作区扩容、超出缓存上限的批次、并发查询、FP32 距离、非法参数、快照重载以及 256/512/1024 行小索引。
本地 profiler 的汇总只用于确认真实 GPU kernel 执行，不能代替组织方正式抽检。

## 提测

提测应明确选择本分支 `amd-cagra-submit` 或其完整 commit。
正式评测还需满足题目的私有答题仓库及只读令牌登记要求；令牌不得写入源码或提交记录。
本次不调整仓库可见性、默认分支及其他仓库属性，也不修改其他现有分支。
