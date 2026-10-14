package io.coolder.libai;
import java.util.*;
import java.nio.charset.StandardCharsets;

/** Independent Java RAG types and owners over the shared C ABI. */
public final class Rag implements AutoCloseable {
    static {System.loadLibrary("ai_jni");}
    public static class Document {
        public String id,source,revision,text,metadataJson="{}";
        public Document(String id,String source,String revision,String text){this.id=id;this.source=source;this.revision=revision;this.text=text;}
        private Map<String,Object> value(){bound(id,128);bound(source,2048);bound(revision,128);bound(text,2*1024*1024);bound(metadataJson,16384);return RagJson.object("id",id,"source",source,"revision",revision,"text",text,"metadata_json",metadataJson);}
        private static Document from(Object value){Map<String,Object> d=RagJson.map(value);Document out=new Document(RagJson.text(d,"id"),RagJson.text(d,"source"),RagJson.text(d,"revision"),RagJson.text(d,"text"));out.metadataJson=RagJson.text(d,"metadata_json");return out;}
    }
    public static final class Span {public final long begin,end;public Span(long begin,long end){this.begin=begin;this.end=end;}}
    public static final class Chunk extends Document {
        public String documentId;
        /** Original UTF-8 byte positions; null means no declared span. */
        public Span span;
        public Chunk(String id,String documentId,String source,String revision,String text){super(id,source,revision,text);this.documentId=documentId;}
        private Map<String,Object> value(){bound(id,512);bound(documentId,128);bound(source,2048);bound(revision,128);bound(text,65536);bound(metadataJson,16384);Map<String,Object> out=RagJson.object("id",id,"document_id",documentId,"source",source,"revision",revision,"text",text,"metadata_json",metadataJson,"has_span",span!=null);if(span!=null){out.put("begin",span.begin);out.put("end",span.end);}return out;}
        private static Chunk from(Object value){Map<String,Object> d=RagJson.map(value);Chunk out=new Chunk(RagJson.text(d,"id"),RagJson.text(d,"document_id"),RagJson.text(d,"source"),RagJson.text(d,"revision"),RagJson.text(d,"text"));out.metadataJson=RagJson.text(d,"metadata_json");if(RagJson.bool(d,"has_span"))out.span=new Span(RagJson.integer(d,"begin"),RagJson.integer(d,"end"));return out;}
    }
    public static final class Request {
        public String query,filterJson="{}";public long topK=8;
        public Request(String query){this.query=query;}
        private Map<String,Object> value(){bound(query,8192);bound(filterJson,16384);return RagJson.object("query",query,"filter_json",filterJson,"top_k",topK);}
        private static Request from(Object value){Map<String,Object> d=RagJson.map(value);Request out=new Request(RagJson.text(d,"query"));out.filterJson=RagJson.text(d,"filter_json");out.topK=RagJson.integer(d,"top_k");return out;}
    }
    public static final class Hit {
        public Chunk chunk;public String accessScope;public double score;
        public Hit(Chunk chunk,String scope,double score){this.chunk=chunk;accessScope=scope;this.score=score;}
        private Object value(){if(chunk==null)throw new IllegalArgumentException("null chunk");bound(accessScope,128);return RagJson.object("chunk",chunk.value(),"access_scope",accessScope,"score",score);}
    }
    public static final class Result {
        public Hit[] hits;public boolean truncated;
        public Result(Hit... hits){this.hits=hits;}
        private Object value(){count(hits,64);Object[] values=new Object[hits.length];long bytes=0;for(int i=0;i<hits.length;++i){if(hits[i]==null||hits[i].chunk==null)throw new IllegalArgumentException("null hit");values[i]=hits[i].value();bytes+=payload(hits[i].chunk);total(bytes);}total(bytes);return RagJson.object("hits",values,"truncated",truncated);}
        private static Result from(Object value){Map<String,Object> d=RagJson.map(value);List<Object> hits=RagJson.list(d.get("hits"));Hit[] values=new Hit[hits.size()];for(int i=0;i<values.length;++i){Map<String,Object> h=RagJson.map(hits.get(i));values[i]=new Hit(Chunk.from(h.get("chunk")),RagJson.text(h,"access_scope"),RagJson.number(h,"score"));}Result out=new Result(values);out.truncated=RagJson.bool(d,"truncated");return out;}
    }
    public static final class VectorRecord {
        public Chunk chunk;public double[] vector;
        public VectorRecord(Chunk chunk,double[] vector){this.chunk=chunk;this.vector=vector;}
        private Object value(){if(chunk==null)throw new IllegalArgumentException("null chunk");vector(vector);return RagJson.object("chunk",chunk.value(),"vector",vector);}
    }
    public static final class VectorQuery {
        public final Request request;public final String embeddingSpace;public final double[] vector;
        private VectorQuery(Object value){Map<String,Object> d=RagJson.map(value);request=Request.from(d.get("request"));embeddingSpace=RagJson.text(d,"embedding_space");vector=vectorFrom(d.get("vector"));}
    }
    public static final class PreparedIndex {
        public String[] documents;public VectorRecord[] records;public String embeddingSpace;
        public PreparedIndex(String[] documents,VectorRecord[] records,String space){this.documents=documents;this.records=records;embeddingSpace=space;}
        /** Save/review without writing a store; restore through commitIndexJson unchanged. */
        public String toJson(){count(documents,64);count(records,256);bound(embeddingSpace,128);for(String id:documents)bound(id,128);Object[] values=new Object[records.length];long bytes=0;for(int i=0;i<records.length;++i){if(records[i]==null||records[i].chunk==null)throw new IllegalArgumentException("null record");values[i]=records[i].value();bytes+=payload(records[i].chunk);total(bytes);}total(bytes);return RagJson.write(RagJson.object("documents",documents,"records",values,"embedding_space",embeddingSpace));}
        private static PreparedIndex from(Object value){Map<String,Object> d=RagJson.map(value);List<Object> docs=RagJson.list(d.get("documents")),records=RagJson.list(d.get("records"));String[] ids=new String[docs.size()];for(int i=0;i<ids.length;++i)ids[i]=(String)docs.get(i);VectorRecord[] values=new VectorRecord[records.size()];for(int i=0;i<values.length;++i){Map<String,Object> r=RagJson.map(records.get(i));values[i]=new VectorRecord(Chunk.from(r.get("chunk")),vectorFrom(r.get("vector")));}return new PreparedIndex(ids,values,RagJson.text(d,"embedding_space"));}
    }
    public static final class Limits {
        public long maxQueryBytes=8192,maxDocuments=64,maxChunks=256,maxHits=64,maxDocumentBytes=2*1024*1024,
            maxChunkBytes=65536,maxMetadataBytes=16384,maxTotalBytes=4*1024*1024,maxDimension=16384;
        private Object value(){return RagJson.object("max_query_bytes",maxQueryBytes,"max_documents",maxDocuments,"max_chunks",maxChunks,"max_hits",maxHits,"max_document_bytes",maxDocumentBytes,"max_chunk_bytes",maxChunkBytes,"max_metadata_bytes",maxMetadataBytes,"max_total_bytes",maxTotalBytes,"max_dimension",maxDimension);}
    }
    public static final class Options {
        public String accessScope,idempotencyKey="";public long timeoutMs=300000;public Agent.Cancellation cancellation;
        private Context parent;
        public Options(String scope){accessScope=scope;}
        private String json(){if(timeoutMs<0||timeoutMs>86400000)throw new IllegalArgumentException("RAG timeout range");long timeout=timeoutMs;if(parent!=null){long remaining=parent.remainingMs();if(remaining==0||parent.stopped())throw new IllegalStateException("parent retrieval deadline expired");if(remaining>0)timeout=timeout==0?remaining:Math.min(timeout,remaining);}return RagJson.write(RagJson.object("access_scope",accessScope,"timeout_ms",timeout,"idempotency_key",idempotencyKey));}
    }
    public static final class Binding {
        public String executionRevision,accessScope,filterJson="{}";public boolean requiresApproval=true,sideEffectFree;
        public long maxTopK=8,maxResultBytes=32768;public Limits limits=new Limits();
        public Binding(String revision,String scope){executionRevision=revision;accessScope=scope;}
        String json(){if(limits==null)throw new IllegalArgumentException("null limits");return RagJson.write(RagJson.object("execution_revision",executionRevision,"access_scope",accessScope,"filter_json",filterJson,"requires_approval",requiresApproval,"side_effect_free",sideEffectFree,"max_top_k",maxTopK,"max_result_bytes",maxResultBytes,"limits",limits.value()));}
    }
    public static final class Context {
        private final Agent.Invocation invocation;
        public final String accessScope;
        private Context(Agent.Invocation invocation,String metadata){this.invocation=invocation;accessScope=RagJson.text(RagJson.map(RagJson.read(metadata)),"access_scope");}
        public boolean stopped(){return invocation.stopped();}
        public long remainingMs(){return invocation.remainingMs();}
        public String idempotencyKey(){return invocation.idempotencyKey();}
        public Agent.Cancellation retainCancellation(){return invocation.retainCancellation();}
        public ChildOptions operationOptions(){if(stopped())throw new IllegalStateException("retrieval stopped");Agent.Cancellation token=retainCancellation();try{Options options=new Options(accessScope);options.timeoutMs=0;options.idempotencyKey=idempotencyKey();options.cancellation=token;options.parent=this;options.json();return new ChildOptions(options,token);}catch(RuntimeException|Error error){token.close();throw error;}}
    }
    public static final class ChildOptions implements AutoCloseable {
        public final Options options;private final Agent.Cancellation token;
        private ChildOptions(Options options,Agent.Cancellation token){this.options=options;this.token=token;}
        @Override public void close(){token.close();}
    }
    public interface DocumentLoader {Document[] load(String source,Context context);}
    public interface TextSplitter {Chunk[] split(Document[] documents,Context context);}
    public interface EmbeddingModel {String spaceId();double[][] embed(String[] texts,Context context);}
    public interface VectorStore {void replaceDocuments(String[] documents,VectorRecord[] records,String space,Context context);Result search(VectorQuery query,Context context);}
    public interface Retriever {Result search(Request request,Context context);}

    private final String kind;private long handle;private boolean busy;
    private Rag(String kind,long handle){this.kind=kind;this.handle=handle;}
    private static Rag host(String kind,Object host,Limits limits){if(host==null||limits==null)throw new IllegalArgumentException("null RAG host/limits");return new Rag(kind,nativeCreate(kind,host,config(limits)));}
    private static String config(Limits limits){if(limits==null)throw new IllegalArgumentException("null limits");return RagJson.write(RagJson.object("limits",limits.value()));}
    public static Rag loader(DocumentLoader host){return loader(host,new Limits());}
    public static Rag loader(DocumentLoader host,Limits limits){return host("loader",host,limits);}
    public static Rag splitter(TextSplitter host){return splitter(host,new Limits());}
    public static Rag splitter(TextSplitter host,Limits limits){return host("splitter",host,limits);}
    public static Rag embedding(EmbeddingModel host){return embedding(host,new Limits());}
    public static Rag embedding(EmbeddingModel host,Limits limits){return host("embedding",host,limits);}
    public static Rag vectorStore(VectorStore host){return vectorStore(host,new Limits());}
    public static Rag vectorStore(VectorStore host,Limits limits){return host("store",host,limits);}
    public static Rag retriever(Retriever host){return retriever(host,new Limits());}
    public static Rag retriever(Retriever host,Limits limits){return host("retriever",host,limits);}
    public static Rag utf8Splitter(long bytes,long overlap){return utf8Splitter(bytes,overlap,new Limits());}
    public static Rag utf8Splitter(long bytes,long overlap,Limits limits){if(limits==null)throw new IllegalArgumentException("null limits");return new Rag("splitter",nativeBuiltin("splitter",RagJson.write(RagJson.object("bytes",bytes,"overlap",overlap,"limits",limits.value())),0,0));}
    public static Rag memoryVectorStore(){return memoryVectorStore(4096,16*1024*1024,new Limits());}
    public static Rag memoryVectorStore(long maxRecords,long maxBytes,Limits limits){if(limits==null)throw new IllegalArgumentException("null limits");return new Rag("store",nativeBuiltin("store",RagJson.write(RagJson.object("max_records",maxRecords,"max_bytes",maxBytes,"limits",limits.value())),0,0));}
    public static Rag vectorRetriever(Rag embedding,Rag store){return vectorRetriever(embedding,store,new Limits());}
    public static Rag vectorRetriever(Rag embedding,Rag store,Limits limits){if(embedding==null||store==null)throw new IllegalArgumentException("null components");embedding.require("embedding");store.require("store");String config=config(limits);long e=embedding.begin(),s=0;try{s=store.begin();return new Rag("retriever",nativeBuiltin("retriever",config,e,s));}finally{if(s!=0)store.end();embedding.end();}}
    private void require(String expected){if(!kind.equals(expected))throw new IllegalArgumentException("RAG operation requires "+expected);}
    synchronized long begin(){if(handle==0)throw new IllegalStateException("RAG component closed");if(busy)throw new IllegalStateException("RAG component busy");busy=true;return handle;}
    synchronized void end(){busy=false;}
    long beginRetriever(){require("retriever");return begin();}
    private String call(String op,String input,Options options,String policy,Rag other){if(options==null)throw new IllegalArgumentException("null options");long pointer=begin(),second=0,token=0;try{if(other!=null)second=other.begin();Agent.Cancellation cancellation=options.cancellation;if(cancellation!=null)token=cancellation.retain();String context=options.json();return nativeCall(pointer,op,RagJson.checked(input),context,policy,second,token);}finally{if(token!=0)Agent.releaseToken(token);if(second!=0)other.end();end();}}
    public Document[] loadDocuments(String source,Options options){require("loader");return documentsFrom(RagJson.read(call("load",source,options,"{}",null)));}
    public PreparedIndex prepareIndex(Rag embedding,Document[] documents,Options options){return prepareIndex(embedding,documents,options,new Limits());}
    public PreparedIndex prepareIndex(Rag embedding,Document[] documents,Options options,Limits limits){require("splitter");if(embedding==null||limits==null)throw new IllegalArgumentException("null embedding/limits");embedding.require("embedding");return PreparedIndex.from(RagJson.read(call("prepare",RagJson.write(documentsValue(documents)),options,RagJson.write(limits.value()),embedding)));}
    public void commitIndex(PreparedIndex batch,Options options){if(batch==null)throw new IllegalArgumentException("null prepared batch");commitIndexJson(batch.toJson(),options);}
    /** Saved JSON is passed untouched to native duplicate/type/bounds validation. */
    public void commitIndexJson(String wire,Options options){require("store");call("commit",wire,options,"{}",null);}
    public Result retrieve(Request request,Options options){require("retriever");if(request==null)throw new IllegalArgumentException("null request");return Result.from(RagJson.read(call("retrieve",RagJson.write(request.value()),options,"{}",null)));}
    @Override public void close(){synchronized(this){if(handle==0)return;}long pointer=begin();try{nativeDestroy(pointer);synchronized(this){handle=0;}}finally{end();}}

    private static void bound(String text,int maximum){RagJson.checked(text);if(text.length()>maximum||text.getBytes(StandardCharsets.UTF_8).length>maximum)throw new IllegalArgumentException("RAG text limit");}
    private static void count(Object[] array,int maximum){if(array==null||array.length>maximum)throw new IllegalArgumentException("RAG array limit");}
    private static long bytes(String s){return RagJson.checked(s).getBytes(StandardCharsets.UTF_8).length;}
    private static long payload(Document d){return bytes(d.id)+bytes(d.source)+bytes(d.revision)+bytes(d.text)+bytes(d.metadataJson)+(d instanceof Chunk?bytes(((Chunk)d).documentId):0);}
    private static void total(long bytes){if(bytes>4*1024*1024)throw new IllegalArgumentException("RAG payload limit");}
    private static Object[] documentsValue(Document[] docs){count(docs,64);Object[] values=new Object[docs.length];long bytes=0;for(int i=0;i<values.length;++i){if(docs[i]==null)throw new IllegalArgumentException("null document");values[i]=docs[i].value();bytes+=payload(docs[i]);total(bytes);}total(bytes);return values;}
    private static Document[] documentsFrom(Object value){List<Object> list=RagJson.list(value);Document[] out=new Document[list.size()];for(int i=0;i<out.length;++i)out[i]=Document.from(list.get(i));return out;}
    private static void vector(double[] values){if(values==null||values.length==0||values.length>16384)throw new IllegalArgumentException("RAG vector limit");}
    private static double[] vectorFrom(Object value){List<Object> list=RagJson.list(value);double[] out=new double[list.size()];for(int i=0;i<out.length;++i){if(!(list.get(i) instanceof Number))throw new IllegalStateException("RAG vector element");out[i]=((Number)list.get(i)).doubleValue();}return out;}
    // JNI caches this method/class globally and gets each callback thread's own env.
    private static String dispatch(Object host,String operation,String wire,String metadata,long pointer){Agent.Invocation invocation=pointer==0?null:Agent.invocation(pointer);try{Object args=RagJson.read(wire);Context context=invocation==null?null:new Context(invocation,metadata);Object output;
        switch(operation){
            case "load":output=documentsValue(((DocumentLoader)host).load(RagJson.text(RagJson.map(args),"source"),context));break;
            case "split":Chunk[] chunks=((TextSplitter)host).split(documentsFrom(args),context);count(chunks,256);Object[] values=new Object[chunks.length];long bytes=0;for(int i=0;i<values.length;++i){if(chunks[i]==null)throw new IllegalArgumentException("null chunk");values[i]=chunks[i].value();bytes+=payload(chunks[i]);total(bytes);}total(bytes);output=values;break;
            case "space_id":output=((EmbeddingModel)host).spaceId();break;
            case "embed":List<Object> texts=RagJson.list(args);String[] inputs=new String[texts.size()];for(int i=0;i<inputs.length;++i)inputs[i]=(String)texts.get(i);double[][] vectors=((EmbeddingModel)host).embed(inputs,context);count(vectors,256);for(double[] v:vectors)vector(v);output=vectors;break;
            case "replace_documents":PreparedIndex batch=PreparedIndex.from(args);((VectorStore)host).replaceDocuments(batch.documents,batch.records,batch.embeddingSpace,context);output=RagJson.object();break;
            case "search":Result searched=((VectorStore)host).search(new VectorQuery(args),context);if(searched==null)throw new IllegalStateException("null result");output=searched.value();break;
            case "retrieve":Result result=((Retriever)host).search(Request.from(args),context);if(result==null)throw new IllegalStateException("null result");output=result.value();break;
            default:throw new IllegalStateException("unknown RAG callback");
        }return RagJson.write(output);
    }finally{if(invocation!=null)Agent.expire(invocation);}}
    private static native long nativeCreate(String kind,Object host,String config);
    private static native long nativeBuiltin(String kind,String config,long embedding,long store);
    private static native String nativeCall(long pointer,String operation,String input,String context,String limits,long other,long token);
    private static native void nativeDestroy(long pointer);
}
