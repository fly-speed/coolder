//! Independent Rust values; JSON metadata/filter remain host-defined object text.
use crate::{Cancellation, Error, Invocation};
use serde_json::{json, Value};

#[derive(Debug, Clone)]
pub struct Document {
    pub id: String,
    pub source: String,
    pub revision: String,
    pub text: String,
    pub metadata_json: String,
}
impl Document {
    pub fn new(id: &str, source: &str, revision: &str, text: &str) -> Self {
        Self {
            id: id.into(),
            source: source.into(),
            revision: revision.into(),
            text: text.into(),
            metadata_json: "{}".into(),
        }
    }
}
#[derive(Debug, Clone)]
pub struct DocumentChunk {
    pub id: String,
    pub document_id: String,
    pub source: String,
    pub revision: String,
    pub text: String,
    pub metadata_json: String,
    /// Original UTF-8 byte positions, not character offsets.
    pub span: Option<(usize, usize)>,
}
#[derive(Debug, Clone)]
pub struct RetrievalRequest {
    pub query: String,
    pub filter_json: String,
    pub top_k: usize,
}
impl RetrievalRequest {
    pub fn new(query: &str) -> Self {
        Self {
            query: query.into(),
            filter_json: "{}".into(),
            top_k: 8,
        }
    }
}
#[derive(Debug, Clone)]
pub struct RetrievalHit {
    pub chunk: DocumentChunk,
    pub access_scope: String,
    pub score: f64,
}
#[derive(Debug, Clone, Default)]
pub struct RetrievalResult {
    pub hits: Vec<RetrievalHit>,
    pub truncated: bool,
}
#[derive(Debug, Clone)]
pub struct VectorRecord {
    pub chunk: DocumentChunk,
    pub vector: Vec<f64>,
}
#[derive(Debug, Clone)]
pub struct VectorQuery {
    pub request: RetrievalRequest,
    pub embedding_space: String,
    pub vector: Vec<f64>,
}
#[derive(Debug, Clone)]
pub struct PreparedIndex {
    pub documents: Vec<String>,
    pub records: Vec<VectorRecord>,
    pub embedding_space: String,
}
impl PreparedIndex {
    /// Save/review without writing a vector store. Restore with commit_index_json,
    /// which passes original JSON to native duplicate/type/provenance validation.
    pub fn to_json(&self) -> Result<String, Error> {
        crate::rag_values::encode(&crate::rag_values::batch_value(self)?)
    }
}
#[derive(Debug, Clone)]
pub struct RagLimits {
    pub max_query_bytes: usize,
    pub max_documents: usize,
    pub max_chunks: usize,
    pub max_hits: usize,
    pub max_document_bytes: usize,
    pub max_chunk_bytes: usize,
    pub max_metadata_bytes: usize,
    pub max_total_bytes: usize,
    pub max_dimension: usize,
}
impl Default for RagLimits {
    fn default() -> Self {
        Self {
            max_query_bytes: 8192,
            max_documents: 64,
            max_chunks: 256,
            max_hits: 64,
            max_document_bytes: 2 * 1024 * 1024,
            max_chunk_bytes: 65536,
            max_metadata_bytes: 16384,
            max_total_bytes: 4 * 1024 * 1024,
            max_dimension: 16384,
        }
    }
}
impl RagLimits {
    pub(crate) fn value(&self) -> Value {
        json!({"max_query_bytes":self.max_query_bytes,"max_documents":self.max_documents,
        "max_chunks":self.max_chunks,"max_hits":self.max_hits,"max_document_bytes":self.max_document_bytes,
        "max_chunk_bytes":self.max_chunk_bytes,"max_metadata_bytes":self.max_metadata_bytes,
        "max_total_bytes":self.max_total_bytes,"max_dimension":self.max_dimension})
    }
}
pub struct RagOptions<'a> {
    pub access_scope: &'a str,
    pub timeout_ms: u64,
    pub idempotency_key: &'a str,
    pub cancellation: Option<&'a Cancellation>,
    pub(crate) parent: Option<&'a Invocation<'a>>,
}
impl<'a> RagOptions<'a> {
    pub fn new(scope: &'a str) -> Self {
        Self {
            access_scope: scope,
            timeout_ms: 300000,
            idempotency_key: "",
            cancellation: None,
            parent: None,
        }
    }
    pub(crate) fn value(&self) -> Result<Value, Error> {
        if self.timeout_ms > 86400000 {
            return Err(Error("RAG timeout exceeds 24 hours".into()));
        }
        let mut timeout = self.timeout_ms;
        if let Some(parent) = self.parent {
            let remaining = parent.remaining_ms();
            if remaining == 0 || parent.stopped() {
                return Err(Error("parent retrieval deadline expired".into()));
            }
            if remaining > 0 {
                timeout = if timeout == 0 {
                    remaining as u64
                } else {
                    timeout.min(remaining as u64)
                };
            }
        }
        Ok(
            json!({"access_scope":self.access_scope,"timeout_ms":timeout,"idempotency_key":self.idempotency_key}),
        )
    }
}
/// Callback borrow cannot escape or cross threads; only copied scope/key may be retained.
///
/// ```compile_fail
/// use libai::{Retriever,RetrievalContext,RetrievalRequest,RetrievalResult,Error};
/// struct Invalid { saved: Option<&'static RetrievalContext<'static>> }
/// impl Retriever for Invalid {
///     fn search(&mut self,_:&RetrievalRequest,context:&RetrievalContext<'_>)->Result<RetrievalResult,Error>{
///         self.saved=Some(context); // Callback borrow cannot become static.
///         Ok(RetrievalResult::default())
///     }
/// }
/// ```
pub struct RetrievalContext<'a> {
    pub invocation: Invocation<'a>,
    pub access_scope: String,
}
impl RetrievalContext<'_> {
    pub fn stopped(&self) -> bool {
        self.invocation.stopped()
    }
    /// Borrowed child options plus an owned stop lease, confined to this closure.
    /// Parent deadline is rechecked at each nested operation, not just at entry.
    pub fn with_options<T>(
        &self,
        callback: impl FnOnce(&RagOptions<'_>) -> Result<T, Error>,
    ) -> Result<T, Error> {
        if self.stopped() {
            return Err(Error("retrieval stopped before child operation".into()));
        }
        let cancellation = self.invocation.retain_cancellation()?;
        let key = self.invocation.idempotency_key();
        let options = RagOptions {
            access_scope: &self.access_scope,
            timeout_ms: 0,
            idempotency_key: &key,
            cancellation: Some(&cancellation),
            parent: Some(&self.invocation),
        };
        options.value()?;
        callback(&options)
    }
}
#[derive(Debug, Clone)]
pub struct RagBinding {
    pub execution_revision: String,
    pub access_scope: String,
    pub filter_json: String,
    pub requires_approval: bool,
    pub side_effect_free: bool,
    pub max_top_k: usize,
    pub max_result_bytes: usize,
    pub limits: RagLimits,
}
impl RagBinding {
    pub fn new(revision: &str, scope: &str) -> Self {
        Self {
            execution_revision: revision.into(),
            access_scope: scope.into(),
            filter_json: "{}".into(),
            requires_approval: true,
            side_effect_free: false,
            max_top_k: 8,
            max_result_bytes: 32768,
            limits: RagLimits::default(),
        }
    }
    pub(crate) fn json(&self) -> Result<String, Error> {
        crate::rag_values::encode(&json!({"execution_revision":self.execution_revision,
        "access_scope":self.access_scope,"filter_json":self.filter_json,"requires_approval":self.requires_approval,
        "side_effect_free":self.side_effect_free,"max_top_k":self.max_top_k,"max_result_bytes":self.max_result_bytes,"limits":self.limits.value()}))
    }
}
pub trait DocumentLoader {
    fn load(
        &mut self,
        source: &str,
        context: &RetrievalContext<'_>,
    ) -> Result<Vec<Document>, Error>;
}
pub trait TextSplitter {
    fn split(
        &mut self,
        documents: &[Document],
        context: &RetrievalContext<'_>,
    ) -> Result<Vec<DocumentChunk>, Error>;
}
pub trait EmbeddingModel {
    /// Pure, fast, stable configuration read; no invocation/implicit network I/O.
    fn space_id(&mut self) -> Result<String, Error>;
    fn embed(
        &mut self,
        texts: &[String],
        context: &RetrievalContext<'_>,
    ) -> Result<Vec<Vec<f64>>, Error>;
}
pub trait VectorStore {
    /// Explicit effect. Failure may follow a write; no automatic retry.
    fn replace_documents(
        &mut self,
        documents: &[String],
        records: &[VectorRecord],
        space: &str,
        context: &RetrievalContext<'_>,
    ) -> Result<(), Error>;
    fn search(
        &mut self,
        query: &VectorQuery,
        context: &RetrievalContext<'_>,
    ) -> Result<RetrievalResult, Error>;
}
pub trait Retriever {
    fn search(
        &mut self,
        request: &RetrievalRequest,
        context: &RetrievalContext<'_>,
    ) -> Result<RetrievalResult, Error>;
}
