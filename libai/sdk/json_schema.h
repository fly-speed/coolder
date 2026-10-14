#pragma once
#include "json_guard.h"
#include "schema_meta.h"
#include "vendor/rapidjson/schema.h"
#include "vendor/rapidjson/stringbuffer.h"
#include <memory>
namespace libai {
struct schema_validation_t {
    bool valid=false;
    std::string code,keyword,instance_pointer,schema_pointer;
};
namespace governance_detail {
inline std::string pointer_text(const json::Pointer &pointer) {
    json::StringBuffer buffer;pointer.StringifyUriFragment(buffer);
    std::string value(buffer.GetString(),buffer.GetSize());
    if (value.size()>1024) {
        size_t n=1024;
        while (n && (static_cast<unsigned char>(value[n])&0xc0)==0x80) --n;
        value.resize(n);
    }
    return value;
}
inline const json::SchemaDocument &meta_schema() {
    static const std::shared_ptr<json::SchemaDocument> schema=[] {
        json::Document meta;parse_json(draft4_meta,meta);
        return std::make_shared<json::SchemaDocument>(meta);
    }();
    return *schema;
}
// Reject reference/combinator cycles that revisit the same instance indefinitely.
inline void no_progress_cycle(const json::Value &s,const json::Value &root,
    std::set<const json::Value *> &active) {
    if (!s.IsObject()) return;
    if (active.size()>=64 || !active.insert(&s).second)
        throw std::invalid_argument("schema reference cycle or depth limit");
    if (s.HasMember("$ref")) {
        const auto &ref=s["$ref"];
        if (!ref.IsString() || !ref.GetStringLength() || ref.GetString()[0]!='#')
            throw std::invalid_argument("remote schema references are disabled");
        json::Pointer pointer(ref.GetString(),ref.GetStringLength());
        if (!pointer.IsValid()) throw std::invalid_argument("invalid schema reference");
        const auto *target=pointer.Get(root);
        if (!target || !target->IsObject()) throw std::invalid_argument("unresolved schema reference");
        no_progress_cycle(*target,root,active);
    }
    for (const char *key:{"allOf","anyOf","oneOf"}) {
        if (s.HasMember(key)) for(auto i=s[key].Begin();i!=s[key].End();++i)
            no_progress_cycle(*i,root,active);
    }
    if (s.HasMember("not")) no_progress_cycle(s["not"],root,active);
    if (s.HasMember("dependencies")) {
        for (auto i=s["dependencies"].MemberBegin();i!=s["dependencies"].MemberEnd();++i)
            if(i->value.IsObject()) no_progress_cycle(i->value,root,active);
    }
    active.erase(&s);
}
// Check only schema locations: enum/default payloads are data, not schemas.
inline void schema_profile(const json::Value &s,const json::Value &root) {
    if (!s.IsObject()) throw std::invalid_argument("schema must be a Draft-04 object");
    std::set<const json::Value *> active;no_progress_cycle(s,root,active);
    static const std::set<std::string> vocabulary={"$schema","id","$ref","definitions",
        "title","description","default","type","enum","allOf","anyOf","oneOf","not",
        "multipleOf","maximum","exclusiveMaximum","minimum","exclusiveMinimum","maxLength",
        "minLength","pattern","additionalItems","items","maxItems","minItems","uniqueItems",
        "maxProperties","minProperties","required","additionalProperties","properties",
        "patternProperties","dependencies","format",
        "examples","example","readOnly","writeOnly","deprecated","$comment"};
    for (auto i=s.MemberBegin();i!=s.MemberEnd();++i) {
        const std::string key(i->name.GetString(),i->name.GetStringLength());
        if (!vocabulary.count(key) && key.compare(0,2,"x-")!=0)
            throw std::invalid_argument("schema uses an unsupported keyword");
    }
    if (s.HasMember("$schema")) {
        const std::string dialect=s["$schema"].GetString();
        if (dialect!="http://json-schema.org/draft-04/schema#" &&
            dialect!="http://json-schema.org/draft-04/schema" &&
            dialect!="https://json-schema.org/draft-04/schema#" &&
            dialect!="https://json-schema.org/draft-04/schema")
            throw std::invalid_argument("unsupported schema dialect");
    }
    if (s.HasMember("$ref")) {
        const auto &ref=s["$ref"];
        if (!ref.IsString() || !ref.GetStringLength() || ref.GetString()[0]!='#')
            throw std::invalid_argument("remote schema references are disabled");
        json::Pointer pointer(ref.GetString(),ref.GetStringLength());
        if (!pointer.IsValid()) throw std::invalid_argument("invalid local schema reference");
        const auto *target=pointer.Get(root);
        if (!target || !target->IsObject())
            throw std::invalid_argument("unresolved local schema reference");
    }
    for (const char *map:{"properties","patternProperties","definitions","dependencies"}) {
        if (!s.HasMember(map)) continue;
        const auto &entries=s[map];
        for (auto i=entries.MemberBegin();i!=entries.MemberEnd();++i)
            if (i->value.IsObject()) schema_profile(i->value,root);
    }
    for (const char *key:{"additionalProperties","additionalItems","not"}) {
        if (s.HasMember(key) && s[key].IsObject()) schema_profile(s[key],root);
    }
    if (s.HasMember("items")) {
        const auto &items=s["items"];
        if (items.IsObject()) schema_profile(items,root);
        else if (items.IsArray()) for (auto i=items.Begin();i!=items.End();++i) schema_profile(*i,root);
    }
    for (const char *key:{"allOf","anyOf","oneOf"}) {
        if (s.HasMember(key)) for (auto i=s[key].Begin();i!=s[key].End();++i) schema_profile(*i,root);
    }
}
}
class json_schema_t {
    std::unique_ptr<governance_detail::json::SchemaDocument> compiled_;
public:
    explicit json_schema_t(const std::string &schema_json) {
        namespace json=governance_detail::json;
        json::Document schema;governance_detail::parse_json(schema_json,schema);
        json::SchemaValidator meta(governance_detail::meta_schema());
        if (!schema.Accept(meta)) throw std::invalid_argument("invalid Draft-04 schema");
        governance_detail::schema_profile(schema,schema);
        compiled_.reset(new json::SchemaDocument(schema));
        const auto &errors=compiled_->GetError();
        if (errors.IsObject() && !errors.ObjectEmpty())
            throw std::invalid_argument("schema compilation failed");
    }
    schema_validation_t validate(const std::string &instance_json) const {
        namespace json=governance_detail::json;
        schema_validation_t result;
        try {
            json::Document instance;governance_detail::parse_json(instance_json,instance);
            json::SchemaValidator validator(*compiled_);
            result.valid=instance.Accept(validator);
            result.code=result.valid?"valid":"schema_violation";
            if (!result.valid) {
                const char *keyword=validator.GetInvalidSchemaKeyword();
                result.keyword=keyword?keyword:"";
                result.instance_pointer=governance_detail::pointer_text(validator.GetInvalidDocumentPointer());
                result.schema_pointer=governance_detail::pointer_text(validator.GetInvalidSchemaPointer());
            }
        } catch (const std::exception &) {result.code="invalid_json_or_resource_limit";}
        return result;
    }
};
inline schema_validation_t validate_json_schema(const std::string &schema,const std::string &instance) {
    try {return json_schema_t(schema).validate(instance);}
    catch (const std::exception &) {schema_validation_t r;r.code="invalid_or_unsupported_schema";return r;}
}
}
