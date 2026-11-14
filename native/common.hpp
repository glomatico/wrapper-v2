#pragma once
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <array>
#include <unistd.h>
namespace worker {
using Json = nlohmann::json;
inline constexpr const char* profile = "apple-music-3.6.0.1109-x86_64-research-v1";
inline void wipe(void* p, size_t n) { auto b=static_cast<volatile unsigned char*>(p); while(n--) *b++=0; }
inline void wipe(std::string& s) { if(!s.empty()) wipe(&s[0],s.size()); s.clear(); }
void wipe_json(Json& j);
struct JsonGuard { Json& j; ~JsonGuard(){wipe_json(j);} };
struct Secret { std::string value; ~Secret(){wipe(value);} };
struct Error : std::exception {
    const char* code; bool retryable, fatal;
    explicit Error(const char* c, bool r=false, bool f=false):code(c),retryable(r),fatal(f){}
    const char* what() const noexcept override { return code; }
};
struct Fd {
    int value;
    explicit Fd(int fd):value(fd){}
    ~Fd(){if(value>=0) close(value);}
    Fd(const Fd&)=delete; Fd& operator=(const Fd&)=delete;
};
std::string base64(const unsigned char*, size_t);
std::string sha256_file(const std::string&);
std::string sha256(const unsigned char*, size_t);
std::string string_param(const Json&, const char*, size_t maximum, bool empty=false);
void validate_adam(const std::string&);
void validate_uri(const std::string&);
}
