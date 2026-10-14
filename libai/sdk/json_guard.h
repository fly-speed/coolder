#pragma once
#include "vendor/rapidjson/document.h"
#include "vendor/rapidjson/reader.h"
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
namespace libai { namespace governance_detail {
namespace json = ::libai_schema_json;
// Reject ambiguous keys and enforce limits before allocating a DOM or compiling a schema.
class json_guard : public json::BaseReaderHandler<json::UTF8<>, json_guard> {
    struct frame { bool object; std::set<std::string> keys; };
    std::vector<frame> stack_;
    size_t nodes_ = 0;
    size_t maximum_nodes_;
public:
    explicit json_guard(size_t maximum_nodes=65536):maximum_nodes_(maximum_nodes){}
    std::string error;
    bool value() {
        if (++nodes_ > maximum_nodes_) { error="JSON node limit exceeded"; return false; }
        return true;
    }
    bool Null(){return value();}
    bool Bool(bool){return value();}
    bool Int(int){return value();}
    bool Uint(unsigned){return value();}
    bool Int64(int64_t){return value();}
    bool Uint64(uint64_t){return value();}
    bool Double(double){return value();}
    bool RawNumber(const char*,json::SizeType,bool){return value();}
    bool String(const char*,json::SizeType,bool){return value();}
    bool Key(const char *s,json::SizeType n,bool) {
        if (!value()) return false;
        if (stack_.empty() || !stack_.back().object ||
            !stack_.back().keys.insert(std::string(s,n)).second) {
            error="duplicate JSON object key";return false;
        }
        return true;
    }
    bool begin(bool object) {
        if (!value()) return false;
        if (stack_.size()>=64) {error="JSON depth limit exceeded";return false;}
        frame f;f.object=object;stack_.push_back(std::move(f));return true;
    }
    bool StartObject(){return begin(true);}
    bool StartArray(){return begin(false);}
    bool EndObject(json::SizeType){stack_.pop_back();return true;}
    bool EndArray(json::SizeType){stack_.pop_back();return true;}
};
// Trusted storage/RAG bridges may select bounded larger envelopes (vector arrays
// need more nodes). Model/Schema/metadata callers retain the default limits.
inline void parse_json_bounded(const std::string &wire,json::Document &document,size_t max_bytes,size_t max_nodes=65536) {
    if (wire.size()>max_bytes || wire.find('\0')!=std::string::npos)
        throw std::invalid_argument("JSON byte limit exceeded or raw NUL");
    constexpr unsigned flags=json::kParseIterativeFlag|json::kParseValidateEncodingFlag|
        json::kParseFullPrecisionFlag;
    json::Reader reader;json::StringStream stream(wire.c_str());json_guard guard(max_nodes);
    if (!reader.Parse<flags>(stream,guard))
        throw std::invalid_argument(guard.error.empty()?"invalid JSON or UTF-8":guard.error);
    document.Parse<flags>(wire.c_str());
    if (document.HasParseError()) throw std::invalid_argument("invalid JSON");
}
inline void parse_json(const std::string &wire,json::Document &document) {
    parse_json_bounded(wire,document,2*1024*1024);
}
}}
