#include <libai/storage/durable_record_writer.h>
#include <libai/common/file_ops.h>
#include <cassert>
#include <fstream>
#include <iterator>
#include <dirent.h>
#include <sys/wait.h>
using namespace webcool::ai;
static std::string read(const std::string &path){
    std::ifstream file(path.c_str(),std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>());
}
static void no_temporary(const std::string &root){
    DIR *dir=opendir(root.c_str());assert(dir);
    while(auto *entry=readdir(dir))assert(std::string(entry->d_name).find(".state.json.tmp.")==std::string::npos);
    closedir(dir);
}
int main(int argc,char **argv){
    assert(argc==2);const std::string root=argv[1],target=root+"/state.json";
    assert(file_ops::make_private_directory(root));std::string error;
    assert(storage_detail::write_private_record(root,"state.json","original",error));
    struct stat st;assert(lstat(target.c_str(),&st)==0&&(st.st_mode&0777)==0600);
    unsigned calls=0;
    assert(!storage_detail::write_private_record(root,"state.json","uncommitted",error,[&](int){++calls;errno=EIO;return -1;}));
    assert(calls==1&&read(target)=="original"&&error.find("file sync failed")!=std::string::npos);no_temporary(root);
    calls=0;
    assert(!storage_detail::write_private_record(root,"state.json","installed",error,[&](int fd){
        struct stat kind;assert(fstat(fd,&kind)==0);
        if(++calls==2){assert(S_ISDIR(kind.st_mode));errno=EIO;return -1;}assert(S_ISREG(kind.st_mode));return fsync(fd);
    }));
    assert(calls==2&&read(target)=="installed"&&error.find("installed but directory sync failed")!=std::string::npos);no_temporary(root);
    calls=0;
    assert(storage_detail::write_private_record(root,"state.json","synced",error,[&](int fd){
        if(++calls==1){errno=EINTR;return -1;}return fsync(fd);
    }));assert(calls==3&&read(target)=="synced");
    // Poisoning the previous fixed .tmp name neither changes it nor its target.
    const std::string sentinel=root+"/sentinel",fixed=root+"/state.json.tmp";
    {std::ofstream out(sentinel.c_str());out<<"untouched";}
    unlink(fixed.c_str());assert(symlink(sentinel.c_str(),fixed.c_str())==0);
    assert(storage_detail::write_private_record(root,"state.json","safe",error));
    assert(read(sentinel)=="untouched");assert(lstat(fixed.c_str(),&st)==0&&S_ISLNK(st.st_mode));unlink(fixed.c_str());
    const std::string unsafe=root+"/unsafe.json";unlink(unsafe.c_str());assert(symlink(sentinel.c_str(),unsafe.c_str())==0);
    assert(!storage_detail::write_private_record(root,"unsafe.json","poison",error));assert(read(sentinel)=="untouched");unlink(unsafe.c_str());
    assert(link(target.c_str(),unsafe.c_str())==0);
    assert(!storage_detail::write_private_record(root,"state.json","poison",error));assert(read(target)=="safe");unlink(unsafe.c_str());
    assert(!storage_detail::write_private_record(root,"../escape","poison",error));
    assert(chmod(root.c_str(),0755)==0);
    assert(!storage_detail::write_private_record(root,"state.json","poison",error));assert(read(target)=="safe");assert(chmod(root.c_str(),0700)==0);
    // Both processes create independent temporary files. Readers see whole records.
    assert(storage_detail::write_private_record(root,"state.json","seed",error));
    pid_t children[2];
    for(unsigned i=0;i<2;++i){
        children[i]=fork();assert(children[i]>=0);
        if(children[i]==0){std::string value(1024*1024,i?'B':'A');
            for(unsigned n=0;n<5;++n)if(!storage_detail::write_private_record(root,"state.json",value,error))_exit(2);
            _exit(0);
        }
    }
    unsigned alive=2;
    while(alive){
        std::string value=read(target);
        assert(value=="seed"||(value.size()==1024*1024&&(value==std::string(value.size(),'A')||value==std::string(value.size(),'B'))));
        for(auto &child:children)if(child>0){int status=0;pid_t done=waitpid(child,&status,WNOHANG);
            assert(done>=0);if(done){assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);child=-1;--alive;}}
    }
    no_temporary(root);
}
