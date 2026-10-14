import io.coolder.libai.*;
public final class StructuredOutputTest {
 static void check(boolean value){if(!value)throw new AssertionError();}
 public static void main(String[] args){
  int[] turns={0},effects={0},visible={0};
  try(Agent agent=new Agent((q,inv)->{
   check(q.contains("\"output_schema\""));Agent.Response r=new Agent.Response("{}");
   if(++turns[0]==1)r.calls.add(new Agent.ToolCall("call","write","{}"));
   else if(turns[0]==3){check(q.contains("\"output_repair\":true") && q.contains("\"tools\":[]"));r.text="{\"n\":2}";}
   return r;
  },"")){
   agent.configureOutput("{\"type\":\"object\",\"required\":[\"n\"],\"properties\":{\"n\":{\"type\":\"integer\"}}}",1);
   boolean invalid=false;try{agent.configureOutput("{\"const\":1}",1);}catch(RuntimeException e){invalid=true;}check(invalid);
   for(long count:new long[]{-1,9,4294967296L}){invalid=false;try{agent.configureOutput("",count);}catch(RuntimeException e){invalid=true;}check(invalid);}
   agent.addTool("write","write","{}",false,(c,i)->{effects[0]++;return new Agent.ToolResult("ok");});
   Agent.Hooks hooks=new Agent.Hooks();hooks.onText=text->{visible[0]++;check(text.equals("{\"n\":2}"));return true;};
   String result=agent.run("work",new Agent.Options(),hooks);
   check(result.contains("\"output_validated\":true") && result.contains("\"output_repairs\":1"));
   check(result.contains("\"accepted\":false") && effects[0]==1 && turns[0]==3 && visible[0]==1);
  }
 }
}
