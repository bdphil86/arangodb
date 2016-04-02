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

#ifndef ARANGOD_V8_SERVER_APPLICATION_V8_H
#define ARANGOD_V8_SERVER_APPLICATION_V8_H 1

#include "ApplicationServer/ApplicationFeature.h"

#include <v8.h>

struct TRI_server_t;
struct TRI_vocbase_t;

namespace arangodb {
class Thread;

namespace aql {
class QueryRegistry;
}

namespace rest {
class ApplicationDispatcher;
class ApplicationScheduler;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief a buffer allocator used for V8
////////////////////////////////////////////////////////////////////////////////

class BufferAllocator : public v8::ArrayBuffer::Allocator {
 public:
  virtual void* Allocate(size_t length) {
    void* data = AllocateUninitialized(length);
    if (data != nullptr) {
      memset(data, 0, length);
    }
    return data;
  }
  virtual void* AllocateUninitialized(size_t length) { return malloc(length); }
  virtual void Free(void* data, size_t) {
    if (data != nullptr) {
      free(data);
    }
  }
};

////////////////////////////////////////////////////////////////////////////////
/// @brief application simple user and session management feature
////////////////////////////////////////////////////////////////////////////////

class ApplicationV8 : public rest::ApplicationFeature {
  ApplicationV8(ApplicationV8 const&) = delete;
  ApplicationV8& operator=(ApplicationV8 const&) = delete;

 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief V8 isolate and context
  //////////////////////////////////////////////////////////////////////////////

 public:
  ApplicationV8(TRI_server_t*, arangodb::aql::QueryRegistry*,
                rest::ApplicationScheduler*, rest::ApplicationDispatcher*);

  ~ApplicationV8();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief return the app-path
  //////////////////////////////////////////////////////////////////////////////

  std::string const& appPath() const { return _appPath; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief sets the concurrency
  //////////////////////////////////////////////////////////////////////////////

  void setConcurrency(size_t);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief sets the database
  //////////////////////////////////////////////////////////////////////////////

  void setVocbase(TRI_vocbase_t*);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief enters an context
  //////////////////////////////////////////////////////////////////////////////

  //////////////////////////////////////////////////////////////////////////////
  /// @brief exists an context
  //////////////////////////////////////////////////////////////////////////////

  //////////////////////////////////////////////////////////////////////////////
  /// @brief adds a global context function to be executed asap
  //////////////////////////////////////////////////////////////////////////////

  bool addGlobalContextMethod(std::string const&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief runs the garbage collection
  //////////////////////////////////////////////////////////////////////////////

  void collectGarbage();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief disables actions
  //////////////////////////////////////////////////////////////////////////////

  void disableActions();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief defines a boolean variable
  //////////////////////////////////////////////////////////////////////////////

  void defineBoolean(std::string const& name, bool value) {
    _definedBooleans[name] = value;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief defines a double constant
  //////////////////////////////////////////////////////////////////////////////

  void defineDouble(std::string const& name, double value) {
    _definedDoubles[name] = value;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief upgrades the database
  //////////////////////////////////////////////////////////////////////////////

  void upgradeDatabase(bool skip, bool perform);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief runs the version check
  //////////////////////////////////////////////////////////////////////////////

  void versionCheck();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief prepares the server
  //////////////////////////////////////////////////////////////////////////////

  void prepareServer();

 public:
  // void setupOptions(std::map<std::string, basics::ProgramOptionsDescription>&) override final;

  bool prepare() override final;

  bool prepare2() override final;

  bool start() override final;

  void close() override final;

  void stop() override final;

 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief determine which of the free contexts should be picked for the GC
  //////////////////////////////////////////////////////////////////////////////

  V8Context* pickFreeContextForGc();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief prepares a V8 instance
  //////////////////////////////////////////////////////////////////////////////

  bool prepareV8Instance(size_t i, bool useActions);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief prepares a V8 instance, multi-threaded version calling the above
  //////////////////////////////////////////////////////////////////////////////

  void prepareV8InstanceInThread(size_t i, bool useAction);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief prepares the V8 server
  //////////////////////////////////////////////////////////////////////////////

  void prepareV8Server(size_t i, std::string const& startupFile);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief shuts down a V8 instance
  //////////////////////////////////////////////////////////////////////////////

  void shutdownV8Instance(size_t i);

 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief server object
  //////////////////////////////////////////////////////////////////////////////

  TRI_server_t* _server;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief query registry object
  //////////////////////////////////////////////////////////////////////////////

  arangodb::aql::QueryRegistry* _queryRegistry;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief a buffer allocator for V8
  //////////////////////////////////////////////////////////////////////////////

  BufferAllocator _bufferAllocator;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief path to the directory containing the startup scripts
  ///
  /// `--javascript.startup-directory directory`
  ///
  /// Specifies the *directory* path to the JavaScript files used for
  /// bootstraping.
  //////////////////////////////////////////////////////////////////////////////

  std::string _startupPath;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief semicolon separated list of application directories
  /// `--javascript.app-path directory`
  ///
  /// Specifies the *directory* path where the applications are located.
  /// Multiple paths can be specified separated with commas.
  //////////////////////////////////////////////////////////////////////////////

  std::string _appPath;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief only used for backwards compatibility
  //////////////////////////////////////////////////////////////////////////////

  std::string _devAppPath;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief use actions
  //////////////////////////////////////////////////////////////////////////////

  bool _useActions;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief enables frontend version check
  //////////////////////////////////////////////////////////////////////////////

  bool _frontendVersionCheck;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief was docuBlock jsStartupGcInterval
  ////////////////////////////////////////////////////////////////////////////////

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief was docuBlock jsGcFrequency
  ////////////////////////////////////////////////////////////////////////////////

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief was docuBlock jsV8Options
  ////////////////////////////////////////////////////////////////////////////////

  std::string _v8Options;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief V8 startup loader
  //////////////////////////////////////////////////////////////////////////////

  JSLoader _startupLoader;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief system database
  //////////////////////////////////////////////////////////////////////////////

  TRI_vocbase_t* _vocbase;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief number of instances to create
  //////////////////////////////////////////////////////////////////////////////

  size_t _nrInstances;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief startup file
  //////////////////////////////////////////////////////////////////////////////

  std::string _startupFile;
};
}

#endif
