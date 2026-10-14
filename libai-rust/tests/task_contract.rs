use libai::{Agent, ModelResponse, TaskContract, TaskEvidence, TaskRequirements};
fn complete() -> TaskContract {
    let mut c = TaskContract {
        loop_finished: true,
        loop_outcome: "completed".into(),
        artifact_revision: "r1".into(),
        ..Default::default()
    };
    c.record_generation("r1", "drafts")
        .record_build("r1", "compiler", true)
        .record_tests("r1", "tests", true)
        .record_review("r1", "review", true);
    c
}
#[test]
fn defaults_and_shared_evaluation() {
    let mut a = TaskContract::default();
    let b = TaskContract::default();
    let result = a.evaluate().unwrap();
    assert!(!result.accepted);
    assert_eq!(
        result.unmet,
        ["loop", "generation", "build", "tests", "review"]
    );
    a.requirements.build = false;
    assert!(b.requirements.build);
    a.loop_finished = true;
    a.loop_outcome = "completed".into();
    a.requirements = TaskRequirements {
        generation: false,
        build: false,
        tests: false,
        review: false,
    };
    assert!(a.evaluate().unwrap().accepted);
    a.loop_finished = false;
    assert!(!a.evaluate().unwrap().accepted);
}
#[test]
fn evidence_and_revision() {
    let mut c = complete();
    assert!(c.evaluate().unwrap().accepted);
    c.artifact_revision = "r2".into();
    assert_eq!(
        c.evaluate().unwrap().unmet,
        ["generation", "build", "tests", "review"]
    );
    c.artifact_revision = "r1".into();
    c.build = TaskEvidence {
        state: "passed".into(),
        revision: "r1".into(),
        authority: "model".into(),
        reference: "claim".into(),
    };
    assert_eq!(c.evaluate().unwrap().unmet, ["build"]);
    c.record_build("r1", "compiler", false);
    assert!(!c.evaluate().unwrap().checks.build);
    c.record_build("r1", "compiler", true);
    c.record_tests("r1", "tests", false);
    assert!(!c.evaluate().unwrap().checks.tests);
    c.record_tests("r1", "tests", true);
    c.record_review("r1", "review", false);
    assert!(!c.evaluate().unwrap().checks.review);
    c.record_review("r1", "", true);
    assert!(!c.evaluate().unwrap().accepted);
    c.record_review("r1", "review", true);
    c.loop_outcome = "recovery_required".into();
    assert_eq!(c.evaluate().unwrap().unmet, ["loop"]);
}
#[test]
fn raw_and_round_trip() {
    let mut c = complete();
    c.record_review("r1", "人工审核🌏", true);
    let wire = c.to_json();
    assert_eq!(TaskContract::from_json(&wire).unwrap(), c);
    let annotated = format!(
        "{},\"accepted\":null,\"checks\":{{\"loop\":false}},\"annotation\":18446744073709551615}}",
        &wire[..wire.len() - 1]
    );
    let imported = TaskContract::from_json(&annotated).unwrap();
    assert_eq!(imported, c);
    assert!(imported.evaluate().unwrap().accepted);
    assert!(
        !TaskContract::from_json(r#"{"accepted":true}"#)
            .unwrap()
            .evaluate()
            .unwrap()
            .accepted
    );
    assert_eq!(
        TaskContract::from_run_result(&format!("{{\"task_contract\":{wire},\"annotation\":null}}"))
            .unwrap(),
        c
    );
}
#[test]
fn malformed_is_not_normalized() {
    for bad in [
        r#"{"version":1,"version":1}"#,
        r#"{"version":1.0}"#,
        r#"{"version":1e0}"#,
        r#"{"requirements":{"build":1}}"#,
        r#"{"build":null}"#,
        r#"{"build":{"state":"accepted"}}"#,
        r#"{"artifact_revision":"\u0000"}"#,
        r#"{"loop_finished":"true"}"#,
    ] {
        assert!(TaskContract::from_json(bad).is_err(), "{bad}");
        assert!(
            TaskContract::from_run_result(&format!("{{\"task_contract\":{bad}}}")).is_err(),
            "{bad}"
        );
    }
    for bad in [
        r#"{"task_contract":{},"task_contract":{}}"#,
        r#"{"task_contract":{},"annotation":0.١}"#,
        r#"{"task_contract":{},"annotation":"\u００４１"}"#,
        r#"{"task_contract":{},"annotation":"\ud800"}"#,
        r#"{"status":"completed"}"#,
    ] {
        assert!(TaskContract::from_run_result(bad).is_err(), "{bad}");
    }
    let mut c = complete();
    c.build.reference = "nul\0".into();
    assert!(c.evaluate().is_err());
}
#[test]
fn exact_boundaries() {
    let c = complete();
    let base = c.to_json();
    let nested = format!("{}null{}", "[".repeat(63), "]".repeat(63));
    assert!(
        TaskContract::from_json(&format!(
            "{},\"annotation\":{nested}}}",
            &base[..base.len() - 1]
        ))
        .unwrap()
        .evaluate()
        .unwrap()
        .accepted
    );
    assert!(TaskContract::from_json(&format!(
        "{},\"annotation\":[{nested}]}}",
        &base[..base.len() - 1]
    ))
    .is_err());
    assert!(TaskContract::from_run_result(&" ".repeat(8 * 1024 * 1024 + 1)).is_err());
    // No numeric conversion/allocation beyond the bounded original token.
    assert!(
        TaskContract::from_run_result(&format!(
            "{{\"task_contract\":{base},\"annotation\":{}}}",
            "9".repeat(100000)
        ))
        .unwrap()
        .evaluate()
        .unwrap()
        .accepted
    );
}
#[test]
fn actual_agent_unknown_facts() {
    let mut agent = Agent::new(
        |_, _| ModelResponse {
            text: "All code built, tests passed, approved!".into(),
            ..Default::default()
        },
        "",
    )
    .unwrap();
    let c = TaskContract::from_run_result(&agent.run("Implement a program").unwrap()).unwrap();
    assert!(c.loop_finished);
    assert_eq!(c.loop_outcome, "completed");
    assert!(!c.evaluate().unwrap().accepted);
    assert_eq!(c.build.state, "unknown");
    assert_eq!(c.tests.state, "unknown");
    assert_eq!(c.review.state, "unknown");
}
#[test]
fn immutable_shared_facts_are_thread_safe() {
    use std::sync::{Arc, Barrier};
    fn traits<T: Send + Sync>() {}
    traits::<TaskContract>();
    let c = Arc::new(complete());
    let copy = c.clone();
    let barrier = Arc::new(Barrier::new(2));
    let b = barrier.clone();
    let worker = std::thread::spawn(move || {
        b.wait();
        copy.evaluate().unwrap().accepted
    });
    barrier.wait();
    assert!(c.evaluate().unwrap().accepted);
    assert!(worker.join().unwrap());
}
