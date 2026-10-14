//! Private codec. Decodes only native-validated inputs/results; saved JSON is never
//! normalized before native commit. No protocol/ACL/index semantics live here.
use crate::{rag_types::*, Error};
use serde_json::{json, Value};
fn invalid() -> Error {
    Error("invalid native RAG JSON value".into())
}
pub(crate) fn parse(wire: &str) -> Result<Value, Error> {
    serde_json::from_str(wire).map_err(|_| invalid())
}
pub(crate) fn encode(value: &Value) -> Result<String, Error> {
    serde_json::to_string(value).map_err(|_| invalid())
}
fn member<'a>(v: &'a Value, key: &str) -> Result<&'a Value, Error> {
    v.get(key).ok_or_else(invalid)
}
pub(crate) fn string(v: &Value) -> Result<String, Error> {
    v.as_str().map(str::to_owned).ok_or_else(invalid)
}
fn text(v: &Value, key: &str) -> Result<String, Error> {
    string(member(v, key)?)
}
pub(crate) fn strings(v: &Value) -> Result<Vec<String>, Error> {
    v.as_array()
        .ok_or_else(invalid)?
        .iter()
        .map(string)
        .collect()
}
fn size(v: &Value, key: &str) -> Result<usize, Error> {
    usize::try_from(member(v, key)?.as_u64().ok_or_else(invalid)?).map_err(|_| invalid())
}
fn boolean(v: &Value, key: &str) -> Result<bool, Error> {
    member(v, key)?.as_bool().ok_or_else(invalid)
}
fn bound(s: &str, maximum: usize) -> Result<(), Error> {
    if s.len() > maximum {
        Err(Error("RAG text exceeds bridge limit".into()))
    } else {
        Ok(())
    }
}
pub(crate) fn document_value(v: &Document) -> Result<Value, Error> {
    bound(&v.id, 128)?;
    bound(&v.source, 2048)?;
    bound(&v.revision, 128)?;
    bound(&v.text, 2 * 1024 * 1024)?;
    bound(&v.metadata_json, 16384)?;
    Ok(
        json!({"id":v.id,"source":v.source,"revision":v.revision,"text":v.text,"metadata_json":v.metadata_json}),
    )
}
pub(crate) fn documents_value(docs: &[Document]) -> Result<Value, Error> {
    if docs.len() > 64 {
        return Err(Error("RAG document count exceeded".into()));
    }
    let bytes: usize = docs
        .iter()
        .map(|d| {
            d.id.len() + d.source.len() + d.revision.len() + d.text.len() + d.metadata_json.len()
        })
        .sum();
    if bytes > 4 * 1024 * 1024 {
        return Err(Error("RAG document payload exceeded".into()));
    }
    Ok(Value::Array(
        docs.iter().map(document_value).collect::<Result<_, _>>()?,
    ))
}
pub(crate) fn documents(v: &Value) -> Result<Vec<Document>, Error> {
    v.as_array()
        .ok_or_else(invalid)?
        .iter()
        .map(|v| {
            Ok(Document {
                id: text(v, "id")?,
                source: text(v, "source")?,
                revision: text(v, "revision")?,
                text: text(v, "text")?,
                metadata_json: text(v, "metadata_json")?,
            })
        })
        .collect()
}
pub(crate) fn chunk_value(v: &DocumentChunk) -> Result<Value, Error> {
    bound(&v.id, 512)?;
    bound(&v.document_id, 128)?;
    bound(&v.source, 2048)?;
    bound(&v.revision, 128)?;
    bound(&v.text, 65536)?;
    bound(&v.metadata_json, 16384)?;
    let mut result = json!({"id":v.id,"document_id":v.document_id,"source":v.source,"revision":v.revision,"text":v.text,"metadata_json":v.metadata_json,"has_span":v.span.is_some()});
    if let Some((begin, end)) = v.span {
        result["begin"] = json!(begin);
        result["end"] = json!(end);
    }
    Ok(result)
}
fn chunks_bytes<'a>(chunks: impl Iterator<Item = &'a DocumentChunk>) -> Result<(), Error> {
    let bytes: usize = chunks
        .map(|d| {
            d.id.len()
                + d.document_id.len()
                + d.source.len()
                + d.revision.len()
                + d.text.len()
                + d.metadata_json.len()
        })
        .sum();
    if bytes > 4 * 1024 * 1024 {
        return Err(Error("RAG chunk payload exceeded".into()));
    }
    Ok(())
}
pub(crate) fn chunks_value(chunks: &[DocumentChunk]) -> Result<Value, Error> {
    if chunks.len() > 256 {
        return Err(Error("RAG chunk count exceeded".into()));
    }
    chunks_bytes(chunks.iter())?;
    Ok(Value::Array(
        chunks.iter().map(chunk_value).collect::<Result<_, _>>()?,
    ))
}
fn chunk(v: &Value) -> Result<DocumentChunk, Error> {
    Ok(DocumentChunk {
        id: text(v, "id")?,
        document_id: text(v, "document_id")?,
        source: text(v, "source")?,
        revision: text(v, "revision")?,
        text: text(v, "text")?,
        metadata_json: text(v, "metadata_json")?,
        span: if boolean(v, "has_span")? {
            Some((size(v, "begin")?, size(v, "end")?))
        } else {
            None
        },
    })
}
pub(crate) fn vector_value(v: &[f64]) -> Result<Value, Error> {
    if v.is_empty() || v.len() > 16384 {
        return Err(Error("RAG vector dimension invalid".into()));
    }
    Ok(Value::Array(
        v.iter()
            .map(|&n| {
                serde_json::Number::from_f64(n)
                    .map(Value::Number)
                    .ok_or_else(|| Error("RAG vector must be finite".into()))
            })
            .collect::<Result<_, _>>()?,
    ))
}
pub(crate) fn vectors_value(v: &[Vec<f64>]) -> Result<Value, Error> {
    if v.len() > 256 {
        return Err(Error("RAG vector count exceeded".into()));
    }
    Ok(Value::Array(
        v.iter()
            .map(|v| vector_value(v))
            .collect::<Result<_, _>>()?,
    ))
}
fn vector(v: &Value) -> Result<Vec<f64>, Error> {
    v.as_array()
        .ok_or_else(invalid)?
        .iter()
        .map(|v| v.as_f64().ok_or_else(invalid))
        .collect()
}
pub(crate) fn request_value(v: &RetrievalRequest) -> Result<Value, Error> {
    bound(&v.query, 8192)?;
    bound(&v.filter_json, 16384)?;
    Ok(json!({"query":v.query,"filter_json":v.filter_json,"top_k":v.top_k}))
}
pub(crate) fn request(v: &Value) -> Result<RetrievalRequest, Error> {
    Ok(RetrievalRequest {
        query: text(v, "query")?,
        filter_json: text(v, "filter_json")?,
        top_k: size(v, "top_k")?,
    })
}
pub(crate) fn query(v: &Value) -> Result<VectorQuery, Error> {
    Ok(VectorQuery {
        request: request(member(v, "request")?)?,
        embedding_space: text(v, "embedding_space")?,
        vector: vector(member(v, "vector")?)?,
    })
}
pub(crate) fn result_value(v: &RetrievalResult) -> Result<Value, Error> {
    if v.hits.len() > 64 {
        return Err(Error("RAG hit count exceeded".into()));
    }
    chunks_bytes(v.hits.iter().map(|h| &h.chunk))?;
    let hits = v
        .hits
        .iter()
        .map(|h| {
            bound(&h.access_scope, 128)?;
            let score = serde_json::Number::from_f64(h.score)
                .ok_or_else(|| Error("RAG score must be finite".into()))?;
            Ok(json!({"chunk":chunk_value(&h.chunk)?,"access_scope":h.access_scope,"score":score}))
        })
        .collect::<Result<Vec<_>, Error>>()?;
    Ok(json!({"hits":hits,"truncated":v.truncated}))
}
pub(crate) fn result(v: &Value) -> Result<RetrievalResult, Error> {
    Ok(RetrievalResult {
        truncated: boolean(v, "truncated")?,
        hits: member(v, "hits")?
            .as_array()
            .ok_or_else(invalid)?
            .iter()
            .map(|h| {
                Ok(RetrievalHit {
                    chunk: chunk(member(h, "chunk")?)?,
                    access_scope: text(h, "access_scope")?,
                    score: member(h, "score")?.as_f64().ok_or_else(invalid)?,
                })
            })
            .collect::<Result<_, Error>>()?,
    })
}
pub(crate) fn batch_value(v: &PreparedIndex) -> Result<Value, Error> {
    if v.documents.len() > 64 || v.records.len() > 256 {
        return Err(Error("RAG batch count exceeded".into()));
    }
    for id in &v.documents {
        bound(id, 128)?;
    }
    bound(&v.embedding_space, 128)?;
    chunks_bytes(v.records.iter().map(|r| &r.chunk))?;
    let records = v
        .records
        .iter()
        .map(|r| Ok(json!({"chunk":chunk_value(&r.chunk)?,"vector":vector_value(&r.vector)?})))
        .collect::<Result<Vec<_>, Error>>()?;
    Ok(json!({"documents":v.documents,"records":records,"embedding_space":v.embedding_space}))
}
pub(crate) fn batch(v: &Value) -> Result<PreparedIndex, Error> {
    Ok(PreparedIndex {
        documents: strings(member(v, "documents")?)?,
        embedding_space: text(v, "embedding_space")?,
        records: member(v, "records")?
            .as_array()
            .ok_or_else(invalid)?
            .iter()
            .map(|r| {
                Ok(VectorRecord {
                    chunk: chunk(member(r, "chunk")?)?,
                    vector: vector(member(r, "vector")?)?,
                })
            })
            .collect::<Result<_, Error>>()?,
    })
}
pub(crate) fn source(v: &Value) -> Result<String, Error> {
    text(v, "source")
}
pub(crate) fn scope(v: &Value) -> Result<String, Error> {
    text(v, "access_scope")
}
