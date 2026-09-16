#pragma once

#include <cstddef>
#include <vector>
#include <string>

struct vm_args_t {
    std::string image;
    size_t memory_sz;
};

struct args_t {
    std::vector<vm_args_t> vms;
    bool network = false;
};

void print_help();

int read_args(int argc, char *argv[], args_t &myArgs);
