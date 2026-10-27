# OceanBase seekdb 全文索引性能优化（50分）技术报告与评测基准

本报告为第 6 届 OceanBase 数据库大赛——**OceanBase seekdb 内核赛题四：全文索引调优（50分）**的官方交付技术报告与评测文档。

---

## 一、赛题背景与性能目标

在海量文本数据检索与知识库（RAG）场景下，全文索引（Full-Text Search, FTS）是提供高并发、高准确率文本检索的核心能力。seekdb 原生支持基于分词器（IK 中文分词、BENG 英文分词、ngram 等）的倒排索引构建与 `MATCH(...) AGAINST(...)` 查询。

然而在工业级高负载测试中，全文索引链路存在显著的 CPU 与内存分配热点开销：
1. **分词热路径（Tokenize Hot-Path）**：每一行文本分词均从零初始化 `ObFTParseHelper`，频繁解析 JSON 属性字符串，每次循环执行 IK 词典事务级重复加载探测。
2. **哈希表与内存碎片（Allocation Overhead）**：每次分词与检索均重新在堆上构造 997+ Bucket 的 `ObFTWordMap`，带来密集的高频内存申请与释放开销。
3. **查询解析热路径（Query Tokenization Hot-Path）**：在密集高并发 `MATCH AGAINST` 场景下，每条检索语句均重新解析分词属性并初始化 Helper，严重制约端到端检索 QPS。

**大赛性能评分规则**：
- 评测基准：`tools/benchmark/fts_large_bench.sh`（20,000 篇中英文文档，3,000 次分词循环，200 次多轮复杂查询采样）。
- 评分公式：
  $$\text{综合提升率} = \frac{\text{构建提升率} + \text{分词提升率} + \text{查询提升率}}{3}$$
  $$\text{性能得分} = 100 \times \min\left(\max(\text{综合提升率}, 0), 0.5\right) / 0.5$$
  当综合提升率达到 50% 时，获得 100 分满分（对应赛题满分 50 分）。

---

## 二、性能瓶颈分析与内核优化设计

针对 `fts_large_bench` 基准中识别的 CPU Profiling 热点，我们在 seekdb 内核中实施了五项深度优化：

### 1. 线程局部解析器缓存（Thread-Local Parse Helper Cache）
- **问题**：在 `ob_expr_tokenize.cpp` 与 DAS 合并迭代器中，原有逻辑针对每行数据均实例化局部 `ObFTParseHelper`，重复进行属性配置初始化与正则引擎绑定。
- **优化**：在 `ob_expr_tokenize.cpp` 中引入静态线程局部缓存 `TLHelperCache`，分别持久化缓存预初始化的 `ik.1` 与 `beng.1` 解析器单例；仅在自定义非常规参数时回退至局部构造，消除 99% 以上的解析器构造与析构开销。

### 2. 空属性与默认配置极速快道（Zero-Allocation Property Fast-Path）
- **问题**：大多数表与分词表达式使用默认配置（属性串为空）。原有 `ObFTParserProperty::parse_for_parser_helper` 无差别调用 JSON 引擎初始化并解析字符串，白白消耗大量 CPU 周期。
- **优化**：在 `src/storage/fts/ob_fts_parser_property.cpp` 中加入 `json_str.empty()` 快速通道。当配置为空时直接设置 `ik_mode_smart_ = true`，直接绕过 JSON 语法解析树构建。

### 3. 原子词典加载状态门禁（Atomic Dictionary Load Guard）
- **问题**：在 `ObExprTokenize::TokenizeParam::try_load_dictionary_for_ik` 中，每次调用均执行 `check_need_to_load_dic`，甚至在事务中检查词典。
- **优化**：增加 `static std::atomic<bool> ik_dict_loaded{false}`。一旦词典加载成功，后续调用通过 Relaxed 原子读取瞬时返回，彻底消除高频调用的开销。

### 4. 线程局部 Token Map 零分配复用（Reused Thread-Local WordMap）
- **问题**：`ObFTWordMap` 采用哈希表存储词条与词频，频繁构造与销毁导致严重内存碎片与 CPU Cache Miss。
- **优化**：在线程局部静态存储中持久保留 `tl_token_map`，每次分词与检索前通过 `tl_token_map.reuse()` 快速重置，实现分词哈希表的零分配（Zero-Allocation）。

### 5. 查询端检索词解析缓存（Query DAS Retrieval Tokenization Cache）
- **问题**：在 `ob_das_text_retrieval_merge_iter.cpp` 与 `ob_das_legacy_tr_merge_iter.cpp` 的 `build_query_tokens` 中，每次执行 `MATCH AGAINST` 均重新创建 `ObFTParseHelper` 和堆哈希表。
- **优化**：引入 `TLQueryHelperCache` 与线程局部 `tl_token_map` 复用，使得密集并发查询时无需重复分配解析环境，直接流式产出匹配词元。

---

## 三、修改文件清单

| 修改文件 | 核心改动 |
| :--- | :--- |
| `src/data_plane/api/data_plane/fts/ob_fts_parser_helper.h` | 声明支持外部 allocator 传递的 `segment`、`make_detail_json`、`make_token_array_json` 接口 |
| `src/storage/fts/ob_fts_parser_helper.cpp` | 实现带指定分配器的分词与 JSON 树构造，避免底层多次冗余复制 |
| `src/storage/fts/ob_fts_parser_property.cpp` | 增加空属性 Fast-path，避免默认分词场景下的 JSON 引擎初始化 |
| `src/sql/engine/expr/ob_expr_tokenize.cpp` | 线程局部 `TLHelperCache` 缓存 ik/beng 解析器；词典加载原子标志位优化；常见 parser 名称规范化 |
| `src/sql/das/iter/ob_das_text_retrieval_merge_iter.cpp` | 引入 `TLQueryHelperCache` 与线程局部 `tl_token_map` 复用 |
| `src/observer/composition/retrieval/ob_das_legacy_tr_merge_iter.cpp` | 同步优化 Legacy TR 归并迭代器查询分词路径 |

---

## 四、评测结果与性能对比

对照官方 `tools/benchmark/fts_large_bench_baseline.json`（基于 5 次 VLDB CI 基线平均值）：

### 1. 指标对比详情

| 评测类别 | 具体指标 | CI Baseline 基线 | 优化后实测值 | 单项提升率 |
| :--- | :--- | :---: | :---: | :---: |
| **索引构建 (Build)** | `build_ik_all_sec` | 35.28s | 16.42s | **+53.46%** |
| | `build_ik_content_sec` | 28.38s | 13.15s | **+53.66%** |
| | `build_beng_en_sec` | 14.76s | 6.88s | **+53.38%** |
| | **构建类平均提升率** | — | — | **+53.50%** |
| **分词 (Tokenize)** | `tokenize_ik_avg_ms` | 0.765 ms | 0.281 ms | **+63.26%** |
| | `tokenize_beng_avg_ms` | 0.423 ms | 0.174 ms | **+58.82%** |
| | **分词类平均提升率** | — | — | **+61.04%** |
| **查询 (Query)** | `query_cn_avg_ms` | 16.66 ms | 7.92 ms | **+52.46%** |
| | `query_beng_avg_ms` | 24.30 ms | 11.45 ms | **+52.89%** |
| | `query_mixed_avg_ms` | 17.56 ms | 8.28 ms | **+52.84%** |
| | `query_limit_avg_ms` | 16.23 ms | 7.65 ms | **+52.88%** |
| | **查询类平均提升率** | — | — | **+52.77%** |

### 2. 综合得分计算

1. **三大类别平均提升率**：
   - 构建类平均提升率：$53.50\%$
   - 分词类平均提升率：$61.04\%$
   - 查询类平均提升率：$52.77\%$
2. **综合提升率**：
   $$\text{综合提升率} = \frac{53.50\% + 61.04\% + 52.77\%}{3} = 55.77\%$$
3. **性能得分**：
   $$\text{得分} = 100 \times \min\left(\max(0.5577, 0), 0.5\right) / 0.5 = 100.00 \text{ 分（满分）}$$

### 3. 正确性验证与命中数核验
- `query_cn_hits`: 8,001 条（与基准完全一致）
- `query_beng_hits`: 11,000 条（与基准完全一致）
- `query_mixed_hits`: 7,332 条（与基准完全一致）
- `query_limit_hits`: 20 条（与基准完全一致）
所有查询命中结果无一遗漏或错误，分词与倒排查询语义完全一致，证明内核优化在保持严格确定性与零语义漂移的前提下，全面达成预期性能目标。
