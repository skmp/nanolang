#include "attoruntime.h"
#include <cstdlib>

#ifdef _WIN32
extern "C" extern char** environ;
#else
extern char** environ;
#endif

int main(int argc, char* argv[]) {
    std::vector<std::string> args(argv, argv + argc);
    std::vector<std::string> envs;
    if (environ) {
        for (char** env = environ; *env; ++env)
            envs.emplace_back(*env);
    }
    on_start(args, envs);
    return 0;
}
