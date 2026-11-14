#pragma once
#include "common.hpp"
namespace worker {
class Protocol {
    int input_,output_;
public:
    Protocol(int input,int output):input_(input),output_(output){}
    bool read(Json&);
    void write(const Json&);
    static void validate(const Json&);
};
}
