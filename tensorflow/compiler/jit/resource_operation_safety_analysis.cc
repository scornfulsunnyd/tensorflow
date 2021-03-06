/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

// ALGORITHM OVERVIEW
// ==================
//
// An XLA cluster hoists all resource reads to be beginning of the cluster
// execution and all the resource writes to the end.  This means it cannot
// enforce arbitrary ordering dependencies (via control or data edges) between
// resource operations.  Since all resource reads happen before all resource
// writes, edges constraining resource reads to happen before resource writes
// are fine, but all other kinds of edges are problematic.  This analysis
// computes the set of pairs of resource operations that cannot be put in the
// same cluster because XLA cannot respect the dependencies between them in the
// TensorFlow program.
//
// TODO(b/112856632): We can, in theory, support Read->Read and Write->Write
// dependencies.
//
// Specifically the result computed by this analysis contains the edge {W, R}
// iff all of these hold true:
//
//   - In the graph (g - {edges from NextIteration to Merge}) there is a path
//     from W to R.
//   - IsEdgeSafe(W, R) == False [defined below]
//   - W != R (note: some resource operations both read from and write to
//     resource variables).
//
// The result is incorrect around loops because we ignore edges from
// NextIteration to Merge, but that should be fine because we don't cluster
// these edges.  For instance, in:
//
// Init -----> Merge <-------+
//               |           |
//               v           |
//             Read          |
//               |           |
//               v           |
//             Write         |
//               |           |
//               v           |
//           NextIteration --+
//
// we won't put (Read, Write) in the returned set.  This is fine if
// auto-clustering can only cluster the Read->Write edge, but it is a problem if
// it clusters the Write->NextIteration->Merge->Read edges instead.  The same
// problem is present for the functional version of the loop above.  We rely on
// auto-clustering to not cluster control flow edges like NextIteration->Merge.
// This is enough to avoid the explicit-control-flow problem shown above.  One
// way to think about this is that we only care about cases where two nodes, A
// and B, would normally have been put in the same cluster but cannot legally be
// in the same cluster because of resourcevar-dependencies.  If A and B would
// normally have been put in the same cluster then all paths between A and B
// would have to be clusterable (otherwise we'd have introduced a cycle).  Ergo
// there could not have been a NextIteration->Merge edge between A and B since
// we don't cluster these edges.
//
// We also rely on auto-clustering to not cluster functional control flow nodes
// that contain resource operations.
//
// IMPLEMENTATION
// --------------
//
// We traverse the graph minus backedges in reverse post order, mapping each
// node to the set of resource operation reaching that node.  Since we visit
// producers before consumers, we can construct the set of reaching operations
// by taking the union of the operations reaching the input nodes.  These
// "reaching resource operations" can then be used to create the pairs of
// incompatible nodes using `IsEdgeSafe`.

#include "tensorflow/compiler/jit/resource_operation_safety_analysis.h"

#include "absl/memory/memory.h"
#include "absl/strings/str_join.h"
#include "absl/types/optional.h"
#include "tensorflow/compiler/tf2xla/resource_operation_table.h"
#include "tensorflow/core/framework/node_def.pb.h"
#include "tensorflow/core/graph/algorithm.h"
#include "tensorflow/core/graph/tensor_id.h"
#include "tensorflow/core/lib/gtl/flatmap.h"
#include "tensorflow/core/lib/gtl/flatset.h"
#include "tensorflow/core/lib/hash/hash.h"
#include "tensorflow/core/util/ptr_util.h"

namespace tensorflow {
namespace {
// Returns true if `n` may call a function.
Status MayCallFunction(const Node& n, const FunctionLibraryDefinition* flib_def,
                       bool* out_result) {
  if (flib_def->Contains(n.type_string())) {
    *out_result = true;
  } else {
    *out_result =
        std::any_of(n.def().attr().begin(), n.def().attr().end(),
                    [](const std::pair<string, AttrValue>& name_attr_pair) {
                      return name_attr_pair.second.has_func();
                    });
  }

  return Status::OK();
}

// Maps `n` to the XlaResourceOpKind corresponding to its operation.  If `n` is
// not a resource operation recognized by XLA then sets `out_resource_op_kind`
// to nullopt.
Status XlaResourceOpKindForNode(
    const Node& n, const FunctionLibraryDefinition* flib_def,
    const std::function<Status(const Node&, bool*)>& resource_ops_to_ignore,
    absl::optional<XlaResourceOpKind>* out_resource_op_kind) {
  bool should_ignore = false;
  if (resource_ops_to_ignore) {
    TF_RETURN_IF_ERROR(resource_ops_to_ignore(n, &should_ignore));
  }
  if (should_ignore) {
    *out_resource_op_kind = absl::nullopt;
    return Status::OK();
  }

  const XlaResourceOpInfo* op_info = GetResourceOpInfoForOp(n.type_string());
  if (op_info) {
    *out_resource_op_kind = op_info->kind();
    return Status::OK();
  }

  // We conservatively assume that functions will both read and write resource
  // variables.  In the future we may consider doing some form of
  // inter-procedural analysis.
  bool may_call_function;
  TF_RETURN_IF_ERROR(MayCallFunction(n, flib_def, &may_call_function));
  if (may_call_function) {
    *out_resource_op_kind = XlaResourceOpKind::kReadWrite;
  } else {
    *out_resource_op_kind = absl::nullopt;
  }

  return Status::OK();
}

// Returns true if a control or data dependence from a TensorFlow operation of
// resource op kind `from` to a TensorFlow operation of resource op kind `to`
// can be represented by an XLA cluster and needs no special handling around
// auto-jit.
bool IsEdgeSafe(XlaResourceOpKind from, XlaResourceOpKind to) {
  // XLA clusters forces all reads to happen before all writes, which means the
  // kinds of edges it can faithfully represent are: Read->Write, Read->Modify,
  // Modify->Write, Read->Read, Write->Write.
  //
  // TODO(b/112856632): We can, in theory, support Read->Read and Write->Write
  // dependencies.
  return from == XlaResourceOpKind::kRead && to == XlaResourceOpKind::kWrite;
}

using ResourceOp = std::pair<int, XlaResourceOpKind>;

string ResourceOpToString(const ResourceOp& resource_op) {
  return absl::StrCat(
      resource_op.first, ": ",
      XlaResourceOpInfo::XlaResourceOpKindToString(resource_op.second));
}

// A copy-on-write set used to store the set of ResourceOps reaching a node in a
// TensorFlow graph.
//
// TODO(sanjoy): It may be useful to pull this out into its own header at some
// point.
class ResourceOpSet {
 private:
  using Impl = gtl::FlatSet<ResourceOp>;

 public:
  ResourceOpSet() = default;

  // Adds all ResourceOp s in `other` to this set.
  void Add(const ResourceOpSet& other) {
    CHECK(!frozen_);
    if (other.impl_ == impl_) {
      other.frozen_ = true;
      return;
    }

    if (!impl_) {
      other.frozen_ = true;
      impl_ = other.impl_;
      return;
    }

    for (ResourceOp resource_op : other) {
      Add(resource_op);
    }
  }

  void Add(const ResourceOp& resource_op) {
    CHECK(!frozen_);
    if (!IsCopy() && Contains(resource_op)) {
      // We can avoid the copy if the item we want to insert already exists.
      return;
    }

    EnsureIsCopied();
    impl_->insert(resource_op);
  }

  Impl::const_iterator begin() const {
    return impl_ ? impl_->begin() : GetEmptyImpl()->begin();
  }

  Impl::const_iterator end() const {
    return impl_ ? impl_->end() : GetEmptyImpl()->end();
  }

  bool Contains(const ResourceOp& resource_op) const {
    return impl_ != nullptr && impl_->count(resource_op);
  }

 private:
  bool IsCopy() const { return storage_ != nullptr; }

  void EnsureIsCopied() {
    if (storage_ == nullptr) {
      storage_ = absl::make_unique<Impl>();
      for (ResourceOp op : *this) {
        storage_->insert(op);
      }
      impl_ = storage_.get();
    }
  }

  static Impl* GetEmptyImpl() {
    static Impl* empty_impl = new Impl;
    return empty_impl;
  }

  Impl* impl_ = nullptr;
  std::unique_ptr<Impl> storage_;

  // frozen_ is true if there is another set pointing to this set's impl_.  We
  // can no longer add elements to this set in that case since the sets pointing
  // to this set expect the contents of this set to be stable.
  mutable bool frozen_ = false;

  TF_DISALLOW_COPY_AND_ASSIGN(ResourceOpSet);
};

string ResourceOpSetToString(const ResourceOpSet& resource_op_set) {
  std::vector<string> elements_debug_string;
  std::transform(resource_op_set.begin(), resource_op_set.end(),
                 std::back_inserter(elements_debug_string), ResourceOpToString);
  return absl::StrCat("{", absl::StrJoin(elements_debug_string, ","), "}");
}

string NodeToString(const Node& n, XlaResourceOpKind resource_op_kind) {
  return absl::StrCat(
      "[", n.name(), ": ", n.type_string(), "(",
      XlaResourceOpInfo::XlaResourceOpKindToString(resource_op_kind), ")", "]");
}
}  // namespace

Status ComputeIncompatibleResourceOperationPairs(
    const Graph& g, const FunctionLibraryDefinition* flib_def,
    const std::function<Status(const Node&, bool*)>& resource_ops_to_ignore,
    std::vector<std::pair<int, int>>* result) {
  CHECK(result->empty());

  std::vector<Node*> rpo;
  GetReversePostOrder(g, &rpo, /*stable_comparator=*/NodeComparatorName(),
                      /*edge_filter=*/[](const Edge& edge) {
                        return !edge.src()->IsNextIteration();
                      });

  auto resource_op_set_for_node =
      absl::make_unique<ResourceOpSet[]>(g.num_node_ids());

  const bool vlog = VLOG_IS_ON(2);

  for (Node* n : rpo) {
    absl::optional<XlaResourceOpKind> op_kind;
    TF_RETURN_IF_ERROR(XlaResourceOpKindForNode(
        *n, flib_def, resource_ops_to_ignore, &op_kind));

    ResourceOpSet* resource_op_set = &resource_op_set_for_node[n->id()];

    // Merge the reaching resource operations for all the incoming edges to
    // create the set of all possible resource ops reaching `n`.
    for (const Edge* e : n->in_edges()) {
      if (n->IsMerge() && e->src()->IsNextIteration()) {
        // Ignore back-edges (see file comment).
        continue;
      }

      const ResourceOpSet& incoming_op_set =
          resource_op_set_for_node[e->src()->id()];
      resource_op_set->Add(incoming_op_set);
    }

    // Add to the "incompatible resource ops" set if necessary.
    if (op_kind) {
      for (ResourceOp incoming_op : *resource_op_set) {
        if (IsEdgeSafe(incoming_op.second, *op_kind)) {
          continue;
        }

        if (vlog) {
          VLOG(2) << "Unsafe edge: "
                  << NodeToString(*g.FindNodeId(incoming_op.first),
                                  incoming_op.second)
                  << " -> " << NodeToString(*n, *op_kind);
        }
        result->push_back({incoming_op.first, n->id()});
      }

      resource_op_set->Add({n->id(), *op_kind});
    }

    if (vlog) {
      VLOG(3) << n->name() << " -> " << ResourceOpSetToString(*resource_op_set);
    }
  }

  std::sort(result->begin(), result->end());
  CHECK(std::unique(result->begin(), result->end()) == result->end());

  return Status::OK();
}
}  // namespace tensorflow
