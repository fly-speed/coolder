#pragma once
#include "json_schema.h"
#include "recovery_inspection.h"
#include "task_contract_json.h"
#include "vendor/rapidjson/writer.h"
namespace libai {
inline std::string governance_call(const std::string &operation,const std::string &arguments="{}") {
    if(operation=="executions.inspect")return inspect_execution_json(arguments);
    if(operation=="tasks.evaluate")return evaluate_task_contract_json(arguments);
    namespace json=governance_detail::json;
    json::StringBuffer buffer;json::Writer<json::StringBuffer> out(buffer);
    out.StartObject();
    auto text=[&](const char *key,const std::string &value) {
        out.Key(key);out.String(value.data(),static_cast<json::SizeType>(value.size()));
    };
    try {
        json::Document args;governance_detail::parse_json(arguments,args);
        if (!args.IsObject()) throw std::invalid_argument("governance arguments must be an object");
        if (operation=="capabilities") {
            out.Key("ok");out.Bool(true);
            out.Key("model_profile_configuration");out.Bool(true);
            text("schema_dialect","draft4");
            out.Key("schema_validation");out.Bool(true);
            out.Key("task_contract_evaluation");out.Bool(true);
            out.Key("execution_inspection");out.Bool(true);
            out.Key("batch_schema_preflight");out.Bool(true);
            out.Key("remote_schema_refs");out.Bool(false);
            out.Key("format_assertion");out.Bool(false);
            out.Key("strict_schema_keywords");out.Bool(true);
            out.Key("max_json_bytes");out.Uint(2*1024*1024);
            out.Key("max_json_depth");out.Uint(64);
            out.Key("max_json_nodes");out.Uint(65536);
            out.Key("cooperative_deadlines");out.Bool(true);
            out.Key("streamed_text_byte_limit");out.Bool(true);
            out.Key("max_schema_reference_depth");out.Uint(64);
            text("model_retries","explicit_retryable_errors_only");
            out.Key("tool_retries");out.Bool(false);
            text("authorization","after_schema_before_execution");
            text("tool_output_limit","run_options.max_tool_result_bytes");
            out.Key("events");out.StartArray();
            for (const char *e:{"model_start","model_end","tool_start","tool_end","finished","tool_intent_committed","tool_result_committed","tool_result_reused"}) out.String(e);
            out.EndArray();
            out.Key("operations");out.StartArray();out.String("capabilities");out.String("schema.validate");out.String("tasks.evaluate");out.String("executions.inspect");out.EndArray();
        } else if (operation=="schema.validate") {
            if (!args.HasMember("schema_json") || !args["schema_json"].IsString() ||
                !args.HasMember("instance_json") || !args["instance_json"].IsString())
                throw std::invalid_argument("schema_json and instance_json must be strings");
            const auto &s=args["schema_json"],&i=args["instance_json"];
            auto r=validate_json_schema(std::string(s.GetString(),s.GetStringLength()),
                std::string(i.GetString(),i.GetStringLength()));
            out.Key("ok");out.Bool(true);out.Key("valid");out.Bool(r.valid);
            text("code",r.code);text("keyword",r.keyword);text("instance_pointer",r.instance_pointer);
            text("schema_pointer",r.schema_pointer);text("schema_dialect","draft4");
        } else throw std::invalid_argument("unknown governance operation");
    } catch (const std::exception &e) {
        out.Key("ok");out.Bool(false);text("error",e.what());
    }
    out.EndObject();return std::string(buffer.GetString(),buffer.GetSize());
}
}
