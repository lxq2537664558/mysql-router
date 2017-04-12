/*
  Copyright (c) 2016, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#define MYSQL_ROUTER_LOG_DOMAIN "my_domain"

#ifdef _WINDOWS
#  define NOMINMAX
#  define getpid GetCurrentProcessId
#endif

#include "logger.h"
#include "mysql/harness/filesystem.h"
#include "include/magic.h"
#include "common.h"

////////////////////////////////////////
// Internal interfaces
#include "mysql/harness/loader.h"
#include "logging_registry.h"

////////////////////////////////////////
// Third-party include files
MYSQL_HARNESS_DISABLE_WARNINGS()
#include "gmock/gmock.h"
#include "gtest/gtest.h"
MYSQL_HARNESS_ENABLE_WARNINGS()

////////////////////////////////////////
// Standard include files
#include <stdexcept>

using mysql_harness::Path;
using mysql_harness::logging::FileHandler;
using mysql_harness::logging::LogLevel;
using mysql_harness::logging::Logger;
using mysql_harness::logging::Record;
using mysql_harness::logging::StreamHandler;
using mysql_harness::logging::create_logger;
using mysql_harness::logging::log_debug;
using mysql_harness::logging::log_error;
using mysql_harness::logging::log_info;
using mysql_harness::logging::log_warning;
using mysql_harness::logging::remove_logger;


using testing::Combine;
using testing::EndsWith;
using testing::Eq;
using testing::Ge;
using testing::Gt;
using testing::HasSubstr;
using testing::StartsWith;
using testing::Test;
using testing::Values;
using testing::ValuesIn;
using testing::WithParamInterface;

Path g_here;

TEST(TestBasic, Setup) {
  // Test that creating a logger will give it a name and a default log
  // level.
  Logger logger("my_module");
  EXPECT_EQ(logger.get_name(), "my_module");
  EXPECT_EQ(logger.get_level(), LogLevel::kWarning);

  logger.set_level(LogLevel::kDebug);
  EXPECT_EQ(logger.get_level(), LogLevel::kDebug);
}

class LoggingTest : public Test {
 public:
  // Here we are just testing that messages are written and in the
  // right format. We use kNotSet log level, which will print all
  // messages.
  Logger logger{"my_module", LogLevel::kNotSet};
};

TEST_F(LoggingTest, StreamHandler) {
  std::stringstream buffer;
  logger.add_handler(std::make_shared<StreamHandler>(buffer));

  // A bunch of casts to int for tellp to avoid C2666 in MSVC
  ASSERT_THAT((int)buffer.tellp(), Eq(0));
  logger.handle(Record{LogLevel::kInfo, getpid(), 0, "my_module", "Message"});
  EXPECT_THAT((int)buffer.tellp(), Gt(0));
  EXPECT_THAT(buffer.str(), StartsWith("1970-01-01 01:00:00 my_module INFO"));
  EXPECT_THAT(buffer.str(), EndsWith("Message\n"));
}

TEST_F(LoggingTest, FileHandler) {
  // Check that an exception is thrown for a path that cannot be
  // opened.
  EXPECT_ANY_THROW(FileHandler("/something/very/unlikely/to/exist"));

  // We do not use mktemp or friends since we want this to work on
  // Windows as well.
  Path log_file(g_here.join("log4-" + std::to_string(getpid()) + ".log"));
  logger.add_handler(std::make_shared<FileHandler>(log_file));

  // Log one record
  logger.handle(Record{LogLevel::kInfo, getpid(), 0, "my_module", "Message"});

  // Open and read the entire file into memory.
  std::vector<std::string> lines;
  {
    std::ifstream ifs_log(log_file.str());
    std::string line;
    while (std::getline(ifs_log, line))
      lines.push_back(line);
  }

  // We do the assertion here to ensure that we can do as many tests
  // as possible and report issues.
  ASSERT_THAT(lines.size(), Ge(1));

  // Check basic properties for the first line.
  EXPECT_THAT(lines.size(), Eq(1));
  EXPECT_THAT(lines.at(0), StartsWith("1970-01-01 01:00:00 my_module INFO"));
  EXPECT_THAT(lines.at(0), EndsWith("Message"));
}

TEST_F(LoggingTest, Messages) {
  std::stringstream buffer;
  logger.add_handler(std::make_shared<StreamHandler>(buffer));

  time_t now;
  time(&now);

  auto pid = getpid();

  auto check_message = [this, &buffer, now, pid](
      const std::string& message, LogLevel level,
      const std::string& level_str) {
    buffer.str("");
    ASSERT_THAT((int)buffer.tellp(), Eq(0));

    Record record{level, pid, now, "my_module", message};
    logger.handle(record);

    EXPECT_THAT(buffer.str(), EndsWith(message + "\n"));
    EXPECT_THAT(buffer.str(), HasSubstr(level_str));
  };

  check_message("Crazy noodles", LogLevel::kError, " ERROR ");
  check_message("Sloth tantrum", LogLevel::kWarning, " WARNING ");
  check_message("Russel's teapot", LogLevel::kInfo, " INFO ");
  check_message("Bugs galore", LogLevel::kDebug, " DEBUG ");
}

class LogLevelTest
    : public LoggingTest,
      public WithParamInterface<std::tuple<LogLevel, LogLevel>> {};

// Check that messages are not emitted when the level is set higher.
TEST_P(LogLevelTest, Level) {
  LogLevel logger_level = std::get<0>(GetParam());
  LogLevel handler_level = std::get<1>(GetParam());

  std::stringstream buffer;
  logger.add_handler(std::make_shared<StreamHandler>(buffer, handler_level));

  time_t now;
  time(&now);

  auto pid = getpid();

  // Set the log level of the logger.
  logger.set_level(logger_level);

  // Some handy shorthands for the levels as integers.
  const int min_level = std::min(static_cast<int>(logger_level),
                                 static_cast<int>(handler_level));
  const int max_level = static_cast<int>(LogLevel::kNotSet);

  // Loop over all levels below or equal to the provided level and
  // make sure that something is printed.
  for (int lvl = 0 ; lvl < min_level + 1 ; ++lvl) {
    buffer.str("");
    ASSERT_THAT((int)buffer.tellp(), Eq(0));
    logger.handle(Record{
        static_cast<LogLevel>(lvl), pid, now, "my_module", "Some message"});
    auto output = buffer.str();
    EXPECT_THAT(output.size(), Gt(0));
  }

  // Loop over all levels above the provided level and make sure
  // that nothing is printed.
  for (int lvl = min_level + 1 ; lvl < max_level ; ++lvl) {
    buffer.str("");
    ASSERT_THAT((int)buffer.tellp(), Eq(0));
    logger.handle(Record{
        static_cast<LogLevel>(lvl), pid, now, "my_module", "Some message"});
    auto output = buffer.str();
    EXPECT_THAT(output.size(), Eq(0));
  }

}

const LogLevel all_levels[]{
  LogLevel::kFatal, LogLevel::kError, LogLevel::kWarning, LogLevel::kInfo,
  LogLevel::kDebug
};

INSTANTIATE_TEST_CASE_P(CheckLogLevel, LogLevelTest,
                        Combine(ValuesIn(all_levels), ValuesIn(all_levels)));

////////////////////////////////////////////////////////////////
// Tests of the functional interface to the logger.
////////////////////////////////////////////////////////////////

TEST(FunctionalTest, CreateRemove) {
  // Test that creating two modules with different names succeed.
  EXPECT_NO_THROW(create_logger("my_first"));
  EXPECT_NO_THROW(create_logger("my_second"));

  // Test that trying to create two loggers for the same module fails.
  EXPECT_THROW(create_logger("my_first"), std::logic_error);
  EXPECT_THROW(create_logger("my_second"), std::logic_error);

  // Check that we can remove one of the modules and that removing it
  // a second time fails (mostly to get full coverage).
  ASSERT_NO_THROW(remove_logger("my_second"));
  EXPECT_THROW(remove_logger("my_second"), std::logic_error);

  // Clean up after the tests
  ASSERT_NO_THROW(remove_logger("my_first"));
}

void expect_no_log(void (*func)(const char*, ...), std::stringstream& buffer) {
  // Clear the buffer first and ensure that it was cleared to avoid
  // triggering other errors.
  buffer.str("");
  ASSERT_THAT((int)buffer.tellp(), Eq(0));

  // Write a simple message with a variable
  const int x = 3;
  func("Just a test of %d", x);

  // Log should be empty
  EXPECT_THAT((int)buffer.tellp(), Eq(0));
}

void expect_log(void (*func)(const char*, ...),
                std::stringstream& buffer, const char* kind) {
  // Clear the buffer first and ensure that it was cleared to avoid
  // triggering other errors.
  buffer.str("");
  ASSERT_THAT((int)buffer.tellp(), Eq(0));

  // Write a simple message with a variable
  const int x = 3;
  func("Just a test of %d", x);

  auto log = buffer.str();

  // Check that only one line was generated for the message. If the
  // message was sent to more than one logger, it could result in
  // multiple messages.
  EXPECT_THAT(std::count(log.begin(), log.end(), '\n'), Eq(1));

  // Check that the log contain the (expanded) message, the correct
  // indication (e.g., ERROR or WARNING), and the module name.
  EXPECT_THAT(log, HasSubstr("Just a test of 3"));
  EXPECT_THAT(log, HasSubstr(kind));
  EXPECT_THAT(log, HasSubstr(MYSQL_ROUTER_LOG_DOMAIN));
}

TEST(FunctionalTest, Handlers) {
  // The loader create these modules during start, so tests of the
  // logger that involve the loader are inside the loader unit
  // test. Here we instead call these functions directly.
  ASSERT_NO_THROW(create_logger(MYSQL_ROUTER_LOG_DOMAIN));

  std::stringstream buffer;
  auto handler = std::make_shared<StreamHandler>(buffer);
  register_handler(handler);

  set_log_level(LogLevel::kDebug);
  expect_log(log_error, buffer, "ERROR");
  expect_log(log_warning, buffer, "WARNING");
  expect_log(log_info, buffer, "INFO");
  expect_log(log_debug, buffer, "DEBUG");

  set_log_level(LogLevel::kError);
  expect_log(log_error, buffer, "ERROR");
  expect_no_log(log_warning, buffer);
  expect_no_log(log_info, buffer);
  expect_no_log(log_debug, buffer);

  set_log_level(LogLevel::kWarning);
  expect_log(log_error, buffer, "ERROR");
  expect_log(log_warning, buffer, "WARNING");
  expect_no_log(log_info, buffer);
  expect_no_log(log_debug, buffer);

  // Check that nothing is logged when the handler is unregistered.
  unregister_handler(handler);
  set_log_level(LogLevel::kNotSet);
  expect_no_log(log_error, buffer);
  expect_no_log(log_warning, buffer);
  expect_no_log(log_info, buffer);
  expect_no_log(log_debug, buffer);
}

int main(int argc, char *argv[]) {
  g_here = Path(argv[0]).dirname();

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
