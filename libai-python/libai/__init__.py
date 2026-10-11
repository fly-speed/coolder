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


class Invocation:
    """Borrowed stream/stop boundary, automatically expired after a callback."""
    def __init__(self, native, pointer):
        self._native, self._pointer = native, pointer
        self._thread = threading.get_ident()

    def _check(self):
        if not self._pointer or threading.get_ident() != self._thread:
            raise RuntimeError("invocation has expired")

    def stopped(self):
        self._check()
        return bool(self._native.libai_invocation_stopped(self._pointer))

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

    def add_tool(self, name, handler, description="", parameters=None, requires_approval=False):
        """Handlers receive a ToolCall and a callback-local Invocation."""
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
            code = self._lib.libai_agent_add_tool(self._handle, n.utf8(name), n.utf8(description),
                                                n.utf8(_json(schema)), requires_approval, callback, None)
            if code:
                self._raise_native()
            self._tools.append(callback)
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
            if self._lib.libai_agent_destroy(self._handle):
                self._raise_native()
            self._handle = None
            self._tools.clear()

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
