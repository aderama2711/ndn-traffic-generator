/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2014-2025, Arizona Board of Regents.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Jerald Paul Abraham <jeraldabraham@email.arizona.edu>
 */

#include "util.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/face.hpp>
#include <ndn-cxx/interest.hpp>
#include <ndn-cxx/lp/tags.hpp>
#include <ndn-cxx/util/random.hpp>
#include <ndn-cxx/util/time.hpp>

#include <chrono>
#include <limits>
#include <optional>
#include <sstream>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/core/noncopyable.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>

//header for zipf distribution
#include "discrete_distribution.h"
#include "discrete_distribution_ii.h"
#include "zipf-mandelbrot.h"

//header for custom log
#include <iostream>
using std::cerr;
using std::endl;
#include <fstream>
using std::ofstream;
using std::to_string;
#include <cstdlib>

using namespace std::chrono_literals;

// default configuration
int mode = 1, nprefix = 0, total_percentage=0;
float zipffactor = 0.8, qvalue = 3; 

void mode_selection(int x){
  mode = x;
}

void qvalue_assign(float x){
  qvalue = x;
}

void zipffactor_assign(float x){
  zipffactor = x;
}

namespace ndntg {

using namespace ndn::time_literals;
using namespace std::string_literals;
namespace time = ndn::time;

class NdnTrafficClient : boost::noncopyable
{
public:
  explicit
  NdnTrafficClient(std::string configFile)
    : m_configurationFile(std::move(configFile))
  {
  }

  void
  setMaximumInterests(uint64_t maxInterests)
  {
    m_nMaximumInterests = maxInterests;
  }

  void
  setInterestInterval(std::chrono::milliseconds interval)
  {
    BOOST_ASSERT(interval > 0ms);
    m_interestInterval = interval;
  }

  void
  setTimestampFormat(std::string format)
  {
    m_timestampFormat = std::move(format);
  }

  void
  setQuietLogging()
  {
    m_wantQuiet = true;
  }

  void
  setVerboseLogging()
  {
    m_wantVerbose = true;
  }

  int
  run()
  {
    m_logger.initialize(std::to_string(ndn::random::generateWord32()), m_timestampFormat);

    if (!readConfigurationFile(m_configurationFile, m_trafficPatterns, m_logger)) {
      return 2;
    }

    if (!checkTrafficPatternCorrectness()) {
      m_logger.log("ERROR: Traffic configuration provided is not proper", false, true);
      return 2;
    }

    m_logger.log("Traffic configuration file processing completed\n", true, false);
    for (std::size_t i = 0; i < m_trafficPatterns.size(); i++) {
      m_logger.log("Traffic Pattern Type #" + std::to_string(i + 1), false, false);
      m_trafficPatterns[i].printTrafficConfiguration(m_logger);
      m_logger.log("", false, false);
    }

    if (m_nMaximumInterests == 0) {
      logStatistics();
      return 0;
    }

    m_signalSet.async_wait([this] (auto&&...) { stop(); });

    boost::asio::steady_timer timer(m_io, m_interestInterval);
    timer.async_wait([this, &timer] (auto&&...) { generateTraffic(timer); });

    try {
      m_face.processEvents();
      return m_hasError ? 1 : 0;
    }
    catch (const std::exception& e) {
      m_logger.log("ERROR: "s + e.what(), true, true);
      m_io.stop();
      return 1;
    }
  }

private:
  class InterestTrafficConfiguration
  {
  public:
    void
    printTrafficConfiguration(Logger& logger) const
    {
      std::ostringstream os;

      os << "TrafficPercentage=" << m_trafficPercentage << ", ";
      os << "Name=" << m_name << ", ";
      if (m_nameAppendBytes) {
        os << "NameAppendBytes=" << *m_nameAppendBytes << ", ";
      }
      if (m_nameAppendSeqNum) {
        os << "NameAppendSequenceNumber=" << *m_nameAppendSeqNum << ", ";
      }
      if (m_canBePrefix) {
        os << "CanBePrefix=" << m_canBePrefix << ", ";
      }
      if (m_mustBeFresh) {
        os << "MustBeFresh=" << m_mustBeFresh << ", ";
      }
      if (m_nonceDuplicationPercentage > 0) {
        os << "NonceDuplicationPercentage=" << m_nonceDuplicationPercentage << ", ";
      }
      if (m_interestLifetime >= 0_ms) {
        os << "InterestLifetime=" << m_interestLifetime.count() << ", ";
      }
      if (m_nextHopFaceId > 0) {
        os << "NextHopFaceId=" << m_nextHopFaceId << ", ";
      }
      if (m_expectedContent) {
        os << "ExpectedContent=" << *m_expectedContent << ", ";
      }

      auto str = os.str();
      str = str.substr(0, str.length() - 2); // remove suffix ", "
      logger.log(str, false, false);
    }

    bool
    parseConfigurationLine(const std::string& line, Logger& logger, int lineNumber)
    {
      std::string parameter, value;
      if (!extractParameterAndValue(line, parameter, value)) {
        logger.log("Line " + std::to_string(lineNumber) + " - Invalid syntax: " + line,
                   false, true);
        return false;
      }

      if (parameter == "TrafficPercentage") {
        m_trafficPercentage = std::stod(value);
        if (!std::isfinite(m_trafficPercentage)) {
          logger.log("Line " + std::to_string(lineNumber) +
                     " - TrafficPercentage must be a finite floating point value", false, true);
          return false;
        }
      }
      else if (parameter == "Name") {
        m_name = value;

        //calculate number of prefix
        nprefix++;
      }
      else if (parameter == "Name") {
        m_name = value;
      }
      else if (parameter == "NameAppendBytes") {
        m_nameAppendBytes = std::stoul(value);
      }
      else if (parameter == "NameAppendSequenceNumber") {
        m_nameAppendSeqNum = std::stoull(value);
      }
      else if (parameter == "CanBePrefix") {
        m_canBePrefix = parseBoolean(value);
      }
      else if (parameter == "MustBeFresh") {
        m_mustBeFresh = parseBoolean(value);
      }
      else if (parameter == "NonceDuplicationPercentage") {
        m_nonceDuplicationPercentage = std::stoul(value);
      }
      else if (parameter == "InterestLifetime") {
        m_interestLifetime = time::milliseconds(std::stoul(value));
      }
      else if (parameter == "NextHopFaceId") {
        m_nextHopFaceId = std::stoull(value);
      }
      else if (parameter == "ExpectedContent") {
        m_expectedContent = value;
      }
      else {
        logger.log("Line " + std::to_string(lineNumber) + " - Ignoring unknown parameter: " + parameter,
                   false, true);
      }
      return true;
    }

    bool
    checkTrafficDetailCorrectness() const
    {
      return true;
    }

  public:
    double m_trafficPercentage = 0.0;
    std::string m_name;
    std::optional<std::size_t> m_nameAppendBytes;
    std::optional<uint64_t> m_nameAppendSeqNum;
    bool m_canBePrefix = false;
    bool m_mustBeFresh = false;
    unsigned m_nonceDuplicationPercentage = 0;
    time::milliseconds m_interestLifetime = -1_ms;
    uint64_t m_nextHopFaceId = 0;
    std::optional<std::string> m_expectedContent;

    uint64_t m_nInterestsSent = 0;
    uint64_t m_nInterestsReceived = 0;
    uint64_t m_nNacks = 0;
    uint64_t m_nContentInconsistencies = 0;

    // RTT is stored as milliseconds with fractional sub-milliseconds precision
    double m_minimumInterestRoundTripTime = std::numeric_limits<double>::max();
    double m_maximumInterestRoundTripTime = 0;
    double m_totalInterestRoundTripTime = 0;
  };

  void
  logStatistics()
  {
    using std::to_string;

    m_logger.log("\n\n== Traffic Report ==\n", false, true);
    m_logger.log("Total Traffic Pattern Types = " + to_string(m_trafficPatterns.size()), false, true);
    m_logger.log("Total Interests Sent        = " + to_string(m_nInterestsSent), false, true);
    m_logger.log("Total Responses Received    = " + to_string(m_nInterestsReceived), false, true);
    m_logger.log("Total Nacks Received        = " + to_string(m_nNacks), false, true);

    double loss = 0.0;
    if (m_nInterestsSent > 0) {
      loss = (m_nInterestsSent - m_nInterestsReceived) * 100.0 / m_nInterestsSent;
    }
    m_logger.log("Total Interest Loss         = " + to_string(loss) + "%", false, true);

    double average = 0.0;
    double inconsistency = 0.0;
    if (m_nInterestsReceived > 0) {
      average = m_totalInterestRoundTripTime / m_nInterestsReceived;
      inconsistency = m_nContentInconsistencies * 100.0 / m_nInterestsReceived;
    }
    m_logger.log("Total Data Inconsistency    = " + to_string(inconsistency) + "%", false, true);
    m_logger.log("Total Round Trip Time       = " + to_string(m_totalInterestRoundTripTime) + "ms", false, true);
    m_logger.log("Average Round Trip Time     = " + to_string(average) + "ms\n", false, true);

    //generate log.csv for overall status
    ofstream outdata;
    outdata.open("log.csv");
    if( !outdata){
      cerr << "Error FILE" << endl;
    }

    outdata << "PatternID,InterestSent,ResponsesReceived,Nacks,InterestLoss(%),Inconsistency(%),TotalRTT(ms),AverageRTT(ms)" << endl;
    outdata << "Overall," << to_string(m_nInterestsSent) << "," << to_string(m_nInterestsReceived) << "," << to_string(m_nNacks) << "," << to_string(loss) << "," << to_string(inconsistency) << "," << to_string(m_totalInterestRoundTripTime) << "," << to_string(average) << "," << endl;

    for (std::size_t patternId = 0; patternId < m_trafficPatterns.size(); patternId++) {
      const auto& pattern = m_trafficPatterns[patternId];

      m_logger.log("Traffic Pattern Type #" + to_string(patternId + 1), false, true);
      pattern.printTrafficConfiguration(m_logger);
      m_logger.log("Total Interests Sent        = " + to_string(pattern.m_nInterestsSent), false, true);
      m_logger.log("Total Responses Received    = " + to_string(pattern.m_nInterestsReceived), false, true);
      m_logger.log("Total Nacks Received        = " + to_string(pattern.m_nNacks), false, true);

      loss = 0.0;
      if (pattern.m_nInterestsSent > 0) {
        loss = (pattern.m_nInterestsSent - pattern.m_nInterestsReceived) * 100.0 / pattern.m_nInterestsSent;
      }
      m_logger.log("Total Interest Loss         = " + to_string(loss) + "%", false, true);

      average = 0.0;
      inconsistency = 0.0;
      if (pattern.m_nInterestsReceived > 0) {
        average = pattern.m_totalInterestRoundTripTime / pattern.m_nInterestsReceived;
        inconsistency = pattern.m_nContentInconsistencies * 100.0 / pattern.m_nInterestsReceived;
      }
      m_logger.log("Total Data Inconsistency    = " + to_string(inconsistency) + "%", false, true);
      m_logger.log("Total Round Trip Time       = " +
                   to_string(pattern.m_totalInterestRoundTripTime) + "ms", false, true);
      m_logger.log("Average Round Trip Time     = " + to_string(average) + "ms\n", false, true);

      //per traffic log
      outdata << to_string(patternId + 1) << "," << to_string(m_trafficPatterns[patternId].m_nInterestsSent) << "," << to_string(m_trafficPatterns[patternId].m_nInterestsReceived) << "," << to_string(m_trafficPatterns[patternId].m_nNacks) << "," << to_string(loss) << "," << to_string(inconsistency) << "," << to_string(m_trafficPatterns[patternId].m_totalInterestRoundTripTime) << "," << to_string(average) << endl;     
    }
    outdata.close();
  }

  bool
  checkTrafficPatternCorrectness() const
  {
    // TODO
    return true;
  }

  uint32_t
  getNewNonce()
  {
    if (m_nonces.size() >= 1000)
      m_nonces.clear();

    auto randomNonce = ndn::random::generateWord32();
    while (std::find(m_nonces.begin(), m_nonces.end(), randomNonce) != m_nonces.end())
      randomNonce = ndn::random::generateWord32();

    m_nonces.push_back(randomNonce);
    return randomNonce;
  }

  uint32_t
  getOldNonce()
  {
    if (m_nonces.empty())
      return getNewNonce();

    std::uniform_int_distribution<std::size_t> dist(0, m_nonces.size() - 1);
    return m_nonces[dist(ndn::random::getRandomNumberEngine())];
  }

  static auto
  generateRandomNameComponent(std::size_t length)
  {
    // per ISO C++ std, cannot instantiate uniform_int_distribution with uint8_t
    static std::uniform_int_distribution<unsigned> dist(std::numeric_limits<uint8_t>::min(),
                                                        std::numeric_limits<uint8_t>::max());

    ndn::Buffer buf(length);
    for (std::size_t i = 0; i < length; i++) {
      buf[i] = static_cast<uint8_t>(dist(ndn::random::getRandomNumberEngine()));
    }
    return ndn::name::Component(buf);
  }

  auto
  prepareInterest(std::size_t patternId)
  {
    ndn::Interest interest;
    auto& pattern = m_trafficPatterns[patternId];

    ndn::Name name(pattern.m_name);
    if (pattern.m_nameAppendBytes > 0) {
      name.append(generateRandomNameComponent(*pattern.m_nameAppendBytes));
    }
    if (pattern.m_nameAppendSeqNum) {
      auto seqNum = *pattern.m_nameAppendSeqNum;
      name.appendSequenceNumber(seqNum);
      pattern.m_nameAppendSeqNum = seqNum + 1;
    }
    interest.setName(name);

    interest.setCanBePrefix(pattern.m_canBePrefix);
    interest.setMustBeFresh(pattern.m_mustBeFresh);

    static std::uniform_int_distribution<unsigned> duplicateNonceDist(1, 100);
    if (duplicateNonceDist(ndn::random::getRandomNumberEngine()) <= pattern.m_nonceDuplicationPercentage)
      interest.setNonce(getOldNonce());
    else
      interest.setNonce(getNewNonce());

    if (pattern.m_interestLifetime >= 0_ms)
      interest.setInterestLifetime(pattern.m_interestLifetime);

    if (pattern.m_nextHopFaceId > 0)
      interest.setTag(std::make_shared<ndn::lp::NextHopFaceIdTag>(pattern.m_nextHopFaceId));

    return interest;
  }

  void
  onData(const ndn::Interest&, const ndn::Data& data, int globalRef, int localRef,
         std::size_t patternId, const time::steady_clock::time_point& sentTime)
  {
    auto now = time::steady_clock::now();
    auto logLine = "Data Received      - PatternType=" + std::to_string(patternId + 1) +
                   ", GlobalID=" + std::to_string(globalRef) +
                   ", LocalID=" + std::to_string(localRef) +
                   ", Name=" + data.getName().toUri();

    m_nInterestsReceived++;
    m_trafficPatterns[patternId].m_nInterestsReceived++;

    if (m_trafficPatterns[patternId].m_expectedContent) {
      std::string receivedContent = readString(data.getContent());
      if (receivedContent != *m_trafficPatterns[patternId].m_expectedContent) {
        m_nContentInconsistencies++;
        m_trafficPatterns[patternId].m_nContentInconsistencies++;
        logLine += ", IsConsistent=No";
      }
      else {
        logLine += ", IsConsistent=Yes";
      }
    }
    else {
      logLine += ", IsConsistent=NotChecked";
    }
    if (!m_wantQuiet) {
      m_logger.log(logLine, true, false);
    }

    double rtt = time::duration_cast<time::nanoseconds>(now - sentTime).count() / 1e6;
    if (m_wantVerbose) {
      auto rttLine = "RTT                - Name=" + data.getName().toUri() +
                     ", RTT=" + std::to_string(rtt) + "ms";
      m_logger.log(rttLine, true, false);
    }
    if (m_minimumInterestRoundTripTime > rtt)
      m_minimumInterestRoundTripTime = rtt;
    if (m_maximumInterestRoundTripTime < rtt)
      m_maximumInterestRoundTripTime = rtt;
    if (m_trafficPatterns[patternId].m_minimumInterestRoundTripTime > rtt)
      m_trafficPatterns[patternId].m_minimumInterestRoundTripTime = rtt;
    if (m_trafficPatterns[patternId].m_maximumInterestRoundTripTime < rtt)
      m_trafficPatterns[patternId].m_maximumInterestRoundTripTime = rtt;
    m_totalInterestRoundTripTime += rtt;
    m_trafficPatterns[patternId].m_totalInterestRoundTripTime += rtt;

    if (m_nMaximumInterests == globalRef) {
      stop();
    }
  }

  void
  onNack(const ndn::Interest& interest, const ndn::lp::Nack& nack,
         int globalRef, int localRef, std::size_t patternId)
  {
    auto logLine = "Interest Nack'd    - PatternType=" + std::to_string(patternId + 1) +
                   ", GlobalID=" + std::to_string(globalRef) +
                   ", LocalID=" + std::to_string(localRef) +
                   ", Name=" + interest.getName().toUri() +
                   ", NackReason=" + boost::lexical_cast<std::string>(nack.getReason());
    m_logger.log(logLine, true, false);

    m_nNacks++;
    m_trafficPatterns[patternId].m_nNacks++;

    if (m_nMaximumInterests == globalRef) {
      stop();
    }
  }

  void
  onTimeout(const ndn::Interest& interest, int globalRef, int localRef, std::size_t patternId)
  {
    auto logLine = "Interest Timed Out - PatternType=" + std::to_string(patternId + 1) +
                   ", GlobalID=" + std::to_string(globalRef) +
                   ", LocalID=" + std::to_string(localRef) +
                   ", Name=" + interest.getName().toUri();
    m_logger.log(logLine, true, false);

    if (m_nMaximumInterests == globalRef) {
      stop();
    }
  }

  void
  generateTraffic(boost::asio::steady_timer& timer)
  {
    if (m_nMaximumInterests && m_nInterestsSent >= *m_nMaximumInterests) {
      return;
    }

    double trafficKey;

    if (mode == 1){
    static std::uniform_real_distribution<> trafficDist(std::numeric_limits<double>::min(), 100.0);
    trafficKey = trafficDist(ndn::random::getRandomNumberEngine());
    }

    if (mode == 2){
      static rng::zipf_mandelbrot_distribution<rng::discrete_distribution_30bit,int> trafficDistZipf(zipffactor, qvalue, nprefix);
      trafficKey = trafficDistZipf(ndn::random::getRandomNumberEngine());
      trafficKey -= qvalue;
    }

    double cumulativePercentage = 0.0;
    std::size_t patternId = 0;
    for (; patternId < m_trafficPatterns.size(); patternId++) {
      auto& pattern = m_trafficPatterns[patternId];
      cumulativePercentage += pattern.m_trafficPercentage;
      if (trafficKey <= cumulativePercentage) {
        m_nInterestsSent++;
        pattern.m_nInterestsSent++;
        auto interest = prepareInterest(patternId);
        try {
          int globalRef = m_nInterestsSent;
          int localRef = pattern.m_nInterestsSent;
          m_face.expressInterest(interest,
            [=, now = time::steady_clock::now()] (auto&&... args) {
              onData(std::forward<decltype(args)>(args)..., globalRef, localRef, patternId, now);
            },
            [=] (auto&&... args) {
              onNack(std::forward<decltype(args)>(args)..., globalRef, localRef, patternId);
            },
            [=] (auto&&... args) {
              onTimeout(std::forward<decltype(args)>(args)..., globalRef, localRef, patternId);
            });

          if (!m_wantQuiet) {
            auto logLine = "Sending Interest   - PatternType=" + std::to_string(patternId + 1) +
                           ", GlobalID=" + std::to_string(m_nInterestsSent) +
                           ", LocalID=" + std::to_string(pattern.m_nInterestsSent) +
                           ", Name=" + interest.getName().toUri();
            m_logger.log(logLine, true, false);
          }

          timer.expires_at(timer.expiry() + m_interestInterval);
          timer.async_wait([this, &timer] (auto&&...) { generateTraffic(timer); });
        }
        catch (const std::exception& e) {
          m_logger.log("ERROR: "s + e.what(), true, true);
        }
        break;
      }
    }

    if (patternId == m_trafficPatterns.size()) {
      timer.expires_at(timer.expiry() + m_interestInterval);
      timer.async_wait([this, &timer] (auto&&...) { generateTraffic(timer); });
    }
  }

  void
  stop()
  {
    if (m_nContentInconsistencies > 0 || m_nInterestsSent != m_nInterestsReceived) {
      m_hasError = true;
    }

    logStatistics();
    m_face.shutdown();
    m_io.stop();
  }

private:
  Logger m_logger{"NdnTrafficClient"};
  boost::asio::io_context m_io;
  boost::asio::signal_set m_signalSet{m_io, SIGINT, SIGTERM};
  ndn::Face m_face{m_io};

  std::string m_configurationFile;
  std::string m_timestampFormat;
  std::optional<uint64_t> m_nMaximumInterests;
  std::chrono::milliseconds m_interestInterval{1s};

  std::vector<InterestTrafficConfiguration> m_trafficPatterns;
  std::vector<uint32_t> m_nonces;
  uint64_t m_nInterestsSent = 0;
  uint64_t m_nInterestsReceived = 0;
  uint64_t m_nNacks = 0;
  uint64_t m_nContentInconsistencies = 0;

  // RTT is stored as milliseconds with fractional sub-milliseconds precision
  double m_minimumInterestRoundTripTime = std::numeric_limits<double>::max();
  double m_maximumInterestRoundTripTime = 0;
  double m_totalInterestRoundTripTime = 0;

  bool m_wantQuiet = false;
  bool m_wantVerbose = false;
  bool m_hasError = false;
};

} // namespace ndntg

namespace po = boost::program_options;

static void
usage(std::ostream& os, std::string_view programName, const po::options_description& desc)
{
  os << "Usage: " << programName << " [options] <Traffic_Configuration_File>\n"
     << "\n"
     << "Generate Interest traffic as per provided Traffic_Configuration_File.\n"
     << "Interests are continuously generated unless a total number is specified.\n"
     << "Set the environment variable NDN_TRAFFIC_LOGFOLDER to redirect output to a log file.\n"
     << "\n"
     << "Modification :\n"
     << "+ Zipf-Mandelbrot Distribution\n"
     << "Warning\n"
     << "- Please set all traffic percentage to 1\n"
     << "\n"
     << desc;
}

int
main(int argc, char* argv[])
{
  std::string configFile;
  std::string timestampFormat;

  po::options_description visibleOptions("Options");
  visibleOptions.add_options()
    ("help,h",      "print this help message and exit")
    ("count,c",     po::value<int64_t>(), "total number of Interests to be generated")
    ("interval,i",  po::value<std::chrono::milliseconds::rep>()->default_value(1000),
                    "Interest generation interval in milliseconds")
    ("timestamp-format,t", po::value<std::string>(&timestampFormat), "format string for timestamp output")
    ("quiet,q",     po::bool_switch(), "turn off logging of Interest generation and Data reception")
    ("verbose,v",   po::bool_switch(), "log additional per-packet information")
    ("mode,m",      po::value<int>(), "(int) Distribution choice : 1. Uniform, 2. Zipf-Mandelbrot; Default = Uniform")
    ("zipffactor,z",po::value<float>(), "(float) Used in Zipf-Mandelbrot as s value, default = 0.5")
    ("qvalue,v",   po::value<float>(), "(float) Used in Zipf-Mandelbrot as q value, default = 0")
    ;

  po::options_description hiddenOptions;
  hiddenOptions.add_options()
    ("config-file", po::value<std::string>(&configFile))
    ;

  po::positional_options_description posOptions;
  posOptions.add("config-file", -1);

  po::options_description allOptions;
  allOptions.add(visibleOptions).add(hiddenOptions);

  po::variables_map vm;
  try {
    po::store(po::command_line_parser(argc, argv).options(allOptions).positional(posOptions).run(), vm);
    po::notify(vm);
  }
  catch (const po::error& e) {
    std::cerr << "ERROR: " << e.what() << std::endl;
    return 2;
  }
  catch (const boost::bad_any_cast& e) {
    std::cerr << "ERROR: " << e.what() << std::endl;
    return 2;
  }

  if (vm.count("mode") > 0) {
    int y = vm["mode"].as<int>();
    if (y != 1 && y != 2){
      return 2;
    }
    mode_selection(y);
  }

  if (vm.count("zipffactor") > 0) {
    float y = vm["zipffactor"].as<float>();
    zipffactor_assign(y);
  }

  if (vm.count("qvalue") > 0) {
    float y = vm["qvalue"].as<float>();
    qvalue_assign(y);
  }

  if (vm.count("help") > 0) {
    usage(std::cout, argv[0], visibleOptions);
    return 0;
  }

  if (configFile.empty()) {
    usage(std::cerr, argv[0], visibleOptions);
    return 2;
  }

  ndntg::NdnTrafficClient client(std::move(configFile));

  if (vm.count("count") > 0) {
    auto count = vm["count"].as<int64_t>();
    if (count < 0) {
      std::cerr << "ERROR: the argument for option '--count' cannot be negative\n";
      return 2;
    }
    client.setMaximumInterests(static_cast<uint64_t>(count));
  }

  if (vm.count("interval") > 0) {
    std::chrono::milliseconds interval(vm["interval"].as<std::chrono::milliseconds::rep>());
    if (interval <= 0ms) {
      std::cerr << "ERROR: the argument for option '--interval' must be positive\n";
      return 2;
    }
    client.setInterestInterval(interval);
  }

  if (!timestampFormat.empty()) {
    client.setTimestampFormat(std::move(timestampFormat));
  }

  if (vm["quiet"].as<bool>()) {
    if (vm["verbose"].as<bool>()) {
      std::cerr << "ERROR: cannot set both '--quiet' and '--verbose'\n";
      return 2;
    }
    client.setQuietLogging();
  }

  if (vm["verbose"].as<bool>()) {
    client.setVerboseLogging();
  }

  return client.run();
}
