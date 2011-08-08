#define BOOST_TEST_MODULE json_spirit_parser
#include "succinct/test_common.hpp"

#include "json_spirit_parser.hpp"

BOOST_AUTO_TEST_CASE(json_spirit_parser)
{
    using boost::get;
    json::parser::value value;
    BOOST_CHECK(json::parser::parse("\"foo\\\"}{][\"", value));
    BOOST_CHECK_EQUAL("foo\"}{][", get<std::string>(value));

    BOOST_CHECK(json::parser::parse("3.14", value));
    BOOST_CHECK_EQUAL(3.14, get<double>(value));

    BOOST_CHECK(json::parser::parse(" [{}, 1, 2, \"bar\", {\"a\": 2, \"b\": 2}]", value));
    BOOST_CHECK(2 == get<double>(get<json::parser::array>(value)[2]));
    BOOST_CHECK_EQUAL("bar", get<std::string>(get<json::parser::array>(value)[3]));

    BOOST_CHECK(json::parser::parse("{\"a\": [1, 2, \"bar\"], \"b\": \"foobar\"}", value));
    json::parser::object const& obj = get<json::parser::object>(value);
    BOOST_CHECK_EQUAL(1U, obj.count("a"));
    BOOST_CHECK_EQUAL(1U, obj.count("b"));
    BOOST_CHECK(2 == get<double>(get<json::parser::array>(obj.find("a")->second)[1]));
    BOOST_CHECK_EQUAL("bar", get<std::string>(get<json::parser::array>(obj.find("a")->second)[2]));

    BOOST_CHECK(json::parser::parse("{} {", value));
}

