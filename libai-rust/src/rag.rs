//! Native ownership and synchronous, same-thread host callbacks.
use crate::{
    callbacks::{cstring, discard_panic, text},
    ffi, native_error, owned_result, Error, Invocation,
};
use crate::{rag_types::*, rag_values as wire};
use serde_json::{json, Value};
use std::{
    cell::RefCell,
    ffi::{c_char, c_void},
    marker::PhantomData,
    panic::{catch_unwind, AssertUnwindSafe},
    rc::Rc,
    thread::{self, ThreadId},
};
enum Host {
    Loader(Box<dyn DocumentLoader>),
    Splitter(Box<dyn TextSplitter>),
    Embedding(Box<dyn EmbeddingModel>),
    Store(Box<dyn VectorStore>),
    Retriever(Box<dyn Retriever>),
}
struct Holder {
    caller: ThreadId,
    host: RefCell<Host>,
}
unsafe extern "C" fn callback(
    user: *mut c_void,
    operation: *const c_char,
    input: *const c_char,
    metadata: *const c_char,
    result: *mut c_void,
    pointer: *mut c_void,
) {
    let response = catch_unwind(AssertUnwindSafe(|| -> Result<(), Error> {
        let holder = &*(user as *const Holder);
        if holder.caller != thread::current().id() {
            return Err(Error("local RAG callback on another thread".into()));
        }
        let mut host = holder
            .host
            .try_borrow_mut()
            .map_err(|_| Error("RAG host busy".into()))?;
        let op = text(operation);
        let input = wire::parse(&text(input))?;
        let output = if op == "space_id" {
            if !pointer.is_null() {
                return Err(Error("space_id unexpectedly has invocation".into()));
            }
            match &mut *host {
                Host::Embedding(host) => Value::String(host.space_id()?),
                _ => return Err(Error("RAG operation/kind mismatch".into())),
            }
        } else {
            if pointer.is_null() {
                return Err(Error("RAG invocation missing".into()));
            }
            let context = RetrievalContext {
                invocation: Invocation {
                    pointer,
                    lifetime: PhantomData,
                },
                access_scope: wire::scope(&wire::parse(&text(metadata))?)?,
            };
            match (&mut *host, op.as_str()) {
                (Host::Loader(host), "load") => {
                    wire::documents_value(&host.load(&wire::source(&input)?, &context)?)?
                }
                (Host::Splitter(host), "split") => {
                    wire::chunks_value(&host.split(&wire::documents(&input)?, &context)?)?
                }
                (Host::Embedding(host), "embed") => {
                    wire::vectors_value(&host.embed(&wire::strings(&input)?, &context)?)?
                }
                (Host::Store(host), "replace_documents") => {
                    let batch = wire::batch(&input)?;
                    host.replace_documents(
                        &batch.documents,
                        &batch.records,
                        &batch.embedding_space,
                        &context,
                    )?;
                    json!({})
                }
                (Host::Store(host), "search") => {
                    wire::result_value(&host.search(&wire::query(&input)?, &context)?)?
                }
                (Host::Retriever(host), "retrieve") => {
                    wire::result_value(&host.search(&wire::request(&input)?, &context)?)?
                }
                _ => return Err(Error("RAG operation/kind mismatch".into())),
            }
        };
        let output = cstring(&wire::encode(&output)?)?;
        if ffi::libai_rag_result_set(result, output.as_ptr(), output.as_bytes().len()) != 0 {
            return Err(Error("RAG callback output invalid or oversized".into()));
        }
        Ok(())
    }));
    let error = match response {
        Ok(Ok(())) => return,
        Ok(Err(error)) => error.0,
        Err(payload) => {
            discard_panic(payload);
            "Rust RAG callback panicked".into()
        }
    };
    let mut end = error.len().min(4096);
    while !error.is_char_boundary(end) {
        end -= 1;
    }
    let message = error[..end].replace('\0', "");
    let message = cstring(if message.is_empty() {
        "Rust RAG callback failed"
    } else {
        &message
    })
    .unwrap_or_else(|_| c"Rust RAG callback failed".to_owned());
    ffi::libai_rag_result_error(result, message.as_ptr());
}
unsafe extern "C" fn release(user: *mut c_void) {
    if let Err(payload) = catch_unwind(AssertUnwindSafe(|| {
        drop(Box::from_raw(user as *mut Holder))
    })) {
        discard_panic(payload);
    }
}
struct Component {
    pointer: *mut c_void,
    _local: PhantomData<Rc<()>>,
}
impl Component {
    fn own(pointer: *mut c_void) -> Result<Self, Error> {
        if pointer.is_null() {
            Err(native_error())
        } else {
            Ok(Self {
                pointer,
                _local: PhantomData,
            })
        }
    }
    fn new(kind: &str, host: Host, limits: &RagLimits) -> Result<Self, Error> {
        let kind = cstring(kind)?;
        let config = cstring(&wire::encode(&json!({"limits":limits.value()}))?)?;
        let user = Box::into_raw(Box::new(Holder {
            caller: thread::current().id(),
            host: RefCell::new(host),
        }));
        let pointer = unsafe {
            ffi::libai_rag_create(
                kind.as_ptr(),
                callback,
                user.cast(),
                release,
                config.as_ptr(),
            )
        };
        if pointer.is_null() {
            let error = native_error();
            unsafe {
                release(user.cast());
            }
            return Err(error);
        }
        Self::own(pointer)
    }
    fn check(&self) -> Result<(), Error> {
        if self.pointer.is_null() {
            Err(Error("RAG component closed".into()))
        } else {
            Ok(())
        }
    }
    fn close(&mut self) -> Result<(), Error> {
        if self.pointer.is_null() {
            return Ok(());
        }
        if unsafe { ffi::libai_rag_destroy(self.pointer) } != 0 {
            return Err(native_error());
        }
        self.pointer = std::ptr::null_mut();
        Ok(())
    }
}
impl Drop for Component {
    fn drop(&mut self) {
        let _ = self.close();
    }
}
fn options(options: &RagOptions<'_>) -> Result<std::ffi::CString, Error> {
    cstring(&wire::encode(&options.value()?)?)
}
fn cancellation(options: &RagOptions<'_>) -> *mut c_void {
    options
        .cancellation
        .map_or(std::ptr::null_mut(), |c| c.pointer)
}
/// Own a host loader. Neither Send nor Sync; C++ never exposes its implementation types.
pub struct RagLoader(Component);
impl RagLoader {
    pub fn new(host: impl DocumentLoader + 'static, limits: &RagLimits) -> Result<Self, Error> {
        Ok(Self(Component::new(
            "loader",
            Host::Loader(Box::new(host)),
            limits,
        )?))
    }
    pub fn load_documents(
        &mut self,
        source: &str,
        context: &RagOptions<'_>,
    ) -> Result<Vec<Document>, Error> {
        self.0.check()?;
        let source = cstring(source)?;
        let options = options(context)?;
        let output = owned_result(unsafe {
            ffi::libai_rag_load(
                self.0.pointer,
                source.as_ptr(),
                options.as_ptr(),
                cancellation(context),
            )
        })?;
        wire::documents(&wire::parse(&output)?)
    }
    pub fn close(&mut self) -> Result<(), Error> {
        self.0.close()
    }
}
pub struct RagSplitter(Component);
impl RagSplitter {
    pub fn new(host: impl TextSplitter + 'static, limits: &RagLimits) -> Result<Self, Error> {
        Ok(Self(Component::new(
            "splitter",
            Host::Splitter(Box::new(host)),
            limits,
        )?))
    }
    pub fn utf8(bytes: usize, overlap: usize, limits: &RagLimits) -> Result<Self, Error> {
        let config = cstring(&wire::encode(
            &json!({"bytes":bytes,"overlap":overlap,"limits":limits.value()}),
        )?)?;
        Ok(Self(Component::own(unsafe {
            ffi::libai_rag_create_splitter(config.as_ptr())
        })?))
    }
    /// Does not write any vector store. The host may save/review PreparedIndex first.
    pub fn prepare_index(
        &mut self,
        embedding: &mut RagEmbedding,
        documents: &[Document],
        context: &RagOptions<'_>,
        limits: &RagLimits,
    ) -> Result<PreparedIndex, Error> {
        self.0.check()?;
        embedding.0.check()?;
        let docs = cstring(&wire::encode(&wire::documents_value(documents)?)?)?;
        let limits = cstring(&wire::encode(&limits.value())?)?;
        let options = options(context)?;
        let output = owned_result(unsafe {
            ffi::libai_rag_prepare_index(
                self.0.pointer,
                embedding.0.pointer,
                docs.as_ptr(),
                options.as_ptr(),
                limits.as_ptr(),
                cancellation(context),
            )
        })?;
        wire::batch(&wire::parse(&output)?)
    }
    pub fn close(&mut self) -> Result<(), Error> {
        self.0.close()
    }
}
pub struct RagEmbedding(Component);
impl RagEmbedding {
    pub fn new(host: impl EmbeddingModel + 'static, limits: &RagLimits) -> Result<Self, Error> {
        Ok(Self(Component::new(
            "embedding",
            Host::Embedding(Box::new(host)),
            limits,
        )?))
    }
    pub fn close(&mut self) -> Result<(), Error> {
        self.0.close()
    }
}
pub struct RagVectorStore(Component);
impl RagVectorStore {
    pub fn new(host: impl VectorStore + 'static, limits: &RagLimits) -> Result<Self, Error> {
        Ok(Self(Component::new(
            "store",
            Host::Store(Box::new(host)),
            limits,
        )?))
    }
    /// Bounded temporary exact-cosine index. No durable database/ANN guarantee.
    pub fn memory(max_records: usize, max_bytes: usize, limits: &RagLimits) -> Result<Self, Error> {
        let config = cstring(&wire::encode(
            &json!({"max_records":max_records,"max_bytes":max_bytes,"limits":limits.value()}),
        )?)?;
        Ok(Self(Component::own(unsafe {
            ffi::libai_rag_create_memory_store(config.as_ptr())
        })?))
    }
    pub fn commit_index(
        &mut self,
        batch: &PreparedIndex,
        context: &RagOptions<'_>,
    ) -> Result<(), Error> {
        self.commit_index_json(&batch.to_json()?, context)
    }
    /// Pass saved JSON untouched to native validation, including duplicate keys,
    /// unknown fields and exact types. Failure/cancellation may follow a write.
    pub fn commit_index_json(
        &mut self,
        batch_json: &str,
        context: &RagOptions<'_>,
    ) -> Result<(), Error> {
        self.0.check()?;
        let batch = cstring(batch_json)?;
        let options = options(context)?;
        if unsafe {
            ffi::libai_rag_commit_index(
                self.0.pointer,
                batch.as_ptr(),
                options.as_ptr(),
                cancellation(context),
            )
        } != 0
        {
            return Err(native_error());
        }
        Ok(())
    }
    pub fn close(&mut self) -> Result<(), Error> {
        self.0.close()
    }
}
/// Owning same-thread retriever; binding to an Agent retains host dependencies.
///
/// ```compile_fail
/// use libai::{RagRetriever,RagLimits,Retriever,RetrievalRequest,RetrievalContext,RetrievalResult,Error};
/// struct Search;
/// impl Retriever for Search {fn search(&mut self,_:&RetrievalRequest,_:&RetrievalContext<'_>)->Result<RetrievalResult,Error>{Ok(RetrievalResult::default())}}
/// let r=RagRetriever::new(Search,&RagLimits::default()).unwrap();
/// std::thread::spawn(move || drop(r)); // Local callback owner is not Send.
/// ```
pub struct RagRetriever(Component);
impl RagRetriever {
    pub fn new(host: impl Retriever + 'static, limits: &RagLimits) -> Result<Self, Error> {
        Ok(Self(Component::new(
            "retriever",
            Host::Retriever(Box::new(host)),
            limits,
        )?))
    }
    pub fn vector(
        embedding: &RagEmbedding,
        store: &RagVectorStore,
        limits: &RagLimits,
    ) -> Result<Self, Error> {
        embedding.0.check()?;
        store.0.check()?;
        let config = cstring(&wire::encode(&json!({"limits":limits.value()}))?)?;
        Ok(Self(Component::own(unsafe {
            ffi::libai_rag_create_vector_retriever(
                embedding.0.pointer,
                store.0.pointer,
                config.as_ptr(),
            )
        })?))
    }
    pub fn retrieve(
        &mut self,
        request: &RetrievalRequest,
        context: &RagOptions<'_>,
    ) -> Result<RetrievalResult, Error> {
        self.0.check()?;
        let query = cstring(&wire::encode(&wire::request_value(request)?)?)?;
        let options = options(context)?;
        let output = owned_result(unsafe {
            ffi::libai_rag_retrieve(
                self.0.pointer,
                query.as_ptr(),
                options.as_ptr(),
                cancellation(context),
            )
        })?;
        wire::result(&wire::parse(&output)?)
    }
    pub fn close(&mut self) -> Result<(), Error> {
        self.0.close()
    }
    pub(crate) fn pointer(&self) -> Result<*mut c_void, Error> {
        self.0.check()?;
        Ok(self.0.pointer)
    }
}
