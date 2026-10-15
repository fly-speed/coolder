#include "../validation/source_contract_review.h"
#include "../validation/go_build_checkpoint.h"
#include <cassert>
#include <iostream>

int main() {
    using namespace libai::ai;
    const std::vector<std::string> args{"build","-o",".libai-go-app","./src"};
    assert(plain_go_build_script("#!/bin/sh\nset -eu\nmkdir -p build\ngo build -o build/app ./src\n",args));
    for(const auto &script:{"go generate ./...\ngo build -o build/app ./src\n",
            "go build -tags custom -o build/app ./src\n", "go build -o build/app ./cmd/app\n",
            "cd src\ngo build -o build/app ./src\n", "go build -o build/app ./src\ngo test ./...\n",
            "GOPATH=/tmp/go go build -o build/app ./src\n", "go build -o $(pwd)/app ./src\n",
            "go build -o ../app ./src\n", "go build -o build/app ./src; echo done\n"})
        assert(!plain_go_build_script(script,args));
    std::map<std::string,std::string> source{
        {"src/model.go","package app\nimport \"encoding/json\"\ntype Position struct {\nX, Y int\n}\n"},
        {"src/page.go","package app\nconst page=`<script>draw(position.x, position.y)</script>`\n"},
        {"src/ws.go","package app\nconst header=\"Sec-WebSocket-Accept: \"\nfunc parse(){reader.ReadByte()}\n"},
        {"src/speed_test.go","package app\nfunc TestSpeed(){ measure := func(n int) int { return n }; _ = measure(1); _ = measure(2) }\n"}
    };
    auto has=[](const std::vector<source_contract_finding_t> &fs,const char *kind) {
        return std::any_of(fs.begin(),fs.end(),[&](const source_contract_finding_t &f){return f.kind==kind;});
    };
    auto findings=source_contract_review(source);
    assert(findings.size()==4 && has(findings,"json_field_case_boundary") && has(findings,"manual_websocket_transport") && has(findings,"discarded_test_observations"));
    for(const auto &f:findings)if(f.kind=="json_field_case_boundary") {
        assert(f.path=="src/model.go" && f.line==4 && f.related_path=="src/page.go" && f.related_line==2);
    }
    source["src/model.go"]="package app\nimport \"encoding/json\"\ntype Position struct {\nX int `json:\"x\"`\nY int `json:\"y\"`\n}\n";
    assert(!has(source_contract_review(source),"json_field_case_boundary"));
    source["src/speed_test.go"]="package app\nfunc TestSpeed(){measure:=func(n int)int{return n}; slow,fast:=measure(1),measure(2); if slow>=fast {t.Fatal(\"speed\")} }";
    assert(!has(source_contract_review(source),"discarded_test_observations"));
    // Comments and strings describing code cannot become code findings.
    source["src/model.go"]="package app\nimport \"encoding/json\"\n// type Position struct { X int }\nconst note=\"type Position struct { X int }\"\n";
    source["src/ws.go"]="package app\n// Sec-WebSocket-Accept ReadByte\n";
    assert(source_contract_review(source).empty());
    source["src/model.go"]="package app\nimport \"encoding/json\"\ntype Position struct {\nX int\n}\nfunc(p Position)MarshalJSON()([]byte,error){return nil,nil}\n";
    assert(!has(source_contract_review(source),"json_field_case_boundary"));
    // No consuming member access, or no JSON producer, means no case signal.
    source["src/model.go"]="package app\ntype Position struct {\nX int\n}\n";
    assert(source_contract_review(source).empty());
    source["src/model.go"]="package app\nimport \"encoding/json\"\ntype Position struct {\nX int\n}\n";
    source["src/page.go"]="package app\nconst page=`<script>// position.x\nvar text='position.x';</script>`";
    assert(source_contract_review(source).empty());
    std::map<std::string,std::string> split={
        {"server.go", "package app\nconst header=\"HTTP/1.1 101\\r\\nSec-WebSocket-Accept: \""},
        {"frames.go", "package app\nfunc read(){io.ReadFull(r,p)}"},
        {"timer.go", "package app\nfunc run(){ticker:=time.NewTicker(interval);_ = ticker}"},
        {"settings.go", "package app\nfunc SetRate(n int){interval=n}"}};
    auto boundaries=source_contract_review(split);
    assert(has(boundaries,"manual_websocket_transport") && has(boundaries,"runtime_timer_configuration"));
    split["settings.go"]="package app\nfunc SetRate(n int){ticker.Reset(n)}";
    assert(!has(source_contract_review(split),"runtime_timer_configuration"));
    std::cout<<"PASS: bounded source review signals and conservative Go checkpoint selection\n";
}
