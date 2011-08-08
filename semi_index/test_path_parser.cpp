#define BOOST_TEST_MODULE path_parser
#include "succinct/test_common.hpp"

#include "path_parser.hpp"

BOOST_AUTO_TEST_CASE(path_parser)
{
    using boost::get;
    json::path::path_list_t paths = json::path::parse("foo.bar[12].foobar,,abc.def,abc");
    BOOST_CHECK_EQUAL(4U, paths.size());
    BOOST_CHECK_EQUAL(0U, paths[1].size());
    
    json::path::path_t path = paths[0];
    BOOST_CHECK_EQUAL(4U, path.size());
    BOOST_CHECK_EQUAL("foo", get<std::string>(path[0]));
    BOOST_CHECK_EQUAL("bar", get<std::string>(path[1]));
    BOOST_CHECK_EQUAL(12, get<int>(path[2]));
    BOOST_CHECK_EQUAL("foobar", get<std::string>(path[3]));
    
    path = paths[3];
    BOOST_CHECK_EQUAL("abc", get<std::string>(path[0]));
}
