#include "host/cli.hpp"

#include <getopt.h>
#include <unistd.h>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

// Error codes
// 1: missing required configuration
// 2: invalid format (bad --vm string, unknown key, missing '=')
// 3: duplicate key within one --vm
// 4: invalid value (bad memory size, missing image, not executable, etc.)
// 5: help requested

static bool split_pair(const std::string& s, std::string& k, std::string& v) {
    const auto pos = s.find('=');
    if (pos == std::string::npos) return false;
    k = s.substr(0, pos);
    v = s.substr(pos + 1);
    return !(k.empty() || v.empty());
}

static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

static bool parse_int(const std::string& s, int& out) {
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    out = static_cast<int>(v);
    return true;
}

static int parse_vm(const char* vm_c, vm_args_t& vm_args) {
    bool image_set = false, mem_set = false;

    const std::string vm(vm_c ? vm_c : "");
    if (vm.empty()) return 2;

    for (const auto& token : split(vm, ',')) {
        if (token.empty()) return 2;

        std::string key, val;
        if (!split_pair(token, key, val)) return 2;

        if (key == "image") {
            if (image_set) return 3;
            if (access(val.c_str(), X_OK) != 0) return 4;
            vm_args.image = val;
            image_set = true;
        } else if (key == "mem") {
            if (mem_set) return 3;
            int m = 0;
            if (!parse_int(val, m)) return 4;
            if (m != 2 && m != 4 && m != 8) return 4;
            vm_args.memory_sz = static_cast<size_t>(m) << 20; // MiB
            mem_set = true;
        } else {
            return 2; // unknown
        }
    }

    if (!image_set || !mem_set) return 1;

    return 0;
}

void print_help() {
    printf("Usage: mini_hypervisor.a [OPTIONS]\n");
    printf("Options:\n");
    printf("  --vm=KEY=VAL,...     Define a VM configuration (can be specified multiple times)\n");
    printf("                       Required keys: image, mem\n");
    printf("                           image: specifies the path to guest image binary. Valid entries: any path to executable file\n");
    printf("                           mem: specifies total guest memory. Valid entries: 2, 4, 8 [MiB]\n");
    printf("                       Each VM uses one vCPU and 2 MiB paging.\n");
    printf("                       Example: --vm=image=vm1.bin,mem=2\n");
    printf("  --net                Attach one vhost-net/TAP Virtio-net device to every VM\n");
    printf("                       TAP names are vmtap0, vmtap1, ...\n");
    printf("  --help               Show this help message and exit\n");
}

int read_args(int argc, char* argv[], args_t &myArgs) {
    static option longopts[] = {
        {"vm",      required_argument,  nullptr, 'v'},
        {"net",     no_argument,        nullptr, 'n'},
        {"help",    no_argument,        nullptr, 'h'},
        {nullptr,   0,                  nullptr, 0}
    };

    const char* optstring = "+n";

    opterr = 0;
    optind = 1;

    int c;
    int option_index = 0;
    while (-1 != (c = getopt_long(argc, argv, optstring, longopts, &option_index))) {

        if (c == 'v') { // --vm
            vm_args_t vm{};
            int e = parse_vm(optarg, vm);
            if (e != 0) return e;
            myArgs.vms.push_back(std::move(vm));
            continue;
        }

        if (c == 'n') {
            myArgs.network = true;
            continue;
        }

        if (c == 'h') { // --help
            print_help();
            return 5;
        }

        return 2;
    }

    // If there are stray args left
    if (optind < argc) return 2;

    if (myArgs.vms.empty()) return 1;

    return 0;
}
