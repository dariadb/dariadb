#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE Main
#include <boost/test/unit_test.hpp>

#include <timedb.h>

BOOST_AUTO_TEST_CASE(UtilsEmpty) {
  BOOST_CHECK(timedb::utils::inInterval(1, 5, 1));
  BOOST_CHECK(timedb::utils::inInterval(1, 5, 2));
  BOOST_CHECK(timedb::utils::inInterval(1, 5, 5));
  BOOST_CHECK(!timedb::utils::inInterval(1, 5, 0));
  BOOST_CHECK(!timedb::utils::inInterval(0, 1, 2));
}

BOOST_AUTO_TEST_CASE(FileUtils) {
  std::string filename = "foo/bar/test.txt";
  BOOST_CHECK_EQUAL(timedb::utils::filename(filename), "test");
  BOOST_CHECK_EQUAL(timedb::utils::parent_path(filename), "foo/bar");
}