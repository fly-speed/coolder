import io.coolder.libai.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import java.lang.ref.WeakReference;
import java.nio.file.*;
import java.nio.file.attribute.PosixFilePermissions;
import java.io.IOException;

public final class RagTest {
    static void check(boolean value){if(!value)throw new AssertionError();}
    static void rejects(Runnable action){try{action.run();}catch(RuntimeException expected){return;}throw new AssertionError("invalid action accepted");}
    static void status(String value,String status){check(value.contains("\"status\":\""+status+"\""));}
    static Rag.Options options(){Rag.Options o=new Rag.Options("tenant_a");o.timeoutMs=5000;o.idempotencyKey="host-key";return o;}
    static Rag.Request query(){Rag.Request q=new Rag.Request("Alpha");q.filterJson="{\"kind\":\"manual\"}";q.topK=2;return q;}
    static Rag.Document[] docs(){Rag.Document a=new Rag.Document("a","manual.txt","r1","Alpha beta gamma");a.metadataJson="{\"kind\":\"manual\"}";return new Rag.Document[]{a,new Rag.Document("b","other.txt","r1","Other")};}
    static class Embed implements Rag.EmbeddingModel {
        int calls,spaces,mode,dimension=2;
        public String spaceId(){return mode==3&&++spaces%2==0?"changed":"embedding-v1";}
        public double[][] embed(String[] texts,Rag.Context c){++calls;check(!c.stopped());if(mode==1)return new double[0][];double[][] result=new double[texts.length][dimension];for(double[] v:result)Arrays.fill(v,mode==2?0:mode==4?Double.NaN:1);return result;}
    }
    static class Search implements Rag.Retriever {
        int calls,mode;String key="",filter="";Rag.Context saved;Rag owner;double score=0.5;String text="Alpha";
        public Rag.Result search(Rag.Request q,Rag.Context c){++calls;saved=c;key=c.idempotencyKey();filter=q.filterJson;
            if(owner!=null){rejects(owner::close);rejects(()->owner.retrieve(q,options()));}
            if(mode==5)throw new IllegalStateException("response lost after possible effect🌏");
            if(mode==8)throw new RuntimeException("bad formatter"){@Override public String toString(){throw new RuntimeException("formatter failed");}};
            Rag.Chunk chunk=new Rag.Chunk("a#0-5@r1","a","manual.txt","r1",text);chunk.metadataJson=mode==2?"{\"kind\":\"public\"}":"{\"kind\":\"manual\"}";
            Rag.Hit h=new Rag.Hit(chunk,mode==1?"tenant_b":c.accessScope,mode==4?Double.NaN:score);return mode==3?new Rag.Result(h,h):new Rag.Result(h);
        }
    }
    static Rag.TextSplitter splitter(boolean invalid){return (docs,c)->{Rag.Chunk[] chunks=new Rag.Chunk[docs.length];for(int i=0;i<docs.length;++i){Rag.Document d=docs[i];chunks[i]=new Rag.Chunk(d.id+".chunk",d.id,invalid?"wrong":d.source,d.revision,d.text);chunks[i].metadataJson=d.metadataJson;chunks[i].span=new Rag.Span(0,d.text.getBytes(java.nio.charset.StandardCharsets.UTF_8).length);}return chunks;};}
    static class Backend implements Rag.VectorStore {
        int commits;boolean lost;Map<String,List<Rag.VectorRecord>> stored=new HashMap<>();
        public void replaceDocuments(String[] docs,Rag.VectorRecord[] records,String space,Rag.Context c){++commits;String key=c.accessScope+"/"+space;List<Rag.VectorRecord> kept=new ArrayList<>(stored.getOrDefault(key,Collections.emptyList()));Set<String> ids=new HashSet<>(Arrays.asList(docs));kept.removeIf(r->ids.contains(r.chunk.documentId));kept.addAll(Arrays.asList(records));stored.put(key,kept);if(lost)throw new IllegalStateException("write applied; response lost");}
        public Rag.Result search(Rag.VectorQuery q,Rag.Context c){check(q.vector.length>0);List<Rag.Hit> hits=new ArrayList<>();for(Rag.VectorRecord r:stored.getOrDefault(c.accessScope+"/"+q.embeddingSpace,Collections.emptyList()))if(r.chunk.metadataJson.equals(q.request.filterJson)&&hits.size()<q.request.topK)hits.add(new Rag.Hit(r.chunk,c.accessScope,0.5));return new Rag.Result(hits.toArray(new Rag.Hit[0]));}
    }
    static void pipeline(){
        Embed host=new Embed();try(Rag loader=Rag.loader((s,c)->{check(s.equals("manual.txt")&&c.accessScope.equals("tenant_a"));return docs();});Rag e=Rag.embedding(host);Rag split=Rag.utf8Splitter(64,0);Rag store=Rag.memoryVectorStore();Rag r=Rag.vectorRetriever(e,store)){
            Rag.PreparedIndex batch=split.prepareIndex(e,loader.loadDocuments("manual.txt",options()),options());check(r.retrieve(query(),options()).hits.length==0);store.commitIndexJson(batch.toJson(),options());Rag.Result result=r.retrieve(query(),options());check(result.hits.length==1&&result.hits[0].chunk.span.begin==0&&result.hits[0].chunk.span.end==16);check(r.retrieve(query(),new Rag.Options("tenant_b")).hits.length==0);
            Rag.Request all=new Rag.Request("Alpha");all.topK=1;check(r.retrieve(all,options()).truncated);store.commitIndex(new Rag.PreparedIndex(new String[]{"a"},new Rag.VectorRecord[0],"embedding-v1"),options());check(r.retrieve(query(),options()).hits.length==0);
            rejects(()->store.commitIndexJson("{\"documents\":[\"b\"],\"documents\":[],\"records\":[],\"embedding_space\":\"embedding-v1\"}",options()));check(r.retrieve(new Rag.Request("Other"),options()).hits.length==1);
            try(Rag u=Rag.utf8Splitter(4,0)){Rag.PreparedIndex unicode=u.prepareIndex(e,new Rag.Document[]{new Rag.Document("u","utf8.txt","r1","中文a🌏")},options());check(unicode.records[0].chunk.text.equals("中")&&unicode.records[0].chunk.span.end==3);check(unicode.records[1].chunk.text.equals("文a")&&unicode.records[1].chunk.span.begin==3&&unicode.records[1].chunk.span.end==7);check(unicode.records[2].chunk.text.equals("🌏"));}
            rejects(()->loader.loadDocuments("manual.txt",new Rag.Options("../bad")));
        }
    }
    static void customAndInvalid(){
        Embed host=new Embed();Backend backend=new Backend();try(Rag e=Rag.embedding(host);Rag split=Rag.splitter(splitter(false));Rag store=Rag.vectorStore(backend)){
            Rag.PreparedIndex batch=split.prepareIndex(e,docs(),options());check(backend.commits==0);store.commitIndex(batch,options());try(Rag r=Rag.vectorRetriever(e,store)){check(r.retrieve(query(),options()).hits[0].chunk.text.equals("Alpha beta gamma"));}
            backend.lost=true;rejects(()->store.commitIndex(batch,options()));check(backend.commits==2);rejects(()->store.commitIndexJson("{\"documents\":[],\"records\":[],\"embedding_space\":\"embedding-v1\",\"extra\":1}",options()));check(backend.commits==2);
            for(int mode=1;mode<=4;++mode){host.mode=mode;host.spaces=0;rejects(()->split.prepareIndex(e,docs(),options()));}host.mode=0;int calls=host.calls;
            try(Rag bad=Rag.splitter(splitter(true))){rejects(()->bad.prepareIndex(e,docs(),options()));}check(host.calls==calls);
            Rag.Document d=docs()[0];rejects(()->split.prepareIndex(e,new Rag.Document[]{d,d},options()));check(host.calls==calls);
            d.text="\ud800";rejects(()->split.prepareIndex(e,new Rag.Document[]{d},options()));check(host.calls==calls);
        }
        rejects(()->Rag.utf8Splitter(4,4));rejects(()->Rag.memoryVectorStore(0,100,new Rag.Limits()));
    }
    static Rag.Binding binding(){Rag.Binding b=new Rag.Binding("corpus-acl-v1","tenant_a");b.filterJson="{\"kind\":\"manual\"}";b.maxTopK=2;return b;}
    static Agent agent(List<String> turns,String arguments){return new Agent((q,c)->{turns.add(q);Agent.Response r=new Agent.Response("done");if(turns.size()==1)r.calls.add(new Agent.ToolCall("r1","knowledge_search",arguments));return r;});}
    static Agent.Hooks approval(){Agent.Hooks h=new Agent.Hooks();h.authorize=(name,args,required)->{check(required&&name.equals("knowledge_search"));return true;};return h;}
    static Path root()throws IOException{return Files.createTempDirectory("libai-java-rag-",PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rwx------")));}
    static void remove(Path root)throws IOException{try(java.util.stream.Stream<Path> files=Files.walk(root)){files.sorted(Comparator.reverseOrder()).forEach(path->{try{Files.delete(path);}catch(IOException e){throw new RuntimeException(e);}});}}
    static void policyCacheAndBorrow()throws Exception{
        Search host=new Search();try(Rag r=Rag.retriever(host)){
            for(String args:new String[]{"{\"query\":\"Alpha\",\"access_scope\":\"tenant_b\"}","{\"query\":\"Alpha\",\"top_k\":1.5}"})try(Agent a=agent(new ArrayList<>(),args)){a.addRetrievalTool(r,"knowledge_search",binding());Agent.Hooks h=new Agent.Hooks();h.authorize=(name,json,required)->{throw new AssertionError("invalid args reached approval");};a.run("search",new Agent.Options(),h);}check(host.calls==0);
            try(Agent denied=agent(new ArrayList<>(),"{\"query\":\"Alpha\"}")){denied.addRetrievalTool(r,"knowledge_search",binding());denied.run("search");}check(host.calls==0);
            Path root=root();try(FileExecutionStore store=FileExecutionStore.plaintext(root.toString())){List<String> turns=new ArrayList<>();try(Agent a=agent(turns,"{\"query\":\"Alpha\",\"top_k\":2.0}")){Rag.Binding b=binding();a.addRetrievalTool(r,"knowledge_search",b);b.filterJson="{\"kind\":\"public\"}";a.setExecutionStore("java-rag",store);r.close();String result=a.run("search",new Agent.Options(),approval());status(result,"completed");check(result.contains("\"accepted\":false"));check(host.calls==1&&host.key.equals("java-rag.0")&&host.filter.equals("{\"kind\":\"manual\"}"));check(turns.get(1).contains("citation"));rejects(host.saved::stopped);a.run("search");check(host.calls==1&&turns.size()==2);}}finally{remove(root);}
        }
    }
    static void backendErrorsAndTruncation(){Search host=new Search();try(Rag r=Rag.retriever(host)){host.owner=r;r.retrieve(query(),options());host.owner=null;for(int mode:new int[]{1,2,3,4,8}){host.mode=mode;rejects(()->r.retrieve(query(),options()));rejects(host.saved::remainingMs);}host.mode=0;host.text=String.join("",Collections.nCopies(2000,"x"));List<String> turns=new ArrayList<>();try(Agent a=agent(turns,"{\"query\":\"Alpha\"}")){Rag.Binding b=binding();b.maxResultBytes=256;a.addRetrievalTool(r,"knowledge_search",b);status(a.run("search",new Agent.Options(),approval()),"completed");check(turns.get(1).contains("\\\"truncated\\\":true")&&turns.get(1).contains("\\\"hits\\\":[]"));}}}
    static void unknownEffect()throws Exception{Path root=root();try(FileExecutionStore store=FileExecutionStore.plaintext(root.toString())){Search lost=new Search();lost.mode=5;try(Rag r=Rag.retriever(lost);Agent a=agent(new ArrayList<>(),"{\"query\":\"Alpha\"}")){a.addRetrievalTool(r,"knowledge_search",binding());a.setExecutionStore("unknown-rag",store);status(a.run("search",new Agent.Options(),approval()),"recovery_required");check(lost.calls==1);}Search fresh=new Search();try(Rag r=Rag.retriever(fresh);Agent a=agent(new ArrayList<>(),"{\"query\":\"Alpha\"}")){a.addRetrievalTool(r,"knowledge_search",binding());a.setExecutionStore("unknown-rag",store);status(a.run("search"),"recovery_required");check(fresh.calls==0);}}finally{remove(root);}}
    static void cancellationAndNested()throws Exception{
        CountDownLatch entered=new CountDownLatch(1),release=new CountDownLatch(1);AtomicReference<Rag.Context> saved=new AtomicReference<>();AtomicReference<Agent.Cancellation> retained=new AtomicReference<>();AtomicReference<Throwable> error=new AtomicReference<>();
        try(Rag r=Rag.retriever((q,c)->{saved.set(c);retained.set(c.retainCancellation());entered.countDown();try{check(release.await(3,TimeUnit.SECONDS));}catch(InterruptedException e){throw new RuntimeException(e);}check(c.stopped());return new Rag.Result();})){
            Agent.Cancellation token=new Agent.Cancellation();Rag.Options o=options();o.cancellation=token;Thread worker=new Thread(()->{try{r.retrieve(query(),o);}catch(Throwable e){error.set(e);}});worker.start();check(entered.await(3,TimeUnit.SECONDS));rejects(saved.get()::stopped);rejects(r::close);token.close();retained.get().cancel();release.countDown();worker.join(3000);check(!worker.isAlive()&&error.get()!=null);retained.get().close();rejects(saved.get()::remainingMs);
        }
        Search child=new Search();try(Rag nested=Rag.retriever(child);Rag parent=Rag.retriever((q,c)->{try(Rag.ChildOptions opts=c.operationOptions()){check(opts.options.accessScope.equals("tenant_a")&&opts.options.idempotencyKey.equals("host-key"));return nested.retrieve(q,opts.options);}})){check(parent.retrieve(query(),options()).hits.length==1&&child.calls==1);}
        try(Rag childRag=Rag.retriever(child);Rag parent=Rag.retriever((q,c)->{try(Rag.ChildOptions opts=c.operationOptions()){try{Thread.sleep(30);}catch(InterruptedException e){throw new RuntimeException(e);}rejects(()->childRag.retrieve(q,opts.options));return new Rag.Result();}})){Rag.Options o=options();o.timeoutMs=10;rejects(()->parent.retrieve(query(),o));check(child.calls==1);}
    }
    static void nativeWorkers(){
        Set<Thread> threads=Collections.synchronizedSet(new HashSet<Thread>());List<Rag.Context> contexts=Collections.synchronizedList(new ArrayList<Rag.Context>());CountDownLatch barrier=new CountDownLatch(2);
        Rag.Retriever host=(q,c)->{threads.add(Thread.currentThread());contexts.add(c);check(!c.stopped());barrier.countDown();try{check(barrier.await(3,TimeUnit.SECONDS));}catch(InterruptedException e){throw new RuntimeException(e);}return new Rag.Result();};
        try(Rag first=Rag.retriever(host);Rag second=Rag.retriever(host);Workflow graph=new Workflow()){
            Rag[] components={first,second};for(int i=0;i<2;++i){final int index=i;String key="branch"+i;Workflow.NodeSpec n=new Workflow.NodeSpec(key);n.writes=new String[]{key};n.parallelSafe=true;graph.addNode(n,(state,c)->{components[index].retrieve(new Rag.Request("Alpha"),options());return new Workflow.NodeResult("{\"branch"+index+"\":true}");});}
            Workflow.Options o=new Workflow.Options();o.maxParallel=2;status(graph.run("{}",o),"completed");check(threads.size()==2);for(Rag.Context context:contexts)rejects(context::stopped);
        }
    }
    static void collected(WeakReference<?> ref)throws Exception{for(int i=0;i<80&&ref.get()!=null;++i){System.gc();Thread.sleep(20);}check(ref.get()==null);}
    static WeakReference<Embed> failedFactory(){Embed host=new Embed();WeakReference<Embed> ref=new WeakReference<>(host);Rag.Limits l=new Rag.Limits();l.maxDimension=0;rejects(()->Rag.embedding(host,l));return ref;}
    static void lifetimeLargeAndScore()throws Exception{
        collected(failedFactory());Embed host=new Embed();host.dimension=16384;WeakReference<Embed> ref=new WeakReference<>(host);Rag e=Rag.embedding(host),store=Rag.memoryVectorStore(),r=Rag.vectorRetriever(e,store);try(Rag split=Rag.utf8Splitter(4,0)){Rag.PreparedIndex batch=split.prepareIndex(e,docs(),options());long dimensions=0;for(Rag.VectorRecord record:batch.records)dimensions+=record.vector.length;check(dimensions>65536);store.commitIndexJson(batch.toJson(),options());}e.close();store.close();e=null;store=null;host=null;System.gc();check(ref.get()!=null);check(r.retrieve(query(),options()).hits.length>0);r.close();r=null;collected(ref);
        Search exact=new Search();exact.score=Double.longBitsToDouble(0x3feb0e7009b61ce0L);try(Rag retriever=Rag.retriever(exact)){check(Double.doubleToLongBits(retriever.retrieve(query(),options()).hits[0].score)==0x3feb0e7009b61ce0L);}
        Search handler=new Search();WeakReference<Search> h=new WeakReference<>(handler);Rag outer=Rag.retriever(handler);try(Agent a=agent(new ArrayList<>(),"{\"query\":\"Alpha\"}")){a.addRetrievalTool(outer,"knowledge_search",binding());outer.close();outer=null;handler=null;System.gc();check(h.get()!=null);status(a.run("search",new Agent.Options(),approval()),"completed");}collected(h);
    }
    public static void main(String[] args)throws Exception{pipeline();customAndInvalid();policyCacheAndBorrow();backendErrorsAndTruncation();unknownEffect();cancellationAndNested();nativeWorkers();lifetimeLargeAndScore();System.out.println("Java RAG tests passed");}
}
