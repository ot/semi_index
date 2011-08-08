#define BOOST_TEST_MODULE bit_vector
#include "test_common.hpp"
#include "test_rank_select_common.hpp"

#include <cstdlib>
#include <boost/foreach.hpp>

#include "mapper.hpp"
#include "bit_vector.hpp"

BOOST_AUTO_TEST_CASE(bit_vector)
{
    srand(42);
    
    std::vector<bool> v = random_bit_vector();

    {
        succinct::bit_vector_builder bvb;
        for (size_t i = 0; i < v.size(); ++i) {
            bvb.push_back(v[i]);
        }
        succinct::bit_vector bitmap(&bvb);
        test_equal_bits(v, bitmap, "Random bits (push_back)");
    }

    {
        succinct::bit_vector_builder bvb(v.size());
        for (size_t i = 0; i < v.size(); ++i) {
            bvb.set(i, v[i]);
        }
        bvb.push_back(0);
        v.push_back(0);
        bvb.push_back(1);
        v.push_back(1);

        succinct::bit_vector bitmap(&bvb);
        test_equal_bits(v, bitmap, "Random bits (set)");
    }
    
    {
        uint64_t ints[] = {uint64_t(-1), uint64_t(1) << 63, 1, 1, 1, 3, 5, 7, 0xFFF, 0xF0F, 1, 0xFFFFFF, 0x123456, uint64_t(1) << 63, uint64_t(-1)};
        succinct::bit_vector_builder bvb;
        BOOST_FOREACH(uint64_t i, ints) {
            uint64_t len = succinct::broadword::msb(i) + 1;
            bvb.append_bits(i, len);
        }
        succinct::bit_vector bitmap(&bvb);
        uint64_t pos = 0;
        BOOST_FOREACH(uint64_t i, ints) {
            uint64_t len = succinct::broadword::msb(i) + 1;
            BOOST_REQUIRE_EQUAL(i, bitmap.get_bits(pos, len));
            pos += len;
        }
    }    
}

