#![cfg(feature = "rag")]
use libai::*;
use std::{
    cell::{Cell, RefCell},
    rc::Rc,
    sync::{Arc, Barrier},
    time::Duration,
};
#[cfg(unix)]
fn private_root(label: &str) -> std::path::PathBuf {
    use std::os::unix::fs::PermissionsExt;
    static SEQUENCE: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    let path = std::env::temp_dir().join(format!(
        "rust-rag-{label}-{}-{}",
        std::process::id(),
        SEQUENCE.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
    ));
    std::fs::create_dir(&path).unwrap();
    std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o700)).unwrap();
    path
}
fn docs() -> Vec<Document> {
    let mut a = Document::new("a", "manual.txt", "r1", "Alpha beta gamma");
    a.metadata_json = r#"{"kind":"manual"}"#.into();
    let b = Document::new("b", "other.txt", "r1", "Other");
    vec![a, b]
}
fn options() -> RagOptions<'static> {
    let mut o = RagOptions::new("tenant_a");
    o.timeout_ms = 5000;
    o.idempotency_key = "host-key";
    o
}
fn query() -> RetrievalRequest {
    RetrievalRequest {
        query: "Alpha".into(),
        filter_json: r#"{"kind":"manual"}"#.into(),
        top_k: 2,
    }
}
#[derive(Default)]
struct State {
    calls: Cell<usize>,
    dropped: Cell<usize>,
    mode: Cell<u8>,
    dimension: Cell<usize>,
    spaces: Cell<usize>,
    key: RefCell<String>,
    filter: RefCell<String>,
}
struct Embed(Rc<State>);
impl Drop for Embed {
    fn drop(&mut self) {
        self.0.dropped.set(self.0.dropped.get() + 1);
    }
}
impl EmbeddingModel for Embed {
    fn space_id(&mut self) -> Result<String, Error> {
        let n = self.0.spaces.get() + 1;
        self.0.spaces.set(n);
        Ok(if self.0.mode.get() == 3 && n % 2 == 0 {
            "changed"
        } else {
            "embedding-v1"
        }
        .into())
    }
    fn embed(
        &mut self,
        texts: &[String],
        c: &RetrievalContext<'_>,
    ) -> Result<Vec<Vec<f64>>, Error> {
        assert!(!c.stopped());
        self.0.calls.set(self.0.calls.get() + 1);
        *self.0.key.borrow_mut() = c.invocation.idempotency_key();
        Ok(match self.0.mode.get() {
            1 => vec![],
            2 => vec![vec![0.0; 2]; texts.len()],
            4 => vec![vec![f64::NAN, 1.0]; texts.len()],
            _ => vec![vec![1.0; self.0.dimension.get().max(2)]; texts.len()],
        })
    }
}
struct Loader;
impl DocumentLoader for Loader {
    fn load(&mut self, source: &str, c: &RetrievalContext<'_>) -> Result<Vec<Document>, Error> {
        assert_eq!(source, "manual.txt");
        assert_eq!(c.access_scope, "tenant_a");
        Ok(docs())
    }
}
struct Split(bool);
impl TextSplitter for Split {
    fn split(
        &mut self,
        docs: &[Document],
        _: &RetrievalContext<'_>,
    ) -> Result<Vec<DocumentChunk>, Error> {
        Ok(docs
            .iter()
            .map(|d| DocumentChunk {
                id: format!("{}-chunk", d.id),
                document_id: d.id.clone(),
                source: if self.0 {
                    "wrong".into()
                } else {
                    d.source.clone()
                },
                revision: d.revision.clone(),
                text: d.text.clone(),
                metadata_json: d.metadata_json.clone(),
                span: Some((0, d.text.len())),
            })
            .collect())
    }
}
struct Search(Rc<State>);
impl Drop for Search {
    fn drop(&mut self) {
        self.0.dropped.set(self.0.dropped.get() + 1);
        if self.0.mode.get() == 9 {
            std::panic::panic_any(Payload);
        }
    }
}
impl Retriever for Search {
    fn search(
        &mut self,
        q: &RetrievalRequest,
        c: &RetrievalContext<'_>,
    ) -> Result<RetrievalResult, Error> {
        self.0.calls.set(self.0.calls.get() + 1);
        *self.0.key.borrow_mut() = c.invocation.idempotency_key();
        *self.0.filter.borrow_mut() = q.filter_json.clone();
        if self.0.mode.get() == 5 {
            return Err(Error("lost response after possible effect".into()));
        }
        if self.0.mode.get() == 8 {
            std::panic::panic_any(Payload);
        }
        let chunk = DocumentChunk {
            id: "a#0-5@r1".into(),
            document_id: "a".into(),
            source: "manual.txt".into(),
            revision: "r1".into(),
            text: if self.0.mode.get() == 10 {
                "x".repeat(2000)
            } else {
                "Alpha".into()
            },
            metadata_json: if self.0.mode.get() == 2 {
                r#"{"kind":"public"}"#.into()
            } else {
                r#"{"kind":"manual"}"#.into()
            },
            span: None,
        };
        let hit = RetrievalHit {
            chunk,
            access_scope: if self.0.mode.get() == 1 {
                "tenant_b".into()
            } else {
                c.access_scope.clone()
            },
            score: if self.0.mode.get() == 4 {
                f64::NAN
            } else {
                0.5
            },
        };
        Ok(RetrievalResult {
            hits: if self.0.mode.get() == 3 {
                vec![hit.clone(), hit]
            } else {
                vec![hit]
            },
            truncated: false,
        })
    }
}
struct Payload;
impl Drop for Payload {
    fn drop(&mut self) {
        panic!("payload destructor panic");
    }
}
#[test]
fn index_query_scope_replacement_and_utf8() {
    let l = RagLimits::default();
    let s = Rc::new(State::default());
    let mut loader = RagLoader::new(Loader, &l).unwrap();
    let mut e = RagEmbedding::new(Embed(s.clone()), &l).unwrap();
    let mut split = RagSplitter::utf8(64, 0, &l).unwrap();
    let mut store = RagVectorStore::memory(4096, 16 * 1024 * 1024, &l).unwrap();
    let mut r = RagRetriever::vector(&e, &store, &l).unwrap();
    let batch = split
        .prepare_index(
            &mut e,
            &loader.load_documents("manual.txt", &options()).unwrap(),
            &options(),
            &l,
        )
        .unwrap();
    assert!(r.retrieve(&query(), &options()).unwrap().hits.is_empty());
    store
        .commit_index_json(&batch.to_json().unwrap(), &options())
        .unwrap();
    let hits = r.retrieve(&query(), &options()).unwrap();
    assert_eq!(hits.hits.len(), 1);
    assert_eq!(hits.hits[0].chunk.span, Some((0, 16)));
    assert!(r
        .retrieve(&query(), &RagOptions::new("tenant_b"))
        .unwrap()
        .hits
        .is_empty());
    let mut all = RetrievalRequest::new("Alpha");
    all.top_k = 1;
    assert!(r.retrieve(&all, &options()).unwrap().truncated);
    store
        .commit_index(
            &PreparedIndex {
                documents: vec!["a".into()],
                records: vec![],
                embedding_space: "embedding-v1".into(),
            },
            &options(),
        )
        .unwrap();
    assert!(r.retrieve(&query(), &options()).unwrap().hits.is_empty());
    assert!(store
        .commit_index_json(
            r#"{"documents":["b"],"documents":[],"records":[],"embedding_space":"embedding-v1"}"#,
            &options()
        )
        .is_err());
    assert!(!r
        .retrieve(&RetrievalRequest::new("Other"), &options())
        .unwrap()
        .hits
        .is_empty());
    let mut unicode = RagSplitter::utf8(4, 0, &l).unwrap();
    let batch = unicode
        .prepare_index(
            &mut e,
            &[Document::new("u", "utf8.txt", "r1", "中文a")],
            &options(),
            &l,
        )
        .unwrap();
    assert_eq!(
        batch
            .records
            .iter()
            .map(|v| (v.chunk.text.as_str(), v.chunk.span))
            .collect::<Vec<_>>(),
        vec![("中", Some((0, 3))), ("文a", Some((3, 7)))]
    );
    assert!(loader
        .load_documents("manual.txt", &RagOptions::new("../bad"))
        .is_err());
}
#[test]
fn failed_creation_and_invalid_embeddings_or_provenance() {
    let l = RagLimits::default();
    let s = Rc::new(State::default());
    let mut bad = l.clone();
    bad.max_dimension = 0;
    assert!(RagEmbedding::new(Embed(s.clone()), &bad).is_err());
    assert_eq!(s.dropped.get(), 1);
    let mut e = RagEmbedding::new(Embed(s.clone()), &l).unwrap();
    let mut split = RagSplitter::new(Split(false), &l).unwrap();
    for m in 1..=4 {
        s.mode.set(m);
        s.spaces.set(0);
        assert!(split
            .prepare_index(&mut e, &docs(), &options(), &l)
            .is_err());
    }
    s.mode.set(0);
    let calls = s.calls.get();
    let mut bad = RagSplitter::new(Split(true), &l).unwrap();
    assert!(bad.prepare_index(&mut e, &docs(), &options(), &l).is_err());
    assert_eq!(s.calls.get(), calls);
    let d = docs();
    assert!(split
        .prepare_index(&mut e, &[d[0].clone(), d[0].clone()], &options(), &l)
        .is_err());
    assert_eq!(s.calls.get(), calls);
    e.close().unwrap();
    assert_eq!(s.dropped.get(), 2);
    assert!(split
        .prepare_index(&mut e, &docs(), &options(), &l)
        .is_err());
    assert!(RagSplitter::utf8(4, 4, &l).is_err());
    assert!(RagVectorStore::memory(0, 100, &l).is_err());
}
#[derive(Default)]
struct Backend {
    records: Vec<VectorRecord>,
    commits: usize,
    lost: bool,
}
struct Store(Rc<RefCell<Backend>>);
impl VectorStore for Store {
    fn replace_documents(
        &mut self,
        docs: &[String],
        records: &[VectorRecord],
        _: &str,
        c: &RetrievalContext<'_>,
    ) -> Result<(), Error> {
        assert_eq!(c.access_scope, "tenant_a");
        let mut s = self.0.borrow_mut();
        s.commits += 1;
        s.records.retain(|r| !docs.contains(&r.chunk.document_id));
        s.records.extend_from_slice(records);
        if s.lost {
            Err(Error("write applied; response lost".into()))
        } else {
            Ok(())
        }
    }
    fn search(
        &mut self,
        q: &VectorQuery,
        c: &RetrievalContext<'_>,
    ) -> Result<RetrievalResult, Error> {
        assert_eq!(q.embedding_space, "embedding-v1");
        assert!(!q.vector.is_empty());
        let s = self.0.borrow();
        Ok(RetrievalResult {
            hits: s
                .records
                .iter()
                .filter(|r| r.chunk.metadata_json == q.request.filter_json)
                .take(q.request.top_k)
                .map(|r| RetrievalHit {
                    chunk: r.chunk.clone(),
                    access_scope: c.access_scope.clone(),
                    score: 0.5,
                })
                .collect(),
            truncated: false,
        })
    }
}
#[test]
fn custom_backend_prepare_no_write_and_unknown_commit() {
    let l = RagLimits::default();
    let backend = Rc::new(RefCell::new(Backend::default()));
    let mut store = RagVectorStore::new(Store(backend.clone()), &l).unwrap();
    let mut e = RagEmbedding::new(Embed(Rc::new(State::default())), &l).unwrap();
    let mut split = RagSplitter::new(Split(false), &l).unwrap();
    let batch = split
        .prepare_index(&mut e, &docs(), &options(), &l)
        .unwrap();
    assert_eq!(backend.borrow().commits, 0);
    store.commit_index(&batch, &options()).unwrap();
    let mut r = RagRetriever::vector(&e, &store, &l).unwrap();
    assert_eq!(
        r.retrieve(&query(), &options()).unwrap().hits[0].chunk.text,
        "Alpha beta gamma"
    );
    backend.borrow_mut().lost = true;
    assert!(store
        .commit_index(&batch, &options())
        .unwrap_err()
        .0
        .contains("write applied"));
    assert_eq!(backend.borrow().commits, 2);
    assert_eq!(backend.borrow().records.len(), 2);
    assert!(store
        .commit_index_json(
            r#"{"documents":[],"records":[],"embedding_space":"embedding-v1","extra":true}"#,
            &options()
        )
        .is_err());
    assert_eq!(backend.borrow().commits, 2);
}
fn model(turns: Rc<RefCell<Vec<String>>>, args: &str) -> Agent {
    let args = args.to_owned();
    Agent::new(
        move |q, _| {
            let mut s = turns.borrow_mut();
            s.push(q.into());
            if s.len() == 1 {
                ModelResponse {
                    calls: vec![ToolCall {
                        id: "r1".into(),
                        name: "knowledge_search".into(),
                        arguments: args.clone(),
                    }],
                    ..Default::default()
                }
            } else {
                ModelResponse {
                    text: "done".into(),
                    ..Default::default()
                }
            }
        },
        "",
    )
    .unwrap()
}
fn binding() -> RagBinding {
    let mut b = RagBinding::new("corpus-acl-v1", "tenant_a");
    b.filter_json = r#"{"kind":"manual"}"#.into();
    b.max_top_k = 2;
    b
}
fn status(s: &str, v: &str) {
    assert!(s.contains(&format!("\"status\":\"{v}\"")), "{s}");
}
#[test]
#[cfg(unix)]
fn agent_schema_policy_lifetime_cache_and_contract() {
    let l = RagLimits::default();
    let s = Rc::new(State::default());
    let mut r = RagRetriever::new(Search(s.clone()), &l).unwrap();
    for args in [
        r#"{"query":"Alpha","access_scope":"tenant_b"}"#,
        r#"{"query":"Alpha","top_k":1.5}"#,
    ] {
        let mut a = model(Rc::new(RefCell::new(vec![])), args);
        a.add_retrieval_tool(&r, "knowledge_search", &binding())
            .unwrap();
        let mut approval =
            |_: &str, _: &str, _: bool| -> bool { panic!("invalid parameters reached approval") };
        a.run_with(
            "search",
            &Options::default(),
            Hooks {
                authorize: Some(&mut approval),
                ..Default::default()
            },
        )
        .unwrap();
        assert_eq!(s.calls.get(), 0);
    }
    let mut denied = model(Rc::new(RefCell::new(vec![])), r#"{"query":"Alpha"}"#);
    denied
        .add_retrieval_tool(&r, "knowledge_search", &binding())
        .unwrap();
    denied
        .run_with("search", &Options::default(), Hooks::default())
        .unwrap();
    assert_eq!(s.calls.get(), 0);
    drop(denied);
    let turns = Rc::new(RefCell::new(vec![]));
    let mut a = model(turns.clone(), r#"{"query":"Alpha","top_k":2.0}"#);
    let mut b = binding();
    a.add_retrieval_tool(&r, "knowledge_search", &b).unwrap();
    b.filter_json = r#"{"kind":"public"}"#.into();
    let root = private_root("cache");
    let file = FileExecutionStore::plaintext(root.to_str().unwrap()).unwrap();
    let store: SharedExecutionStore = Rc::new(RefCell::new(file));
    a.set_execution_store("rust-rag", Some(store)).unwrap();
    r.close().unwrap();
    drop(r);
    assert_eq!(s.dropped.get(), 0);
    let mut approval = |_: &str, _: &str, required: bool| {
        assert!(required);
        true
    };
    let result = a
        .run_with(
            "search",
            &Options::default(),
            Hooks {
                authorize: Some(&mut approval),
                ..Default::default()
            },
        )
        .unwrap();
    status(&result, "completed");
    assert!(result.contains("\"accepted\":false"));
    assert_eq!(*s.filter.borrow(), r#"{"kind":"manual"}"#);
    assert_eq!(*s.key.borrow(), "rust-rag.0");
    assert!(turns.borrow()[1].contains("citation"));
    a.run_with("search", &Options::default(), Hooks::default())
        .unwrap();
    assert_eq!((s.calls.get(), turns.borrow().len()), (1, 2));
    drop(a);
    assert_eq!(s.dropped.get(), 1);
    std::fs::remove_dir_all(root).unwrap();
}
#[test]
fn bad_backend_truncation_and_panic_boundaries() {
    let l = RagLimits::default();
    let s = Rc::new(State::default());
    let mut r = RagRetriever::new(Search(s.clone()), &l).unwrap();
    for m in [1, 2, 3, 4, 8] {
        s.mode.set(m);
        assert!(r.retrieve(&query(), &options()).is_err());
    }
    s.mode.set(10);
    let turns = Rc::new(RefCell::new(vec![]));
    let mut a = model(turns.clone(), r#"{"query":"Alpha"}"#);
    let mut b = binding();
    b.max_result_bytes = 256;
    a.add_retrieval_tool(&r, "knowledge_search", &b).unwrap();
    let mut authorize = |_: &str, _: &str, _: bool| true;
    status(
        &a.run_with(
            "search",
            &Options::default(),
            Hooks {
                authorize: Some(&mut authorize),
                ..Default::default()
            },
        )
        .unwrap(),
        "completed",
    );
    assert!(turns.borrow()[1].contains("\\\"truncated\\\":true"));
    drop(a);
    s.mode.set(9);
    drop(r);
    assert_eq!(s.dropped.get(), 1);
}
#[test]
#[cfg(unix)]
fn unknown_effect_is_not_replayed() {
    let root = private_root("unknown");
    let file = FileExecutionStore::plaintext(root.to_str().unwrap()).unwrap();
    let store: SharedExecutionStore = Rc::new(RefCell::new(file));
    let l = RagLimits::default();
    let s = Rc::new(State::default());
    s.mode.set(5);
    let r = RagRetriever::new(Search(s.clone()), &l).unwrap();
    let mut a = model(Rc::new(RefCell::new(vec![])), r#"{"query":"Alpha"}"#);
    a.add_retrieval_tool(&r, "knowledge_search", &binding())
        .unwrap();
    a.set_execution_store("unknown-rag", Some(store.clone()))
        .unwrap();
    let mut approval = |_: &str, _: &str, _: bool| true;
    status(
        &a.run_with(
            "search",
            &Options::default(),
            Hooks {
                authorize: Some(&mut approval),
                ..Default::default()
            },
        )
        .unwrap(),
        "recovery_required",
    );
    assert_eq!(s.calls.get(), 1);
    drop(a);
    drop(r);
    let fresh = Rc::new(State::default());
    let r = RagRetriever::new(Search(fresh.clone()), &l).unwrap();
    let mut a = model(Rc::new(RefCell::new(vec![])), r#"{"query":"Alpha"}"#);
    a.add_retrieval_tool(&r, "knowledge_search", &binding())
        .unwrap();
    a.set_execution_store("unknown-rag", Some(store)).unwrap();
    status(
        &a.run_with("search", &Options::default(), Hooks::default())
            .unwrap(),
        "recovery_required",
    );
    assert_eq!(fresh.calls.get(), 0);
    drop(a);
    drop(r);
    std::fs::remove_dir_all(root).unwrap();
}
struct Slow {
    gate: Option<Arc<Barrier>>,
    expired: bool,
}
impl Retriever for Slow {
    fn search(
        &mut self,
        _: &RetrievalRequest,
        c: &RetrievalContext<'_>,
    ) -> Result<RetrievalResult, Error> {
        if let Some(gate) = &self.gate {
            gate.wait();
            while !c.stopped() {
                std::thread::sleep(Duration::from_millis(1));
            }
        } else {
            std::thread::sleep(Duration::from_millis(30));
        }
        if self.expired {
            let error = c.with_options(|_| Ok(())).unwrap_err();
            assert!(error.0.contains("stopped"));
        }
        Ok(RetrievalResult::default())
    }
}
#[test]
fn cancellation_deadline_and_nested_parent_refresh() {
    let l = RagLimits::default();
    let gate = Arc::new(Barrier::new(2));
    let token = Arc::new(Cancellation::new().unwrap());
    let parent = token.clone();
    let worker = gate.clone();
    let stop = std::thread::spawn(move || {
        worker.wait();
        parent.cancel();
    });
    let mut r = RagRetriever::new(
        Slow {
            gate: Some(gate),
            expired: false,
        },
        &l,
    )
    .unwrap();
    let mut o = options();
    o.cancellation = Some(&token);
    assert!(r.retrieve(&query(), &o).is_err());
    stop.join().unwrap();
    let mut r = RagRetriever::new(
        Slow {
            gate: None,
            expired: true,
        },
        &l,
    )
    .unwrap();
    o.cancellation = None;
    o.timeout_ms = 10;
    assert!(r.retrieve(&query(), &o).is_err());
    struct Nested {
        child: RagRetriever,
        calls: Rc<State>,
        delay: bool,
    }
    impl Retriever for Nested {
        fn search(
            &mut self,
            q: &RetrievalRequest,
            c: &RetrievalContext<'_>,
        ) -> Result<RetrievalResult, Error> {
            c.with_options(|opts| {
                if self.delay {
                    std::thread::sleep(Duration::from_millis(30));
                    let error = self.child.retrieve(q, opts).unwrap_err();
                    assert!(error.0.contains("parent retrieval deadline expired"));
                    Ok(RetrievalResult::default())
                } else {
                    let result = self.child.retrieve(q, opts)?;
                    assert_eq!(*self.calls.key.borrow(), "host-key");
                    Ok(result)
                }
            })
        }
    }
    let s = Rc::new(State::default());
    let child = RagRetriever::new(Search(s.clone()), &l).unwrap();
    let mut nested = RagRetriever::new(
        Nested {
            child,
            calls: s.clone(),
            delay: false,
        },
        &l,
    )
    .unwrap();
    assert!(!nested
        .retrieve(&query(), &options())
        .unwrap()
        .hits
        .is_empty());
    let child = RagRetriever::new(Search(s.clone()), &l).unwrap();
    let mut delayed = RagRetriever::new(
        Nested {
            child,
            calls: s.clone(),
            delay: true,
        },
        &l,
    )
    .unwrap();
    assert!(delayed.retrieve(&query(), &o).is_err());
    assert_eq!(s.calls.get(), 1);
}
#[test]
fn composed_lifetime_large_vectors_and_precise_score() {
    let l = RagLimits::default();
    let s = Rc::new(State::default());
    s.dimension.set(16384);
    let mut e = RagEmbedding::new(Embed(s.clone()), &l).unwrap();
    let mut split = RagSplitter::utf8(4, 0, &l).unwrap();
    let mut store = RagVectorStore::memory(4096, 16 * 1024 * 1024, &l).unwrap();
    let batch = split
        .prepare_index(&mut e, &docs(), &options(), &l)
        .unwrap();
    assert!(batch.records.iter().map(|r| r.vector.len()).sum::<usize>() > 65536);
    store.commit_index(&batch, &options()).unwrap();
    let mut r = RagRetriever::vector(&e, &store, &l).unwrap();
    e.close().unwrap();
    store.close().unwrap();
    drop(e);
    drop(store);
    assert_eq!(s.dropped.get(), 0);
    assert!(!r.retrieve(&query(), &options()).unwrap().hits.is_empty());
    r.close().unwrap();
    assert_eq!(s.dropped.get(), 1);
}

#[test]
fn nontrivial_float_score_roundtrip() {
    struct Exact;
    impl Retriever for Exact {
        fn search(
            &mut self,
            _: &RetrievalRequest,
            c: &RetrievalContext<'_>,
        ) -> Result<RetrievalResult, Error> {
            Ok(RetrievalResult {
                hits: vec![RetrievalHit {
                    chunk: DocumentChunk {
                        id: "exact".into(),
                        document_id: "a".into(),
                        source: "a".into(),
                        revision: "1".into(),
                        text: "a".into(),
                        metadata_json: "{}".into(),
                        span: None,
                    },
                    access_scope: c.access_scope.clone(),
                    score: f64::from_bits(0x3feb0e7009b61ce0),
                }],
                truncated: false,
            })
        }
    }
    let mut r = RagRetriever::new(Exact, &RagLimits::default()).unwrap();
    assert_eq!(
        r.retrieve(&RetrievalRequest::new("a"), &options())
            .unwrap()
            .hits[0]
            .score
            .to_bits(),
        0x3feb0e7009b61ce0
    );
}
