//! Owning facade over the built-in POSIX JSON file store, with no JSON dependency.
use crate::{callbacks::cstring, ffi, native_error, Error, ExecutionSnapshot, ExecutionStore};
use std::ffi::{c_char, c_void, CStr};

struct NativeText(*mut c_char);
impl NativeText {
    fn copy(&self) -> Result<String, Error> {
        unsafe { CStr::from_ptr(self.0).to_str() }
            .map(str::to_owned)
            .map_err(|_| Error("invalid UTF-8 execution state".into()))
    }
}
impl Drop for NativeText {
    fn drop(&mut self) {
        unsafe { ffi::libai_string_free(self.0) }
    }
}
/// One leased scope per instance. Store is intentionally neither Send nor Sync.
/// SharedExecutionStore/Rc keeps it alive for configured Agent callbacks.
pub struct FileExecutionStore {
    pointer: *mut c_void,
    scope: String,
}
impl FileExecutionStore {
    pub fn available() -> bool {
        unsafe { ffi::libai_file_execution_store_available() != 0 }
    }
    /// Explicit unencrypted storage. Root is an existing private absolute directory.
    /// Hex JSON encoding provides no encryption or authentication.
    pub fn plaintext(root: &str) -> Result<Self, Error> {
        let root = cstring(root)?;
        let pointer = unsafe { ffi::libai_file_execution_store_create_plaintext(root.as_ptr()) };
        if pointer.is_null() {
            return Err(native_error());
        }
        Ok(Self {
            pointer,
            scope: String::new(),
        })
    }
    /// Release the owned lease. A wrong scope fails without releasing another lease.
    pub fn release(&mut self, scope: &str) -> Result<(), Error> {
        let scope = cstring(scope)?;
        if unsafe { ffi::libai_file_execution_store_close(self.pointer, scope.as_ptr()) } != 0 {
            return Err(native_error());
        }
        self.scope.clear();
        Ok(())
    }
}
impl ExecutionStore for FileExecutionStore {
    fn open(&mut self, scope: &str) -> Result<ExecutionSnapshot, Error> {
        let name = cstring(scope)?;
        let (mut revision, mut state) = (std::ptr::null_mut(), std::ptr::null_mut());
        if unsafe {
            ffi::libai_file_execution_store_open_values(
                self.pointer,
                name.as_ptr(),
                &mut revision,
                &mut state,
            )
        } != 0
        {
            return Err(native_error());
        }
        self.scope = scope.into();
        let revision = NativeText(revision);
        let state = NativeText(state);
        let snapshot = revision.copy().and_then(|revision| {
            state.copy().map(|state_json| ExecutionSnapshot {
                revision,
                state_json,
            })
        });
        if snapshot.is_err() {
            let _ = self.release(scope);
        }
        snapshot
    }
    fn commit(
        &mut self,
        scope: &str,
        expected_revision: &str,
        state_json: &str,
    ) -> Result<String, Error> {
        let scope = cstring(scope)?;
        let expected = cstring(expected_revision)?;
        let state = cstring(state_json)?;
        let pointer = unsafe {
            ffi::libai_file_execution_store_commit(
                self.pointer,
                scope.as_ptr(),
                expected.as_ptr(),
                state.as_ptr(),
            )
        };
        if pointer.is_null() {
            return Err(native_error());
        }
        NativeText(pointer).copy()
    }
    fn close(&mut self, scope: &str) {
        let _ = self.release(scope);
    }
}
impl Drop for FileExecutionStore {
    fn drop(&mut self) {
        if !self.scope.is_empty() {
            if let Ok(scope) = cstring(&self.scope) {
                unsafe {
                    ffi::libai_file_execution_store_close(self.pointer, scope.as_ptr());
                }
            }
        }
        unsafe {
            ffi::libai_file_execution_store_destroy(self.pointer);
        }
    }
}
