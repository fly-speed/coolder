"""Stable ctypes declarations; no Python extension or third-party runtime is needed."""
import ctypes as c
import ctypes.util
import os

P = c.c_void_p
MODEL = c.CFUNCTYPE(None, P, c.c_char_p, P, P)
TOOL = c.CFUNCTYPE(None, P, c.c_char_p, c.c_char_p, c.c_char_p, P, P)
TEXT = c.CFUNCTYPE(c.c_int, P, c.c_char_p)
AUTHORIZE = c.CFUNCTYPE(c.c_int, P, c.c_char_p, c.c_char_p, c.c_int)
EVENT = c.CFUNCTYPE(None, P, c.c_char_p)


class Options(c.Structure):
    """Native size_t budgets followed by a signed millisecond deadline."""
    _fields_ = [(name, c.c_size_t) for name in (
        "max_model_calls", "max_tool_calls", "max_output_tokens", "max_tokens_per_call",
        "max_context_bytes", "max_tool_result_bytes", "max_model_retries")]
    _fields_.append(("timeout_ms", c.c_int64))


class Hooks(c.Structure):
    """Keep callback objects alive until the synchronous native run returns."""
    _fields_ = [("user", P), ("on_text", TEXT), ("authorize", AUTHORIZE), ("on_event", EVENT)]


def utf8(text):
    """C ABI strings are UTF-8 text, not binary buffers."""
    if not isinstance(text, str) or "\0" in text:
        raise ValueError("expected text without embedded NUL")
    return text.encode("utf-8")


def load(path=None):
    path = path or os.environ.get("LIBAI_LIBRARY") or ctypes.util.find_library("ai_c")
    if not path:
        raise RuntimeError("Build libai-c and set LIBAI_LIBRARY to its shared library")
    lib = c.CDLL(path)
    signatures = {
        "libai_abi_version": (c.c_uint, []),
        "libai_last_error": (c.c_char_p, []),
        "libai_options_default": (None, [c.POINTER(Options)]),
        "libai_agent_create": (P, [MODEL, P, c.c_char_p]),
        "libai_agent_destroy": (c.c_int, [P]),
        "libai_agent_add_tool": (c.c_int, [P, c.c_char_p, c.c_char_p, c.c_char_p, c.c_int, TOOL, P]),
        "libai_agent_run": (P, [P, c.c_char_p, c.POINTER(Options), c.POINTER(Hooks)]),
        "libai_agent_cancel": (None, [P]), "libai_agent_reset": (c.c_int, [P]),
        "libai_string_free": (None, [P]),
        "libai_response_text": (None, [P, c.c_char_p, c.c_char_p]),
        "libai_response_call": (None, [P, c.c_char_p, c.c_char_p, c.c_char_p]),
        "libai_response_usage": (None, [P, c.c_size_t, c.c_size_t]),
        "libai_response_error": (None, [P, c.c_char_p, c.c_int]),
        "libai_tool_result_set": (None, [P, c.c_char_p, c.c_int]),
        "libai_invocation_stopped": (c.c_int, [P]),
        "libai_invocation_emit": (c.c_int, [P, c.c_char_p]),
    }
    for name, (result, args) in signatures.items():
        fn = getattr(lib, name)
        fn.restype, fn.argtypes = result, args
    if lib.libai_abi_version() != 1:
        raise RuntimeError("unsupported libai native ABI version")
    return lib
