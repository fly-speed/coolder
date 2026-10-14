package io.coolder.app;

import java.util.*;

/** Trusted frozen runtime inputs. Neither caller fields nor credentials define recovery. */
final class ExecutionContext {
  static final List<String> INPUT_KEYS = Arrays.asList(
      "agent_id", "provider_id", "project_id", "path", "prompt", "execution_mode",
      "remember_session", "resume_after_restart", "session_id", "_images", "_history",
      "_model", "_model_profile", "_policy");
  static final List<String> CHECKPOINT_KEYS = Arrays.asList(
      "agent_id", "provider_id", "project_id", "path", "prompt", "execution_mode",
      "remember_session", "resume_after_restart", "session_id");

  static Map<String,Object> copy(Map<String,Object> value) {
    return Json.map(Json.parse(Json.encode(value)));
  }

  static Map<String,Object> select(Map<String,Object> value, List<String> keys) {
    Map<String,Object> selected = new LinkedHashMap<>();
    for (String key : keys) if (value.containsKey(key)) selected.put(key, value.get(key));
    return copy(selected);
  }

  static Map<String,Object> providerIdentity(Map<String,Object> provider) {
    Map<String,Object> identity = new LinkedHashMap<>();
    for (String key : Arrays.asList("id", "protocol", "base_url", "model", "allow_file_content",
        "model_profile_json", "response_state_mode", "responses_store", "responses_background",
        "responses_compact", "responses_strict_tools", "responses_min_reasoning_effort",
        "responses_reasoning_summary", "responses_text_verbosity", "responses_service_tier",
        "responses_cache_ttl", "qwen_session_cache", "openai_organization", "openai_project"))
      identity.put(key, provider.get(key));
    return copy(identity);
  }

  static Map<String,Object> policy(Map<String,Object> frozen, Map<String,Object> current) {
    Map<String,Object> limits = new LinkedHashMap<>();
    for (String key : Arrays.asList("max_output_tokens", "standard_mode_tool_calls",
        "large_mode_tool_calls", "quick_mode_tool_calls", "provider_request_timeout_seconds")) {
      long fallback = key.equals("max_output_tokens") ? 32768L
          : key.equals("provider_request_timeout_seconds") ? 900L : 64L;
      limits.put(key, Math.min(Json.number(frozen,key,fallback), Json.number(current,key,fallback)));
    }
    long output = Json.number(limits,"max_output_tokens",32768L);
    limits.put("quick_mode_output_tokens", Math.min(output, Math.min(
        Json.number(frozen,"quick_mode_output_tokens",output),
        Json.number(current,"quick_mode_output_tokens",output))));
    return limits;
  }

  static void validate(Map<String,Object> context, Map<String,Object> request,
      Map<String,Object> identity, String instructions) {
    boolean valid = Json.number(context,"version",0)==1
        && Json.text(context,"instructions").equals(instructions)
        && Objects.equals(context.get("provider_identity"),identity)
        && Objects.equals(context.get("path"),request.get("path"))
        && Json.text(context,"scope").matches("[A-Za-z0-9_-]{1,128}")
        && context.get("input") instanceof String && context.get("args") instanceof Map;
    if (valid) {
      Map<String,Object> args = Json.map(context.get("args"));
      valid = args.get("_images") instanceof List && args.get("_history") instanceof List
          && args.get("_policy") instanceof Map
          && Json.text(args,"_model").equals(Json.text(identity,"model"))
          && Json.text(args,"_model_profile").equals(Json.text(identity,"model_profile_json"));
      for (String key : Arrays.asList("provider_id","project_id","path","prompt","execution_mode"))
        valid &= Objects.equals(args.get(key),request.get(key));
    }
    if (!valid) throw new App.Error(409,"frozen execution context changed or invalid");
  }
}
