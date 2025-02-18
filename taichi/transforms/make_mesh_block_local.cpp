#include "taichi/ir/ir.h"
#include "taichi/ir/statements.h"
#include "taichi/ir/transforms.h"
#include "taichi/ir/analysis.h"
#include "taichi/transforms/make_mesh_block_local.h"

namespace taichi {
namespace lang {

const PassID MakeMeshBlockLocal::id = "MakeMeshBlockLocal";

void MakeMeshBlockLocal::simplify_nested_conversion() {
  std::vector<MeshIndexConversionStmt *> stmts;
  std::vector<Stmt *> ori_indices;

  irpass::analysis::gather_statements(offload->body.get(), [&](Stmt *stmt) {
    if (auto conv1 = stmt->cast<MeshIndexConversionStmt>()) {
      if (auto conv2 = conv1->idx->cast<MeshIndexConversionStmt>()) {
        if (conv1->conv_type == mesh::ConvType::g2r &&
            conv2->conv_type == mesh::ConvType::l2g &&
            conv1->mesh == conv2->mesh &&
            conv1->idx_type == conv2->idx_type) {  // nested
          stmts.push_back(conv1);
          ori_indices.push_back(conv2->idx);
        }
      }
    }
    return false;
  });

  for (size_t i = 0; i < stmts.size(); ++i) {
    stmts[i]->replace_with(Stmt::make<MeshIndexConversionStmt>(
        stmts[i]->mesh, stmts[i]->idx_type, ori_indices[i],
        mesh::ConvType::l2r));
  }
}

void MakeMeshBlockLocal::gather_candidate_mapping() {
  irpass::analysis::gather_statements(offload->body.get(), [&](Stmt *stmt) {
    if (auto conv = stmt->cast<MeshIndexConversionStmt>()) {
      if (conv->conv_type != mesh::ConvType::g2r) {
        bool is_from_end = (conv->idx_type == offload->major_from_type);
        bool is_to_end = false;
        for (auto type : offload->major_to_types) {
          is_to_end |= (conv->idx_type == type);
        }
        for (auto rel : offload->minor_relation_types) {
          auto from_type =
              mesh::MeshElementType(mesh::from_end_element_order(rel));
          auto to_type = mesh::MeshElementType(mesh::to_end_element_order(rel));
          is_from_end |= (conv->idx_type == from_type);
          is_to_end |= (conv->idx_type == to_type);
        }
        if ((is_to_end && config.mesh_localize_to_end_mapping) ||
            (is_from_end && config.mesh_localize_from_end_mapping)) {
          mappings.insert(std::make_pair(conv->idx_type, conv->conv_type));
        }
      }
    }
    return false;
  });
}

void MakeMeshBlockLocal::replace_conv_statements() {
  std::vector<MeshIndexConversionStmt *> idx_conv_stmts;

  irpass::analysis::gather_statements(offload->body.get(), [&](Stmt *stmt) {
    if (auto idx_conv = stmt->cast<MeshIndexConversionStmt>()) {
      if (idx_conv->mesh == offload->mesh && idx_conv->conv_type == conv_type &&
          idx_conv->idx_type == element_type) {
        idx_conv_stmts.push_back(idx_conv);
      }
    }
    return false;
  });

  for (auto stmt : idx_conv_stmts) {
    VecStatement bls;
    Stmt *bls_element_offset_bytes = bls.push_back<ConstStmt>(
        LaneAttribute<TypedConstant>{(int32)mapping_bls_offset_in_bytes});
    Stmt *idx_byte = bls.push_back<BinaryOpStmt>(
        BinaryOpType::mul, stmt->idx,
        bls.push_back<ConstStmt>(TypedConstant(mapping_dtype_size)));
    Stmt *offset = bls.push_back<BinaryOpStmt>(
        BinaryOpType::add, bls_element_offset_bytes, idx_byte);
    Stmt *bls_ptr = bls.push_back<BlockLocalPtrStmt>(
        offset,
        TypeFactory::create_vector_or_scalar_type(1, mapping_data_type, true));
    [[maybe_unused]] Stmt *bls_load = bls.push_back<GlobalLoadStmt>(bls_ptr);
    stmt->replace_with(std::move(bls));
  }
}

void MakeMeshBlockLocal::replace_global_ptrs(SNode *snode) {
  auto data_type = snode->dt.ptr_removed();
  auto dtype_size = data_type_size(data_type);
  auto offset_in_bytes = attr_bls_offset_in_bytes.find(snode)->second;

  std::vector<GlobalPtrStmt *> global_ptrs;
  irpass::analysis::gather_statements(offload->body.get(), [&](Stmt *stmt) {
    if (auto global_ptr = stmt->cast<GlobalPtrStmt>()) {
      TI_ASSERT(global_ptr->width() == 1);
      if (global_ptr->snodes[0] == snode &&
          global_ptr->indices[0]->is<MeshIndexConversionStmt>()) {
        global_ptrs.push_back(global_ptr);
      }
    }
    return false;
  });

  for (auto global_ptr : global_ptrs) {
    VecStatement bls;
    Stmt *local_idx =
        global_ptr->indices[0]->as<MeshIndexConversionStmt>()->idx;
    Stmt *local_idx_byte = bls.push_back<BinaryOpStmt>(
        BinaryOpType::mul, local_idx,
        bls.push_back<ConstStmt>(TypedConstant(dtype_size)));
    Stmt *offset =
        bls.push_back<ConstStmt>(TypedConstant(int32(offset_in_bytes)));
    Stmt *index =
        bls.push_back<BinaryOpStmt>(BinaryOpType::add, offset, local_idx_byte);
    [[maybe_unused]] Stmt *bls_ptr = bls.push_back<BlockLocalPtrStmt>(
        index, TypeFactory::create_vector_or_scalar_type(1, data_type, true));
    global_ptr->replace_with(std::move(bls));
  }

  // in the cpu backend, atomic op in body block could be demoted to non-atomic
  if (config.arch != Arch::x64) {
    return;
  }
  std::vector<AtomicOpStmt *> atomic_ops;
  irpass::analysis::gather_statements(offload->body.get(), [&](Stmt *stmt) {
    if (auto atomic_op = stmt->cast<AtomicOpStmt>()) {
      if (atomic_op->op_type == AtomicOpType::add &&
          atomic_op->dest->is<BlockLocalPtrStmt>()) {
        atomic_ops.push_back(atomic_op);
      }
    }
    return false;
  });

  for (auto atomic_op : atomic_ops) {
    VecStatement non_atomic;
    Stmt *dest_val = non_atomic.push_back<GlobalLoadStmt>(atomic_op->dest);
    Stmt *res_val = non_atomic.push_back<BinaryOpStmt>(
        BinaryOpType::add, dest_val, atomic_op->val);
    non_atomic.push_back<GlobalStoreStmt>(atomic_op->dest, res_val);
    atomic_op->replace_with(std::move(non_atomic));
  }
}

// This function creates loop like:
// int i = start_val;
// while (i < end_val) {
//  body(i);
//  i += blockDim.x;
// }
Stmt *MakeMeshBlockLocal::create_xlogue(
    Stmt *start_val,
    Stmt *end_val,
    std::function<void(Block * /*block*/, Stmt * /*idx_val*/)> body_) {
  Stmt *idx = block->push_back<AllocaStmt>(mapping_data_type);
  [[maybe_unused]] Stmt *init_val =
      block->push_back<LocalStoreStmt>(idx, start_val);
  Stmt *block_dim_val;
  if (config.arch == Arch::x64) {
    block_dim_val = block->push_back<ConstStmt>(TypedConstant(1));
  } else {
    block_dim_val = block->push_back<ConstStmt>(
        LaneAttribute<TypedConstant>{offload->block_dim});
  }

  std::unique_ptr<Block> body = std::make_unique<Block>();
  {
    Stmt *idx_val = body->push_back<LocalLoadStmt>(LocalAddress{idx, 0});
    Stmt *cond =
        body->push_back<BinaryOpStmt>(BinaryOpType::cmp_lt, idx_val, end_val);
    body->push_back<WhileControlStmt>(nullptr, cond);
    body_(body.get(), idx_val);
    Stmt *idx_val_ = body->push_back<BinaryOpStmt>(BinaryOpType::add, idx_val,
                                                   block_dim_val);
    [[maybe_unused]] Stmt *idx_store =
        body->push_back<LocalStoreStmt>(idx, idx_val_);
  }
  block->push_back<WhileStmt>(std::move(body));
  Stmt *idx_val = block->push_back<LocalLoadStmt>(LocalAddress{idx, 0});
  return idx_val;
}

// This function creates loop like:
// int i = start_val;
// while (i < end_val) {
//  mapping_shared[i] = global_val(i);
//  i += blockDim.x;
// }
Stmt *MakeMeshBlockLocal::create_cache_mapping(
    Stmt *start_val,
    Stmt *end_val,
    std::function<Stmt *(Block * /*block*/, Stmt * /*idx_val*/)> global_val) {
  Stmt *bls_element_offset_bytes = block->push_back<ConstStmt>(
      LaneAttribute<TypedConstant>{(int32)mapping_bls_offset_in_bytes});
  return create_xlogue(start_val, end_val, [&](Block *body, Stmt *idx_val) {
    Stmt *idx_val_byte = body->push_back<BinaryOpStmt>(
        BinaryOpType::mul, idx_val,
        body->push_back<ConstStmt>(TypedConstant(mapping_dtype_size)));
    Stmt *offset = body->push_back<BinaryOpStmt>(
        BinaryOpType::add, bls_element_offset_bytes, idx_val_byte);
    Stmt *bls_ptr = body->push_back<BlockLocalPtrStmt>(
        offset,
        TypeFactory::create_vector_or_scalar_type(1, mapping_data_type, true));
    [[maybe_unused]] Stmt *bls_store =
        body->push_back<GlobalStoreStmt>(bls_ptr, global_val(body, idx_val));
  });
}

void MakeMeshBlockLocal::fetch_attr_to_bls(Block *body,
                                           Stmt *idx_val,
                                           Stmt *mapping_val) {
  auto attrs = rec.find(std::make_pair(element_type, conv_type));
  if (attrs == rec.end()) {
    return;
  }
  for (auto [snode, total_flags] : attrs->second) {
    auto data_type = snode->dt.ptr_removed();
    auto dtype_size = data_type_size(data_type);

    bool bls_has_read = total_flags & AccessFlag::read;
    bool bls_has_write = total_flags & AccessFlag::write;
    bool bls_has_accumulate = total_flags & AccessFlag::accumulate;

    TI_ASSERT_INFO(!bls_has_write, "BLS with write accesses is not supported.");
    TI_ASSERT_INFO(!(bls_has_accumulate && bls_has_read),
                   "BLS with both read and accumulation is not supported.");

    bool first_allocate = {false};
    if (attr_bls_offset_in_bytes.find(snode) ==
        attr_bls_offset_in_bytes.end()) {
      first_allocate = {true};
      bls_offset_in_bytes +=
          (dtype_size - bls_offset_in_bytes % dtype_size) % dtype_size;
      attr_bls_offset_in_bytes.insert(
          std::make_pair(snode, bls_offset_in_bytes));
      bls_offset_in_bytes +=
          dtype_size *
          offload->mesh->patch_max_element_num.find(element_type)->second;
    }
    auto offset_in_bytes = attr_bls_offset_in_bytes.find(snode)->second;

    Stmt *value{nullptr};
    if (bls_has_read) {
      // Read access
      // Fetch from global to BLS
      Stmt *global_ptr = body->push_back<GlobalPtrStmt>(
          LaneAttribute<SNode *>{snode}, std::vector<Stmt *>{mapping_val});
      value = body->push_back<GlobalLoadStmt>(global_ptr);
    } else {
      // Accumulation access
      // Zero-fill
      value = body->push_back<ConstStmt>(TypedConstant(data_type, 0));
    }

    Stmt *offset =
        body->push_back<ConstStmt>(TypedConstant(int32(offset_in_bytes)));
    Stmt *idx_val_byte = body->push_back<BinaryOpStmt>(
        BinaryOpType::mul, idx_val,
        body->push_back<ConstStmt>(TypedConstant(dtype_size)));
    Stmt *index =
        body->push_back<BinaryOpStmt>(BinaryOpType::add, offset, idx_val_byte);
    Stmt *bls_ptr = body->push_back<BlockLocalPtrStmt>(
        index, TypeFactory::create_vector_or_scalar_type(1, data_type, true));
    body->push_back<GlobalStoreStmt>(bls_ptr, value);

    // Step 3-2-1:
    // Make loop body load from BLS instead of global fields
    // NOTE that first_allocate ensures this step only do ONCE
    if (first_allocate) {
      replace_global_ptrs(snode);
    }
  }
}

void MakeMeshBlockLocal::push_attr_to_global(Block *body,
                                             Stmt *idx_val,
                                             Stmt *mapping_val) {
  auto attrs = rec.find(std::make_pair(element_type, conv_type));
  if (attrs == rec.end()) {
    return;
  }
  for (auto [snode, total_flags] : attrs->second) {
    bool bls_has_accumulate = total_flags & AccessFlag::accumulate;
    if (!bls_has_accumulate) {
      continue;
    }
    auto data_type = snode->dt.ptr_removed();
    auto dtype_size = data_type_size(data_type);
    auto offset_in_bytes = attr_bls_offset_in_bytes.find(snode)->second;

    Stmt *offset =
        body->push_back<ConstStmt>(TypedConstant(int32(offset_in_bytes)));
    Stmt *idx_val_byte = body->push_back<BinaryOpStmt>(
        BinaryOpType::mul, idx_val,
        body->push_back<ConstStmt>(TypedConstant(dtype_size)));
    Stmt *index =
        body->push_back<BinaryOpStmt>(BinaryOpType::add, offset, idx_val_byte);
    Stmt *bls_ptr = body->push_back<BlockLocalPtrStmt>(
        index, TypeFactory::create_vector_or_scalar_type(1, data_type, true));
    Stmt *bls_val = body->push_back<GlobalLoadStmt>(bls_ptr);

    Stmt *global_ptr = body->push_back<GlobalPtrStmt>(
        LaneAttribute<SNode *>{snode}, std::vector<Stmt *>{mapping_val});
    body->push_back<AtomicOpStmt>(AtomicOpType::add, global_ptr, bls_val);
  }
}

void MakeMeshBlockLocal::fetch_mapping(
    std::function<
        Stmt *(Stmt * /*start_val*/,
               Stmt * /*end_val*/,
               std::function<Stmt *(Block * /*block*/, Stmt * /*idx_val*/)>)>
        mapping_callback_handler,
    std::function<void(Block *body, Stmt *idx_val, Stmt *mapping_val)>
        attr_callback_handler) {
  Stmt *thread_idx_stmt;
  if (config.arch == Arch::x64) {
    thread_idx_stmt = block->push_back<ConstStmt>(TypedConstant(0));
  } else {
    thread_idx_stmt = block->push_back<LoopLinearIndexStmt>(
        offload);  // Equivalent to CUDA threadIdx
  }
  Stmt *total_element_num = offload->total_num_local.find(element_type)->second;
  Stmt *total_element_offset =
      offload->total_offset_local.find(element_type)->second;

  if (config.optimize_mesh_reordered_mapping &&
      conv_type == mesh::ConvType::l2r) {
    // int i = threadIdx.x;
    // while (i < owned_{}_num) {
    //  mapping_shared[i] = i + owned_{}_offset;
    //    {
    //      x0_shared[i] = x0[mapping_shared[i]];
    //      ...
    //    }
    //  i += blockDim.x;
    // }
    // while (i < total_{}_num) {
    //  mapping_shared[i] = mapping[i + total_{}_offset];
    //    {
    //      x0_shared[i] = x0[mapping_shared[i]];
    //      ...
    //    }
    //  i += blockDim.x;
    // }
    Stmt *owned_element_num =
        offload->owned_num_local.find(element_type)->second;
    Stmt *owned_element_offset =
        offload->owned_offset_local.find(element_type)->second;
    Stmt *pre_idx_val = mapping_callback_handler(
        thread_idx_stmt, owned_element_num, [&](Block *body, Stmt *idx_val) {
          Stmt *global_index = body->push_back<BinaryOpStmt>(
              BinaryOpType::add, idx_val, owned_element_offset);
          attr_callback_handler(body, idx_val, global_index);
          return global_index;
        });
    mapping_callback_handler(
        pre_idx_val, total_element_num, [&](Block *body, Stmt *idx_val) {
          Stmt *global_offset = body->push_back<BinaryOpStmt>(
              BinaryOpType::add, total_element_offset, idx_val);
          Stmt *global_ptr = body->push_back<GlobalPtrStmt>(
              LaneAttribute<SNode *>{mapping_snode},
              std::vector<Stmt *>{global_offset});
          Stmt *global_load = body->push_back<GlobalLoadStmt>(global_ptr);
          attr_callback_handler(body, idx_val, global_load);
          return global_load;
        });
  } else {
    // int i = threadIdx.x;
    // while (i < total_{}_num) {
    //  mapping_shared[i] = mapping[i + total_{}_offset];
    //    {
    //      x0_shared[i] = x0[mapping_shared[i]];
    //      ...
    //    }
    //  i += blockDim.x;
    // }
    mapping_callback_handler(
        thread_idx_stmt, total_element_num, [&](Block *body, Stmt *idx_val) {
          Stmt *global_offset = body->push_back<BinaryOpStmt>(
              BinaryOpType::add, total_element_offset, idx_val);
          Stmt *global_ptr = body->push_back<GlobalPtrStmt>(
              LaneAttribute<SNode *>{mapping_snode},
              std::vector<Stmt *>{global_offset});
          Stmt *global_load = body->push_back<GlobalLoadStmt>(global_ptr);
          attr_callback_handler(body, idx_val, global_load);
          return global_load;
        });
  }
}

MakeMeshBlockLocal::MakeMeshBlockLocal(OffloadedStmt *offload,
                                       const CompileConfig &config)
    : offload(offload), config(config) {
  // Step 0: simplify l2g + g2r -> l2r
  simplify_nested_conversion();

  // Step 1: use Mesh BLS analyzer to gather which mesh attributes user declared
  // to cache
  auto caches = irpass::analysis::initialize_mesh_local_attribute(offload);
  rec = caches->finalize();

  // Step 2: A analyzer to determine which mapping should be localized
  mappings.clear();
  gather_candidate_mapping();
  // If a mesh attribute is in bls, the config makes its index mapping must also
  // be in bls
  if (config.mesh_localize_all_attr_mappings) {
    for (auto [mapping, attr_set] : rec) {
      if (mappings.find(mapping) == mappings.end()) {
        mappings.insert(mapping);
      }
    }
  }

  auto has_acc = [&](mesh::MeshElementType element_type,
                     mesh::ConvType conv_type) {
    auto ptr = rec.find(std::make_pair(element_type, conv_type));
    if (ptr == rec.end()) {
      return false;
    }
    bool has_accumulate = {false};
    for (auto [snode, total_flags] : ptr->second) {
      has_accumulate |= (total_flags & AccessFlag::accumulate);
    }
    return has_accumulate;
  };

  // Step 3: Cache the mappings and the attributes
  bls_offset_in_bytes = offload->bls_size;
  if (offload->bls_prologue == nullptr) {
    offload->bls_prologue = std::make_unique<Block>();
    offload->bls_prologue->parent_stmt = offload;
  }
  if (offload->bls_epilogue == nullptr) {
    offload->bls_epilogue = std::make_unique<Block>();
    offload->bls_epilogue->parent_stmt = offload;
  }

  // Cache both mappings and mesh attribute
  for (auto [element_type, conv_type] : mappings) {
    this->element_type = element_type;
    this->conv_type = conv_type;
    TI_ASSERT(conv_type != mesh::ConvType::g2r);  // g2r will not be cached.
    // There is not corresponding mesh element attribute read/write,
    // It's useless to localize this mapping
    if (offload->total_offset_local.find(element_type) ==
        offload->total_offset_local.end()) {
      continue;
    }

    mapping_snode = (offload->mesh->index_mapping
                         .find(std::make_pair(element_type, conv_type))
                         ->second);
    mapping_data_type = mapping_snode->dt.ptr_removed();
    mapping_dtype_size = data_type_size(mapping_data_type);

    // Ensure BLS alignment
    bls_offset_in_bytes +=
        (mapping_dtype_size - bls_offset_in_bytes % mapping_dtype_size) %
        mapping_dtype_size;
    mapping_bls_offset_in_bytes = bls_offset_in_bytes;
    // allocate storage for the BLS variable
    bls_offset_in_bytes +=
        mapping_dtype_size *
        offload->mesh->patch_max_element_num.find(element_type)->second;

    // Step 3-1:
    // Fetch index mapping to the BLS block
    // Step 3-2
    // Fetch mesh attributes to the BLS block at the same time
    // TODO(changyu): better way to use lambda
    block = offload->bls_prologue.get();
    fetch_mapping(
        [&](Stmt *start_val, Stmt *end_val,
            std::function<Stmt *(Block * /*block*/, Stmt * /*idx_val*/)>
                global_val) {
          return create_cache_mapping(start_val, end_val, global_val);
        },
        [&](Block *body, Stmt *idx_val, Stmt *mapping_val) {
          fetch_attr_to_bls(body, idx_val, mapping_val);
        });

    // Step 3-3:
    // Make mesh index mapping load from BLS instead of global fields
    replace_conv_statements();

    // Step 3-4
    // Atomic-add BLS contribution to its global version if necessary
    if (!has_acc(element_type, conv_type)) {
      continue;
    }
    block = offload->bls_epilogue.get();
    {
      Stmt *thread_idx_stmt = block->push_back<LoopLinearIndexStmt>(
          offload);  // Equivalent to CUDA threadIdx
      Stmt *total_element_num =
          offload->total_num_local.find(element_type)->second;
      Stmt *total_element_offset =
          offload->total_offset_local.find(element_type)->second;
      create_xlogue(
          thread_idx_stmt, total_element_num, [&](Block *body, Stmt *idx_val) {
            Stmt *bls_element_offset_bytes =
                body->push_back<ConstStmt>(LaneAttribute<TypedConstant>{
                    (int32)mapping_bls_offset_in_bytes});
            Stmt *idx_byte = body->push_back<BinaryOpStmt>(
                BinaryOpType::mul, idx_val,
                body->push_back<ConstStmt>(TypedConstant(mapping_dtype_size)));
            Stmt *offset = body->push_back<BinaryOpStmt>(
                BinaryOpType::add, bls_element_offset_bytes, idx_byte);
            Stmt *bls_ptr = body->push_back<BlockLocalPtrStmt>(
                offset, TypeFactory::create_vector_or_scalar_type(
                            1, mapping_data_type, true));
            Stmt *global_val = body->push_back<GlobalLoadStmt>(bls_ptr);
            this->push_attr_to_global(body, idx_val, global_val);
          });
    }
  }

  // Cache mesh attribute only
  for (auto [mapping, attr_set] : rec) {
    if (mappings.find(mapping) != mappings.end()) {
      continue;
    }

    this->element_type = mapping.first;
    this->conv_type = mapping.second;
    TI_ASSERT(conv_type != mesh::ConvType::g2r);  // g2r will not be cached.

    mapping_snode = (offload->mesh->index_mapping
                         .find(std::make_pair(element_type, conv_type))
                         ->second);
    mapping_data_type = mapping_snode->dt.ptr_removed();
    mapping_dtype_size = data_type_size(mapping_data_type);

    // Step 3-1
    // Only fetch mesh attributes to the BLS block
    // TODO(changyu): better way to use lambda
    block = offload->bls_prologue.get();
    fetch_mapping(
        [&](Stmt *start_val, Stmt *end_val,
            std::function<Stmt *(Block * /*block*/, Stmt * /*idx_val*/)>
                global_val) {
          return create_xlogue(
              start_val, end_val,
              [&](Block *block, Stmt *idx_val) { global_val(block, idx_val); });
        },
        [&](Block *body, Stmt *idx_val, Stmt *mapping_val) {
          fetch_attr_to_bls(body, idx_val, mapping_val);
        });

    // Step 3-2
    // Atomic-add BLS contribution to its global version if necessary
    if (!has_acc(element_type, conv_type)) {
      continue;
    }
    block = offload->bls_epilogue.get();
    fetch_mapping(
        [&](Stmt *start_val, Stmt *end_val,
            std::function<Stmt *(Block * /*block*/, Stmt * /*idx_val*/)>
                global_val) {
          return create_xlogue(
              start_val, end_val,
              [&](Block *block, Stmt *idx_val) { global_val(block, idx_val); });
        },
        [&](Block *body, Stmt *idx_val, Stmt *mapping_val) {
          push_attr_to_global(body, idx_val, mapping_val);
        });
  }

  offload->bls_size = std::max(std::size_t(1), bls_offset_in_bytes);
}

void MakeMeshBlockLocal::run(OffloadedStmt *offload,
                             const CompileConfig &config,
                             const std::string &kernel_name) {
  if (offload->task_type != OffloadedStmt::TaskType::mesh_for) {
    return;
  }

  MakeMeshBlockLocal(offload, config);
}

namespace irpass {

// This pass should happen after offloading but before lower_access
void make_mesh_block_local(IRNode *root,
                           const CompileConfig &config,
                           const MakeMeshBlockLocal::Args &args) {
  TI_AUTO_PROF;

  // =========================================================================================
  // This pass generates code like this:
  // // Load V_l2g
  // for (int i = threadIdx.x; i < total_vertices; i += blockDim.x) {
  //   V_l2g[i] = _V_l2g[i + total_vertices_offset];
  //   sx[i] = x[V_l2g[i]];
  //   sJ[i] = 0.0f;
  // }

  if (auto root_block = root->cast<Block>()) {
    for (auto &offload : root_block->statements) {
      MakeMeshBlockLocal::run(offload->cast<OffloadedStmt>(), config,
                              args.kernel_name);
    }
  } else {
    MakeMeshBlockLocal::run(root->as<OffloadedStmt>(), config,
                            args.kernel_name);
  }

  type_check(root, config);
}

}  // namespace irpass
}  // namespace lang
}  // namespace taichi
