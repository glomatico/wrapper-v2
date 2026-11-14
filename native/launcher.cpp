#include <sched.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <cerrno>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <string>
// Linux-only launcher. Mounts are private and disappear on process exit.
int main(int argc,char** argv) {
    if(argc!=2){std::fputs("usage: worker-launch ROOTFS\n",stderr);return 64;}
    if(unshare(CLONE_NEWNS)||mount(nullptr,"/",nullptr,MS_REC|MS_PRIVATE,nullptr)){std::fputs("worker-launch: private mounts unavailable\n",stderr);return 70;}
    std::string root=argv[1];
    for(const char* name:{"/proc","/dev"})if(mkdir((root+name).c_str(),0755)&&errno!=EEXIST){std::fputs("worker-launch: missing mount target\n",stderr);return 70;}
    if(mount("/proc",(root+"/proc").c_str(),nullptr,MS_BIND|MS_REC,nullptr)||
       mount("/dev",(root+"/dev").c_str(),nullptr,MS_BIND|MS_REC,nullptr)||
       chdir(root.c_str())||chroot(".")||chdir("/")){std::fputs("worker-launch: rootfs setup failed\n",stderr);return 70;}
    setenv("ANDROID_DNS_MODE","local",1);
    setenv("ANDROID_DATA","/data",1);setenv("ANDROID_ROOT","/system",1);
    setenv("SSL_CERT_FILE","/etc/ssl/certs/ca-certificates.crt",1);
    setenv("CURL_CA_BUNDLE","/etc/ssl/certs/ca-certificates.crt",1);
    unsetenv("LD_PRELOAD");unsetenv("LD_LIBRARY_PATH");
    rlimit core{0,0};setrlimit(RLIMIT_CORE,&core);
    execl("/system/bin/apple-music-worker","apple-music-worker",static_cast<char*>(nullptr));
    std::fputs("worker-launch: exec failed\n",stderr);return 70;
}
