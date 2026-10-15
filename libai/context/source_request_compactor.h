#pragma once
#include "../provider/ai_provider_client.h"
#include "../workspace/agent_workspace.h"
#include "../common/json_value.h"
#include <map>
#include <vector>

namespace libai { namespace ai {
struct source_compaction_stats_t {
    size_t bytes_before = 0, bytes_after = 0, source_bytes_elided = 0, records_reused = 0;
};

// Operates ONLY on the outgoing request copy. Historical call arguments and
// canonical checkpoints stay exact. A reference always points to bytes still
// present in this same outgoing request, never to an evicted previous turn.
class source_request_compactor_t {
    struct page_t { std::string version, content, location; size_t offset; };
    std::map<std::string, std::vector<page_t>> pages_;
    std::string project_;
    size_t retained_ = 0;
    source_compaction_stats_t stats_;
    std::string path(std::string value) const {
        if (!project_.empty() && value.compare(0,project_.size()+1,project_+"/")==0)
            value.erase(0,project_.size()+1);
        std::string normalized,error;
        return agent_workspace_t::normalize_path(value,normalized,false,error) ? normalized : "";
    }
    std::string find(const std::string &name,const std::string &version,size_t offset,const std::string &content) const {
        const auto found=pages_.find(name); if(found==pages_.end())return "";
        for(const auto &page:found->second)
            if(page.version==version && offset>=page.offset && offset-page.offset<=page.content.size() &&
                content.size()<=page.content.size()-(offset-page.offset) &&
                page.content.compare(offset-page.offset,content.size(),content)==0)return page.location;
        return "";
    }
    void retain(const std::string &name,const std::string &version,size_t offset,
        const std::string &content,const std::string &location) {
        if(retained_+content.size()>2*1024*1024)return;
        retained_+=content.size(); pages_[name].push_back({version,content,location,offset});
    }
    void visit(acl::json_node &node,const std::string &location,bool seed,unsigned depth=0) {
        using namespace json_value;
        if(depth>64)return;
        auto *object=node.is_object()?&node:node.get_obj();
        if(object) {
            const auto name=path(nullable_text((*object)["path"]));
            auto *source=(*object)["content"];
            const auto content=string_text(source);
            const auto offset=number((*object)["offset"],0);
            if(!name.empty() && !content.empty() && offset>=0) {
                auto version=nullable_text((*object)["file_sha256"]);
                if(version.empty() && offset==0)version=agent_workspace_t::content_sha256(content);
                if(!version.empty()) {
                    const auto anchor=find(name,version,static_cast<size_t>(offset),content);
                    if(!seed && !anchor.empty() && content.size() >= 256) {
                        // ACL free_child removes the CURRENT iterator, not the
                        // pointer argument. Reposition after the metadata reads.
                        for(auto *field=object->first_child();field;field=object->next_child()) {
                            if(field->tag_name() && std::string(field->tag_name())=="content") {
                                object->free_child(field);break;
                            }
                        }
                        object->add_bool("content_omitted",true);
                        object->add_text("source_ref",anchor.c_str());
                        stats_.source_bytes_elided+=content.size(); ++stats_.records_reused;
                    } else if(anchor.empty())retain(name,version,static_cast<size_t>(offset),content,location);
                }
            }
        }
        if(node.is_string() && node.tag_name() && std::string(node.tag_name())=="result") {
            auto nested=json_value::string_text(&node);
            acl::json parsed(nested.c_str());
            if(parsed.finish()) {
                const auto before=stats_.records_reused;visit(parsed.get_root(),location+".result",seed,depth+1);
                if(before!=stats_.records_reused)node.set_text(parsed.to_string().c_str());
            }
        }
        for(auto *child=node.first_child();child;child=node.next_child())visit(*child,location,seed,depth+1);
    }
    void json_text(std::string &text,const std::string &location,bool seed=false) {
        acl::json json(text.c_str());if(!json.finish())return;
        const auto before=stats_.records_reused; visit(json.get_root(),location,seed);
        if(!seed && before!=stats_.records_reused)text=json.to_string().c_str();
    }
    void text(std::string &value,const std::string &location) {
        for(const char *tag:{"bootstrap_source","staged_working_set","read_working_set","repair_review_sources","repair_source_context","tool_result"}) {
            const std::string open="<"+std::string(tag)+">",close="</"+std::string(tag)+">";
            size_t pos=0;
            while((pos=value.find(open,pos))!=std::string::npos) {
		if(pos>0 && value[pos-1]!='\n') {pos+=open.size();continue;}
                const auto begin=pos+open.size(),end=value.find(close,begin);if(end==std::string::npos)break;
                auto body=value.substr(begin,end-begin);const auto old=body.size();json_text(body,location+":"+tag);
                if(body.size()!=old || value.compare(begin,old,body)!=0)value.replace(begin,old,body);
                pos=begin+body.size()+close.size();
            }
        }
    }
    static size_t bytes(const completion_request_t &r) {
        size_t size=r.system_prompt.size()+r.user_prompt.size()+r.turn_instructions.size();
        for(const auto &e:r.tool_history) {
            size+=e.preceding_instructions.size();
            for(const auto &c:e.calls)size+=c.content.size()+c.old_text.size()+c.path.size()+c.query.size();
            for(const auto &o:e.outputs)size+=o.output.size();
        }
        for(const auto &o:r.tool_outputs)size+=o.output.size();
        return size;
    }
public:
    explicit source_request_compactor_t(const std::string &project):project_(project){}
    source_compaction_stats_t compact(completion_request_t &r) {
        // A compactor may be reused, but anchors are valid for one request only.
        pages_.clear(); retained_=0; stats_=source_compaction_stats_t{};
        stats_.bytes_before=bytes(r);
        // Stateful continuations send tool_outputs, not the local prompt/history.
        // Never point at local context which the provider will not receive.
        if(r.previous_response_id.empty()) {
            for(size_t i=0;i<r.tool_history.size();++i)for(size_t j=0;j<r.tool_history[i].calls.size();++j) {
                const auto &c=r.tool_history[i].calls[j];
                if(c.name!="workspace.propose" && c.name!="workspace.replace" && c.name!="workspace.propose_batch")continue;
                const auto location="tool_history["+std::to_string(i)+"].calls["+std::to_string(j)+"]";
                if(c.name=="workspace.propose_batch") {auto content=c.content;json_text(content,location,true);}
                else if(!c.path.empty() && !c.content.empty()) {
                    const auto name=path(c.path);if(!name.empty())retain(name,agent_workspace_t::content_sha256(c.content),0,c.content,location);
                }
            }
            text(r.user_prompt,"user_prompt");
            for(size_t i=0;i<r.tool_history.size();++i) {
                auto &e=r.tool_history[i];const auto location="tool_history["+std::to_string(i)+"]";
                text(e.preceding_instructions,location+".instructions");
                for(size_t j=0;j<e.outputs.size();++j) {
                    auto &out=e.outputs[j].output;const auto label=location+".outputs["+std::to_string(j)+"]";
                    json_text(out,label);text(out,label);
                }
            }
            text(r.system_prompt,"system_prompt");text(r.turn_instructions,"turn_instructions");
        }
        for(size_t i=0;i<r.tool_outputs.size();++i)json_text(r.tool_outputs[i].output,"tool_outputs["+std::to_string(i)+"]");
        stats_.bytes_after=bytes(r);return stats_;
    }
};
} }
