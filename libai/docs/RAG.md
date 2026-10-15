# 可插拔 RAG SDK

`<libai/rag.h>`提供C++11接口，链接libai::agent。无需ACL、OpenSSL、模型厂商SDK或
向量数据库。SDK、独立C++、C ABI、Python、Rust、Java和Objective-C已接入。其它未完成项继续保留，不将这部分实现当作全部五项目标完成。

## 接口与流程

- document_loader_t：宿主解析source并加载document_t；库不自动访问URL、文件或网络。
- text_splitter_t：文档到document_chunk_t；内置utf8_text_splitter_t按UTF-8字节窗口分块。
- embedding_model_t：批量文本到向量，space_id标识模型/归一化/维数语义。
- vector_store_t：按文档ID替换该scope/space的完整chunk集，以及向量查询。
- retriever_t：可直接接入全文、SQL、向量或混合搜索，不强制经过Embedding。
- function_retriever_t：以宿主回调快速适配既有检索服务。

load_documents执行加载并检查文档；prepare_index分块并计算向量，不写向量存储。
commit_index显式调用replace_documents；外部适配器负责持久原子性、去重和收据。
提交抛错/取消不能证明未写入，不能自动重试；可将该写操作作为受治理Agent工具或
工作流节点，按已有执行版本、意图、幂等键和宿主收据规则处理未知效果。
空文档会准备空chunk集，替换时删除该文档旧chunk；不会自动删除其它文档。

vector_retriever_t将查询Embedding与VectorStore组合。retrieve是完整校验入口，检查
请求、停止状态与全部命中后才返回；直接实现trait并不自动取得这些校验保证。
取消/期限使用retrieval_context_t.control，均为协作式；宿主网络回调须配合停止。

## 文档、来源和访问边界

Document包含id、source、revision、UTF-8 text、metadata_json对象。Chunk保留document_id、
source、revision、metadata和稳定id；has_span时begin/end为原文UTF-8字节范围。
内置分块id由文档ID、字节范围及revision组成，窗口不切断代码点，重叠量至多配置字节。
自定义Splitter不得改写来源/版本/metadata，声明span须与原文内容一致；不声明span的
转换型chunk由宿主负责来源真实性。source/revision只是宿主声明，不是签名或内容证明。

access_scope为宿主提供的非空ASCII索引/租户范围，模型不决定它。每个Hit须返回
匹配scope；过滤条件是metadata顶层键的精确JSON值匹配（对象/数组可作值），对命中
再次校验。外部适配器仍须真正执行权限隔离，不能仅给越权结果贴上正确scope标签。

索引embedding_space由宿主适配器标识；改变向量语义时必须改变space_id。准备/查询
前后ID须相同，数量、维数、有限值、非零向量均检查。分数含义由检索器定义，须有限；
不同后端分数不承诺可比较。内置内存库使用稳定缩放的cosine，避免大幅值向量溢出。

## 可选内存向量存储

memory_vector_store_t是有界、临时、精确cosine适配器，适合本地小索引和测试；不是
持久数据库、近似索引或生产容量承诺。按scope/space隔离，批次验证后复制/原子交换，
拒绝维数或其它文档chunk ID冲突。查询只保留top_k命中，分数相同按chunk ID排序。
默认最多4096记录/16MiB，可收紧；硬上限4096记录/64MiB、64个非空scope/space。
内存库不保存到accounts/项目/会话目录，不提供默认Embedding模型或隐藏网络请求。

## Agent检索工具

```cpp
#include <libai/rag.h>
// retriever可以是vector_retriever_t，也可以直接适配业务检索服务。
libai::rag_binding_t policy;
policy.access_scope="tenant-a";
policy.filter_json=R"({"category":"manual"})";
policy.execution_revision="corpus-v1-acl-v1-embedding-v1";
libai::add_retrieval_tool(agent,retriever,"knowledge.search",policy);
```

宿主必须提供execution_revision，它需覆盖检索器、语料、Embedding、ACL和过滤规则；
改变任何语义必须改版本。库不能识别任意回调/远端实现变化，也不自动推断策略版本。
默认requires_approval=true、side_effect_free=false；仅宿主审查后可声明纯读取安全重放。
模型参数只有query和可选top_k，不能覆盖scope/filter；整批Schema与授权在检索前执行。

结果JSON包含version、truncated和hits。每条含citation（chunk ID）、document_id、source、
revision、text、metadata、score及可选span；访问scope不放入模型结果。检索文本保持
工具数据，不自动提升为system指令、不执行其中命令、不下载引用URL。标注不等于
完全防止提示注入，宿主仍需自己的业务控制。

结果默认32KiB，按整条命中截断并明确truncated，不截断文本或JSON；全部后端结果
先验证，不能借截断隐藏非法/越权命中。宿主应将工具结果预算配置为至少该上限，
否则Agent通用输出限制仍会将其标成截断工具错误。引用格式正确不证明答案事实正确，
检索完成也不改变TaskContract生成/构建/测试/人工证据。

## 默认资源限制

query8KiB、64文档、256chunk、64命中、单文档2MiB、单chunk64KiB、单metadata/filter
16KiB、累计文档或chunk文本/元数据4MiB、Embedding维数16384。rag_limits_t只允许
收紧这些限制。JSON继续检查重复键、UTF-8、深度64/65536节点；文档文本禁止NUL。
批量向量字节另受数量/维数限制，内存库容量同时计入向量字节。宿主回调可能在
返回之前分配更多内存，SDK不能限制任意外部实现的内部资源。

## 验证范围

离线测试覆盖加载/分块/Embedding/替换/检索/Agent工具完整链路、UTF-8 span、来源与
metadata校验、scope/filter隔离、重复/非法命中、维数/空间漂移、大幅值cosine、索引
容量失败不提交、整条命中截断、授权/参数阻止检索及持久结果复用。使用确定性模拟
Embedding，不将其作为语义质量、厂商线上兼容或外部数据库事务验证。

主目录轻量SDK15项与完整库20项回归通过，包含RAG及Provider/Workspace/编程组件/
完整归档链接；仅使用安装SDK的独立消费项目运行同一RAG测试通过，未依赖源码
头文件。Visual Studio工程XML校验通过，未验证Windows或移动设备运行。

## 独立 C++ facade

`<libai_cpp/rag.h>`提供独立公开值类型、宿主适配器、Rag静态操作、UTF-8分块、有界
内存向量存储与VectorRetriever。Agent.add_retrieval_tool复用SDK治理和日志，保留
Retriever owner，取消转换共享原子状态。转换与所有SDK引用仅位于libai-c++/src，
公开头文件无原生类型。例子见 [C++ README](../../libai-c++/README.md#独立-rag-接口)。
六语言RAG入口已补齐，其它未完成能力继续保留。

独立C++阶段主目录静态/动态各11项回归及两个仅用安装包的独立消费项目通过，
覆盖公开类型、索引/检索/Agent治理/缓存/取消。coolder重建与iOS arm64模拟器
交叉编译成功，未验证设备或Windows。公共头文件隔离测试通过。

## C ABI

`libai-c/include/libai.h` 提供 opaque RAG 组件，宿主回调可实现Loader/Splitter/
Embedding/VectorStore/Retriever。内置UTF-8分块、内存向量存储、组合检索器复用上述
SDK执行器；显式 prepare/commit、scope/filter、取消/期限和Agent检索工具边界相同。
成功创建才转移回调owner；Agent/组合检索器保留组件，外层句柄可独立销毁。
具体JSON协议、回调生命周期和包络上限见 [C README](../../libai-c/README.md#可插拔-rag-c-abi)。
其余语言RAG对象见下文。

## Python

`libai.rag`提供独立Python数据类、宿主接口和拥有组件的Rag工厂；加载、准备、提交、
检索与Agent检索工具调用共享C ABI。支持Python实现的Loader/Splitter/Embedding/
VectorStore/Retriever、内置UTF-8分块、内存库和组合检索器；无需厂商Python SDK。
回调context只在本线程/本次回调有效，嵌套options保留取消且重新检查父期限；
组合检索器/Agent保留回调所有权，旧原生库缺少RAG时明确报错而不破坏已有Agent。
用法及保存/审核索引的JSON接口见 [Python README](../../libai-python/README.md#可插拔-rag)。
其余语言RAG对象见下文。

## Rust

默认rag feature提供独立Rust值、五类宿主trait与拥有组件；可关闭该feature保留
无需JSON crate的基础Agent/MCP/工作流。serde_json只编码/解码native已验证的值，
保存索引JSON原样进入commit_index_json的C ABI校验，避免重复键被提前归一化。
组件非Send/Sync，回调检查创建线程；组合检索器和Agent保留宿主至最后释放。
Context.with_options提供闭包范围的取消/父期限，panic和析构边界沿用既有规则。
用法见 [Rust README](../../libai-rust/README.md#可插拔-rag)。Objective-C RAG对象见下文。

## Java

Rag提供独立Java值、五类宿主接口、拥有组件与Agent绑定；内置分块/内存库/组合
检索器复用C ABI，prepare/commit分离，保存JSON原样校验。无第三方JSON库依赖。
JNI保留宿主和类的全局引用，缓存方法并在当前线程取得JNIEnv；回调使用本地帧，
所有路径过期Context。运行/关闭使用忙状态但不跨回调持有Java监视器；不同组件
可从native工作流并行节点调用。用法见 [Java README](../../libai-java/README.md#可插拔-rag)。
其它未完成平台能力继续保留。

## Objective-C

LAIRag提供独立Foundation值、五类宿主协议、拥有组件及Agent绑定；内置组件复用
C ABI，公开头文件无原生C/C++类型。ARC保留宿主至最后原生引用释放，失败factory及
Objective-C分配失败清理本次引用。回调有autoreleasepool，NSException与C++异常
保持失败语义，Context在所有路径过期；嵌套options重新检查父期限并保留取消。
保存JSON原样进入C ABI提交验证。见 [Objective-C README](../../libai-objc/README.md#可插拔-rag)。
六语言RAG入口已接入；更多平台、后端适配及其它未完成能力仍需继续验证。
