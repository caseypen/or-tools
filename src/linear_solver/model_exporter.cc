// Copyright 2010-2014 Google
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "linear_solver/model_exporter.h"

#include <cmath>
#include <limits>

#include "base/commandlineflags.h"
#include "base/integral_types.h"
#include "base/logging.h"
#include "base/stringprintf.h"
#include "base/join.h"
#include "base/strutil.h"
#include "base/map_util.h"
#include "linear_solver/linear_solver.pb.h"
#include "util/fp_utils.h"

DEFINE_bool(lp_shows_unused_variables, false,
            "Decides wether variable unused in the objective and constraints"
            " are shown when exported to a file using the lp format.");

DEFINE_int32(lp_max_line_length, 10000,
             "Maximum line length in exported .lp files. The default was chosen"
             " so that SCIP can read the files.");

DEFINE_bool(lp_log_invalid_name, false,
            "Whether to log invalid variable and contraint names.");

namespace operations_research {

MPModelProtoExporter::MPModelProtoExporter(const MPModelProto& proto)
    : proto_(proto),
      var_id_to_index_map_(),
      num_integer_variables_(0),
      num_binary_variables_(0),
      num_continuous_variables_(0),
      num_digits_for_variables_(0),
      num_digits_for_constraints_(0),
      current_mps_column_(0),
      use_fixed_mps_format_(false),
      use_obfuscated_names_(false),
      setup_done_(false) {}

// Note(user): This method is static. It is also used by MPSolver.
bool MPModelProtoExporter::CheckNameValidity(const std::string& name) {
  if (name.empty()) {
    LOG_IF(WARNING, FLAGS_lp_log_invalid_name)
        << "CheckNameValidity() should not be passed an empty name.";
    return false;
  }
  // Allow names that conform to the LP and MPS format.
  const int kMaxNameLength = 255;
  if (name.size() > kMaxNameLength) {
    LOG_IF(WARNING, FLAGS_lp_log_invalid_name)
        << "Invalid name " << name << ": length > " << kMaxNameLength << "."
        << " Will be unable to write model to file.";
    return false;
  }
  const std::string kForbiddenChars = " +-*/<>=:\\";
  if (name.find_first_of(kForbiddenChars) != std::string::npos) {
    LOG_IF(WARNING, FLAGS_lp_log_invalid_name)
        << "Invalid name " << name
        << " contains forbidden character: " << kForbiddenChars << " or space."
        << " Will be unable to write model to file.";
    return false;
  }
  const std::string kForbiddenFirstChars = "$.0123456789";
  if (kForbiddenFirstChars.find(name[0]) != std::string::npos) {
    LOG_IF(WARNING, FLAGS_lp_log_invalid_name)
        << "Invalid name " << name
        << ". First character is one of: " << kForbiddenFirstChars
        << " Will be unable to write model to file.";
    return false;
  }
  return true;
}

std::string MPModelProtoExporter::GetVariableName(int var_index) const {
  const MPVariableProto& var_proto = proto_.variable(var_index);
  if (use_obfuscated_names_ || !var_proto.has_name()) {
    return StringPrintf("V%0*d", num_digits_for_variables_, var_index);
  } else {
    return var_proto.name();
  }
}

std::string MPModelProtoExporter::GetConstraintName(int cst_index) const {
  const MPConstraintProto& ct_proto = proto_.constraint(cst_index);
  if (use_obfuscated_names_ || !ct_proto.has_name()) {
    return StringPrintf("C%0*d", num_digits_for_constraints_, cst_index);
  } else {
    return ct_proto.name();
  }
}

void MPModelProtoExporter::AppendComments(const std::string& separator,
                                          std::string* output) const {
  const char* const sep = separator.c_str();
  StringAppendF(output, "%s Generated by MPModelProtoExporter\n", sep);
  StringAppendF(output, "%s   %-16s : %s\n", sep, "Name",
                proto_.has_name() ? proto_.name().c_str() : "NoName");
  StringAppendF(output, "%s   %-16s : %s\n", sep, "Format",
                use_fixed_mps_format_ ? "Fixed" : "Free");
  StringAppendF(output, "%s   %-16s : %d\n", sep, "Constraints",
                proto_.constraint_size());
  StringAppendF(output, "%s   %-16s : %d\n", sep, "Variables",
                proto_.variable_size());
  StringAppendF(output, "%s     %-14s : %d\n", sep, "Binary",
                num_binary_variables_);
  StringAppendF(output, "%s     %-14s : %d\n", sep, "Integer",
                num_integer_variables_);
  StringAppendF(output, "%s     %-14s : %d\n", sep, "Continuous",
                num_continuous_variables_);
  if (FLAGS_lp_shows_unused_variables) {
    StringAppendF(output, "%s Unused variables are shown\n", sep);
  }
}

namespace {
class LineBreaker {
 public:
  explicit LineBreaker(int max_line_size) :
      max_line_size_(max_line_size), line_size_(0), output_() {}
  // Lines are broken in such a way that:
  // - Strings that are given to Append() are never split.
  // - Lines are split so that their length doesn't exceed the max length;
  //   unless a single std::string given to Append() exceeds that length (in which
  //   case it will be put alone on a single unsplit line).
  void Append(const std::string& s);

  // Returns true if std::string s will fit on the current line without adding
  // a carriage return.
  bool WillFit(const std::string& s) {
    return line_size_ + s.size() < max_line_size_;
  }

  // "Consumes" size characters on the line. Used when starting the constraint
  // lines.
  void Consume(int size) { line_size_ += size; }

  std::string GetOutput() const { return output_; }

 private:
  int max_line_size_;
  int line_size_;
  std::string output_;
};

void LineBreaker::Append(const std::string& s) {
  line_size_ += s.size();
  if (line_size_ > max_line_size_) {
    line_size_ = s.size();
    StrAppend(&output_, "\n ");
  }
  StrAppend(&output_, s);
}

}  // namespace

bool MPModelProtoExporter::WriteLpTerm(int var_index, double coefficient,
                                       std::string* output) const {
  output->clear();
  if (var_index < 0 || var_index >= proto_.variable_size()) {
    LOG(DFATAL) << "Reference to out-of-bounds variable index # " << var_index;
    return false;
  }
  if (coefficient != 0.0) {
    *output = StringPrintf("%+.16G %-s ", coefficient,
                           GetVariableName(var_index).c_str());
  }
  return true;
}

namespace {
bool IsBoolean(const MPVariableProto& var) {
  return var.is_integer() && ceil(var.lower_bound()) == 0.0 &&
         floor(var.upper_bound()) == 1.0;
}
}  // namespace

bool MPModelProtoExporter::Setup() {
  num_digits_for_constraints_ =
      StringPrintf("%d", proto_.constraint_size()).size();
  num_digits_for_variables_ = StringPrintf("%d", proto_.variable_size()).size();
  num_binary_variables_ = 0;
  num_integer_variables_ = 0;
  for (const MPVariableProto& var : proto_.variable()) {
    if (var.is_integer()) {
      if (IsBoolean(var)) {
        ++num_binary_variables_;
      } else {
        ++num_integer_variables_;
      }
    }
  }
  num_continuous_variables_ =
      proto_.variable_size() - num_binary_variables_ - num_integer_variables_;
  return true;
}

bool MPModelProtoExporter::CheckAllNamesValidity() const {
  // Note: CheckNameValidity() takes care of the logging.
  for (int i = 0; i < proto_.variable_size(); ++i) {
    if (!CheckNameValidity(GetVariableName(i))) return false;
  }
  for (int i = 0; i < proto_.constraint_size(); ++i) {
    if (!CheckNameValidity(GetConstraintName(i))) return false;
  }
  return true;
}

bool MPModelProtoExporter::ExportModelAsLpFormat(bool obfuscated,
                                                 std::string* output) {
  // TODO(user):
  // - Sort constraints by category (implication, knapsack, logical or, etc...).

  if (!obfuscated && !CheckAllNamesValidity()) {
    return false;
  }
  if (!setup_done_) {
    if (!Setup()) {
      return false;
    }
  }
  setup_done_ = true;
  use_obfuscated_names_ = obfuscated;
  output->clear();

  // Comments section.
  AppendComments("\\", output);

  // Objective
  StrAppend(output, proto_.maximize() ? "Maximize\n" : "Minimize\n");
  LineBreaker obj_line_breaker(FLAGS_lp_max_line_length);
  obj_line_breaker.Append(" Obj: ");
  if (proto_.objective_offset() != 0.0) {
    obj_line_breaker.Append(StringPrintf("%-+.16G Constant ",
                                         proto_.objective_offset()));
  }
  std::vector<bool> show_variable(proto_.variable_size(),
                             FLAGS_lp_shows_unused_variables);
  for (int var_index = 0; var_index < proto_.variable_size(); ++var_index) {
    const double coeff = proto_.variable(var_index).objective_coefficient();
    std::string term;
    if (!WriteLpTerm(var_index, coeff, &term)) {
      return false;
    }
    obj_line_breaker.Append(term);
    show_variable[var_index] = coeff != 0.0 || FLAGS_lp_shows_unused_variables;
  }
  // Constraints
  StrAppend(output, obj_line_breaker.GetOutput(), "\nSubject to\n");
  for (int cst_index = 0; cst_index < proto_.constraint_size(); ++cst_index) {
    const MPConstraintProto& ct_proto = proto_.constraint(cst_index);
    std::string name = GetConstraintName(cst_index);
    LineBreaker line_breaker(FLAGS_lp_max_line_length);
    const int kNumFormattingChars = 10;  // Overevaluated.
    // Account for the size of the constraint name + possibly "_rhs" +
    // the formatting characters here.
    line_breaker.Consume(kNumFormattingChars + name.size());
    for (int i = 0; i < ct_proto.var_index_size(); ++i) {
      const int var_index = ct_proto.var_index(i);
      const double coeff = ct_proto.coefficient(i);
      std::string term;
      if (!WriteLpTerm(var_index, coeff, &term)) {
        return false;
      }
      line_breaker.Append(term);
      show_variable[var_index] =
          coeff != 0.0 || FLAGS_lp_shows_unused_variables;
    }
    const double lb = ct_proto.lower_bound();
    const double ub = ct_proto.upper_bound();
    if (lb == ub) {
      line_breaker.Append(StringPrintf(" = %-.16G\n", ub));
      StrAppend(output, " ", name, ": ", line_breaker.GetOutput());
    } else {
      if (ub != +std::numeric_limits<double>::infinity()) {
        std::string rhs_name = name;
        if (lb != -std::numeric_limits<double>::infinity()) {
          rhs_name += "_rhs";
        }
        StrAppend(output, " ", rhs_name, ": ", line_breaker.GetOutput());
        const std::string relation = StringPrintf(" <= %-.16G\n", ub);
        // Here we have to make sure we do not add the relation to the contents
        // of line_breaker, which may be used in the subsequent clause.
        if (!line_breaker.WillFit(relation)) StrAppend(output, "\n ");
        StrAppend(output, relation);
      }
      if (lb != -std::numeric_limits<double>::infinity()) {
        std::string lhs_name = name;
        if (ub != +std::numeric_limits<double>::infinity()) {
          lhs_name += "_lhs";
        }
        StrAppend(output, " ", lhs_name, ": ", line_breaker.GetOutput());
        const std::string relation = StringPrintf(" >= %-.16G\n", lb);
        if (!line_breaker.WillFit(relation)) StrAppend(output, "\n ");
        StrAppend(output, relation);
      }
    }
  }

  // Bounds
  StringAppendF(output, "Bounds\n");
  if (proto_.objective_offset() != 0.0) {
    StringAppendF(output, " 1 <= Constant <= 1\n");
  }
  for (int var_index = 0; var_index < proto_.variable_size(); ++var_index) {
    if (!show_variable[var_index]) continue;
    const MPVariableProto& var_proto = proto_.variable(var_index);
    const double lb = var_proto.lower_bound();
    const double ub = var_proto.upper_bound();
    if (var_proto.is_integer() && lb == round(lb) && ub == round(ub)) {
      StringAppendF(output, " %.0f <= %s <= %.0f\n", lb,
                    GetVariableName(var_index).c_str(), ub);
    } else {
      if (lb != -std::numeric_limits<double>::infinity()) {
        StringAppendF(output, " %-.16G <= ", lb);
      }
      StringAppendF(output, "%s", GetVariableName(var_index).c_str());
      if (ub != std::numeric_limits<double>::infinity()) {
        StringAppendF(output, " <= %-.16G", ub);
      }
      StringAppendF(output, "\n");
    }
  }

  // Binaries
  if (num_binary_variables_ > 0) {
    StringAppendF(output, "Binaries\n");
    for (int var_index = 0; var_index < proto_.variable_size(); ++var_index) {
      if (!show_variable[var_index]) continue;
      const MPVariableProto& var_proto = proto_.variable(var_index);
      if (IsBoolean(var_proto)) {
        StringAppendF(output, " %s\n", GetVariableName(var_index).c_str());
      }
    }
  }

  // Generals
  if (num_integer_variables_ > 0) {
    StringAppendF(output, "Generals\n");
    for (int var_index = 0; var_index < proto_.variable_size(); ++var_index) {
      if (!show_variable[var_index]) continue;
      const MPVariableProto& var_proto = proto_.variable(var_index);
      if (var_proto.is_integer() && !IsBoolean(var_proto)) {
        StringAppendF(output, " %s\n", GetVariableName(var_index).c_str());
      }
    }
  }
  StringAppendF(output, "End\n");
  return true;
}

void MPModelProtoExporter::AppendMpsPair(const std::string& name, double value,
                                         std::string* output) const {
  const int kFixedMpsDoubleWidth = 12;
  if (use_fixed_mps_format_) {
    int precision = kFixedMpsDoubleWidth;
    std::string value_str = StringPrintf("%.*G", precision, value);
    // Use the largest precision that can fit into the field witdh.
    while (value_str.size() > kFixedMpsDoubleWidth) {
      --precision;
      value_str = StringPrintf("%.*G", precision, value);
    }
    StringAppendF(output, "  %-8s  %*s ", name.c_str(), kFixedMpsDoubleWidth,
                  value_str.c_str());
  } else {
    StringAppendF(output, "  %-16s  %21.16G ", name.c_str(), value);
  }
}

void MPModelProtoExporter::AppendMpsLineHeader(const std::string& id,
                                               const std::string& name,
                                               std::string* output) const {
  StringAppendF(output, use_fixed_mps_format_ ? " %-2s %-8s" : " %-2s  %-16s",
                id.c_str(), name.c_str());
}

void MPModelProtoExporter::AppendMpsLineHeaderWithNewLine(
    const std::string& id, const std::string& name, std::string* output) const {
  AppendMpsLineHeader(id, name, output);
  *output += "\n";
}

void MPModelProtoExporter::AppendMpsTermWithContext(const std::string& head_name,
                                                    const std::string& name,
                                                    double value,
                                                    std::string* output) {
  if (current_mps_column_ == 0) {
    AppendMpsLineHeader("", head_name, output);
  }
  AppendMpsPair(name, value, output);
  AppendNewLineIfTwoColumns(output);
}

void MPModelProtoExporter::AppendMpsBound(const std::string& bound_type,
                                          const std::string& name, double value,
                                          std::string* output) const {
  AppendMpsLineHeader(bound_type, "BOUND", output);
  AppendMpsPair(name, value, output);
  *output += "\n";
}

void MPModelProtoExporter::AppendNewLineIfTwoColumns(std::string* output) {
  ++current_mps_column_;
  if (current_mps_column_ == 2) {
    *output += "\n";
    current_mps_column_ = 0;
  }
}

// Decide whether to use fixed- or free-form MPS format.
bool MPModelProtoExporter::CanUseFixedMpsFormat() const {
  const int kMpsFieldSize = 8;
  if (use_obfuscated_names_) {
    return num_digits_for_constraints_ < kMpsFieldSize &&
           num_digits_for_variables_ < kMpsFieldSize;
  }
  for (const MPConstraintProto& ct_proto : proto_.constraint()) {
    if (ct_proto.name().size() > kMpsFieldSize) return false;
  }
  for (const MPVariableProto& var_proto : proto_.variable()) {
    if (var_proto.name().size() > kMpsFieldSize) return false;
  }
  return true;
}

void MPModelProtoExporter::AppendMpsColumns(bool integrality,
    const std::vector<std::vector<std::pair<int, double>>>& transpose, std::string* output) {
  current_mps_column_ = 0;
  for (int var_index = 0; var_index < proto_.variable_size(); ++var_index) {
    const MPVariableProto& var_proto = proto_.variable(var_index);
    if (var_proto.is_integer() != integrality) continue;
    const std::string var_name = GetVariableName(var_index);
    current_mps_column_ = 0;
    if (var_proto.objective_coefficient() != 0.0) {
      AppendMpsTermWithContext(var_name, "COST",
                               var_proto.objective_coefficient(),
                               output);
    }
    for (const std::pair<int, double> cst_index_and_coeff : transpose[var_index]) {
      const std::string cst_name = GetConstraintName(cst_index_and_coeff.first);
      AppendMpsTermWithContext(var_name, cst_name, cst_index_and_coeff.second,
                               output);
    }
    AppendNewLineIfTwoColumns(output);
  }
}

bool MPModelProtoExporter::ExportModelAsMpsFormat(bool fixed_format,
                                                  bool obfuscated,
                                                  std::string* output) {
  if (!obfuscated && !CheckAllNamesValidity()) {
    return false;
  }
  if (!setup_done_) {
    if (!Setup()) {
      return false;
    }
  }
  setup_done_ = true;
  use_obfuscated_names_ = obfuscated;
  use_fixed_mps_format_ = fixed_format;
  if (fixed_format && !CanUseFixedMpsFormat()) {
    LOG(WARNING) << "Cannot use fixed format. Falling back to free format";
    use_fixed_mps_format_ = false;
  }
  output->clear();

  // Comments.
  AppendComments("*", output);

  // NAME section.
  StringAppendF(output, "%-14s%s\n", "NAME", proto_.name().c_str());

  // ROWS section.
  current_mps_column_ = 0;
  std::string rows_section;
  AppendMpsLineHeaderWithNewLine("N", "COST", &rows_section);
  for (int cst_index = 0; cst_index < proto_.constraint_size(); ++cst_index) {
    const MPConstraintProto& ct_proto = proto_.constraint(cst_index);
    const double lb = ct_proto.lower_bound();
    const double ub = ct_proto.upper_bound();
    const std::string cst_name = GetConstraintName(cst_index);
    if (lb == ub) {
      AppendMpsLineHeaderWithNewLine("E", cst_name, &rows_section);
    } else if (lb == -std::numeric_limits<double>::infinity()) {
      DCHECK_NE(std::numeric_limits<double>::infinity(), ub);
      AppendMpsLineHeaderWithNewLine("L", cst_name, &rows_section);
    } else {
      DCHECK_NE(-std::numeric_limits<double>::infinity(), lb);
      AppendMpsLineHeaderWithNewLine("G", cst_name, &rows_section);
    }
  }
  if (!rows_section.empty()) {
    *output += "ROWS\n" + rows_section;
  }

  // As the information regarding a column needs to be contiguous, we create
  // a map associating a variable to a vector containing the indices of the
  // constraints where this variable appears.
  std::vector<std::vector<std::pair<int, double>>> transpose(proto_.variable_size());
  for (int cst_index = 0; cst_index < proto_.constraint_size(); ++cst_index) {
    const MPConstraintProto& ct_proto = proto_.constraint(cst_index);
    for (int k = 0; k < ct_proto.var_index_size(); ++k) {
      const int var_index = ct_proto.var_index(k);
      if (var_index < 0 || var_index >= proto_.variable_size()) {
        LOG(DFATAL) << "In constraint #" << cst_index << ", var_index #" << k
                    << " is " << var_index << ", which is out of bounds.";
        return false;
      }
      const double coeff = ct_proto.coefficient(k);
      if (coeff != 0.0) {
        transpose[var_index].push_back(std::pair<int, double>(cst_index, coeff));
      }
    }
  }

  // COLUMNS section.
  std::string columns_section;
  AppendMpsColumns(/*integrality=*/true, transpose, &columns_section);
  if (!columns_section.empty()) {
    const char* const kIntMarkerFormat = "  %-10s%-36s%-10s\n";
    columns_section = StringPrintf(kIntMarkerFormat, "INTSTART",
                                   "'MARKER'", "'INTORG'") + columns_section;
    StringAppendF(&columns_section, kIntMarkerFormat,
                  "INTEND", "'MARKER'", "'INTEND'");
  }
  AppendMpsColumns(/*integrality=*/false, transpose, &columns_section);
  if (!columns_section.empty()) {
    *output += "COLUMNS\n" + columns_section;
  }

  // RHS (right-hand-side) section.
  current_mps_column_ = 0;
  std::string rhs_section;
  for (int cst_index = 0; cst_index < proto_.constraint_size(); ++cst_index) {
    const MPConstraintProto& ct_proto = proto_.constraint(cst_index);
    const double lb = ct_proto.lower_bound();
    const double ub = ct_proto.upper_bound();
    const std::string cst_name = GetConstraintName(cst_index);
    if (lb != -std::numeric_limits<double>::infinity()) {
      AppendMpsTermWithContext("RHS", cst_name, lb, &rhs_section);
    } else if (ub != +std::numeric_limits<double>::infinity()) {
      AppendMpsTermWithContext("RHS", cst_name, ub, &rhs_section);
    }
  }
  AppendNewLineIfTwoColumns(&rhs_section);
  if (!rhs_section.empty()) {
    *output += "RHS\n" + rhs_section;
  }

  // RANGES section.
  current_mps_column_ = 0;
  std::string ranges_section;
  for (int cst_index = 0; cst_index < proto_.constraint_size(); ++cst_index) {
    const MPConstraintProto& ct_proto = proto_.constraint(cst_index);
    const double range = fabs(ct_proto.upper_bound() - ct_proto.lower_bound());
    if (range != 0.0 && range != +std::numeric_limits<double>::infinity()) {
      const std::string cst_name = GetConstraintName(cst_index);
      AppendMpsTermWithContext("RANGE", cst_name, range, &ranges_section);
    }
  }
  AppendNewLineIfTwoColumns(&ranges_section);
  if (!ranges_section.empty()) {
    *output += "RANGES\n" + ranges_section;
  }

  // BOUNDS section.
  current_mps_column_ = 0;
  std::string bounds_section;
  for (int var_index = 0; var_index < proto_.variable_size(); ++var_index) {
    const MPVariableProto& var_proto = proto_.variable(var_index);
    const double lb = var_proto.lower_bound();
    const double ub = var_proto.upper_bound();
    const std::string var_name = GetVariableName(var_index);
    if (var_proto.is_integer()) {
      if (IsBoolean(var_proto)) {
        AppendMpsLineHeader("BV", "BOUND", &bounds_section);
        StringAppendF(&bounds_section, "  %s\n", var_name.c_str());
      } else {
        if (lb != 0.0) {
          AppendMpsBound("LI", var_name, lb, &bounds_section);
        }
        if (ub != +std::numeric_limits<double>::infinity()) {
          AppendMpsBound("UI", var_name, ub, &bounds_section);
        }
      }
    } else {
      if (lb == -std::numeric_limits<double>::infinity() &&
          ub == +std::numeric_limits<double>::infinity()) {
        AppendMpsLineHeader("FR", "BOUND", &bounds_section);
        StringAppendF(&bounds_section, "  %s\n", var_name.c_str());
      } else if (lb == ub) {
        AppendMpsBound("FX", var_name, lb, &bounds_section);
      } else {
        if (lb != 0.0) {
          AppendMpsBound("LO", var_name, lb, &bounds_section);
        } else if (ub == +std::numeric_limits<double>::infinity()) {
          AppendMpsLineHeader("PL", "BOUND", &bounds_section);
          StringAppendF(&bounds_section, "  %s\n", var_name.c_str());
        }
        if (ub != +std::numeric_limits<double>::infinity()) {
          AppendMpsBound("UP", var_name, ub, &bounds_section);
        }
      }
    }
  }
  if (!bounds_section.empty()) {
    *output += "BOUNDS\n" + bounds_section;
  }

  *output += "ENDATA\n";
  return true;
}

}  // namespace operations_research
