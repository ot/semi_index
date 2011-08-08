#define BOOST_TEST_MODULE bp_vector
#include "test_common.hpp"

#include <cstdlib>
#include <boost/foreach.hpp>

#include "mapper.hpp"
#include "bp_vector.hpp"

template <class BPVector>
void test_parentheses(std::vector<int> const& v, BPVector const& bitmap, const char* test_name)
{
    std::stack<size_t> stack;
    std::vector<uint64_t> open(v.size());
    std::vector<uint64_t> close(v.size());
    std::vector<uint64_t> enclose(v.size());
    
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i]) { // opening
            if (i) {
                enclose[i] = stack.top();
            }
            stack.push(i);
        } else { // closing
            BOOST_REQUIRE(!stack.empty()); // this is more a test on the test 
            size_t opening = stack.top();
            stack.pop();
            close[opening] = i;
            open[i] = opening;
            
        }
    }
    BOOST_REQUIRE_EQUAL(0U, stack.size()); // ditto as above
    
    for (size_t i = 0; i < bitmap.size(); ++i) {
        if (v[i]) { // opening
            if (i) {
                MY_REQUIRE_EQUAL(enclose[i], bitmap.enclose(i),
				 "enclose (" << test_name << "): i = " << i);
            }
            MY_REQUIRE_EQUAL(close[i], bitmap.find_close(i),
			     "find_close (" << test_name << "): i = " << i);
        } else { // closing
            MY_REQUIRE_EQUAL(open[i], bitmap.find_open(i),
			     "find_open (" << test_name << "): i = " << i);
        }            
    }
}

BOOST_AUTO_TEST_CASE(bp_vector)
{
    srand(42);

    std::vector<int> v;

    {
	succinct::bp_vector bitmap(v);
	test_parentheses(v, bitmap, "Empty vector");
    }
    

    int excess = 0;
    for (size_t i = 0; i < 100000; ++i) {
        int val = rand() > RAND_MAX / 2;
        if (excess <= 1 && !val) {
            val = 1;
        }
        excess += (val ? 1 : -1);
        v.push_back(val);
    }

    v.insert(v.end(), excess, 0); // close all parentheses
    
    {
	succinct::bp_vector bitmap(v);
	test_parentheses(v, bitmap, "Random parentheses");
    }
    
    v.clear();
    v.insert(v.end(), 1 << 15, 1);
    v.insert(v.end(), 1 << 15, 0);

    {
	succinct::bp_vector bitmap(v);
	test_parentheses(v, bitmap, "2^15 nested parentheses");
    }
}

