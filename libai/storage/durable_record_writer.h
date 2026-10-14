#pragma once
// Private POSIX backend primitive; not a stable public SDK interface.
#ifndef _WIN32
#include <atomic>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <functional>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace libai { namespace ai { namespace storage_detail {
struct record_fd_t {
    int fd;
    explicit record_fd_t(int value):fd(value){}
    ~record_fd_t(){if(fd>=0)::close(fd);}
    record_fd_t(const record_fd_t &)=delete;
    record_fd_t &operator=(const record_fd_t &)=delete;
};
// sync is an explicit private test seam. Production callers use real fsync.
inline bool write_private_record(const std::string &directory,const std::string &name,
    const std::string &content,std::string &error,
    const std::function<int(int)> &sync=std::function<int(int)>())
{
    error.clear();
    auto fail=[&](const char *phase){error=std::string(phase)+": "+std::strerror(errno);return false;};
    if(name.empty()||name=="."||name==".."||name.size()>160){errno=EINVAL;return fail("invalid record name");}
    for(unsigned char c:name)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||
        (c>='0'&&c<='9')||c=='.'||c=='_'||c=='-')){errno=EINVAL;return fail("invalid record name");}
    record_fd_t dir(::open(directory.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
    if(dir.fd<0)return fail("record directory open failed");
    struct stat metadata;
    if(::fstat(dir.fd,&metadata)!=0)return fail("record directory stat failed");
    if(!S_ISDIR(metadata.st_mode)||metadata.st_uid!=::geteuid()||(metadata.st_mode&0777)!=0700){
        errno=EPERM;return fail("record directory is not private");
    }
    if(::fstatat(dir.fd,name.c_str(),&metadata,AT_SYMLINK_NOFOLLOW)==0){
        if(!S_ISREG(metadata.st_mode)||metadata.st_uid!=::geteuid()||
            (metadata.st_mode&0777)!=0600||metadata.st_nlink!=1){errno=EPERM;return fail("record target is not private");}
    }else if(errno!=ENOENT)return fail("record target stat failed");
    static std::atomic<unsigned long long> serial(0);
    std::string temporary;int descriptor=-1;
    for(unsigned attempt=0;attempt<64;++attempt){
        temporary="."+name+".tmp."+std::to_string(::getpid())+"."+std::to_string(++serial);
        descriptor=::openat(dir.fd,temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(descriptor>=0)break;
        if(errno!=EEXIST)return fail("record temporary open failed");
    }
    if(descriptor<0)return fail("record temporary names exhausted");
    record_fd_t file(descriptor);
    auto discard=[&](const char *phase){int saved=errno;::unlinkat(dir.fd,temporary.c_str(),0);errno=saved;return fail(phase);};
    // Set the exact mode even under a restrictive umask, before writing data.
    if(::fchmod(file.fd,0600)!=0)return discard("record protection failed");
    size_t offset=0;
    while(offset<content.size()){
        const size_t remaining=content.size()-offset;
        const ssize_t written=::write(file.fd,content.data()+offset,remaining>65536?65536:remaining);
        if(written<0){if(errno==EINTR)continue;return discard("record write failed");}
        if(written==0){errno=EIO;return discard("record write made no progress");}
        offset+=static_cast<size_t>(written);
    }
    auto flush=[&](int fd){int status;do{status=sync ? sync(fd) : ::fsync(fd);}while(status!=0&&errno==EINTR);return status==0;};
    if(!flush(file.fd))return discard("record file sync failed");
    if(::renameat(dir.fd,temporary.c_str(),dir.fd,name.c_str())!=0)return discard("record installation failed");
    // After rename, failure is an uncertain durable outcome, never 'not written'.
    if(!flush(dir.fd))return fail("record installed but directory sync failed");
    return true;
}
}}}
#endif
