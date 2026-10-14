package io.coolder.libai;

import java.util.*;

/** Independent host facts. All acceptance decisions use the shared native evaluator. */
public final class TaskContract {
    public static final class Evidence {
        public final String state,revision,authority,reference;
        public Evidence(){this("unknown","","","");}
        public Evidence(String state,String revision,String authority,String reference){
            this.state=TaskJson.checked(state);this.revision=TaskJson.checked(revision);
            this.authority=TaskJson.checked(authority);this.reference=TaskJson.checked(reference);
        }
        private Map<String,Object> value(){return TaskJson.object("state",state,"revision",revision,"authority",authority,"reference",reference);}
    }
    public static final class Requirements {
        public boolean generation=true,build=true,tests=true,review=true;
        public Requirements(){ }
        public Requirements(boolean generation,boolean build,boolean tests,boolean review){
            this.generation=generation;this.build=build;this.tests=tests;this.review=review;
        }
        private Map<String,Object> value(){return TaskJson.object("generation",generation,"build",build,"tests",tests,"review",review);}
    }
    public static final class Checks {
        public final boolean loop,generation,build,tests,review;
        private Checks(Map<String,Object> value){loop=TaskJson.bool(value,"loop",false);generation=TaskJson.bool(value,"generation",false);
            build=TaskJson.bool(value,"build",false);tests=TaskJson.bool(value,"tests",false);review=TaskJson.bool(value,"review",false);}
    }
    public static final class Evaluation {
        public final int version=1;
        public final boolean accepted;
        public final Checks checks;
        public final List<String> unmet;
        private Evaluation(Map<String,Object> value){
            accepted=TaskJson.bool(value,"accepted",false);checks=new Checks(TaskJson.map(value.get("checks")));
            List<String> items=new ArrayList<>();for(Object item:(List<?>)value.get("unmet"))items.add((String)item);
            unmet=Collections.unmodifiableList(items);
        }
    }
    public int version=1;
    public boolean loopFinished=false;
    public String loopOutcome="",artifactRevision="";
    public Requirements requirements=new Requirements();
    public Evidence generation=new Evidence(),build=new Evidence(),tests=new Evidence(),review=new Evidence();

    /** Encode owned facts only. Call evaluate to validate and derive acceptance. */
    public String toJson(){
        if(requirements==null||generation==null||build==null||tests==null||review==null)
            throw new IllegalArgumentException("task requirements/evidence required");
        return TaskJson.write(TaskJson.object("version",version,"loop_finished",loopFinished,
            "loop_outcome",TaskJson.checked(loopOutcome),"artifact_revision",TaskJson.checked(artifactRevision),
            "requirements",requirements.value(),"generation",generation.value(),"build",build.value(),"tests",tests.value(),"review",review.value()));
    }
    private static Evaluation evaluateWire(String wire){
        Map<String,Object> result=TaskJson.map(TaskJson.read(Governance.call("tasks.evaluate",wire),2*1024*1024));
        if(!TaskJson.bool(result,"ok",false)){
            String error=TaskJson.text(result,"error","task contract evaluation failed");
            if(error.equals("unknown governance operation"))throw new UnsupportedOperationException("native library lacks task contract evaluation");
            throw new IllegalArgumentException(error);
        }
        return new Evaluation(result);
    }
    /** Does not compile, test, open references, or authenticate authority claims. */
    public Evaluation evaluate(){return evaluateWire(toJson());}
    public static TaskContract fromJson(String wire){
        evaluateWire(wire); // Original wire must validate before any key or number normalization.
        Map<String,Object> value=TaskJson.map(TaskJson.read(wire,2*1024*1024));
        TaskContract out=new TaskContract();out.loopFinished=TaskJson.bool(value,"loop_finished",false);
        out.loopOutcome=TaskJson.text(value,"loop_outcome","");out.artifactRevision=TaskJson.text(value,"artifact_revision","");
        if(value.containsKey("requirements")){Map<String,Object> r=TaskJson.map(value.get("requirements"));
            out.requirements=new Requirements(TaskJson.bool(r,"generation",true),TaskJson.bool(r,"build",true),TaskJson.bool(r,"tests",true),TaskJson.bool(r,"review",true));}
        out.generation=evidence(value,"generation");out.build=evidence(value,"build");out.tests=evidence(value,"tests");out.review=evidence(value,"review");return out;
    }
    private static Evidence evidence(Map<String,Object> root,String stage){
        if(!root.containsKey(stage))return new Evidence();Map<String,Object> e=TaskJson.map(root.get(stage));
        return new Evidence(TaskJson.text(e,"state","unknown"),TaskJson.text(e,"revision",""),TaskJson.text(e,"authority",""),TaskJson.text(e,"reference",""));
    }
    /** Read actual Agent JSON. Missing contracts are never upgraded to trusted facts.
     * Result envelope is bounded to 8 Mi UTF-16 chars, 64 levels and 65,536 nodes. */
    public static TaskContract fromRunResult(String runJson){
        Map<String,Object> run=TaskJson.map(TaskJson.read(runJson,8*1024*1024));
        if(!(run.get("task_contract") instanceof Map))throw new IllegalArgumentException("run result has no task contract");
        return fromJson(TaskJson.write(run.get("task_contract")));
    }
    // Trusted host helpers: supply actual report revision/reference, never model text.
    public TaskContract recordGeneration(String revision,String reference){generation=new Evidence("completed",revision,"host",reference);return this;}
    public TaskContract recordBuild(String revision,String reference,boolean passed){build=new Evidence(passed?"passed":"failed",revision,"host",reference);return this;}
    public TaskContract recordTests(String revision,String reference,boolean passed){tests=new Evidence(passed?"passed":"failed",revision,"host",reference);return this;}
    public TaskContract recordReview(String revision,String reference,boolean accepted){review=new Evidence(accepted?"accepted":"rejected",revision,"human",reference);return this;}
}
