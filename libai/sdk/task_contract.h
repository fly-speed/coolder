#pragma once
#include <string>
#include <cstddef>

namespace libai
{
// Evidence is supplied by a trusted host, never inferred from model prose.
struct task_evidence_t {
    std::string state = "unknown";
    std::string revision, authority, reference;
    task_evidence_t() = default;
    task_evidence_t(std::string state_, std::string revision_, std::string authority_, std::string reference_)
        : state(state_), revision(revision_), authority(authority_), reference(reference_) {}

};
// Versioned acceptance contract; all requirements are conservative by default.
struct task_contract_t {
    bool loop_finished = false;
    std::string loop_outcome, artifact_revision;
    bool require_generation = true, require_build = true,
         require_tests = true, require_review = true;
    task_evidence_t generation, build, tests, review;

    // Typed host values obey the same text/state rules as the JSON interfaces.
    static bool valid_text(const std::string &text)
    {
        for (std::size_t i=0;i<text.size();) {
            const auto first=static_cast<unsigned char>(text[i++]);
            if(first==0)return false;
            if(first<0x80)continue;
            unsigned count=0,point=0,minimum=0;
            if(first>=0xc2&&first<=0xdf){count=1;point=first&0x1f;minimum=0x80;}
            else if(first>=0xe0&&first<=0xef){count=2;point=first&0x0f;minimum=0x800;}
            else if(first>=0xf0&&first<=0xf4){count=3;point=first&0x07;minimum=0x10000;}
            else return false;
            if(text.size()-i<count)return false;
            while(count>0) {
                --count;
                const auto byte=static_cast<unsigned char>(text[i++]);
                if((byte&0xc0)!=0x80)return false;
                point=(point<<6)|(byte&0x3f);
            }
            if(point<minimum||point>0x10ffff||(point>=0xd800&&point<=0xdfff))return false;
        }
        return true;
    }
    static bool valid_evidence(const task_evidence_t &e,const char *positive,bool review=false)
    {
        return valid_text(e.state)&&valid_text(e.revision)&&valid_text(e.authority)&&valid_text(e.reference)&&
            (e.state=="unknown"||e.state==positive||e.state=="failed"||e.state=="not_required"||(review&&e.state=="rejected"))&&
            (e.authority.empty()||e.authority=="host"||e.authority=="human"||e.authority=="model");
    }

    // Evidence must belong to the current artifact revision and trusted producer.
    bool proves(const task_evidence_t &e, const char *state,
        const char *authority) const
    {
        return state&&authority&&valid_text(e.state)&&valid_text(e.authority)&&
            valid_text(artifact_revision)&&valid_text(e.revision)&&valid_text(e.reference)&&
            !artifact_revision.empty() && e.revision == artifact_revision &&
            e.state == state && e.authority == authority && !e.reference.empty();
    }
    // An ended model loop alone is insufficient to accept a task.
    bool accepted() const
    {
        return valid_text(artifact_revision)&&valid_evidence(generation,"completed")&&
            valid_evidence(build,"passed")&&valid_evidence(tests,"passed")&&valid_evidence(review,"accepted",true)&&
            loop_finished && loop_outcome == "completed" &&
            (!require_generation || proves(generation, "completed", "host")) &&
            (!require_build || proves(build, "passed", "host")) &&
            (!require_tests || proves(tests, "passed", "host")) &&
            (!require_review || proves(review, "accepted", "human"));
    }
};
}
