"""Application model/tool callbacks driving the same C++ libai runtime."""
import ctypes as c
from dataclasses import dataclass, field
import json
import threading
from . import _native as n


@dataclass
class ToolCall:
    id: str
    name: str
    arguments: object = field(default_factory=dict)


@dataclass
class ModelResponse:
    text: str = ""
    calls: list = field(default_factory=list)
    reasoning: str = ""
    input_tokens: int = 0
    output_tokens: int = 0
    error: str = ""
    retryable: bool = False


@dataclass
class ToolResult:
    text: str
    error: bool = False


class ExecutionStore:
    """Host store: open returns (revision, raw_state_json) with an exclusive lease.

    commit durably compares expected_revision and returns a fresh revision.
    close releases the lease. Data protection and scope authorization belong to the host.
    """
    def open(self, scope): raise NotImplementedError
    def commit(self, scope, expected_revision, state_json): raise NotImplementedError
    def close(self, scope): raise NotImplementedError


class FileExecutionStore(ExecutionStore):
    """Built-in POSIX store. Explicit plaintext=True accepts unencrypted transcripts.

    Root must already be a private absolute directory (owned uid, mode 0700).
    dispose releases the native owner only while no scope is leased.
    """
    def __init__(self, root, *, plaintext=False, library=None):
        self._handle = None
        self._lock = threading.Lock()
        self._scope = ""
        if plaintext is not True:
            raise ValueError("explicit plaintext=True is required; no encryption is provided")
        self._lib = n.load(library)
        self._handle = self._lib.libai_file_execution_store_create_plaintext(n.utf8(root))
        if not self._handle:
            self._error()

    @staticmethod
    def available(library=None):
        return bool(n.load(library).libai_file_execution_store_available())

    def _error(self):
        raise RuntimeError(self._lib.libai_last_error().decode("utf-8", "replace"))

    def _check(self):
        if not self._handle:
            raise RuntimeError("file execution store disposed")

    def _owned(self, pointer):
        if not pointer:
            self._error()
        try:
            return c.string_at(pointer).decode("utf-8")
        finally:
            self._lib.libai_string_free(pointer)

    def open(self, scope):
        with self._lock:
            self._check()
            raw = self._lib.libai_file_execution_store_open(self._handle, n.utf8(scope))
            if not raw:
                self._error()
            self._scope = scope
            try:
                snapshot = json.loads(self._owned(raw))
                return snapshot["revision"], snapshot["state_json"]
            except BaseException:
                self._lib.libai_file_execution_store_close(self._handle, n.utf8(scope))
                self._scope = ""
                raise

    def commit(self, scope, expected_revision, state_json):
        with self._lock:
            self._check()
            return self._owned(self._lib.libai_file_execution_store_commit(self._handle,
                n.utf8(scope), n.utf8(expected_revision), n.utf8(state_json)))

    def close(self, scope):
        with self._lock:
            self._check()
            if self._lib.libai_file_execution_store_close(self._handle, n.utf8(scope)):
                self._error()
            self._scope = ""

    def dispose(self):
        with self._lock:
            if not self._handle:
                return
            if self._lib.libai_file_execution_store_destroy(self._handle):
                self._error()
            self._handle = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.dispose()

    def __del__(self):
        # Registered callbacks retain this object. An unreachable owner has no live users.
        try:
            if self._handle:
                if self._scope:
                    self.close(self._scope)
                self.dispose()
        except BaseException:
            pass


def _execution_callback(native, store):
    for method in ("open", "commit", "close"):
        if not callable(getattr(store, method, None)):
            raise TypeError("execution store must implement open, commit and close")
    def invoke(user, operation, scope, expected, state, result):
        revision, snapshot = "", ""
        opened = False
        try:
            operation, scope = operation.decode(), scope.decode("utf-8")
            if operation == "open":
                value = store.open(scope)
                opened = True
                revision, snapshot = value
            elif operation == "commit": revision = store.commit(scope, expected.decode("utf-8"), state.decode("utf-8"))
            elif operation == "close": store.close(scope)
            else: raise ValueError("unknown execution store operation")
            native.libai_execution_result_set(result, n.utf8(revision), n.utf8(snapshot), b"")
        except BaseException as error:
            # A failed open must clean up its own partial acquisition. Only close
            # after a successful open; a failed lease attempt may belong to another run.
            if opened:
                try: store.close(scope)
                except BaseException: pass
            try:
                message = (type(error).__name__ + ": " + str(error)).replace("\0", "")
                message = message.encode("utf-8", "replace")[:4096].decode("utf-8", "ignore").encode("utf-8")
            except BaseException:
                message = b"Python execution store callback failed"
            native.libai_execution_result_set(result, b"", b"", message or b"store callback failed")
    return n.EXECUTION(invoke)


def confirm_execution_effect(store, scope, input, instructions, revision, key, output,
                             outcome, reference, library=None):
    """Trusted host reconciliation after checking a definite external receipt."""
    native=n.load(library)
    callback=_execution_callback(native, store)
    output=output if isinstance(output, ToolResult) else ToolResult(output)
    if native.libai_execution_confirm(callback,None,n.utf8(scope),n.utf8(input),n.utf8(instructions),
            n.utf8(revision),n.utf8(key),n.utf8(output.text),output.error,n.utf8(outcome),n.utf8(reference)):
        raise RuntimeError(native.libai_last_error().decode("utf-8", "replace"))


def adopt_execution_deadline(store, scope, input, instructions, revision, remaining_ms,
                            reference, library=None):
    """Migrate an unfinished legacy log only after verifying its remaining budget.

    Zero means unlimited. CAS required; this does not reconcile unknown effects.
    """
    if isinstance(remaining_ms, bool) or not isinstance(remaining_ms, int) or not 0 <= remaining_ms <= (1 << 63) - 1:
        raise ValueError("remaining_ms must be a nonnegative signed 64-bit integer")
    native = n.load(library)
    if not hasattr(native, "libai_execution_adopt_deadline"):
        raise RuntimeError("native library lacks legacy deadline adoption")
    callback = _execution_callback(native, store)
    if native.libai_execution_adopt_deadline(callback, None, n.utf8(scope), n.utf8(input),
            n.utf8(instructions), n.utf8(revision), remaining_ms, n.utf8(reference)):
        raise RuntimeError(native.libai_last_error().decode("utf-8", "replace"))


class Invocation:
    """Borrowed stream/stop boundary, automatically expired after a callback."""
    def __init__(self, native, pointer):
        self._native, self._pointer = native, pointer
        self._thread = threading.get_ident()

    def _check(self):
        if not self._pointer or threading.get_ident() != self._thread:
            raise RuntimeError("invocation has expired")

    def idempotency_key(self):
        self._check()
        return self._native.libai_invocation_idempotency_key(self._pointer).decode("utf-8")

    def stopped(self):
        self._check()
        return bool(self._native.libai_invocation_stopped(self._pointer))

    def remaining_ms(self):
        """Borrowed deadline: -1 unlimited, 0 expired; do not map 0 to unlimited."""
        self._check()
        if not hasattr(self._native, "libai_invocation_remaining_ms"):
            raise RuntimeError("native library does not provide invocation deadline access")
        return self._native.libai_invocation_remaining_ms(self._pointer)

    def retain_cancellation(self):
        """Independently owned Cancellation; fail if absent or retaining it fails."""
        self._check()
        if not hasattr(self._native, "libai_invocation_retain_cancellation"):
            raise RuntimeError("native library does not provide invocation cancellation access")
        handle = self._native.libai_invocation_retain_cancellation(self._pointer)
        if not handle:
            raise RuntimeError("invocation has no cancellation token or retain failed")
        try:
            token = Cancellation.__new__(Cancellation)
            token._lib, token._handle, token._lock = self._native, handle, threading.Lock()
            return token
        except BaseException:
            self._native.libai_cancellation_destroy(handle)
            raise

    def emit(self, text):
        self._check()
        return bool(self._native.libai_invocation_emit(self._pointer, n.utf8(text)))


def _json(value):
    return value if isinstance(value, str) else json.dumps(value, ensure_ascii=False)


class Cancellation:
    """Independent stop state, shareable across runs and threads; close when done."""
    def __init__(self, library=None):
        self._lib = n.load(library)
        self._lock = threading.Lock()
        self._handle = self._lib.libai_cancellation_create()
        if not self._handle:
            raise RuntimeError(self._lib.libai_last_error().decode())

    def _retain(self):
        with self._lock:
            if not self._handle:
                raise RuntimeError("cancellation is closed")
            lease = self._lib.libai_cancellation_retain(self._handle)
            if not lease:
                raise RuntimeError(self._lib.libai_last_error().decode())
            return lease

    def cancel(self):
        with self._lock:
            if not self._handle:
                raise RuntimeError("cancellation is closed")
            self._lib.libai_cancellation_cancel(self._handle)

    @property
    def cancelled(self):
        with self._lock:
            if not self._handle:
                raise RuntimeError("cancellation is closed")
            return bool(self._lib.libai_cancellation_cancelled(self._handle))

    def close(self):
        with self._lock:
            if self._handle:
                self._lib.libai_cancellation_destroy(self._handle)
                self._handle = None

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


class Agent:
    """Own one native agent/session; use as a context manager or call close()."""
    def __init__(self, model, instructions="", library=None):
        if not callable(model):
            raise TypeError("model must be callable")
        self._lib = n.load(library)
        self._lock, self._busy = threading.Lock(), False
        self._handle, self._tools = None, []
        self._model = n.MODEL(lambda user, request, output, invocation:
                              self._complete(model, request, output, invocation))
        self._handle = self._lib.libai_agent_create(self._model, None, n.utf8(instructions))
        if not self._handle:
            self._raise_native()

    @classmethod
    def from_provider(cls, config, api_key, instructions="", library=None):
        """Use built-in desktop transport with normal streaming/tool hooks."""
        agent = cls.__new__(cls)
        agent._lib = n.load(library)
        agent._lock, agent._busy = threading.Lock(), False
        agent._tools, agent._model = [], None
        agent._handle = agent._lib.libai_agent_create_provider(
            n.utf8(_json(config)), n.utf8(api_key), n.utf8(instructions))
        if not agent._handle:
            agent._raise_native()
        return agent

    def messages(self):
        """Copy the idle transcript as dictionaries, including calls/reasoning."""
        with self._lock:
            self._idle()
            pointer = self._lib.libai_agent_messages(self._handle)
            if not pointer:
                self._raise_native()
            try:
                return json.loads(c.string_at(pointer).decode("utf-8"))
            finally:
                self._lib.libai_string_free(pointer)

    def _raise_native(self):
        raise RuntimeError(self._lib.libai_last_error().decode("utf-8"))

    def _idle(self):
        if not self._handle:
            raise RuntimeError("agent is closed")
        if self._busy:
            raise RuntimeError("agent is running")

    def _complete(self, model, request, output, pointer):
        invocation = Invocation(self._lib, pointer)
        try:
            response = model(json.loads(request.decode("utf-8")), invocation)
            if not isinstance(response, ModelResponse):
                raise TypeError("model must return ModelResponse")
            self._lib.libai_response_text(output, n.utf8(response.text), n.utf8(response.reasoning))
            for call in response.calls:
                self._lib.libai_response_call(output, n.utf8(call.id), n.utf8(call.name), n.utf8(_json(call.arguments)))
            for usage in (response.input_tokens, response.output_tokens):
                if not isinstance(usage, int) or usage < 0 or usage > c.c_size_t(-1).value:
                    raise ValueError("token usage must fit size_t")
            self._lib.libai_response_usage(output, response.input_tokens, response.output_tokens)
            self._lib.libai_response_error(output, n.utf8(response.error), response.retryable)
        except BaseException as error:
            self._lib.libai_response_error(output, (type(error).__name__ + ": " + str(error)).replace("\0", "").encode("utf-8", "replace"), 0)
        finally:
            invocation._pointer = None

    def set_execution_store(self, scope, store):
        """Configure an idle agent's durable task; None disables persistence."""
        with self._lock:
            self._idle()
            callback=_execution_callback(self._lib,store) if store is not None else n.EXECUTION()
            if self._lib.libai_agent_set_execution_store(self._handle,n.utf8(scope),callback,None):
                self._raise_native()
            self._execution_callback=callback
        return self

    def configure_output(self, schema, repairs=1):
        """Host-validate final JSON; None/empty string disables validation."""
        if type(repairs) is not int or not 0 <= repairs <= 8:
            raise ValueError("repairs must be an integer from 0 to 8")
        with self._lock:
            self._idle()
            fn = self._lib.libai_agent_configure_output
            fn.argtypes = [c.c_void_p, c.c_char_p, c.c_size_t]; fn.restype = c.c_int
            wire = "" if schema is None else _json(schema)
            if fn(self._handle, n.utf8(wire), repairs):
                self._raise_native()
        return self

    def configure_model(self, profile):
        """Replace idle-agent capabilities with a versioned JSON profile."""
        with self._lock:
            self._idle()
            fn = self._lib.libai_agent_configure_model
            fn.argtypes = [c.c_void_p, c.c_char_p]; fn.restype = c.c_int
            if fn(self._handle, n.utf8(_json(profile))):
                self._raise_native()
        return self

    def add_tool(self, name, handler, description="", parameters=None, requires_approval=False, execution_revision="1", side_effect_free=False):
        """Handlers receive a ToolCall and a callback-local Invocation."""
        if not isinstance(side_effect_free, bool):
            raise TypeError("side_effect_free must be a host boolean")
        def invoke(user, call_id, tool_name, arguments, output, pointer):
            invocation = Invocation(self._lib, pointer)
            try:
                call = ToolCall(call_id.decode(), tool_name.decode(), json.loads(arguments.decode()))
                result = handler(call, invocation)
                if isinstance(result, str):
                    result = ToolResult(result)
                self._lib.libai_tool_result_set(output, n.utf8(result.text), result.error)
            except BaseException as error:
                self._lib.libai_tool_result_set(output, (type(error).__name__ + ": " + str(error)).replace("\0", "").encode("utf-8", "replace"), 1)
            finally:
                invocation._pointer = None
        callback = n.TOOL(invoke)
        schema = {"type": "object", "properties": {}} if parameters is None else parameters
        with self._lock:
            self._idle()
            code = self._lib.libai_agent_add_tool_governed(self._handle, n.utf8(name), n.utf8(description),
                                                n.utf8(_json(schema)), requires_approval, n.utf8(execution_revision), bool(side_effect_free), callback, None)
            if code:
                self._raise_native()
            self._tools.append(callback)
        return self

    def add_mcp_tool(self, client, remote_name, local_name, parameters, execution_revision,
                     output_schema=None, requires_approval=True, side_effect_free=False, timeout_ms=30000):
        """Bind discovered tools using host-reviewed rules; approval defaults to required."""
        from .mcp import McpClient
        if not isinstance(client, McpClient):
            raise TypeError("client must be McpClient")
        if self._lib._name != client._lib._name:
            raise ValueError("agent and MCP client must use the same native library")
        if not isinstance(requires_approval, bool) or not isinstance(side_effect_free, bool):
            raise TypeError("MCP policies must be host booleans")
        policy = {"parameters_schema": _json(parameters), "execution_revision": execution_revision,
                  "requires_approval": requires_approval, "side_effect_free": side_effect_free,
                  "timeout_ms": timeout_ms}
        if output_schema is not None:
            policy["output_schema"] = _json(output_schema)
        with self._lock:
            self._idle()
            with client._access():
                if self._lib.libai_agent_add_mcp_tool(self._handle, client._handle,
                        n.utf8(remote_name), n.utf8(local_name), n.utf8(_json(policy))):
                    self._raise_native()
        return self

    def add_retrieval_tool(self, retriever, name, binding):
        """Host-owned scope/filter/revision; default approval and replay rules apply."""
        from .rag import Rag, RagBinding, _encode
        if not isinstance(retriever, Rag) or not isinstance(binding, RagBinding):
            raise TypeError("retriever/binding must be Rag and RagBinding")
        retriever._require("retriever")
        retriever._same_library(self)
        policy = _encode(binding._wire())
        with self._lock:
            self._idle()
            with retriever._access():
                if self._lib.libai_agent_add_retrieval_tool(self._handle, retriever._handle, n.utf8(name), policy):
                    self._raise_native()
                self._tools.append(retriever)
        return self

    def run(self, text, on_text=None, authorize=None, on_event=None, cancellation=None, **limits):
        """Return a result dictionary; failures have an explicit status/error."""
        options = n.Options()
        self._lib.libai_options_default(c.byref(options))
        names = dict(n.Options._fields_)
        for name, value in limits.items():
            if name not in names:
                raise TypeError("unknown run limit: " + name)
            maximum = (1 << 63) - 1 if name == "timeout_ms" else c.c_size_t(-1).value
            if not isinstance(value, int) or not 0 <= value <= maximum:
                raise ValueError("invalid run limit: " + name)
            setattr(options, name, value)
        errors = []
        def observe(callback, *args):
            try:
                return callback(*args)
            except BaseException as error:
                errors.append(error)
                self.cancel()
                return False
        text_cb = n.TEXT(lambda user, delta: bool(observe(on_text, delta.decode()))) if on_text else n.TEXT()
        auth_cb = n.AUTHORIZE(lambda user, name, args, required:
            bool(observe(lambda: authorize(name.decode(), json.loads(args.decode()), bool(required))))) if authorize else n.AUTHORIZE()
        event_cb = n.EVENT(lambda user, event: observe(lambda: on_event(json.loads(event.decode())))) if on_event else n.EVENT()
        hooks = n.Hooks(None, text_cb, auth_cb, event_cb)
        with self._lock:
            self._idle()
            self._busy = True
        lease = None
        try:
            if cancellation is not None:
                if not isinstance(cancellation, Cancellation):
                    raise TypeError("cancellation must be Cancellation")
                if self._lib._name != cancellation._lib._name:
                    raise ValueError("agent and cancellation must use the same native library")
                lease = cancellation._retain()
            pointer = self._lib.libai_agent_run_with_cancellation(
                self._handle, n.utf8(text), c.byref(options), c.byref(hooks), lease)
            if not pointer:
                self._raise_native()
            try:
                result = json.loads(c.string_at(pointer).decode("utf-8"))
            finally:
                self._lib.libai_string_free(pointer)
            if errors:
                raise RuntimeError("observer callback failed") from errors[0]
            return result
        finally:
            if lease:
                cancellation._lib.libai_cancellation_destroy(lease)
            with self._lock:
                self._busy = False

    def cancel(self):
        with self._lock:
            if self._handle:
                self._lib.libai_agent_cancel(self._handle)

    def reset(self):
        with self._lock:
            self._idle()
            if self._lib.libai_agent_reset(self._handle):
                self._raise_native()

    def close(self):
        with self._lock:
            if not self._handle:
                return
            self._idle()
            handle = self._handle
            # Publish closed before native release, so final host destructors may
            # reenter this facade without a lock deadlock or cancellation/free race.
            self._handle, self._busy = None, True
        try:
            code = self._lib.libai_agent_destroy(handle)
        except BaseException:
            with self._lock:
                self._handle, self._busy = handle, False
            raise
        if code:
            with self._lock:
                self._handle, self._busy = handle, False
            self._raise_native()
        with self._lock:
            self._busy = False
            retired_tools, retired_store = self._tools, getattr(self, "_execution_callback", None)
            self._tools, self._execution_callback = [], None
        # Release captured user objects outside the facade lock too.
        del retired_tools, retired_store

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


class CodingService:
    """Root-scoped desktop coding facilities; same operation protocol as C++/Java/Rust.

    Host code must authorize mutating operations before invoking call().
    Native operation failures return {"ok": False, "error": ...}.
    ABI/availability failures raise RuntimeError.
    """
    def __init__(self, root, library=None):
        self._lib = n.load(library)
        self._root = n.utf8(str(root))

    @property
    def available(self):
        return bool(self._lib.libai_coding_available())

    def call(self, operation, arguments=None):
        encoded = json.dumps({} if arguments is None else arguments, ensure_ascii=False)
        pointer = self._lib.libai_coding_call(self._root, n.utf8(operation), n.utf8(encoded))
        if not pointer:
            raise RuntimeError(self._lib.libai_last_error().decode("utf-8"))
        try:
            return json.loads(c.string_at(pointer).decode("utf-8"))
        finally:
            self._lib.libai_string_free(pointer)

def governance_call(operation, arguments=None, library=None):
    """Portable governance, independent of desktop CodingService availability."""
    native=n.load(library)
    native.libai_governance_call.argtypes=[c.c_char_p,c.c_char_p]
    native.libai_governance_call.restype=c.c_void_p
    payload=json.dumps(arguments if arguments is not None else {},ensure_ascii=False,allow_nan=False)
    pointer=native.libai_governance_call(n.utf8(operation),n.utf8(payload))
    if not pointer:
        raise RuntimeError(native.libai_last_error().decode("utf-8"))
    try:
        return json.loads(c.string_at(pointer).decode("utf-8"))
    finally:
        native.libai_string_free(pointer)

# Imported after Invocation/Cancellation/Agent definitions to keep adapters private.
from .mcp import McpClient, McpTransport
from .workflow import Workflow, WorkflowContext, WorkflowNodeResult

from .rag import (Rag, RagLimits, RagOptions, RagBinding, RetrievalContext,
                  Document, DocumentChunk, RetrievalRequest, RetrievalHit, RetrievalResult,
                  PreparedIndex, VectorRecord, VectorQuery, DocumentLoader, TextSplitter,
                  EmbeddingModel, VectorStore, Retriever)

from .task_contract import (TaskContract, TaskEvidence, TaskRequirements,
                            TaskChecks, TaskEvaluation)
