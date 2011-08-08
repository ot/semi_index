#pragma once

#include <string>
#include <boost/range.hpp>
#include <stdint.h>

#include "succinct/bp_vector.hpp"
#include "succinct/elias_fano.hpp"

#include "json_spirit_parser.hpp"
#include "path_parser.hpp"
#include "escape_table.hpp"

namespace semi_index {

    using json::path::path_t;

    class json_semi_index_test_gateway;

    template <typename Iterator>
    class json_semi_index_base {
    public:

        json_semi_index_base() {}

	template <typename StringsRange>
        json_semi_index_base(StringsRange const& jsons) 
        {
            succinct::bit_vector_builder nav;
            succinct::bit_vector_builder bp;

	    typedef typename boost::range_const_iterator<StringsRange>::type iter_t;
	    for (iter_t s = boost::begin(jsons);
		 s != boost::end(jsons);
		 ++s) {

		typedef typename boost::range_value<StringsRange>::type::const_iterator iter_t;
		bool escaped = false;
		iter_t first;
		iter_t json = boost::begin(*s);
		iter_t json_end = boost::end(*s);
		
		for(; json != json_end; ++json) {
		    char c = *json;
		    switch (c) {
		    case '[':
		    case '{':
			nav.push_back(1);
			bp.push_back(1);
			bp.push_back(1);
			break;
		    
		    case '}':
		    case ']':
			nav.push_back(1);
			bp.push_back(0);
			bp.push_back(0);
			break;

		    case ',':
		    case ':':
			nav.push_back(1);
			bp.push_back(0);
			bp.push_back(1);
			break;
                
		    case '"':
			// string literal
			first = json++;
			while (true) {
			    if (json == json_end) {
				throw std::invalid_argument("Unterminated string literal");
			    }
			    if (escaped) {
				escaped = false;
			    } else {
				if (*json == '"') {
				    break;
				}
				escaped = (*json == '\\');
			    }
			    ++json;
			}
			nav.zero_extend(json - first + 1);
			break;
		    default:
			nav.push_back(0);
		    }
		}
            }
	    
            succinct::elias_fano(&nav).swap(m_nav);
            succinct::bp_vector(&bp, true, false).swap(m_bp);
        }
        
        template <typename Visitor>
        void map(Visitor& visit) {
            visit
                (m_nav, "m_nav")
                (m_bp, "m_bp")
                ;
        }

        void swap(json_semi_index_base& other) {
            m_nav.swap(other.m_nav);
            m_bp.swap(other.m_bp);
        }

	class cursor;

        class accessor {
        public:
            accessor() 
		: is_valid(false)
		, m_json()
		, m_index()
		, m_offset()
            {}

            accessor operator[](int64_t idx) const {
                accessor next = *this;
                if (!is_valid) {
                    next.is_valid = false;
                } else {
                    next.is_valid = m_index->get_array_child(m_json, m_node, m_offset, idx, next.m_node);
                }
                return next;
            }

            accessor operator[](std::string const& key) const {
                accessor next = *this;
                if (!is_valid) {
                    next.is_valid = false;
                } else {
                    next.is_valid = m_index->get_object_child(m_json, m_node, m_offset, key, next.m_node);
                }
                return next;
            }

	    accessor get_path(path_t const& path) const {
		accessor next = *this;
		for (size_t i = 0; i < path.size() && next.is_valid; ++i) {
		    const std::string* key;
		    const int* idx;
		    if ((key = boost::get<std::string>(&path[i]))) {
			next = next[*key];
		    } else if ((idx = boost::get<int>(&path[i]))) {
			next = next[*idx];
		    }
		}
		return next;
	    }

            size_t get_pos() const {
                return m_index->get_pos(m_node, m_offset);
            }

            typedef std::pair<size_t, size_t> range_t;
            
            range_t get_range() const {
                size_t pos = get_pos();
                size_t end = m_index->FindEnd(m_node, m_offset);
                return range_t(pos, end);
            }
            
            json::parser::value parse() const {
                json::parser::value value;
                if (!is_valid) {
                    std::terminate();
                }
                range_t range = get_range();
                bool ret = json::parser::parse(m_json + range.first, m_json + range.second, value);
                if (!ret) {
                    std::terminate();
                }
                return value;
            }
            
            bool is_valid;
            friend class cursor;
        private:
            accessor(Iterator json, const json_semi_index_base* index, uint64_t node, size_t offset) 
                : is_valid(true)
                , m_node(node)
                , m_json(json)
                , m_index(index)
		, m_offset(offset)
            {}

            uint64_t m_node;
            Iterator m_json;
            const json_semi_index_base* m_index;
	    size_t m_offset;
        };

	class cursor {
	public:
	    cursor()
		: m_index(0)
		, m_node(0)
		, m_offset(0)
	    {}
	    
	    accessor get_accessor(Iterator json) const {
		return accessor(json, m_index, m_node, m_offset);
	    }

	    size_t get_offset() const {
		return m_offset;
	    }

	    cursor next() const {
		uint64_t next_node = m_index->find_close(m_node) + 1;
		assert(next_node <= m_index->tree_size());
		if (next_node == m_index->tree_size()) {
		    return cursor();
		} else {
		    return cursor(m_index, next_node);
		}
	    }
	    
	    bool operator==(cursor const& other) const {
		return 
		    (m_index == other.m_index) &&
		    (m_node == other.m_node) &&
		    (m_offset == other.m_offset);
	    }

	    friend class json_semi_index_base;

	private:
	    cursor(const json_semi_index_base* index, uint64_t node)
		: m_index(index)
		, m_node(node)
		, m_offset(m_index->get_pos(m_node, 0))
	    {}

	    const json_semi_index_base* m_index;
	    uint64_t m_node;
	    size_t m_offset;
	};

	cursor get_cursor() const {
	    return cursor(this, 0);
	}

	uint64_t tree_size() const {
	    return m_bp.size();
	}

        friend class json_semi_index_test_gateway;

    protected:
        bool get_object_child(Iterator json, uint64_t node, size_t offset, std::string const& key, uint64_t& child_node) const {
            node += node % 2;
            size_t opening_pos = get_pos(node, offset);
	    Iterator iter = json + opening_pos;
            if (*iter != '{') {
                return false;
            }
            if (*++iter == '}') {
                // Empty objects are a special case ("(())" in BP representation)
                return false;
            }

            uint64_t cur_node = node + 1;
            uint64_t cur_node_pos = opening_pos + 1;

            while (true) {
                if (m_bp[cur_node] == 0) {
                    return false;
                }

                uint64_t cur_node_end = m_bp.find_close(cur_node);
                uint64_t cur_node_val_begin = cur_node_end + 1;

                if (check_key(json, key, cur_node_pos)) {
                    child_node = cur_node_val_begin;
                    return true;
                }

                uint64_t cur_node_val_end = m_bp.find_close(cur_node_val_begin);
                cur_node = cur_node_val_end + 1;
                cur_node_pos = get_pos(cur_node, offset) + 1;
            }
        }

        bool get_array_child(Iterator json, uint64_t node, size_t offset, int64_t idx, uint64_t& child_node) const {
            node += node % 2;
            size_t opening_pos = get_pos(node, offset);

	    Iterator iter = json + opening_pos;
            if (*iter != '[') {
                return false;
            }
            if (*++iter == ']') {
                // Empty arrays are a special case ("(())" in BP representation)
                return false;
            }
            
            int64_t i = 0;
            uint64_t cur_node;
            if (idx >= 0) {
                cur_node = node + 1;
            
                while (true) {
                    if (m_bp[cur_node] == 0) {
                        return false;
                    }
                    if (i == idx) {
                        break;
                    }
                    cur_node = m_bp.find_close(cur_node) + 1;
                    ++i;
                }
            } else {
                cur_node = m_bp.find_close(node) - 1;

                while (true) {
                    if (m_bp[cur_node] == 1) {
                        return false;
                    }
                    if (i == -idx - 1) {
                        break;
                    }
                    cur_node = m_bp.find_open(cur_node) - 1;
                    ++i;
                }

                cur_node = m_bp.find_open(cur_node);
            }
            child_node = cur_node;
            return true;
        }

        size_t get_pos(uint64_t node, size_t offset) const {
	    return m_nav.select(node / 2) + (node % 2) - offset;
        }

        size_t FindEnd(uint64_t node, size_t offset) const {
            uint64_t closer = m_bp.find_close(node);
            return m_nav.select(closer / 2) + (1 - node % 2) - offset;
	}

        uint64_t find_close(uint64_t node) const {
            return m_bp.find_close(node);
        }
        
        // TODO: switch to real parsing
        static bool check_key(Iterator json, std::string const& key, size_t pos) {
            // Go to the beginning of the string
	    Iterator it = json + pos;
            while (*it++ != '"');
            const char* k = key.c_str();
            const char* k_end = k + key.size();
            for (; k != k_end; ++k) {
                char c = *it++;
                if (c == '\\') {
                    c = (char)json::parser::escape_table[(unsigned char)*it++];
                }
                if (c != *k) {
                    return false;
                }
            }
            if (*it == '"') {
                return true;
            } else {
                return false;
            }
        }

        succinct::elias_fano m_nav;
        succinct::bp_vector m_bp;
    };
 
    typedef json_semi_index_base<const char*> json_semi_index;
}

