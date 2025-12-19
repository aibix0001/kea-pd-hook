// Mock cc/data.h
#ifndef MOCK_CC_DATA_H
#define MOCK_CC_DATA_H

#include <string>

namespace isc {
namespace data {

class Element {
public:
    enum Type { string, integer, map };
    virtual Type getType() const = 0;
    virtual std::string stringValue() const = 0;
    virtual long intValue() const = 0;
};

class ConstElementPtr {
public:
    ConstElementPtr get(const std::string& key) const;
    Element::Type getType() const;
    std::string stringValue() const;
    long intValue() const;
};

}
}

#endif