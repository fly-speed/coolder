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
EXECUTION = c.CFUNCTYPE(None, P, c.c_char_p, c.c_char_p, c.c_char_p, c.c_char_p, P)
MCP_TRANSPORT = c.CFUNCTYPE(None, P, c.c_char_p, c.c_char_p, c.c_char_p, P, P)
MCP_RELEASE = c.CFUNCTYPE(None, P)
RAG_CALLBACK = c.CFUNCTYPE(None, P, c.c_char_p, c.c_char_p, c.c_char_p, P, P)
RAG_RELEASE = c.CFUNCTYPE(None, P)
RAG_SIGNATURES = {
    "libai_rag_create": (P, [c.c_char_p, RAG_CALLBACK, P, RAG_RELEASE, c.c_char_p]),
    "libai_rag_create_splitter": (P, [c.c_char_p]),
    "libai_rag_create_memory_store": (P, [c.c_char_p]),
    "libai_rag_create_vector_retriever": (P, [P, P, c.c_char_p]),
    "libai_rag_result_set": (c.c_int, [P, c.c_char_p, c.c_size_t]),
    "libai_rag_result_error": (None, [P, c.c_char_p]),
    "libai_rag_load": (P, [P, c.c_char_p, c.c_char_p, P]),
    "libai_rag_prepare_index": (P, [P, P, c.c_char_p, c.c_char_p, c.c_char_p, P]),
    "libai_rag_commit_index": (c.c_int, [P, c.c_char_p, c.c_char_p, P]),
    "libai_rag_retrieve": (P, [P, c.c_char_p, c.c_char_p, P]),
    "libai_rag_destroy": (c.c_int, [P]),
    "libai_agent_add_retrieval_tool": (c.c_int, [P, P, c.c_char_p, c.c_char_p]),
}
WORKFLOW_NODE = c.CFUNCTYPE(None, P, c.c_char_p, c.c_char_p, P, P)
WORKFLOW_RELEASE = c.CFUNCTYPE(None, P)
WORKFLOW_SIGNATURES = {
    "libai_workflow_create": (P, []),
    "libai_workflow_add_node": (c.c_int, [P, c.c_char_p, WORKFLOW_NODE, P, WORKFLOW_RELEASE]),
    "libai_workflow_add_edge": (c.c_int, [P, c.c_char_p, c.c_char_p, c.c_char_p]),
    "libai_workflow_result_set": (c.c_int, [P, c.c_char_p, c.c_size_t]),
    "libai_workflow_run": (P, [P, c.c_char_p, c.c_char_p, EXECUTION, P, P]),
    "libai_workflow_confirm_node": (c.c_int, [P, EXECUTION, P, c.c_char_p, c.c_char_p,
                                          c.c_char_p, c.c_char_p, c.c_char_p, c.c_char_p, c.c_char_p]),
    "libai_workflow_destroy": (c.c_int, [P]),
}
INVOCATION_SIGNATURES = {
    "libai_invocation_retain_cancellation": (P, [P]),
    "libai_invocation_remaining_ms": (c.c_int64, [P]),
}
MCP_SIGNATURES = {
    "libai_mcp_create": (P, [MCP_TRANSPORT, P, MCP_RELEASE]),
    "libai_mcp_result_response": (c.c_int, [P, c.c_char_p, c.c_size_t]),
    "libai_mcp_result_error": (None, [P, c.c_char_p]),
    "libai_mcp_stdio_available": (c.c_int, []),
    "libai_mcp_create_stdio": (P, [c.c_char_p]),
    "libai_mcp_call": (P, [P, c.c_char_p, c.c_char_p, P]),
    "libai_mcp_close": (c.c_int, [P]),
    "libai_mcp_destroy": (c.c_int, [P]),
    "libai_agent_add_mcp_tool": (c.c_int, [P, P, c.c_char_p, c.c_char_p, c.c_char_p]),
}


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
        "libai_file_execution_store_available": (c.c_int, []),
        "libai_file_execution_store_create_plaintext": (P, [c.c_char_p]),
        "libai_file_execution_store_open": (P, [P,c.c_char_p]),
        "libai_file_execution_store_commit": (P, [P,c.c_char_p,c.c_char_p,c.c_char_p]),
        "libai_file_execution_store_close": (c.c_int, [P,c.c_char_p]),
        "libai_file_execution_store_destroy": (c.c_int, [P]),
        "libai_execution_result_set": (None, [P,c.c_char_p,c.c_char_p,c.c_char_p]),
        "libai_agent_set_execution_store": (c.c_int, [P,c.c_char_p,EXECUTION,P]),
        "libai_execution_confirm": (c.c_int, [EXECUTION,P,c.c_char_p,c.c_char_p,c.c_char_p,c.c_char_p,c.c_char_p,c.c_char_p,c.c_int,c.c_char_p,c.c_char_p]),
        "libai_invocation_idempotency_key": (c.c_char_p, [P]),
        "libai_coding_available": (c.c_int, []),
        "libai_coding_call": (P, [c.c_char_p, c.c_char_p, c.c_char_p]),
        "libai_abi_version": (c.c_uint, []),
        "libai_last_error": (c.c_char_p, []),
        "libai_options_default": (None, [c.POINTER(Options)]),
        "libai_agent_create": (P, [MODEL, P, c.c_char_p]),
        "libai_agent_create_provider": (P, [c.c_char_p, c.c_char_p, c.c_char_p]),
        "libai_agent_messages": (P, [P]),
        "libai_agent_run_with_cancellation": (P, [P, c.c_char_p, c.POINTER(Options), c.POINTER(Hooks), P]),
        "libai_cancellation_create": (P, []),
        "libai_cancellation_retain": (P, [P]),
        "libai_cancellation_destroy": (None, [P]),
        "libai_cancellation_cancel": (None, [P]),
        "libai_cancellation_cancelled": (c.c_int, [P]),
        "libai_agent_destroy": (c.c_int, [P]),
        "libai_agent_add_tool_governed": (c.c_int, [P,c.c_char_p,c.c_char_p,c.c_char_p,c.c_int,c.c_char_p,c.c_int,TOOL,P]),
        "libai_agent_add_tool_versioned": (c.c_int, [P,c.c_char_p,c.c_char_p,c.c_char_p,c.c_int,c.c_char_p,TOOL,P]),
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
    # Additive ABI: older libraries remain usable for their existing Agent APIs.
    deadline_signatures = {"libai_execution_adopt_deadline": (c.c_int,
        [EXECUTION,P,c.c_char_p,c.c_char_p,c.c_char_p,c.c_char_p,c.c_int64,c.c_char_p])}
    for optional in (MCP_SIGNATURES, WORKFLOW_SIGNATURES, RAG_SIGNATURES, deadline_signatures):
        if all(hasattr(lib, name) for name in optional):
            for name, (result, args) in optional.items():
                fn = getattr(lib, name)
                fn.restype, fn.argtypes = result, args
    for name, (result, args) in INVOCATION_SIGNATURES.items():
        if hasattr(lib, name):
            fn = getattr(lib, name)
            fn.restype, fn.argtypes = result, args
    if lib.libai_abi_version() != 1:
        raise RuntimeError("unsupported libai native ABI version")
    return lib
