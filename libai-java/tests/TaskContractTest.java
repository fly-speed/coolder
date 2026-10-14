import io.coolder.libai.*;
import java.util.Arrays;

public final class TaskContractTest {
    static void check(boolean value){if(!value)throw new AssertionError();}
    static TaskContract complete(){TaskContract c=new TaskContract();c.loopFinished=true;c.loopOutcome="completed";c.artifactRevision="r1";
        return c.recordGeneration("r1","drafts").recordBuild("r1","compiler",true).recordTests("r1","tests",true).recordReview("r1","review",true);}
    static void rejected(Runnable operation){boolean failed=false;try{operation.run();}catch(IllegalArgumentException expected){failed=true;}check(failed);}
    public static void main(String[] args){
        TaskContract a=new TaskContract(),b=new TaskContract();TaskContract.Evaluation result=a.evaluate();
        check(!result.accepted&&result.unmet.equals(Arrays.asList("loop","generation","build","tests","review")));
        a.requirements.build=false;check(b.requirements.build);
        boolean frozen=false;try{result.unmet.add("fake");}catch(UnsupportedOperationException expected){frozen=true;}check(frozen);
        TaskContract c=complete();check(c.evaluate().accepted);
        c.artifactRevision="r2";check(c.evaluate().unmet.equals(Arrays.asList("generation","build","tests","review")));
        c.artifactRevision="r1";c.build=new TaskContract.Evidence("passed","r1","model","claim");check(c.evaluate().unmet.equals(Arrays.asList("build")));
        c.recordBuild("r1","compiler",false);check(!c.evaluate().checks.build);c.recordBuild("r1","compiler",true);
        c.recordTests("r1","tests",false);check(!c.evaluate().checks.tests);c.recordTests("r1","tests",true);
        c.recordReview("r1","review",false);check(!c.evaluate().checks.review);c.recordReview("r1","",true);check(!c.evaluate().accepted);
        c.recordReview("r1","人工审核🌏",true);check(c.evaluate().accepted);
        String wire=c.toJson();TaskContract restored=TaskContract.fromJson(wire);check(restored.toJson().equals(wire)&&restored.evaluate().accepted);
        restored.loopOutcome="recovery_required";check(restored.evaluate().unmet.equals(Arrays.asList("loop")));
        TaskContract analysis=new TaskContract();analysis.loopFinished=true;analysis.loopOutcome="completed";
        check(!analysis.evaluate().accepted);analysis.requirements=new TaskContract.Requirements(false,false,false,false);check(analysis.evaluate().accepted);
        StringBuilder nesting=new StringBuilder();for(int i=0;i<63;i++)nesting.append('[');nesting.append("null");for(int i=0;i<63;i++)nesting.append(']');
        String base=analysis.toJson();String deep=base.substring(0,base.length()-1)+",\"annotation\":"+nesting+"}";
        check(TaskContract.fromJson(deep).evaluate().accepted);
        rejected(()->TaskContract.fromJson(base.substring(0,base.length()-1)+",\"annotation\":["+nesting+"]}"));
        for(String bad:new String[]{"{\"version\":1,\"version\":1}","{\"version\":1.0}","{\"version\":1e0}","{\"loop_finished\":\"true\"}",
            "{\"requirements\":{\"build\":1}}","{\"build\":null}","{\"build\":{\"state\":\"accepted\"}}","{\"artifact_revision\":\"\\u0000\"}"}){
            rejected(()->TaskContract.fromJson(bad));rejected(()->TaskContract.fromRunResult("{\"task_contract\":"+bad+"}"));
        }
        rejected(()->TaskContract.fromRunResult("{\"task_contract\":{},\"task_contract\":{}}"));
        rejected(()->TaskContract.fromRunResult("{\"task_contract\":{},\"annotation\":0.١}"));
        rejected(()->TaskContract.fromRunResult("{\"task_contract\":{},\"annotation\":\"\\u００４１\"}"));
        rejected(()->TaskContract.fromRunResult("{\"task_contract\":{},\"annotation\":\"\ud800\"}"));
        rejected(()->TaskContract.fromRunResult("{\"status\":\"completed\"}"));
        check(!TaskContract.fromJson("{\"accepted\":true,\"checks\":{\"loop\":true}}").evaluate().accepted);
        // Unknown annotations are allowed; do not normalize null or large numbers into evidence.
        TaskContract imported=TaskContract.fromJson(wire.substring(0,wire.length()-1)+",\"accepted\":null,\"annotation\":18446744073709551615}");
        check(imported.evaluate().accepted&&!imported.toJson().contains("annotation"));
        check(TaskContract.fromRunResult("{\"task_contract\":"+wire+",\"annotation\":null}").evaluate().accepted);
        try(Agent agent=new Agent((request,invocation)->new Agent.Response("All code built, tests passed, approved!"))){
            TaskContract actual=TaskContract.fromRunResult(agent.run("Implement a program"));
            check(actual.loopFinished&&actual.loopOutcome.equals("completed")&&!actual.evaluate().accepted);
            check(actual.build.state.equals("unknown")&&actual.tests.state.equals("unknown")&&actual.review.state.equals("unknown"));
        }
        System.out.println("PASS: Java independent task facts, raw JSON, current evidence and real Agent result");
    }
}
