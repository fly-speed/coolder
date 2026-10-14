package io.coolder.app;
import java.nio.file.*;
import java.util.*;
public final class ValidationTest {
  static void check(boolean condition){if(!condition)throw new AssertionError();}
  public static void main(String[] args)throws Exception{
    check(!Validation.noCompile(new ArrayList<>()).isEmpty());
    check(!Validation.noCompile(Arrays.asList(Json.obj("operation","write","relative_path","README.md"))).isEmpty());
    check(Validation.noCompile(Arrays.asList(Json.obj("operation","delete","relative_path","README.md"))).isEmpty());
    check(Validation.noCompile(Arrays.asList(Json.obj("operation","write","relative_path","app.py"))).isEmpty());
    StringBuilder output=new StringBuilder();for(int i=0;i<8192;i++)output.append("🌏\n\"");
    List<Object> commands=new ArrayList<>();for(int i=0;i<16;i++)commands.add(Json.obj("command_id","go.build","started",true,"ok",true,
      "exit_code",0,"stdout",output.toString(),"stderr",output.toString()));
    Map<String,Object> report=Validation.boundReport(Json.obj("status","passed","commands",commands,"artifact_revision","revision"));
    check(Json.encode(report).getBytes("UTF-8").length<=60000&&Json.flag(report,"output_truncated",false));
    check(Json.list(report.get("commands")).size()==16);Json.parse(Json.encode(report));
    Path root=Files.createTempDirectory("java-validation-boundary-");
    try{
      Path source=root.resolve("source"),copy=root.resolve("copy"),outside=root.resolve("outside");
      Files.createDirectories(source);Files.write(source.resolve("app.py"),"print(1)".getBytes("UTF-8"));
      Files.createDirectories(source.resolve("private"));Files.write(source.resolve("private/secret"),"secret".getBytes("UTF-8"));
      Validation.copyTree(source,copy,0,new long[2]);check(Files.exists(copy.resolve("app.py"))&&!Files.exists(copy.resolve("private")));
      Map<Path,String> baseline=Validation.sourceSnapshot(copy);
      Files.write(copy.resolve("generated-output"),"build artifact".getBytes("UTF-8"));check(Validation.unchanged(copy,baseline));
      Files.write(copy.resolve("app.py"),"changed".getBytes("UTF-8"));check(!Validation.unchanged(copy,baseline));
      Files.write(copy.resolve("app.py"),"print(1)".getBytes("UTF-8"));check(Validation.unchanged(copy,baseline));
      Files.write(outside,"outside".getBytes("UTF-8"));Files.createSymbolicLink(source.resolve("link"),outside);
      boolean rejected=false;try{Validation.copyTree(source,root.resolve("bad"),0,new long[2]);}catch(Exception expected){rejected=true;}
      check(rejected&&new String(Files.readAllBytes(outside),"UTF-8").equals("outside"));
      Workspace w=new Workspace(copy,new ArrayList<>());rejected=false;
      try{Validation.overlay(w,Json.obj("operation","write","relative_path","../outside","content","bad"));}catch(Exception expected){rejected=true;}
      check(rejected&&new String(Files.readAllBytes(outside),"UTF-8").equals("outside"));
      Validation.overlay(w,Json.obj("operation","mkdir","relative_path","docs"));
      Validation.overlay(w,Json.obj("operation","write","relative_path","docs/a.txt","content","draft"));
      Validation.overlay(w,Json.obj("operation","move","relative_path","docs/a.txt","target_path","docs/b.txt"));
      check(new String(Files.readAllBytes(copy.resolve("docs/b.txt")),"UTF-8").equals("draft"));
      Validation.overlay(w,Json.obj("operation","delete","relative_path","docs"));check(!Files.exists(copy.resolve("docs")));
    }finally{Validation.removeTree(root);check(!Files.exists(root));}
    System.out.println("Java validation boundaries passed");
  }
}
