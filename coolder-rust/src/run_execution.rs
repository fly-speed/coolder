//! Private host bridge between SDK journals and atomically saved APP receipts.
use crate::{app::App, runs::Job, store::*};
use libai::{ExecutionReceipt, SharedExecutionStore, ToolResult};
use serde_json::Value;
use std::{cell::RefCell, rc::Rc};

pub fn store(app: &App, job: &Job) -> Result<SharedExecutionStore, libai::Error> {
    let state=app.0.state.lock().unwrap();
    let parent=state.data.join("auth/executions");
    let root=parent.join(text(&job.actor,"id"));
    private_dir(&parent).and_then(|_|private_dir(&root)).map_err(|error|libai::Error(error.to_string()))?;
    let root=std::fs::canonicalize(root).map_err(|error|libai::Error(error.to_string()))?;
    Ok(Rc::new(RefCell::new(libai::FileExecutionStore::plaintext(&root.to_string_lossy())?)))
}

// Only a matching durable host receipt may close a journal's ambiguous window.
// Missing receipts keep the SDK running intent intact; no model or handler runs.
pub fn reconcile(app: &App, job: &Job, store: SharedExecutionStore, input: &str, instructions: &str)
    -> Result<(), libai::Error>
{
    let scope=text(&job.args,"_execution_scope");
    let snapshot=store.borrow_mut().open(scope)?;
    store.borrow_mut().close(scope);
    if snapshot.state_json.is_empty(){return Ok(());}
    let journal:Value=serde_json::from_str(&snapshot.state_json).map_err(|_|libai::Error("invalid execution journal".into()))?;
    if journal["phase"]!="tools" {return Ok(());}
    let cursor=journal["cursor"].as_u64().ok_or_else(||libai::Error("invalid journal cursor".into()))? as usize;
    let effect=journal["effects"].get(cursor).ok_or_else(||libai::Error("journal effect missing".into()))?;
    if effect["state"]!="running" || effect["side_effect_free"]==true {return Ok(());}
    let mut state=app.0.state.lock().unwrap();
    let receipts:Vec<Value>=state.rows("runs",&job.actor).iter()
        .filter(|run|text(run,"execution_scope")==scope)
        .flat_map(|run|run["tool_effects"].as_array().into_iter().flatten().filter_map(|receipt|{
            if receipt["key"]==effect["key"] && receipt["run_id"]==run["id"] && receipt["scope"]==run["execution_scope"] {
                Some(receipt.clone())
            }else{None}
        })).collect();
    drop(state);
    if receipts.is_empty(){return Ok(());} // SDK returns recovery_required.
    if receipts.len()!=1 {return Err(libai::Error("ambiguous host receipts".into()));}
    let receipt=&receipts[0];
    if receipt["state"]!="done" || number(receipt,"version",0)!=1 ||
        receipt["name"]!=effect["name"] || receipt["call_id"]!=effect["id"] ||
        receipt["arguments"]!=effect["arguments"] || receipt["execution_revision"]!=effect["revision"] ||
        receipt["output_error"]!=false || text(receipt,"reference").is_empty() ||
        !receipt["output"].is_string() || text(receipt,"output").len()>64*1024 ||
        serde_json::from_str::<Value>(text(receipt,"output")).is_err() {
        return Err(libai::Error("host receipt does not confirm pending tool".into()));
    }
    let output=ToolResult{text:text(receipt,"output").into(),error:false};
    libai::confirm_execution_effect(store,ExecutionReceipt{
        scope,input,instructions,expected_revision:&snapshot.revision,key:text(effect,"key"),
        output:&output,outcome:"applied",reference:text(receipt,"reference"),
    })
}


// Legacy clock adoption uses the original scope row, never a resumed row's new start.
// No budget defaults or caller supplied timestamps are trusted here.
pub fn migrate_deadline(app:&App,job:&Job,store:SharedExecutionStore,input:&str,instructions:&str)
    -> std::result::Result<bool,libai::Error>
{
    let scope=text(&job.args,"_execution_scope");
    let snapshot=store.borrow_mut().open(scope)?;store.borrow_mut().close(scope);
    if snapshot.state_json.is_empty(){return Ok(false);}
    let journal:Value=serde_json::from_str(&snapshot.state_json).map_err(|_|libai::Error("invalid execution journal".into()))?;
    if journal.get("time_budget").is_some() || journal["phase"]=="finished" || !text(&journal,"output_failure").is_empty(){return Ok(false);}
    if journal["phase"]=="tools" {
        let cursor=journal["cursor"].as_u64().and_then(|n|usize::try_from(n).ok()).ok_or_else(||libai::Error("invalid journal cursor".into()))?;
        let effect=journal["effects"].get(cursor).ok_or_else(||libai::Error("journal effect missing".into()))?;
        if effect["state"]=="running" && effect["side_effect_free"]!=true {return Ok(false);}
    }
    let original={
        let mut state=app.0.state.lock().unwrap();
        let matches=state.rows("runs",&job.actor).iter().filter(|run|text(run,"id")==scope).cloned().collect::<Vec<_>>();
        if matches.len()!=1 {return Err(libai::Error("无法核实旧任务唯一原始运行记录；保留检查点等待宿主核对".into()));}
        matches[0].clone()
    };
    let exact=|v:&Value,key:&str|v.get(key).and_then(Value::as_i64).ok_or_else(||libai::Error(format!("旧任务时间/策略字段缺失或不是整数：{key}")));
    if original["durable_execution"]!=true || text(&original,"execution_scope")!=scope {return Err(libai::Error("无法核实旧任务原始时限".into()));}
    let context=&original["execution_context"];
    if exact(context,"version")?!=1 || text(context,"scope")!=scope || text(context,"input")!=input || text(context,"instructions")!=instructions || text(&original,"path")!=text(&job.args,"path") {
        return Err(libai::Error("旧任务冻结身份无法核实".into()));
    }
    let args=&context["args"];let policy=&args["_policy"];
    let key=match text(args,"execution_mode"){"large"=>"large_mode_tool_calls","quick"=>"quick_mode_tool_calls",_=>"standard_mode_tool_calls"};
    let started=exact(&original,"started_at")?;let seconds=exact(policy,"provider_request_timeout_seconds")?;let tools=exact(policy,key)?;
    if started<=0||seconds<=0||tools<0 {return Err(libai::Error("旧任务原始时限字段非法".into()));}
    let overflow=||libai::Error("旧任务时限溢出，不能迁移".into());
    let began=started.checked_mul(1000).ok_or_else(overflow)?;
    let duration=seconds.checked_mul(1000).and_then(|n|tools.checked_add(1).and_then(|calls|n.checked_mul(calls))).ok_or_else(overflow)?;
    let now=std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).ok().and_then(|d|i64::try_from(d.as_millis()).ok()).ok_or_else(||libai::Error("invalid host wall time".into()))?;
    if now<began{return Err(libai::Error("旧任务开始时间在未来，不能迁移".into()));}
    let end=began.checked_add(duration).ok_or_else(overflow)?;if now>=end{return Ok(true);}
    let reference=format!("rust-app:v1:{scope}:started_at:{started}:timeout_ms:{duration}");
    libai::adopt_execution_deadline(store,libai::DeadlineAdoption{scope,input,instructions,expected_revision:&snapshot.revision,remaining_ms:end-now,reference:&reference})?;
    Ok(false)
}
