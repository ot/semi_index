#define BOOST_TEST_MODULE json_semi_index
#include "succinct/test_common.hpp"

#include "json_semi_index.hpp"

namespace semi_index {
    class json_semi_index_test_gateway {
    public:
        static void test_balanced(json_semi_index const& index) {
            int excess = 0;
            succinct::bp_vector const& bp = index.m_bp;
            for (size_t i = 0; i < bp.size(); ++i) {
                excess += bp[i] ? 1 : -1;
                BOOST_CHECK(excess >= 0);
            }
            BOOST_CHECK_EQUAL(0, excess);
        }
    };
}

BOOST_AUTO_TEST_CASE(json_semi_index)
{
    using boost::get;
    const std::string jsons[] = {
	"{\"top\": [1, 2, 3, {},  {\"a\": \"b\"},[]], \"d\": {\"a1\": 1, \"a2\": 2}, \"litchars\": \"{}[]:,\\\"\\\\\"} \n",
	"{\"foo\": \"bar\"}"
    };
    
    semi_index::json_semi_index index(jsons);
    semi_index::json_semi_index_test_gateway::test_balanced(index);

    semi_index::json_semi_index::cursor cursor = index.get_cursor();
    semi_index::json_semi_index::accessor accessor, root = cursor.get_accessor(jsons[0].c_str());
    semi_index::json_semi_index::accessor::range_t range;

    BOOST_CHECK(root.is_valid);
    range = root.get_range();
    BOOST_CHECK_EQUAL(0U, range.first);
    BOOST_CHECK_EQUAL(89U, range.second);
    
    accessor = root["top"];
    BOOST_CHECK(accessor.is_valid);
    BOOST_CHECK_EQUAL(7U, accessor.get_pos());
    range = accessor.get_range();
    BOOST_CHECK_EQUAL(7U, range.first);
    BOOST_CHECK_EQUAL(37U, range.second);

    accessor = root["litchars"];
    BOOST_CHECK(accessor.is_valid);
    BOOST_CHECK_EQUAL(75U, accessor.get_pos());
    range = accessor.get_range();
    BOOST_CHECK_EQUAL(75U, range.first);
    BOOST_CHECK_EQUAL(88U, range.second);

    accessor = root["xx"];
    BOOST_CHECK(!accessor.is_valid);

    accessor = root["top"][4];
    BOOST_CHECK(accessor.is_valid);
    BOOST_CHECK_EQUAL(21U, accessor.get_pos());

    accessor = root["top"][-2];
    BOOST_CHECK(accessor.is_valid);
    BOOST_CHECK_EQUAL(21U, accessor.get_pos());

    accessor = root["top"][-7];
    BOOST_CHECK(!accessor.is_valid);

    accessor = root["top"][4]["a"];
    BOOST_CHECK(accessor.is_valid);
    BOOST_CHECK_EQUAL(28U, accessor.get_pos());
    range = accessor.get_range();
    BOOST_CHECK_EQUAL(28U, range.first);
    BOOST_CHECK_EQUAL(32U, range.second);

    accessor = root["top"][4]["xx"];
    BOOST_CHECK(!accessor.is_valid);

    accessor = root["top"][5][0];
    BOOST_CHECK(!accessor.is_valid);

    accessor = root["top"][6];
    BOOST_CHECK(!accessor.is_valid);

    accessor = root[0];
    BOOST_CHECK(!accessor.is_valid);

    accessor = root["top"]["x"];
    BOOST_CHECK(!accessor.is_valid);

    accessor = root["to"];
    BOOST_CHECK(!accessor.is_valid);

    accessor = root["top\""];
    BOOST_CHECK(!accessor.is_valid);

    BOOST_CHECK_EQUAL("b", get<std::string>(root["top"][4]["a"].parse()));
    BOOST_CHECK_EQUAL(1.0, get<double>(root["d"]["a1"].parse()));
    BOOST_CHECK_EQUAL(2.0, get<double>(root["d"]["a2"].parse()));
    BOOST_CHECK_EQUAL("{}[]:,\"\\", get<std::string>(root["litchars"].parse()));

    json::path::path_list_t paths = json::path::parse("top[4].a,d.a1,d.a2,litchars");

    BOOST_CHECK_EQUAL("b", get<std::string>(root.get_path(paths[0]).parse()));
    BOOST_CHECK_EQUAL(1.0, get<double>(root.get_path(paths[1]).parse()));
    BOOST_CHECK_EQUAL(2.0, get<double>(root.get_path(paths[2]).parse()));
    BOOST_CHECK_EQUAL("{}[]:,\"\\", get<std::string>(root.get_path(paths[3]).parse()));

    cursor = cursor.next();
    root = cursor.get_accessor(jsons[1].c_str());

    BOOST_CHECK(root.is_valid);
    range = root.get_range();
    BOOST_CHECK_EQUAL(0U, range.first);
    BOOST_CHECK_EQUAL(14U, range.second);
    BOOST_CHECK_EQUAL("bar", get<std::string>(root["foo"].parse()));

    cursor = cursor.next();
    BOOST_CHECK(cursor == semi_index::json_semi_index::cursor());
}
