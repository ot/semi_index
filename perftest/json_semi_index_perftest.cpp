#include <cstdlib>
#include <iostream>
#include <fstream>
#include <set>
#include <limits>
#include <algorithm>

#include <boost/foreach.hpp>
#include <boost/ptr_container/ptr_vector.hpp>
#include "jsoncpp/json/json.h"
// #include <yajl/yajl_parse.h>

#include "succinct/util.hpp"
#include "succinct/mapper.hpp"

#include "semi_index/json_semi_index.hpp"

#include "perftest_common.hpp"

int main(int argc, char** argv)
{
    srand(42); 

    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <JSON file> <paths> [runs]" << std::endl;
        exit(1);
    }

    const char* filename = argv[1];
    std::ifstream json_file(filename, std::ios::binary);
    if (!json_file) {
	std::cerr << "Error while opening " << filename << std::endl;
	exit(1);
    }
    std::string paths_s(argv[2]);
    
    size_t runs = 10;
    if (argc >= 4) {
        runs = atoi(argv[3]);
    }
    
    int limit = -1;
    if (argc >= 5) {
        limit = atoi(argv[4]);
    }

    std::string line;
    std::vector<std::string> json_strings;
    std::cerr << "Reading input... " << std::endl;
    while (std::getline(json_file, line)) {
        succinct::util::trim_newline_chars(line);
        json_strings.push_back(line);
        if (limit != -1) {
            --limit;
            if (!limit) {
                break;
            }
        }
    }
    std::cerr << json_strings.size() << " JSON strings." << std::endl;
    
    TIMEIT("Linear scan (lower bound):", runs * json_strings.size()) {
        for (size_t i = 0; i < runs; ++i) {
            BOOST_FOREACH(std::string const& json, json_strings) {
                int c = 0;
                const char* p = json.c_str();
                const char* last = p + json.size();
                for (; p != last; ++p) {
                    c += (*p == '"');
                }
                volatile int x = c; (void)x;
            }
        }
    }

    TIMEIT("JSON string copy:", runs * json_strings.size()) {
        for (size_t i = 0; i < runs; ++i) {
            BOOST_FOREACH(std::string const& json, json_strings) {
                char* buf = new char[json.size()];
                memcpy(buf, &json[0], json.size());
                delete [] buf;
            }
        }
    }

    TIMEIT("json_semi_index building:", runs * json_strings.size()) {
        for (size_t i = 0; i < runs; ++i) {
                semi_index::json_semi_index index(json_strings);
        }
    }

    TIMEIT("JsonCpp parsing:", runs * json_strings.size()) {
        Json::Reader reader;
        for (size_t i = 0; i < runs; ++i) {
            BOOST_FOREACH(std::string const& json, json_strings) {
                Json::Value root;
                reader.parse(json, root);
            }
        }
    }

//     TIMEIT("YAJL verification:", runs * json_strings.size()) {
//         yajl_parser_config cfg = { 0, 1 };
    
//         for (size_t i = 0; i < runs; ++i) {
//             BOOST_FOREACH(std::string const& json, json_strings) {
//                 yajl_handle hand = yajl_alloc(NULL, &cfg, NULL, NULL);
//                 yajl_status stat;
//                 stat = yajl_parse(hand, (const uint8_t*)json.c_str(), (unsigned int)json.size());
//                 if (stat != yajl_status_ok) std::terminate();
//                 yajl_parse_complete(hand);
//                 yajl_free(hand);
//             }
//         }
//     }

    TIMEIT("Spirit2 parsing:", runs * json_strings.size()) {
        for (size_t i = 0; i < runs; ++i) {
            BOOST_FOREACH(std::string const& json, json_strings) {
                json::parser::value root;
                json::parser::parse(json, root);
            }
        }
    }

    runs *= 100;

    using json::path::path_element_t;
    using json::path::path_t;
    using json::path::path_list_t;
    using json::path::parse;
    path_list_t paths = parse(paths_s);

    {
        semi_index::json_semi_index index(json_strings);
	size_t index_size = succinct::mapper::size_of(index);

        size_t total_json = 0;
        BOOST_FOREACH(std::string const& json, json_strings) {
            total_json += json.size();
        }

	std::cerr << "Total JSON: " << total_json
		  << " json_semi_index overhead: " << (double)index_size / total_json
                  << std::endl;

        succinct::mapper::size_tree_of(index, "json_semi_index")->dump();

        TIMEIT("Accessing elements with json_semi_index:", runs * json_strings.size()) {
            for (size_t i = 0; i < runs; ++i) {
		semi_index::json_semi_index::cursor cursor = index.get_cursor();
                for (size_t idx = 0; idx < json_strings.size(); ++idx) {
                    semi_index::json_semi_index::accessor accessor, root = cursor.get_accessor(json_strings[idx].c_str());
                    BOOST_FOREACH(path_t const& path, paths) {
                        accessor = root.get_path(path);
                        if (accessor.is_valid) 
                            accessor.parse();
                    }
		    cursor = cursor.next();
                }
            }
        }
    }
    
    {
        boost::ptr_vector<Json::Value> parsed_values;
        Json::Reader reader;
        BOOST_FOREACH(std::string const& json, json_strings) {
            Json::Value* parsed = new Json::Value;
            if (!reader.parse(json, *parsed)) std::terminate();
            parsed_values.push_back(parsed);
        }
        TIMEIT("Accessing elements with JsonCpp:", runs * json_strings.size()) {
            for (size_t i = 0; i < runs; ++i) {
                for (size_t idx = 0; idx < json_strings.size(); ++idx) {
                    Json::Value const& root = parsed_values[idx];
                    Json::Value const* accessor;
                    BOOST_FOREACH(path_t const& path, paths) {
                        accessor = &root;
                        BOOST_FOREACH(path_element_t const& element, path) {
			    const std::string* key;
			    const int* idx;
			    if ((key = boost::get<std::string>(&element))) {
                                if (!accessor->isObject()) break;
                                accessor = &(*accessor)[*key];
			    } else if ((idx = boost::get<int>(&element))) {
                                if (!accessor->isArray()) break;
                                if (*idx >= 0) {
                                    accessor = &(*accessor)[*idx];
                                } else {
                                    accessor = &(*accessor)[accessor->size() + *idx];
                                }
			    }
                        }
                    }
                }
            }
        }
    }
    
    return 0;
}
