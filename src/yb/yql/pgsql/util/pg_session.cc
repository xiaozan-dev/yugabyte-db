//--------------------------------------------------------------------------------------------------
// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
//--------------------------------------------------------------------------------------------------

#include "yb/yql/pgsql/util/pg_session.h"
#include "yb/client/yb_op.h"

namespace yb {
namespace pgsql {

using std::make_shared;
using std::shared_ptr;
using std::string;
using namespace std::literals;  // NOLINT

using client::YBClient;
using client::YBSession;
using client::YBMetaDataCache;

using pgapi::ProtocolVersion;
using pgapi::StringInfo;
using pgapi::PGPort;
using pgapi::PGSend;

// TODO(neil) This should be derived from a GFLAGS.
static MonoDelta kSessionTimeout = 60s;

//--------------------------------------------------------------------------------------------------
// Class PgSession
//--------------------------------------------------------------------------------------------------

PgSession::PgSession(const PgEnv::SharedPtr& pg_env,
                     const StringInfo& postgres_packet,
                     ProtocolVersion protocol)
    : pg_env_(pg_env), session_(pg_env->NewSession()) {
  session_->SetTimeout(kSessionTimeout);

  // Test programs wouldn't have this packet.
  if (postgres_packet != nullptr && postgres_packet->size() > 0) {
    // Collect data from the packet and write to pgport_.
    pgport_.Initialize(postgres_packet, protocol);

    // Postgresql object pgsend_ is used to serializ data into rpc message.
    pgsend_.set_protocol_version(protocol);
  }

  // Check if the given database exist.
  const string& database_name = pgport_.database_name();
  if (!database_name.empty()) {
    Result<bool> has_db = pg_env_->ConnectDatabase(database_name);
    if (has_db.ok() && has_db.get()) {
      current_database_ = database_name;
    }
  }
}

CHECKED_STATUS PgSession::Apply(const std::shared_ptr<client::YBPgsqlOp>& op) {
  return session_->ApplyAndFlush(std::move(op));
}

}  // namespace pgsql
}  // namespace yb
