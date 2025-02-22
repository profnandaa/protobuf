// Protocol Buffers - Google's data interchange format
// Copyright 2023 Google Inc.  All rights reserved.
// https://developers.google.com/protocol-buffers/
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "google/protobuf/feature_resolver.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/absl_check.h"
#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "google/protobuf/cpp_features.pb.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/dynamic_message.h"
#include "google/protobuf/message.h"
#include "google/protobuf/reflection_ops.h"
#include "google/protobuf/text_format.h"

// Must be included last.
#include "google/protobuf/port_def.inc"

#define RETURN_IF_ERROR(expr)                                  \
  do {                                                         \
    const absl::Status _status = (expr);                       \
    if (PROTOBUF_PREDICT_FALSE(!_status.ok())) return _status; \
  } while (0)

namespace google {
namespace protobuf {
namespace {

template <typename... Args>
absl::Status Error(Args... args) {
  return absl::FailedPreconditionError(absl::StrCat(args...));
}

struct EditionsLessThan {
  bool operator()(absl::string_view a, absl::string_view b) const {
    std::vector<absl::string_view> as = absl::StrSplit(a, '.');
    std::vector<absl::string_view> bs = absl::StrSplit(b, '.');
    size_t min_size = std::min(as.size(), bs.size());
    for (size_t i = 0; i < min_size; ++i) {
      if (as[i].size() != bs[i].size()) {
        return as[i].size() < bs[i].size();
      } else if (as[i] != bs[i]) {
        return as[i] < bs[i];
      }
    }
    // Both strings are equal up until an extra element, which makes that string
    // more recent.
    return as.size() < bs.size();
  }
};

absl::Status ValidateDescriptor(const Descriptor& descriptor) {
  if (descriptor.oneof_decl_count() > 0) {
    return Error("Type ", descriptor.full_name(),
                 " contains unsupported oneof feature fields.");
  }
  for (int i = 0; i < descriptor.field_count(); ++i) {
    const FieldDescriptor& field = *descriptor.field(i);

    if (field.is_required()) {
      return Error("Feature field ", field.full_name(),
                   " is an unsupported required field.");
    }
    if (field.is_repeated()) {
      return Error("Feature field ", field.full_name(),
                   " is an unsupported repeated field.");
    }
    if (field.options().targets().empty()) {
      return Error("Feature field ", field.full_name(),
                   " has no target specified.");
    }
  }

  return absl::OkStatus();
}

absl::Status ValidateExtension(const Descriptor& feature_set,
                               const FieldDescriptor* extension) {
  if (extension == nullptr) {
    return Error("Unknown extension of ", feature_set.full_name(), ".");
  }

  if (extension->containing_type() != &feature_set) {
    return Error("Extension ", extension->full_name(),
                 " is not an extension of ", feature_set.full_name(), ".");
  }

  if (extension->message_type() == nullptr) {
    return Error("FeatureSet extension ", extension->full_name(),
                 " is not of message type.  Feature extensions should "
                 "always use messages to allow for evolution.");
  }

  if (extension->is_repeated()) {
    return Error(
        "Only singular features extensions are supported.  Found "
        "repeated extension ",
        extension->full_name());
  }

  if (extension->message_type()->extension_count() > 0 ||
      extension->message_type()->extension_range_count() > 0) {
    return Error("Nested extensions in feature extension ",
                 extension->full_name(), " are not supported.");
  }

  return absl::OkStatus();
}

void CollectEditions(const Descriptor& descriptor,
                     absl::string_view minimum_edition,
                     absl::string_view maximum_edition,
                     absl::btree_set<std::string, EditionsLessThan>& editions) {
  for (int i = 0; i < descriptor.field_count(); ++i) {
    for (const auto& def : descriptor.field(i)->options().edition_defaults()) {
      if (EditionsLessThan()(maximum_edition, def.edition())) continue;
      editions.insert(def.edition());
    }
  }
}

absl::Status FillDefaults(absl::string_view edition, Message& msg) {
  const Descriptor& descriptor = *msg.GetDescriptor();

  auto comparator = [](const FieldOptions::EditionDefault& a,
                       const FieldOptions::EditionDefault& b) {
    return EditionsLessThan()(a.edition(), b.edition());
  };
  FieldOptions::EditionDefault edition_lookup;
  edition_lookup.set_edition(edition);

  for (int i = 0; i < descriptor.field_count(); ++i) {
    const FieldDescriptor& field = *descriptor.field(i);

    msg.GetReflection()->ClearField(&msg, &field);
    ABSL_CHECK(!field.is_repeated());

    std::vector<FieldOptions::EditionDefault> defaults{
        field.options().edition_defaults().begin(),
        field.options().edition_defaults().end()};
    absl::c_sort(defaults, comparator);
    auto first_nonmatch =
        absl::c_upper_bound(defaults, edition_lookup, comparator);
    if (first_nonmatch == defaults.begin()) {
      return Error("No valid default found for edition ", edition,
                   " in feature field ", field.full_name());
    }

    if (field.cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
      for (auto it = defaults.begin(); it != first_nonmatch; ++it) {
        if (!TextFormat::MergeFromString(
                it->value(),
                msg.GetReflection()->MutableMessage(&msg, &field))) {
          return Error("Parsing error in edition_defaults for feature field ",
                       field.full_name(), ". Could not parse: ", it->value());
        }
      }
    } else {
      const std::string& def = std::prev(first_nonmatch)->value();
      if (!TextFormat::ParseFieldValueFromString(def, &field, &msg)) {
        return Error("Parsing error in edition_defaults for feature field ",
                     field.full_name(), ". Could not parse: ", def);
      }
    }
  }

  return absl::OkStatus();
}

absl::Status ValidateMergedFeatures(const Message& msg) {
  const Descriptor& descriptor = *msg.GetDescriptor();
  const Reflection& reflection = *msg.GetReflection();
  for (int i = 0; i < descriptor.field_count(); ++i) {
    const FieldDescriptor& field = *descriptor.field(i);
    // Validate enum features.
    if (field.enum_type() != nullptr) {
      ABSL_DCHECK(reflection.HasField(msg, &field));
      int int_value = reflection.GetEnumValue(msg, &field);
      const EnumValueDescriptor* value =
          field.enum_type()->FindValueByNumber(int_value);
      ABSL_DCHECK(value != nullptr);
      if (value->number() == 0) {
        return Error("Feature field ", field.full_name(),
                     " must resolve to a known value, found ", value->name());
      }
    }
  }

  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<FeatureSetDefaults> FeatureResolver::CompileDefaults(
    const Descriptor* feature_set,
    absl::Span<const FieldDescriptor* const> extensions,
    absl::string_view minimum_edition, absl::string_view maximum_edition) {
  // Find and validate the FeatureSet in the pool.
  if (feature_set == nullptr) {
    return Error(
        "Unable to find definition of google.protobuf.FeatureSet in descriptor pool.");
  }
  RETURN_IF_ERROR(ValidateDescriptor(*feature_set));

  // Collect and validate all the FeatureSet extensions.
  for (const auto* extension : extensions) {
    RETURN_IF_ERROR(ValidateExtension(*feature_set, extension));
    RETURN_IF_ERROR(ValidateDescriptor(*extension->message_type()));
  }

  // Collect all the editions with unique defaults.
  absl::btree_set<std::string, EditionsLessThan> editions;
  CollectEditions(*feature_set, minimum_edition, maximum_edition, editions);
  for (const auto* extension : extensions) {
    CollectEditions(*extension->message_type(), minimum_edition,
                    maximum_edition, editions);
  }

  // Fill the default spec.
  FeatureSetDefaults defaults;
  defaults.set_minimum_edition(minimum_edition);
  defaults.set_maximum_edition(maximum_edition);
  auto message_factory = absl::make_unique<DynamicMessageFactory>();
  for (const auto& edition : editions) {
    auto defaults_dynamic =
        absl::WrapUnique(message_factory->GetPrototype(feature_set)->New());
    RETURN_IF_ERROR(FillDefaults(edition, *defaults_dynamic));
    for (const auto* extension : extensions) {
      RETURN_IF_ERROR(FillDefaults(
          edition, *defaults_dynamic->GetReflection()->MutableMessage(
                       defaults_dynamic.get(), extension)));
    }
    auto* edition_defaults = defaults.mutable_defaults()->Add();
    edition_defaults->set_edition(edition);
    edition_defaults->mutable_features()->MergeFromString(
        defaults_dynamic->SerializeAsString());
  }
  return defaults;
}

absl::StatusOr<FeatureResolver> FeatureResolver::Create(
    absl::string_view edition, const FeatureSetDefaults& compiled_defaults) {
  if (EditionsLessThan()(edition, compiled_defaults.minimum_edition())) {
    return Error("Edition ", edition,
                 " is earlier than the minimum supported edition ",
                 compiled_defaults.minimum_edition());
  }
  if (EditionsLessThan()(compiled_defaults.maximum_edition(), edition)) {
    return Error("Edition ", edition,
                 " is later than the maximum supported edition ",
                 compiled_defaults.maximum_edition());
  }

  // Validate compiled defaults.
  absl::string_view prev_edition;
  for (const auto& edition_default : compiled_defaults.defaults()) {
    if (!prev_edition.empty()) {
      if (!EditionsLessThan()(prev_edition, edition_default.edition())) {
        return Error(
            "Feature set defaults are not strictly increasing.  Edition ",
            prev_edition, " is greater than or equal to edition ",
            edition_default.edition(), ".");
      }
    }
    prev_edition = edition_default.edition();
  }

  // Select the matching edition defaults.
  auto comparator = [](const auto& a, const auto& b) {
    return EditionsLessThan()(a.edition(), b.edition());
  };
  FeatureSetDefaults::FeatureSetEditionDefault search;
  search.set_edition(edition);
  auto first_nonmatch =
      absl::c_upper_bound(compiled_defaults.defaults(), search, comparator);
  if (first_nonmatch == compiled_defaults.defaults().begin()) {
    return Error("No valid default found for edition ", edition);
  }

  return FeatureResolver(std::prev(first_nonmatch)->features());
}

absl::StatusOr<FeatureSet> FeatureResolver::MergeFeatures(
    const FeatureSet& merged_parent, const FeatureSet& unmerged_child) const {
  FeatureSet merged = defaults_;
  merged.MergeFrom(merged_parent);
  merged.MergeFrom(unmerged_child);

  RETURN_IF_ERROR(ValidateMergedFeatures(merged));

  return merged;
}

}  // namespace protobuf
}  // namespace google

#include "google/protobuf/port_undef.inc"
