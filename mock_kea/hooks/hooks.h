// Mock Kea headers for build demo
#ifndef MOCK_HOOKS_H
#define MOCK_HOOKS_H

#include <string>
#include <map>
#include <vector>

namespace isc {
namespace hooks {

class CalloutHandle {
public:
    template<typename T>
    void getArgument(const std::string& name, T& value) {
        // Mock implementation
    }
};

class LibraryHandle {
public:
    void getParameters() {
        // Mock
    }
};

}
}

#endif