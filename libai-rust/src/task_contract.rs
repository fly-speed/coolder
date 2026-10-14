//! Independent host facts; acceptance always comes from the shared native evaluator.
use crate::{
    governance_call,
    mcp::quote,
    task_values::{parse, Value},
    Error,
};
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TaskEvidence {
    pub state: String,
    pub revision: String,
    pub authority: String,
    pub reference: String,
}
impl Default for TaskEvidence {
    fn default() -> Self {
        Self {
            state: "unknown".into(),
            revision: String::new(),
            authority: String::new(),
            reference: String::new(),
        }
    }
}
impl TaskEvidence {
    fn json(&self) -> String {
        format!(
            "{{\"state\":{},\"revision\":{},\"authority\":{},\"reference\":{}}}",
            quote(&self.state),
            quote(&self.revision),
            quote(&self.authority),
            quote(&self.reference)
        )
    }
    fn from(value: &Value) -> Result<Self, Error> {
        Ok(Self {
            state: value.optional_text("state", "unknown")?,
            revision: value.optional_text("revision", "")?,
            authority: value.optional_text("authority", "")?,
            reference: value.optional_text("reference", "")?,
        })
    }
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TaskRequirements {
    pub generation: bool,
    pub build: bool,
    pub tests: bool,
    pub review: bool,
}
impl Default for TaskRequirements {
    fn default() -> Self {
        Self {
            generation: true,
            build: true,
            tests: true,
            review: true,
        }
    }
}
impl TaskRequirements {
    fn json(&self) -> String {
        format!(
            "{{\"generation\":{},\"build\":{},\"tests\":{},\"review\":{}}}",
            self.generation, self.build, self.tests, self.review
        )
    }
    fn from(value: &Value) -> Result<Self, Error> {
        Ok(Self {
            generation: value.optional_bool("generation", true)?,
            build: value.optional_bool("build", true)?,
            tests: value.optional_bool("tests", true)?,
            review: value.optional_bool("review", true)?,
        })
    }
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TaskChecks {
    pub loop_finished: bool,
    pub generation: bool,
    pub build: bool,
    pub tests: bool,
    pub review: bool,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TaskEvaluation {
    pub version: u32,
    pub accepted: bool,
    pub checks: TaskChecks,
    pub unmet: Vec<String>,
}
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct TaskContract {
    pub loop_finished: bool,
    pub loop_outcome: String,
    pub artifact_revision: String,
    pub requirements: TaskRequirements,
    pub generation: TaskEvidence,
    pub build: TaskEvidence,
    pub tests: TaskEvidence,
    pub review: TaskEvidence,
}
fn evaluate_wire(wire: &str) -> Result<TaskEvaluation, Error> {
    let result = parse(&governance_call("tasks.evaluate", wire)?, 2 * 1024 * 1024)?;
    if !result.member("ok")?.boolean()? {
        let error = result.optional_text("error", "task contract evaluation failed")?;
        return Err(Error(if error == "unknown governance operation" {
            "native library lacks task contract evaluation".into()
        } else {
            error
        }));
    }
    let checks = result.member("checks")?;
    Ok(TaskEvaluation {
        version: 1,
        accepted: result.member("accepted")?.boolean()?,
        checks: TaskChecks {
            loop_finished: checks.member("loop")?.boolean()?,
            generation: checks.member("generation")?.boolean()?,
            build: checks.member("build")?.boolean()?,
            tests: checks.member("tests")?.boolean()?,
            review: checks.member("review")?.boolean()?,
        },
        unmet: result
            .member("unmet")?
            .array()?
            .iter()
            .map(|v| v.text().map(str::to_owned))
            .collect::<Result<_, _>>()?,
    })
}
impl TaskContract {
    /// Encode facts only. Evaluation validates them and derives acceptance.
    pub fn to_json(&self) -> String {
        format!("{{\"version\":1,\"loop_finished\":{},\"loop_outcome\":{},\"artifact_revision\":{},\"requirements\":{},\"generation\":{},\"build\":{},\"tests\":{},\"review\":{}}}",
        self.loop_finished,quote(&self.loop_outcome),quote(&self.artifact_revision),self.requirements.json(),
        self.generation.json(),self.build.json(),self.tests.json(),self.review.json())
    }
    /// Does not execute tools, open references, or authenticate host claims.
    pub fn evaluate(&self) -> Result<TaskEvaluation, Error> {
        evaluate_wire(&self.to_json())
    }
    pub fn from_json(wire: &str) -> Result<Self, Error> {
        evaluate_wire(wire)?; // Validate original bytes before decoding or normalization.
        let value = parse(wire, 2 * 1024 * 1024)?;
        let object = value.object()?;
        let evidence = |name: &str| {
            object
                .get(name)
                .map_or_else(|| Ok(TaskEvidence::default()), TaskEvidence::from)
        };
        Ok(Self {
            loop_finished: value.optional_bool("loop_finished", false)?,
            loop_outcome: value.optional_text("loop_outcome", "")?,
            artifact_revision: value.optional_text("artifact_revision", "")?,
            requirements: object
                .get("requirements")
                .map_or_else(|| Ok(TaskRequirements::default()), TaskRequirements::from)?,
            generation: evidence("generation")?,
            build: evidence("build")?,
            tests: evidence("tests")?,
            review: evidence("review")?,
        })
    }
    /// Read actual Agent JSON, never infer facts from model text. The outer
    /// envelope is bounded to 8 MiB, 64 containers and 65,536 values.
    pub fn from_run_result(wire: &str) -> Result<Self, Error> {
        let result = parse(wire, 8 * 1024 * 1024)?;
        let contract = result.member("task_contract")?;
        contract.object()?;
        Self::from_json(&contract.json())
    }
    // Trusted host helpers: pass actual report revisions/references and human decisions.
    pub fn record_generation(
        &mut self,
        revision: impl Into<String>,
        reference: impl Into<String>,
    ) -> &mut Self {
        self.generation = TaskEvidence {
            state: "completed".into(),
            revision: revision.into(),
            authority: "host".into(),
            reference: reference.into(),
        };
        self
    }
    pub fn record_build(
        &mut self,
        revision: impl Into<String>,
        reference: impl Into<String>,
        passed: bool,
    ) -> &mut Self {
        self.build = TaskEvidence {
            state: if passed { "passed" } else { "failed" }.into(),
            revision: revision.into(),
            authority: "host".into(),
            reference: reference.into(),
        };
        self
    }
    pub fn record_tests(
        &mut self,
        revision: impl Into<String>,
        reference: impl Into<String>,
        passed: bool,
    ) -> &mut Self {
        self.tests = TaskEvidence {
            state: if passed { "passed" } else { "failed" }.into(),
            revision: revision.into(),
            authority: "host".into(),
            reference: reference.into(),
        };
        self
    }
    pub fn record_review(
        &mut self,
        revision: impl Into<String>,
        reference: impl Into<String>,
        accepted: bool,
    ) -> &mut Self {
        self.review = TaskEvidence {
            state: if accepted { "accepted" } else { "rejected" }.into(),
            revision: revision.into(),
            authority: "human".into(),
            reference: reference.into(),
        };
        self
    }
}
