#pragma once

#include <string>
#include <vector>
#include <utility>

#include <boost/variant/variant.hpp>
#include <boost/variant/get.hpp>

namespace json {
namespace path {

    typedef boost::variant<
        std::string,
	int
	> path_element_t;
    
    typedef std::vector<path_element_t> path_t;
    typedef std::vector<path_t> path_list_t;

    path_list_t parse(std::string const& s);
}}

