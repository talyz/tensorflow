/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/core/data/service/snapshot/snapshot_chunk_provider.h"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "tensorflow/core/data/service/snapshot/file_utils.h"
#include "tensorflow/core/data/service/snapshot/path_utils.h"
#include "tensorflow/core/framework/dataset.h"
#include "tsl/distributed_runtime/rpc/grpc_util.h"
#include "tsl/platform/env.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/path.h"
#include "tsl/platform/status_to_from_proto.h"
#include "tsl/platform/statusor.h"
#include "tsl/platform/tstring.h"
#include "tsl/protobuf/status.pb.h"

namespace tensorflow {
namespace data {
namespace {

constexpr char kChunksRead[] = "chunks_read";
constexpr absl::string_view kSetElementDelimiter = ",";

// Waits for a short period of time before retrying.
void Backoff(int num_retries, tsl::Env* env) {
  if (num_retries >= 1) {  // Does not backoff for the first try.
    env->SleepForMicroseconds(tsl::ComputeBackoffMicroseconds(num_retries - 1));
  }
}

std::string SetToString(const absl::flat_hash_set<std::string>& s) {
  return absl::StrJoin(s, kSetElementDelimiter);
}

absl::flat_hash_set<std::string> SetFromString(absl::string_view s) {
  if (s.empty()) {
    return {};
  }
  std::vector<std::string> split = absl::StrSplit(s, kSetElementDelimiter);
  return absl::flat_hash_set<std::string>(split.begin(), split.end());
}

}  // namespace

SnapshotChunkProvider::SnapshotChunkProvider(absl::string_view snapshot_path,
                                             tsl::Env* env)
    : snapshot_path_(snapshot_path), env_(env) {}

absl::StatusOr<std::optional<std::string>> SnapshotChunkProvider::GetNext()
    ABSL_LOCKS_EXCLUDED(mu_) {
  for (int num_retries = 0;; ++num_retries) {
    Backoff(num_retries, env_);
    absl::MutexLock l(&mu_);
    TF_RETURN_IF_ERROR(snapshot_state_.status);
    if (!chunks_unread_.empty()) {
      std::string next_chunk = *chunks_unread_.begin();
      chunks_read_.insert(next_chunk);
      chunks_unread_.erase(next_chunk);
      return tsl::io::JoinPath(CommittedChunksDirectory(snapshot_path_),
                               next_chunk);
    }
    if (snapshot_state_.snapshot_is_done) {
      return std::nullopt;
    }
    TF_RETURN_IF_ERROR(UpdateSnapshot());
  }
}

absl::Status SnapshotChunkProvider::UpdateSnapshot()
    ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
  // Reads the state files first then reads the chunks. If we read chunks before
  // reading the state files, the writer could write more chunks in between, and
  // we may see the DONE file but miss those final chunks.
  TF_ASSIGN_OR_RETURN(snapshot_state_, GetSnapshotState());
  TF_RETURN_IF_ERROR(snapshot_state_.status);
  TF_ASSIGN_OR_RETURN(std::vector<std::string> chunks, GetAvailableChunks());
  for (absl::string_view chunk : chunks) {
    if (!chunks_read_.contains(chunk)) {
      chunks_unread_.insert(std::string(chunk));
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<SnapshotChunkProvider::SnapshotState>
SnapshotChunkProvider::GetSnapshotState() {
  std::string error_file_path = SnapshotErrorFilePath(snapshot_path_);
  if (env_->FileExists(error_file_path).ok()) {
    StatusProto status_proto;
    TF_RETURN_IF_ERROR(ReadTextProto(env_, error_file_path, &status_proto));
    absl::Status status = tsl::StatusFromProto(status_proto);
    if (status.ok()) {
      return absl::InternalError(absl::StrCat(
          "Unexpected snapshot ERROR file contains an OK status at ",
          error_file_path, "."));
    }
    return SnapshotState(status);
  }
  return SnapshotState(
      env_->FileExists(SnapshotDoneFilePath(snapshot_path_)).ok());
}

absl::StatusOr<std::vector<std::string>>
SnapshotChunkProvider::GetAvailableChunks() {
  absl::StatusOr<std::vector<std::string>> status_or_chunks =
      GetChildren(CommittedChunksDirectory(snapshot_path_), env_);
  if (status_or_chunks.ok()) {
    return *std::move(status_or_chunks);
  } else if (absl::IsNotFound(status_or_chunks.status())) {
    return std::vector<std::string>{};
  }
  return status_or_chunks.status();
}

absl::Status SnapshotChunkProvider::Save(
    std::function<std::string(std::string)> full_name,
    IteratorStateWriter* writer) {
  absl::MutexLock l(&mu_);
  TF_RETURN_IF_ERROR(
      writer->WriteScalar(full_name(kChunksRead), SetToString(chunks_read_)));
  return absl::OkStatus();
}

absl::Status SnapshotChunkProvider::Restore(
    std::function<std::string(std::string)> full_name,
    IteratorStateReader* reader) {
  absl::MutexLock l(&mu_);
  tsl::tstring chunks_read;
  TF_RETURN_IF_ERROR(reader->ReadScalar(full_name(kChunksRead), &chunks_read));
  chunks_read_ = SetFromString(chunks_read);
  return UpdateSnapshot();
}

}  // namespace data
}  // namespace tensorflow
