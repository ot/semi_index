#pragma once

#include <string>
#include <vector>
#include <utility>
#include <boost/unordered_map.hpp>

#include <boost/variant/variant.hpp>
#include <boost/variant/get.hpp>
#include <boost/variant/recursive_wrapper.hpp>

#include <boost/ptr_container/ptr_vector.hpp>

namespace json {
namespace parser {

    using boost::recursive_wrapper;
    struct object;
    struct array;
    struct null_value {};

    typedef boost::variant<
        null_value,
        bool,
        std::string,
        double,
        recursive_wrapper<object>,
        recursive_wrapper<array> > value;
    
    struct object : boost::unordered_map<std::string, value>
    {
        void steal_append(std::string& k, value& v);

        friend inline 
        void swap(object& a, object& b) {
            a.swap(b);
        }
    };
    
    struct array : boost::ptr_vector<value>
    {
        void steal_append(value& val);

        friend inline 
        void swap(array& a, array& b) {
            a.swap(b);
        }
    };

    bool parse(std::string const& s, value& val);
    bool parse(const char* first, const char* last, value& val);
}
}
