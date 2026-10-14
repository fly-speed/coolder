//! Host-owned durable execution storage. Not callable by the model.
use crate::{
    callbacks::{cstring, text},
    ffi, native_error, Error, ToolResult,
};
use std::{
    cell::RefCell,
    ffi::{c_char, c_void},
    panic::{catch_unwind, AssertUnwindSafe},
    rc::Rc,
};

#[derive(Default, Debug)]
pub struct ExecutionSnapshot {
    pub revision: String,
    pub state_json: String,
}
/// Hold an exclusive scope lease between open and close. Commit must implement
/// durable CAS and return a fresh revision. The host owns isolation/encryption.
pub trait ExecutionStore {
    /// A failed acquisition cleans up only this attempt's partial resources.
    fn open(&mut self, scope: &str) -> Result<ExecutionSnapshot, Error>;
    fn commit(
        &mut self,
        scope: &str,
        expected_revision: &str,
        state_json: &str,
    ) -> Result<String, Error>;
    /// Release reliably without panicking. Native close cannot report errors.
    fn close(&mut self, scope: &str);
}
pub type SharedExecutionStore = Rc<RefCell<dyn ExecutionStore>>;
pub(crate) struct StoreHolder(pub SharedExecutionStore);

pub(crate) unsafe extern "C" fn callback(
    user: *mut c_void,
    operation: *const c_char,
    scope: *const c_char,
    revision: *const c_char,
    state: *const c_char,
    result: *mut c_void,
) {
    let response = catch_unwind(AssertUnwindSafe(|| -> Result<(), Error> {
        let holder = &*(user as *const StoreHolder);
        let mut store = holder
            .0
            .try_borrow_mut()
            .map_err(|_| Error("execution store is busy".into()))?;
        let scope = text(scope);
        let mut opened = false;
        let (next, snapshot) = match text(operation).as_str() {
            "open" => {
                let snapshot = store.open(&scope)?;
                opened = true;
                (snapshot.revision, snapshot.state_json)
            }
            "commit" => (
                store.commit(&scope, &text(revision), &text(state))?,
                String::new(),
            ),
            "close" => {
                store.close(&scope);
                (String::new(), String::new())
            }
            _ => return Err(Error("unknown execution operation".into())),
        };
        let converted =
            cstring(&next).and_then(|next| cstring(&snapshot).map(|snapshot| (next, snapshot)));
        match converted {
            Ok((next, snapshot)) => ffi::libai_execution_result_set(
                result,
                next.as_ptr(),
                snapshot.as_ptr(),
                c"".as_ptr(),
            ),
            Err(error) => {
                if opened {
                    store.close(&scope);
                }
                return Err(error);
            }
        }
        Ok(())
    }));
    match response {
        Ok(Ok(())) => (),
        Ok(Err(error)) => {
            let message = cstring(&error.0.replace('\0', "")).unwrap();
            ffi::libai_execution_result_set(result, c"".as_ptr(), c"".as_ptr(), message.as_ptr());
        }
        Err(payload) => {
            crate::callbacks::discard_panic(payload);
            ffi::libai_execution_result_set(
                result,
                c"".as_ptr(),
                c"".as_ptr(),
                c"Rust execution store panicked".as_ptr(),
            )
        }
    }
}

/// Definite external receipt checked by the host; not a model-generated claim.
pub struct ExecutionReceipt<'a> {
    pub scope: &'a str,
    pub input: &'a str,
    pub instructions: &'a str,
    pub expected_revision: &'a str,
    pub key: &'a str,
    pub output: &'a ToolResult,
    /// Must be "none" or "applied".
    pub outcome: &'a str,
    pub reference: &'a str,
}
/// Host-verified remaining budget for migrating an unfinished legacy execution.
/// Zero means unlimited. Never expose this operation as a model tool.
pub struct DeadlineAdoption<'a> {
    pub scope: &'a str,
    pub input: &'a str,
    pub instructions: &'a str,
    pub expected_revision: &'a str,
    pub remaining_ms: i64,
    pub reference: &'a str,
}
/// CAS migration only; does not reconcile any unknown external effect.
pub fn adopt_execution_deadline(
    store: SharedExecutionStore,
    adoption: DeadlineAdoption<'_>,
) -> Result<(), Error> {
    let mut holder = StoreHolder(store);
    let scope = cstring(adoption.scope)?;
    let input = cstring(adoption.input)?;
    let instructions = cstring(adoption.instructions)?;
    let revision = cstring(adoption.expected_revision)?;
    let reference = cstring(adoption.reference)?;
    let status = unsafe {
        ffi::libai_execution_adopt_deadline(
            callback,
            (&mut holder as *mut StoreHolder).cast(),
            scope.as_ptr(),
            input.as_ptr(),
            instructions.as_ptr(),
            revision.as_ptr(),
            adoption.remaining_ms,
            reference.as_ptr(),
        )
    };
    if status == 0 {
        Ok(())
    } else {
        Err(native_error())
    }
}
/// Reconcile only after verifying a definite receipt. Reject stale task/revision/key.
pub fn confirm_execution_effect(
    store: SharedExecutionStore,
    receipt: ExecutionReceipt<'_>,
) -> Result<(), Error> {
    let mut holder = StoreHolder(store);
    let scope = cstring(receipt.scope)?;
    let input = cstring(receipt.input)?;
    let instructions = cstring(receipt.instructions)?;
    let revision = cstring(receipt.expected_revision)?;
    let key = cstring(receipt.key)?;
    let output = cstring(&receipt.output.text)?;
    let outcome = cstring(receipt.outcome)?;
    let reference = cstring(receipt.reference)?;
    let status = unsafe {
        ffi::libai_execution_confirm(
            callback,
            (&mut holder as *mut StoreHolder).cast(),
            scope.as_ptr(),
            input.as_ptr(),
            instructions.as_ptr(),
            revision.as_ptr(),
            key.as_ptr(),
            output.as_ptr(),
            receipt.output.error as i32,
            outcome.as_ptr(),
            reference.as_ptr(),
        )
    };
    if status == 0 {
        Ok(())
    } else {
        Err(native_error())
    }
}
