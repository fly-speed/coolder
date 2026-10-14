package io.coolder.app;

import io.coolder.libai.*;
import java.nio.file.*;
import java.nio.file.attribute.BasicFileAttributes;
import java.io.*;
import java.util.*;
import java.util.concurrent.*;

/** Java-owned compiler gate. Only fixed libai sandbox plans may execute. */
final class Validation {
  static final Set<String> EXCLUDED=new HashSet<>(Arrays.asList(".git",".libai_agent",".libai-build",
      "build","target","dist","node_modules","var",".env","secrets","private"));
  static final Set<String> BUILDS=new HashSet<>(Arrays.asList("go.build","rust.build","java.compile-main",
      "kotlin.compile-main","swift.build","csharp.build","d.build","objective-c.build",
      "javascript.syntax-check","python.syntax-check","php.syntax-check","c.make","cpp.make"));
  static final class Gate {
    Map<String,Object> report; String error="";
    static Gate failure(String error){Gate g=new Gate();g.error=error;return g;}
    static Gate satisfied(Map<String,Object> report){Gate g=new Gate();g.report=report;return g;}
  }
  static Map<String,Object> raw(Path root,String op,Object args){
    return Json.map(Json.parse(new CodingService(root.toString()).call(op,Json.encode(args))));
  }
  static Map<String,Object> checked(Path root,String op,Object args){
    Map<String,Object> result=raw(root,op,args);
    if(!Json.flag(result,"ok",false))throw new IllegalStateException(Json.text(result,"error"));
    return result;
  }
  static List<Object> pending(Projects projects,Runs.Job job){
    synchronized(projects){
      String session=Json.text(job.record,"session_id");Map<String,Object> latest=new LinkedHashMap<>();
      for(Object value:projects.records("runs",job.actor)){
        Map<String,Object> run=Json.map(value);
        if(!Objects.equals(run.get("path"),job.record.get("path"))
            ||(!Objects.equals(run.get("id"),job.record.get("id"))
               &&(session.isEmpty()||!session.equals(Json.text(run,"session_id")))))continue;
        if(run.get("changes") instanceof List)for(Object item:Json.list(run.get("changes")))
          latest.put(Json.text(Json.map(item),"relative_path"),item);
      }
      List<Object> result=new ArrayList<>();
      for(Object value:latest.values())if(Json.text(Json.map(value),"review_status").equals("pending"))
        result.add(ExecutionContext.copy(Json.map(value)));
      return result;
    }
  }
  static String noCompile(List<Object> changes){
    if(changes.isEmpty())return "本次会话没有待验证的文件修改";
    for(Object value:changes){Map<String,Object> c=Json.map(value);String path=Json.text(c,"relative_path");
      if(!Json.text(c,"operation").equals("write")||!path.matches(".*\\.(md|txt|rst|adoc)$"))return "";}
    return "本次只修改文档或文本文件，无需编译";
  }
  static String revision(Path root,List<Object> changes){
    String revision=Json.text(checked(root,"tasks.revision",Json.obj("changes",changes)),"artifact_revision");
    if(revision.isEmpty())throw new IllegalStateException("missing artifact revision");return revision;
  }
  static void verifyReceipts(Path project,Map<String,Object> report)throws Exception{
    String parent=Json.text(report,"parent_idempotency_key");
    if(parent.isEmpty())throw new IOException("编译收据缺少父执行键");
    Path expected=project.resolve(".libai_agent/validation_runs")
      .resolve(Store.hash(parent.getBytes(java.nio.charset.StandardCharsets.UTF_8))).resolve("project");
    if(!expected.toRealPath().equals(Paths.get(Json.text(checked(project,"storage.canonical_path",Json.obj("path",Json.text(report,"receipt_root"))),"path")).toRealPath()))
      throw new IOException("编译收据目录不匹配");
    Map<String,Object> ownership=checked(expected,"sandbox.parent.inspect",Json.obj("idempotency_key",parent,"project_path",""));
    if(!Json.flag(ownership,"found",false))throw new IOException("编译收据父关联缺失");
    List<Object> commands=Json.list(report.get("commands"));Set<String> keys=new HashSet<>();
    if(commands.isEmpty()||commands.size()>16)throw new IOException("编译收据命令数量无效");
    for(Object item:commands){Map<String,Object> command=Json.map(item);String key=Json.text(command,"idempotency_key");
      if(key.isEmpty()||!keys.add(key))throw new IOException("编译收据键缺失或重复");
      Map<String,Object> inspected=checked(expected,"sandbox.inspect",Json.obj("idempotency_key",key));
      if(!Json.flag(inspected,"found",false))throw new IOException("编译收据缺失");
      Map<String,Object> receipt=Json.map(Json.parse(Json.text(inspected,"receipt_json")));
      if(!Arrays.asList("finished","confirmed_applied").contains(Json.text(receipt,"state"))
          ||!Json.text(receipt,"project_path").isEmpty()||!key.equals(Json.text(receipt,"key"))
          ||!Json.text(command,"command_id").equals(Json.text(receipt,"command_id")))throw new IOException("编译收据状态或身份不匹配");
      Map<String,Object> result=Json.map(receipt.get("result"));
      if(!Json.flag(result,"broker_ok",false)||!Json.flag(result,"started",false)||Json.number(result,"exit_code",-1)!=0
          ||Json.flag(result,"cancelled",true)||Json.flag(result,"timed_out",true)||Json.flag(result,"error_present",true))
        throw new IOException("编译收据没有成功执行证据");
      for(String field:Arrays.asList("started","exit_code","cancelled","timed_out","elapsed_ms"))
        if(!Objects.equals(result.get(field),command.get(field)))throw new IOException("编译收据与报告不一致："+field);
    }
    Set<String> children=new HashSet<>();for(Object child:Json.list(ownership.get("children")))
      if(!(child instanceof String)||!children.add((String)child))throw new IOException("编译收据父关联无效");
    if(!keys.equals(children))throw new IOException("编译收据父关联不完整");
  }
  static Gate gate(Projects projects,Runs.Job job,Path root){
    try {
      List<Object> changes=pending(projects,job);String revision=revision(root,changes),reason=noCompile(changes);
      if(!reason.isEmpty())return Gate.satisfied(Json.obj("status","not_required","reason",reason,"commands",new ArrayList<>(),
          "artifact_revision",revision,"checked_at",System.currentTimeMillis()/1000,"proposal_count",changes.size(),"formal_files_changed",false));
      synchronized(projects){Map<String,Object> run=job.record;
        Map<String,Object> report=run.get("validation") instanceof Map?Json.map(run.get("validation")):Json.obj();
        if(revision.equals(Json.text(report,"artifact_revision"))){
          String status=Json.text(report,"status");
          if(status.equals("passed")){verifyReceipts(root,report);return Gate.satisfied(ExecutionContext.copy(report));}
          if(status.equals("not_required"))return Gate.failure("编译收据不足：当前修改需要编译");
          if(status.equals("unverified"))return Gate.failure("编译未验证："+Json.text(report,"reason"));
        }
        long failures=0;if(run.get("validation_attempts") instanceof List)
          for(Object a:Json.list(run.get("validation_attempts")))if(Json.text(Json.map(a),"status").equals("failed"))++failures;
        if(failures>=4)return Gate.failure("编译验证仍未通过，已达到自动修复次数上限；请查看编译输出后继续任务。");
      }
      return new Gate();
    }catch(Exception e){return Gate.failure("成果版本无法验证："+e);}
  }
  static void removeTree(Path root)throws IOException{
    if(!Files.exists(root,LinkOption.NOFOLLOW_LINKS))return;
    Files.walkFileTree(root,new SimpleFileVisitor<Path>(){
      public FileVisitResult visitFile(Path p,BasicFileAttributes a)throws IOException{Files.delete(p);return FileVisitResult.CONTINUE;}
      public FileVisitResult postVisitDirectory(Path p,IOException e)throws IOException{if(e!=null)throw e;Files.delete(p);return FileVisitResult.CONTINUE;}
    });
  }
  static void copyTree(Path src,Path dst,int depth,long[] counts)throws Exception{
    copyTree(src,dst,depth,counts,null);
  }
  static void copyTree(Path src,Path dst,int depth,long[] counts,Agent.Invocation inv)throws Exception{
    if(Files.isSymbolicLink(src)||depth>24)throw new IOException("invalid validation snapshot directory");
    Store.privateDir(dst);
    try(DirectoryStream<Path> entries=Files.newDirectoryStream(src)){
      for(Path entry:entries){String name=entry.getFileName().toString();if(EXCLUDED.contains(name))continue;
        if(inv!=null&&inv.stopped())throw new IOException("validation cancelled");
        if(++counts[0]>10000)throw new IOException("validation snapshot exceeds entry limit");
        BasicFileAttributes attr=Files.readAttributes(entry,BasicFileAttributes.class,LinkOption.NOFOLLOW_LINKS);
        if(attr.isSymbolicLink())throw new IOException("validation snapshot refuses symlinks");
        if(attr.isDirectory())copyTree(entry,dst.resolve(name),depth+1,counts,inv);
        else if(attr.isRegularFile()){
          try(InputStream in=Files.newInputStream(entry,StandardOpenOption.READ,LinkOption.NOFOLLOW_LINKS);
              OutputStream out=Files.newOutputStream(dst.resolve(name),StandardOpenOption.CREATE_NEW)){
            byte[] buffer=new byte[8192];int n;while((n=in.read(buffer))!=-1){
              if(inv!=null&&inv.stopped())throw new IOException("validation cancelled");
              counts[1]+=n;if(counts[1]>128L*1024*1024)throw new IOException("validation snapshot exceeds copy limit");out.write(buffer,0,n);}
          }
          if(entry.getFileSystem().supportedFileAttributeViews().contains("posix"))
            Files.setPosixFilePermissions(dst.resolve(name),Files.getPosixFilePermissions(entry,LinkOption.NOFOLLOW_LINKS));
        }else throw new IOException("unsupported snapshot file");
      }
    }
  }
  static void overlay(Workspace w,Map<String,Object> c)throws Exception{
    Path p=w.resolve(Json.text(c,"relative_path"),false);
    switch(Json.text(c,"operation")){
      case "write":Store.privateDir(p.getParent());Files.write(p,Json.text(c,"content").getBytes(java.nio.charset.StandardCharsets.UTF_8));break;
      case "mkdir":Store.privateDir(p);break;
      case "delete":removeTree(p);break;
      case "move":Path target=w.resolve(Json.text(c,"target_path"),false);Store.privateDir(target.getParent());Files.move(p,target);break;
      default:throw new IllegalArgumentException("unknown validation proposal operation");
    }
  }
  static boolean build(String id){return BUILDS.contains(id)||id.endsWith(".cmake.configure")||id.endsWith(".cmake.build");}
  static String fileHash(Path path)throws Exception{
    java.security.MessageDigest digest=java.security.MessageDigest.getInstance("SHA-256");long bytes=0;
    try(InputStream in=Files.newInputStream(path,StandardOpenOption.READ,LinkOption.NOFOLLOW_LINKS)){
      byte[] buffer=new byte[8192];int n;while((n=in.read(buffer))!=-1){
        bytes+=n;if(bytes>128L*1024*1024)throw new IOException("source hash exceeds copy limit");digest.update(buffer,0,n);}
    }
    StringBuilder hex=new StringBuilder();for(byte b:digest.digest())hex.append(String.format("%02x",b&255));return hex.toString();
  }
  static Map<Path,String> sourceSnapshot(Path root)throws Exception{
    Map<Path,String> files=new LinkedHashMap<>();
    Files.walkFileTree(root,new SimpleFileVisitor<Path>(){
      public FileVisitResult preVisitDirectory(Path p,BasicFileAttributes a){
        return !p.equals(root)&&EXCLUDED.contains(p.getFileName().toString())?FileVisitResult.SKIP_SUBTREE:FileVisitResult.CONTINUE;
      }
      public FileVisitResult visitFile(Path p,BasicFileAttributes a)throws IOException{
        if(!a.isRegularFile()||a.isSymbolicLink())throw new IOException("invalid snapshot file");
        try{files.put(root.relativize(p),fileHash(p));}catch(Exception e){throw new IOException(e);}return FileVisitResult.CONTINUE;
      }
    });return files;
  }
  static boolean unchanged(Path root,Map<Path,String> files)throws Exception{
    Workspace w=new Workspace(root,new ArrayList<>());
    for(Map.Entry<Path,String> entry:files.entrySet()){
      Path p=w.resolve(entry.getKey().toString().replace(File.separatorChar,'/'),false);
      if(!Files.isRegularFile(p,LinkOption.NOFOLLOW_LINKS)||Files.size(p)>128L*1024*1024
          ||!entry.getValue().equals(fileHash(p)))return false;
    }return true;
  }
  static void bound(Map<String,Object> result){for(String k:Arrays.asList("stdout","stderr")){
    String text=Json.text(result,k);if(text.codePointCount(0,text.length())>8192){
      result.put(k,text.substring(0,text.offsetByCodePoints(0,8192)));result.put("output_truncated",true);}
  }}
  static Map<String,Object> boundReport(Map<String,Object> report){
    for(int limit=1024;Json.encode(report).getBytes(java.nio.charset.StandardCharsets.UTF_8).length>60000;limit/=2){
      if(report.get("commands") instanceof List)for(Object c:Json.list(report.get("commands"))){
        Map<String,Object> command=Json.map(c);
        for(String key:Arrays.asList("stdout","stderr")){
          String value=Json.text(command,key);if(value.codePointCount(0,value.length())>limit){
            command.put(key,value.substring(0,value.offsetByCodePoints(0,limit)));command.put("output_truncated",true);}
        }
        if(limit==0){command.remove("fixed_arguments");command.remove("executable");}
      }
      report.put("output_truncated",true);
      if(limit==0&&Json.encode(report).getBytes(java.nio.charset.StandardCharsets.UTF_8).length>60000)
        return Json.obj("status","unverified","reason","validation report exceeds output budget","commands",new ArrayList<>(),
            "artifact_revision",report.get("artifact_revision"),"output_truncated",true);
    }
    return report;
  }
  static void cancel(Path root,String id){
    try{raw(root,"sandbox.cancel",Json.obj("id",id));}catch(RuntimeException ignored){}
  }
  static Map<String,Object> execute(Path root,String helper,Map<String,Object> plan,Agent.Invocation inv,Runs.Job job,String executionKey)throws Exception{
    ExecutorService worker=Executors.newSingleThreadExecutor();String id=Json.text(plan,"id"),parentKey=inv.idempotencyKey();
    Future<Map<String,Object>> future=worker.submit(()->raw(root,"sandbox.execute",Json.obj("id",id,"confirm",true,"helper",helper,"idempotency_key",executionKey,"parent_idempotency_key",parentKey)));
    boolean interrupted=false;
    try{for(;;){try{return future.get(100,TimeUnit.MILLISECONDS);}
      catch(TimeoutException e){if(inv.stopped()||job.cancellation.cancelled()||interrupted)cancel(root,id);}
      catch(InterruptedException e){interrupted=true;cancel(root,id);}
    }}finally{worker.shutdown();if(interrupted)Thread.currentThread().interrupt();}
  }
  static Map<String,Object> validate(Projects projects,Runs.Job job,Path root,String helper,Agent.Invocation inv){
    List<Object> changes=pending(projects,job);long began=System.nanoTime();Path base=null;Map<String,Object> report;String expected="";
    try{
      expected=revision(root,changes);if(inv.stopped())throw new IOException("validation cancelled");
      String parent=inv.idempotencyKey();if(parent.isEmpty())throw new IOException("validation invocation has no execution key");
      Path directory=root.resolve(".libai_agent/validation_runs");Store.privateDir(directory);
      Path candidate=directory.resolve(Store.hash(parent.getBytes(java.nio.charset.StandardCharsets.UTF_8)));Files.createDirectory(candidate);base=candidate;Store.privateDir(base);Path copy=base.resolve("project");copyTree(root,copy,0,new long[2],inv);
      Workspace w=new Workspace(copy,new ArrayList<>());for(Object c:changes)overlay(w,Json.map(c));
      String artifact=revision(copy,new ArrayList<>());
      if(!artifact.equals(expected))throw new IOException("validation snapshot does not match current proposals");
      Map<Path,String> sourceFiles=sourceSnapshot(copy);
      Map<String,Object> toolchain=checked(copy,"projects.toolchain",Json.obj("project_path",""));List<Object> commands=new ArrayList<>();
      boolean cmake=false;for(Object c:Json.list(toolchain.get("commands")))if(Json.text(Json.map(c),"id").endsWith(".cmake.configure"))cmake=true;
      for(Object c:Json.list(toolchain.get("commands"))){String id=Json.text(Json.map(c),"id");if(build(id)&&!(cmake&&(id.equals("c.make")||id.equals("cpp.make"))))commands.add(c);}
      commands.sort(Comparator.comparingInt(c->Json.text(Json.map(c),"id").endsWith(".cmake.configure")?0:1));
      List<Object> results=new ArrayList<>();report=Json.obj("status","passed","reason","待审核修改已通过编译或语法检查","commands",results,"artifact_revision",artifact);
      if(commands.isEmpty())report=Json.obj("status","unverified","reason","没有可用的受控编译或语法检查命令；请检查项目结构和管理员工具链设置",
          "commands",results,"unavailable_tools",toolchain.get("unavailable_tools"));
      if(commands.size()>16)throw new IOException("too many validation commands");
      String parentKey=inv.idempotencyKey();
      if(parentKey.isEmpty())throw new IOException("validation invocation has no execution key");
      for(Object c:commands){if(inv.stopped())throw new IOException("validation cancelled");String id=Json.text(Json.map(c),"id");
        Map<String,Object> plan=checked(copy,"sandbox.preview",Json.obj("project_path","","command_id",id));
        String executionKey="validation."+Store.hash((parentKey+"\n"+artifact+"\n"+Json.encode(c)).getBytes(java.nio.charset.StandardCharsets.UTF_8));
        Map<String,Object> result=execute(copy,helper,plan,inv,job,executionKey);
        if(!executionKey.equals(Json.text(result,"idempotency_key")))throw new IOException("validation command execution key mismatch");
        bound(result);result.putAll(Json.obj("command_id",id,"executable",plan.get("executable"),"fixed_arguments",plan.get("fixed_arguments")));results.add(result);
        if(!Json.flag(result,"started",false)||!Json.flag(result,"ok",false)||Json.number(result,"exit_code",-1)!=0
            ||Json.flag(result,"cancelled",false)||Json.flag(result,"timed_out",false)){
          report.put("status",Json.flag(result,"started",false)?"failed":"unverified");report.put("reason","编译或语法检查未通过");break;}
      }
      if(!unchanged(copy,sourceFiles))
        throw new IOException("source revision changed during compilation");
    }catch(Exception e){report=Json.obj("status","unverified","reason",e.toString(),"commands",new ArrayList<>());}
    finally{if(base!=null)try{Path copy=base.resolve("project");if(Files.isDirectory(copy))
      try(DirectoryStream<Path> entries=Files.newDirectoryStream(copy)){for(Path entry:entries)
        if(!entry.getFileName().toString().equals(".libai_agent"))removeTree(entry);}
      }catch(IOException e){System.err.println("validation cleanup: "+e);}}
    if(!inv.idempotencyKey().isEmpty())report.putAll(Json.obj("receipt_root",root.resolve(".libai_agent/validation_runs")
      .resolve(Store.hash(inv.idempotencyKey().getBytes(java.nio.charset.StandardCharsets.UTF_8))).resolve("project").toString(),
      "parent_idempotency_key",inv.idempotencyKey()));
    if(!report.containsKey("artifact_revision"))report.put("artifact_revision",expected);
    report=boundReport(report);
    report.putAll(Json.obj("checked_at",System.currentTimeMillis()/1000,"elapsed_ms",(System.nanoTime()-began)/1000000,
        "proposal_count",changes.size(),"formal_files_changed",false));return report;
  }
  static Object tool(Projects projects,Runs.Job job,Path root,String helper,Agent.ToolCall call,Agent.Invocation inv)throws Exception{
    String key=inv.idempotencyKey();if(key.isEmpty())key="host-"+Store.id();Map<String,Object> effect;
    synchronized(projects){Map<String,Object> run=job.record;
      effect=Json.obj("version",1L,"key",key,"scope",run.get("execution_scope"),"run_id",run.get("id"),
          "name",call.name,"call_id",call.id,"arguments",call.arguments,"execution_revision","6-durable-validation-receipts","state","running");
      if(!(run.get("tool_effects") instanceof List))run.put("tool_effects",new ArrayList<>());
      Json.list(run.get("tool_effects")).add(effect);run.put("phase","validating");persist(projects,job);
    }
    Gate g=gate(projects,job,root);boolean executed=g.report==null&&g.error.isEmpty();
    Map<String,Object> report=executed?validate(projects,job,root,helper,inv):g.report;
    if(report==null)report=Json.obj("status","unverified","reason",g.error,"validation_blocked",true,"commands",new ArrayList<>());
    synchronized(projects){Map<String,Object> run=job.record;
      if(executed){run.put("validation",report);if(!(run.get("validation_attempts") instanceof List))run.put("validation_attempts",new ArrayList<>());Json.list(run.get("validation_attempts")).add(report);}
      effect.putAll(Json.obj("state","done","output",Json.encode(report),"output_error",false,"reference",Json.text(run,"id")+":tool:"+key));
      run.put("phase","validation_finished");Json.list(run.get("tool_calls")).add(Json.obj("name",call.name,"arguments",call.arguments,"ok",true,"result",report,"executed",executed,"idempotency_key",key));persist(projects,job);
    }
    // A collected compiler failure is successful tool output, not an unknown effect.
    return report;
  }
  static void persist(Projects projects,Runs.Job job)throws Exception{
    try{projects.save();}catch(Exception e){job.record.put("storage_error",e.toString());job.cancellation.cancel();throw e;}
  }
}
