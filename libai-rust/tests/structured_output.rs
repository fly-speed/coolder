use libai::{Agent,ModelResponse,ToolCall,ToolResult};
use std::{cell::Cell,rc::Rc};
#[test]
fn structured_output_metadata_and_guard(){
 let effects=Rc::new(Cell::new(0));let counted=effects.clone();let mut turns=0;
 let mut agent=Agent::new(move |q,_|{
  assert!(q.contains("\"output_schema\""));turns+=1;
  if turns==1 {ModelResponse{calls:vec![ToolCall{id:"call".into(),name:"write".into(),arguments:"{}".into()}],..Default::default()}}
  else if turns==2 {ModelResponse{text:"{}".into(),..Default::default()}}
  else {assert!(q.contains("\"output_repair\":true") && q.contains("\"tools\":[]"));ModelResponse{text:"{\"n\":2}".into(),..Default::default()}}
 },"").unwrap();
 agent.configure_output(r#"{"type":"object","required":["n"],"properties":{"n":{"type":"integer"}}}"#,1).unwrap();
 assert!(agent.configure_output(r#"{"const":1}"#,1).is_err());assert!(agent.configure_output("",9).is_err());
 agent.add_tool("write","write","{}",false,move|_,_|{counted.set(counted.get()+1);ToolResult{text:"ok".into(),error:false}}).unwrap();
 let result=agent.run("work").unwrap();assert!(result.contains("\"output_validated\":true") && result.contains("\"output_repairs\":1"));
 assert!(result.contains("\"accepted\":false"));assert_eq!(effects.get(),1);
}
