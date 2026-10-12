import io.coolder.libai.CodingService;
/** Exercise JNI coding entry points and supplementary UTF-8 strings. */
public final class CodingTest {
 public static void main(String[] args) {
  CodingService service=new CodingService(args[0]);
  if(!CodingService.available()) {
   try {service.call("capabilities","{}");throw new AssertionError();}
   catch(IllegalStateException expected) {return;}
  }
  if(!service.call("capabilities","{}").contains("workspace.read")) throw new AssertionError();
  String result=service.call("workspace.create","{\"path\":\"java-你好😀.txt\",\"content\":\"JNI😀\"}");
  if(!result.contains("true")) throw new AssertionError(result);
  result=service.call("workspace.read","{\"path\":\"java-你好😀.txt\"}");
  if(!result.contains("JNI😀")) throw new AssertionError(result);
  if(!service.call("workspace.read","{\"path\":\"../escape\"}").contains("false")) throw new AssertionError();
  System.out.println("JNI coding test passed");
 }
}
