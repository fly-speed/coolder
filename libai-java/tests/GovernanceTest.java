import io.coolder.libai.*;
public final class GovernanceTest {
    static void check(boolean value){if(!value)throw new AssertionError();}
    public static void main(String[] args){
        check(Governance.call("capabilities","{}").contains("draft4"));
        check(Governance.call("tasks.evaluate","{\"loop_finished\":true,\"loop_outcome\":\"completed\"}").contains("\"accepted\":false"));
        check(Governance.call("tasks.evaluate","{\"loop_finished\":true,\"loop_outcome\":\"completed\",\"requirements\":{\"generation\":false,\"build\":false,\"tests\":false,\"review\":false}}").contains("\"accepted\":true"));
        check(Governance.call("schema.validate","{\"schema_json\":\"{\\\"type\\\":\\\"integer\\\"}\",\"instance_json\":\"\\\"bad\\\"\"}").contains("\"valid\":false"));
        try(Agent bounded=new Agent((request,inv)->{
            check(request.contains("\"model_profile\""));
            check(request.contains("\"max_output_tokens\":7"));
            check(request.contains("\"tools\":[]"));
            return new Agent.Response("done");
        },"")) {
            bounded.configureModel("{\"version\":1,\"tool_calling\":false,\"max_output_tokens\":7}");
            boolean invalid=false;
            try{bounded.configureModel("{\"version\":2}");}catch(RuntimeException e){invalid=true;}
            check(invalid && bounded.run("hello").contains("completed"));
        }
        int[] turn={0},effects={0};
        try(Agent agent=new Agent((request,inv)->{
            Agent.Response out=new Agent.Response("done");
            if(turn[0]++==0)out.calls.add(new Agent.ToolCall("a","write","{\"n\":\"bad\"}"));
            return out;
        },"")){
            agent.addTool("write","write","{\"type\":\"object\",\"required\":[\"n\"],\"properties\":{\"n\":{\"type\":\"integer\"}}}",false,
                (call,inv)->{effects[0]++;return new Agent.ToolResult("ok");});
            check(agent.run("write").contains("completed"));
        }
        check(effects[0]==0);
        System.out.println("PASS: Java portable governance and invalid tool effect guard");
    }
}
