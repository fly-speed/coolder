#pragma once
#include <sstream>
#include <string>
#include <vector>

namespace libai { namespace ai {
// Deliberately narrow: only a plain Go build of the exact configured target may
// defer its shell wrapper during intermediate checks. Never interpret a shell.
inline bool plain_go_build_script(const std::string &script, const std::vector<std::string> &args)
{
    if(script.size()>4096 || args.size()!=4 || args[0]!="build" || args[1]!="-o")return false;
    const auto safe=[](const std::string &s) {
        return !s.empty() && s[0]!='-' && s.find("..") == std::string::npos &&
            s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_./-")==std::string::npos;
    };
    bool build=false;std::istringstream lines(script);std::string line;
    while(std::getline(lines,line)) {
        const auto first=line.find_first_not_of(" \t\r");
        if(first==std::string::npos || line[first]=='#')continue;
        std::istringstream words(line);std::vector<std::string> tokens;std::string word;
        while(words>>word)tokens.push_back(word);
        if(tokens==std::vector<std::string>{"set","-eu"} || tokens==std::vector<std::string>{"set","-e"})continue;
        if(!build && tokens.size()==3 && tokens[0]=="mkdir" && tokens[1]=="-p" && safe(tokens[2]) && tokens[2][0]!='/')continue;
        if(build || tokens.size()!=5 || tokens[0]!="go" || tokens[1]!="build" || tokens[2]!="-o" ||
            !safe(tokens[3]) || tokens[3][0]=='/' || tokens[4]!=args[3])return false;
        build=true;
    }
    return build;
}
} }
