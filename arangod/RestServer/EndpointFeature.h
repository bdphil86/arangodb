////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_HTTP_SERVER_ENDPOINT_FEATURE_H
#define ARANGOD_HTTP_SERVER_ENDPOINT_FEATURE_H 1

#include "Basics/Common.h"

#include "ApplicationFeatures/ApplicationFeature.h"
#include "ApplicationServer/ApplicationFeature.h"
#include "Rest/EndpointList.h"

namespace arangodb {
namespace rest {
class HttpServer;
class HttpsServer;
}

class EndpointFeature final : public application_features::ApplicationFeature {
 public:
  explicit EndpointFeature(application_features::ApplicationServer* server);

 public:
  void collectOptions(std::shared_ptr<options::ProgramOptions>) override;
  void validateOptions(std::shared_ptr<options::ProgramOptions>) override;
  void prepare() override;
  void start() override;
  void stop() override;

 private:
  void buildEndpointLists();
  void buildServers();

 private:
  std::vector<std::string> _endpoints;
  bool _reuseAddress;
  uint64_t _backlogSize;
  double _keepAliveTimeout;

 private:
  rest::EndpointList _endpointList;
  std::vector<rest::HttpServer*> _servers;
};
}

#endif
